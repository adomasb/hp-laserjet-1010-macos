/* Exercise production replay/end_job/drain with the real encoder. Only the
 * PAPPL job facade, transport and time syscalls are controlled; USB cannot open.
 * One successful run uses actual CLOCK_MONOTONIC and nanosleep for eight seconds.
 */
#include <time.h>
#include <stdarg.h>
int controlled_clock_gettime(clockid_t clock, struct timespec *now);
int controlled_nanosleep(const struct timespec *interval, struct timespec *remaining);
#define main service_program_main
#define papplJobGetData controlled_get_data
#define papplJobSetData controlled_set_data
#define papplJobGetID controlled_get_id
#define papplJobGetState controlled_get_state
#define papplJobIsCanceled controlled_is_canceled
#define papplJobSetMessage controlled_set_message
#define papplJobSetReasons controlled_set_reasons
#define hp_transport_sim_open controlled_sim_open
#define hp_transport_usb_open controlled_usb_open
#define hp_transport_write controlled_write
#define hp_transport_finish controlled_finish
#define hp_transport_close controlled_close
#define hp_transport_error controlled_error
#define hp_transport_bytes controlled_bytes
#define hp_usb_available controlled_usb_available
#define clock_gettime controlled_clock_gettime
#define nanosleep controlled_nanosleep
#include "../src/service.c"
#undef main
#undef clock_gettime
#undef nanosleep

#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)
#define SECOND 1000000000ULL
#define TICK 100000000ULL

typedef struct {
  void *data;
  atomic_bool canceled;
  ipp_jstate_t status;
  char message[160];
  unsigned reasons;
} controlled_job;
struct hp_transport {
  uint64_t bytes, bytes_at_wait;
  uint_fast64_t finish_ns, close_ns;
  unsigned writes, finishes, closes;
  bool success, fail_write, fail_finish;
};
static controlled_job job;
static hp_transport transport;
static uint_fast64_t virtual_ns, wait_started_ns, trigger_ns;
static unsigned sleeps, cancellations, sampled_at_sleep;
static bool real_time, interrupt_once, sleep_error, stop_on_trigger, cancel_on_trigger;
static bool saw_processing;
static unsigned expected_pages;

void *controlled_get_data(pappl_job_t *unused) { (void)unused; return job.data; }
void controlled_set_data(pappl_job_t *unused, void *data) { (void)unused; job.data = data; }
int controlled_get_id(pappl_job_t *unused) { (void)unused; return 42; }
ipp_jstate_t controlled_get_state(pappl_job_t *unused) { (void)unused; return job.status; }
bool controlled_is_canceled(pappl_job_t *unused) {
  (void)unused; cancellations++; return atomic_load(&job.canceled);
}
void controlled_set_message(pappl_job_t *unused, const char *format, ...) {
  (void)unused;
  va_list args; va_start(args, format);
  vsnprintf(job.message, sizeof(job.message), format, args); va_end(args);
}
void controlled_set_reasons(pappl_job_t *unused, pappl_jreason_t add, pappl_jreason_t remove) {
  (void)unused; (void)remove;
  CHECK(add == PAPPL_JREASON_ERRORS_DETECTED); job.reasons++;
}
/* No test path can acquire a USB handle, even if setup regresses. */
hp_transport *controlled_usb_open(char *error, size_t size) {
  (void)error; (void)size; CHECK(false); return NULL;
}
hp_transport *controlled_sim_open(const hp_sim_options *options, unsigned id) {
  (void)options; (void)id; CHECK(false); return NULL;
}
bool controlled_usb_available(char *message, size_t size) {
  (void)message; (void)size; CHECK(false); return false;
}
ssize_t controlled_write(void *context, const void *bytes, size_t length) {
  hp_transport *t = context;
  CHECK(t == &transport && bytes && length && !sleeps && !t->finishes && !t->closes);
  t->writes++;
  if (t->fail_write && t->bytes) { errno = EIO; return -1; }
  size_t written = t->fail_write && length > 5 ? 5 : length;
  t->bytes += written;
  return (ssize_t)written;
}
bool controlled_finish(hp_transport *t, bool success) {
  CHECK(t == &transport && !t->finishes && !t->closes);
  t->finishes++; t->success = success; t->finish_ns = monotonic_ns();
  if (sleeps) CHECK(t->bytes == t->bytes_at_wait);
  return !t->fail_finish;
}
void controlled_close(hp_transport *t) {
  CHECK(t == &transport && t->finishes == 1 && !t->closes && !job.data);
  t->closes++; t->close_ns = monotonic_ns();
}
const char *controlled_error(const hp_transport *t) {
  return t->fail_write ? "Controlled USB write failure; no automatic retry" :
         t->fail_finish ? "Controlled USB finish failure; no automatic retry" : "";
}
uint64_t controlled_bytes(const hp_transport *t) { return t->bytes; }

int controlled_clock_gettime(clockid_t clock, struct timespec *now) {
  CHECK(clock == CLOCK_MONOTONIC);
  if (real_time) return clock_gettime(clock, now);
  now->tv_sec = (time_t)(virtual_ns / SECOND);
  now->tv_nsec = (long)(virtual_ns % SECOND);
  return 0;
}
int controlled_nanosleep(const struct timespec *interval, struct timespec *remaining) {
  CHECK(interval->tv_sec == 0 && interval->tv_nsec > 0 && interval->tv_nsec <= (long)TICK);
  CHECK(!transport.finishes && !transport.closes);
  CHECK(cancellations > sampled_at_sleep);
  sampled_at_sleep = cancellations;
  CHECK(!strcmp(app.job_state, "processing") && app.job_id == 42);
  CHECK(app.pages == expected_pages && app.bytes == transport.bytes);
  CHECK(!strcmp(app.message, "Finishing delivery to the printer…") && !strcmp(app.message, job.message));
  if (!sleeps) {
    wait_started_ns = monotonic_ns();
    transport.bytes_at_wait = transport.bytes;
  }
  sleeps++; saw_processing = true;
  if (sleep_error) { errno = EINVAL; return -1; }
  if (interrupt_once) {
    interrupt_once = false; virtual_ns += 50000000ULL; errno = EINTR; return -1;
  }
  if (real_time) return nanosleep(interval, remaining);
  virtual_ns += (uint_fast64_t)interval->tv_nsec;
  if (trigger_ns && virtual_ns >= trigger_ns) {
    if (stop_on_trigger) atomic_store(&app.stopping, true);
    if (cancel_on_trigger) atomic_store(&job.canceled, true);
  }
  return 0;
}

static job_context *prepare(void) {
  memset(&job, 0, sizeof(job)); atomic_init(&job.canceled, false);
  memset(&transport, 0, sizeof(transport));
  atomic_store(&app.stopping, false);
  app.simulate = false;
  app.job_state[0] = app.message[0] = '\0';
  virtual_ns = SECOND; wait_started_ns = trigger_ns = 0;
  sleeps = cancellations = sampled_at_sleep = 0;
  real_time = interrupt_once = sleep_error = stop_on_trigger = cancel_on_trigger = false;
  saw_processing = false; expected_pages = 1; job.status = IPP_JSTATE_PROCESSING;
  job_context *ctx = calloc(1, sizeof(*ctx)); CHECK(ctx);
  ctx->job = (pappl_job_t *)&job; ctx->transport = &transport;
  ctx->copies = 2; ctx->pages = 1;
  snprintf(ctx->path, sizeof(ctx->path), "%s/hp1010-drain-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
  ctx->fd = mkstemp(ctx->path); CHECK(ctx->fd >= 0);
  /* A regular file cannot contain state.json: production state is inspected
   * in memory without writing reports or relying on filesystem permissions. */
  snprintf(app.state_dir, sizeof(app.state_dir), "%s", ctx->path);
  CHECK(!fchmod(ctx->fd, 0600));
  ctx->encoder = hp_fr_create(encoded_write, canceled, ctx); CHECK(ctx->encoder);
  const unsigned char row[] = {0x80};
  CHECK(hp_fr_begin_job(ctx->encoder, 1));
  CHECK(hp_fr_begin_page(ctx->encoder, 8, 1, 26));
  CHECK(hp_fr_write_row(ctx->encoder, row, sizeof(row)));
  CHECK(hp_fr_end_page(ctx->encoder));
  job.data = ctx;
  return ctx;
}
static void complete(bool expected_ok, const char *status, const char *message) {
  job_context *ctx = job.data;
  char path[PATH_MAX]; snprintf(path, sizeof(path), "%s", ctx->path);
  int fd = ctx->fd;
  CHECK(end_job((pappl_job_t *)&job, NULL, NULL) == expected_ok);
  CHECK(transport.finishes == 1 && transport.closes == 1);
  CHECK(transport.close_ns >= transport.finish_ns && !job.data);
  CHECK(!strcmp(app.job_state, status) && !strcmp(app.message, job.message));
  CHECK(strstr(job.message, message));
  CHECK(app.bytes == transport.bytes && app.pages == expected_pages);
  CHECK(job.reasons == (!expected_ok && !strcmp(status, "aborted") ? 1u : 0u));
  CHECK(access(path, F_OK) && errno == ENOENT);
  CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
}

int main(void) {
  prepare();
  complete(true, "completed", "Job sent to printer");
  CHECK(saw_processing && sleeps == 80 && transport.finish_ns == SECOND + 8 * SECOND);
  CHECK(transport.success && transport.bytes && transport.writes == 2);
  CHECK(cancellations >= sleeps); /* Each bounded sleep is followed by a fresh cancellation sample. */
  puts("PASS monotonic eight-second USB hold, unchanged bytes, processing state, finish/close order");

  prepare(); interrupt_once = true;
  complete(true, "completed", "Job sent to printer");
  CHECK(sleeps == 81 && transport.finish_ns == SECOND + 8 * SECOND);
  puts("PASS EINTR resumes against original deadline with bounded final interval");

  prepare(); cancel_on_trigger = true; trigger_ns = SECOND + 250000000ULL;
  complete(false, "canceled", "partial output may exist");
  CHECK(sleeps == 3 && transport.finish_ns - trigger_ns <= TICK && !transport.success);
  puts("PASS cancellation sampled within 100ms, failed finish and close once");

  prepare(); stop_on_trigger = true; trigger_ns = SECOND + 250000000ULL;
  complete(false, "canceled", "partial output may exist");
  CHECK(sleeps == 3 && transport.finish_ns - trigger_ns <= TICK && !transport.success);
  puts("PASS shutdown sampled within 100ms, no replay or extra bytes");

  prepare(); app.simulate = true;
  complete(true, "completed", "Simulation capture complete; no physical page printed");
  CHECK(!sleeps && transport.success && transport.writes == 2);
  prepare(); transport.fail_write = true;
  complete(false, "aborted", "Controlled USB write failure");
  CHECK(!sleeps && !transport.success && transport.bytes == 5 && transport.writes == 2);
  prepare(); ((job_context *)job.data)->failed = true;
  complete(false, "aborted", "no automatic retry");
  CHECK(!sleeps && !transport.writes && !transport.success);
  job_context *invalid_encoder = prepare();
  hp_fr_destroy(invalid_encoder->encoder);
  invalid_encoder->encoder = hp_fr_create(encoded_write, canceled, invalid_encoder);
  CHECK(invalid_encoder->encoder); /* end_job on an unstarted encoder must fail. */
  complete(false, "aborted", "Job failed:");
  CHECK(!sleeps && !transport.writes && !transport.success);
  prepare(); atomic_store(&job.canceled, true);
  complete(false, "canceled", "partial output may exist");
  CHECK(!sleeps && !transport.writes && !transport.success);
  prepare(); ((job_context *)job.data)->pages = expected_pages = 0;
  complete(false, "aborted", "no automatic retry");
  CHECK(!sleeps && !transport.writes && !transport.success);
  prepare(); job.status = IPP_JSTATE_ABORTED;
  complete(false, "aborted", "no automatic retry");
  CHECK(!sleeps && !transport.writes && !transport.success);
  puts("PASS simulation, transport/encoder failure, prior cancel, empty and rejected job skip hold");

  prepare(); sleep_error = true;
  complete(false, "aborted", "Could not finish delivery; check the printer before trying again");
  CHECK(sleeps == 1 && !transport.success);
  prepare(); transport.fail_finish = true;
  complete(false, "aborted", "Controlled USB finish failure");
  CHECK(sleeps == 80 && transport.success); /* Successful replay, finish itself failed. */
  puts("PASS wait and finish errors abort with truthful diagnostics, cleanup without retry");

  prepare(); real_time = true;
  uint_fast64_t started = monotonic_ns();
  complete(true, "completed", "Job sent to printer");
  uint_fast64_t elapsed = transport.finish_ns - started;
  CHECK(saw_processing && elapsed >= 8 * SECOND && elapsed < 10 * SECOND);
  printf("PASS actual monotonic USB hold %.3fs; no hardware access\n", (double)elapsed / SECOND);
  return 0;
}
