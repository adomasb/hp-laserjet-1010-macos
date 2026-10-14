/* Native lifecycle, IPP, encoder and transport have distinct ownership.
 * PAPPL/CUPS parse standard IPP and URF/PWG; our encoder handles HP FastRaster.
 */
#include <pappl/pappl.h>
#include "fastraster.h"
#include "transport.h"
#include "printer_protocol.h"
#include "local_security.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SHUTDOWN_SECONDS 10
#define USB_DRAIN_SECONDS 8
static struct {
  bool simulate;
  char authorization[128];
  char state_dir[PATH_MAX], spool_dir[PATH_MAX], captures[PATH_MAX];
  hp_sim_options sim;
  pappl_system_t *system;
  pappl_printer_t *printer;
  pthread_mutex_t mutex;
  atomic_bool stopping;
  atomic_bool shutdown_complete;
  atomic_uint_fast64_t shutdown_deadline_ns;
  unsigned job_id, pages;
  uint64_t bytes;
  char job_state[24], message[160], service[24];
} app = {.mutex = PTHREAD_MUTEX_INITIALIZER};

typedef struct {
  pappl_job_t *job;
  hp_fr *encoder;
  hp_transport *transport;
  int fd;
  char path[PATH_MAX];
  unsigned copies, pages;
  uint64_t encoded_bytes;
  bool failed, drain_failed;
} job_context;

static bool private_dir(const char *path) {
  struct stat st;
  if (mkdir(path, 0700) && errno != EEXIST) return false;
  return !lstat(path, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid() && !chmod(path, 0700);
}
/* Only remove our numeric/unique ephemeral spool files, never follow links. */
static void clean_spool(const char *path) {
  DIR *dir = opendir(path);
  if (!dir) return;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char item[PATH_MAX];
    if (snprintf(item, sizeof(item), "%s/%s", path, entry->d_name) >= (int)sizeof(item)) continue;
    struct stat st;
    if (!lstat(item, &st) && !S_ISDIR(st.st_mode)) unlink(item);
  }
  closedir(dir);
  rmdir(path);
}
static void write_state_locked(void) {
  char path[PATH_MAX], temp[PATH_MAX];
  if (snprintf(path, sizeof(path), "%s/state.json", app.state_dir) >= (int)sizeof(path) ||
      snprintf(temp, sizeof(temp), "%s/.state-%ld", app.state_dir, (long)getpid()) >= (int)sizeof(temp)) return;
  int fd = open(temp, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
  if (fd < 0) return;
  char json[700];
  int n = snprintf(json, sizeof(json), "{\"mode\":\"%s\",\"service\":\"%s\",\"last_job_id\":%u,\"last_job_state\":\"%s\",\"pages\":%u,\"bytes\":%llu,\"message\":\"%s\"}\n",
      app.simulate ? "simulation" : "usb", app.service, app.job_id, app.job_state, app.pages,
      (unsigned long long)app.bytes, app.message);
  ssize_t written = write(fd, json, (size_t)n);
  bool ok = written == n && !fsync(fd);
  close(fd);
  if (ok) rename(temp, path);
  else unlink(temp);
}
static void state(unsigned id, const char *status, unsigned pages, uint64_t bytes, const char *message) {
  pthread_mutex_lock(&app.mutex);
  app.job_id = id;
  app.pages = pages;
  app.bytes = bytes;
  snprintf(app.job_state, sizeof(app.job_state), "%s", status);
  snprintf(app.message, sizeof(app.message), "%s", message);
  write_state_locked();
  pthread_mutex_unlock(&app.mutex);
}
static bool canceled(void *context) {
  job_context *ctx = context;
  return atomic_load(&app.stopping) || papplJobIsCanceled(ctx->job);
}
static ssize_t encoded_write(void *context, const void *data, size_t size) {
  job_context *ctx = context;
  if (canceled(ctx)) { errno = ECANCELED; return -1; }
  if (size > HP_DOCUMENT_LIMIT - ctx->encoded_bytes) { errno = EFBIG; return -1; }
  ssize_t n = write(ctx->fd, data, size);
  if (n > 0) ctx->encoded_bytes += (uint64_t)n;
  return n;
}
static void destroy_context(job_context *ctx) {
  if (!ctx) return;
  hp_fr_destroy(ctx->encoder);
  if (ctx->transport) hp_transport_close(ctx->transport);
  if (ctx->fd >= 0) close(ctx->fd);
  if (ctx->path[0]) unlink(ctx->path);
  free(ctx);
}
static bool start_job(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device) {
  (void)device;
  unsigned id = (unsigned)papplJobGetID(job);
  state(id, "processing", 0, 0, app.simulate ? "Encoding simulation job" : "Preparing print job");
  job_context *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return false;
  ctx->fd = -1;
  char usb_message[160] = "Could not connect to the printer over USB";
  ctx->job = job;
  ctx->copies = (unsigned)options->copies;
  if (ctx->copies < 1 || ctx->copies > HP_MAX_COPIES) goto fail;
  if (app.simulate) ctx->transport = hp_transport_sim_open(&app.sim, id);
  else {
    ctx->transport = hp_transport_usb_open(usb_message, sizeof(usb_message));
  }
  if (!ctx->transport) goto fail;
  if (snprintf(ctx->path, sizeof(ctx->path), "%s/encoded-XXXXXX", app.spool_dir) >= (int)sizeof(ctx->path)) goto fail;
  ctx->fd = mkstemp(ctx->path);
  if (ctx->fd < 0 || fchmod(ctx->fd, 0600)) goto fail;
  ctx->encoder = hp_fr_create(encoded_write, canceled, ctx);
  if (!ctx->encoder || !hp_fr_begin_job(ctx->encoder, 1)) goto fail;
  papplJobSetData(job, ctx);
  return true;
fail:
  if (ctx->transport) hp_transport_finish(ctx->transport, false);
  destroy_context(ctx);
  state(id, "aborted", 0, 0, app.simulate ? "Unable to initialize simulation job" : usb_message);
  return false;
}
static bool start_page(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device, unsigned page) {
  (void)device;
  job_context *ctx = papplJobGetData(job);
  unsigned width = options->header.cupsWidth, height = options->header.cupsHeight;
  unsigned media;
  if (!ctx) return false;
  if (!strcmp(options->media.size_name, "iso_a4_210x297mm")) media = 26;
  else if (!strcmp(options->media.size_name, "na_letter_8.5x11in")) media = 2;
  else { ctx->failed = true; return false; }
  if (page >= HP_MAX_PAGES || width == 0 || width > HP_MAX_DIMENSION || height == 0 || height > HP_MAX_DIMENSION ||
      options->header.cupsBitsPerPixel != 1 || options->header.cupsColorSpace != CUPS_CSPACE_K ||
      options->header.HWResolution[0] != HP_RESOLUTION || options->header.HWResolution[1] != HP_RESOLUTION ||
      options->sides != PAPPL_SIDES_ONE_SIDED || canceled(ctx)) { ctx->failed = true; return false; }
  bool ok = hp_fr_begin_page(ctx->encoder, width, height, media);
  ctx->failed |= !ok;
  return ok;
}
static bool write_line(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device, unsigned y, const unsigned char *line) {
  (void)device; (void)y;
  job_context *ctx = papplJobGetData(job);
  if (!ctx || ctx->failed) return false;
  size_t length = (options->header.cupsWidth + 7) / 8;
  unsigned char row[HP_PACKED_ROW_BYTES];
  memcpy(row, line, length);
  unsigned tail = options->header.cupsWidth & 7;
  if (tail) row[length - 1] &= (unsigned char)(255u << (8 - tail));
  bool ok = hp_fr_write_row(ctx->encoder, row, length);
  ctx->failed |= !ok;
  return ok;
}
static bool end_page(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device, unsigned page) {
  (void)options; (void)device; (void)page;
  job_context *ctx = papplJobGetData(job);
  if (!ctx || ctx->failed) return false;
  bool ok = hp_fr_end_page(ctx->encoder);
  ctx->failed |= !ok;
  if (ok) ctx->pages ++;
  return ok;
}
static bool replay(job_context *ctx) {
  unsigned char buffer[16384];
  for (unsigned copy = 0; copy < ctx->copies; copy ++) {
    if (lseek(ctx->fd, 0, SEEK_SET) < 0) return false;
    ssize_t n;
    while ((n = read(ctx->fd, buffer, sizeof(buffer))) > 0) {
      size_t offset = 0;
      while (offset < (size_t)n) {
        if (canceled(ctx)) return false;
        ssize_t written = hp_transport_write(ctx->transport, buffer + offset, (size_t)n - offset);
        if (written <= 0 || (size_t)written > (size_t)n - offset) return false;
        offset += (size_t)written;
      }
    }
    if (n < 0) return false;
  }
  return true;
}
static uint_fast64_t monotonic_ns(void);
/* HPLIP 3.26.6 prnt/backend/hp.c:777-779,969-972,994-998 holds
 * non-PJL devices eight seconds before close. Its HP1010 model has PML
 * status-type=3. This is a drain grace period, not a physical page check. */
static bool usb_drain(job_context *ctx, bool delivered, uint64_t bytes) {
  if (!delivered || app.simulate) return delivered;
  if (canceled(ctx)) return false;
  uint_fast64_t deadline = monotonic_ns() + USB_DRAIN_SECONDS * 1000000000ULL;
  const char *message = "Finishing delivery to the printer…";
  papplJobSetMessage(ctx->job, "%s", message);
  state((unsigned)papplJobGetID(ctx->job), "processing", ctx->pages, bytes, message);
  for (;;) {
    if (canceled(ctx)) return false;
    uint_fast64_t now = monotonic_ns();
    if (now >= deadline) return true;
    uint_fast64_t pause_ns = deadline - now;
    if (pause_ns > 100000000ULL) pause_ns = 100000000ULL;
    struct timespec interval = {.tv_sec = 0, .tv_nsec = (long)pause_ns};
    if (nanosleep(&interval, NULL) && errno != EINTR) {
      ctx->drain_failed = true;
      return false;
    }
  }
}
static bool end_job(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device) {
  (void)options; (void)device;
  job_context *ctx = papplJobGetData(job);
  if (!ctx) return false;
  bool was_canceled = canceled(ctx);
  bool ok = !ctx->failed && !was_canceled && ctx->pages > 0 && papplJobGetState(job) == IPP_JSTATE_PROCESSING;
  if (ok) ok = hp_fr_end_job(ctx->encoder);
  /* Transport contract caps aggregate output at 256 MiB, across copies. */
  if (ok && ctx->encoded_bytes > (HP_TRANSPORT_LIMIT) / ctx->copies) ok = false;
  if (ok) ok = replay(ctx);
  was_canceled |= canceled(ctx);
  ok &= !was_canceled;
  uint64_t bytes = hp_transport_bytes(ctx->transport);
  ok = usb_drain(ctx, ok, bytes);
  was_canceled |= canceled(ctx);
  ok &= !was_canceled;
  ok = hp_transport_finish(ctx->transport, ok) && ok;
  unsigned pages = ctx->pages;
  /* Both APIs return fixed diagnostics owned by our code, never document
   * data. Copy before destruction so USB error codes remain inspectable. */
  char failure[160] = "Job failed; no automatic retry; partial output may exist";
  if (!ok && !was_canceled) {
    const char *encoder_error = hp_fr_error(ctx->encoder);
    const char *transport_error = hp_transport_error(ctx->transport);
    if (ctx->drain_failed) snprintf(failure, sizeof(failure), "Could not finish delivery; check the printer before trying again");
    else if (encoder_error[0]) snprintf(failure, sizeof(failure), "Job failed: %s; no automatic retry", encoder_error);
    else if (transport_error[0]) snprintf(failure, sizeof(failure), "%s", transport_error);
  }
  papplJobSetData(job, NULL);
  destroy_context(ctx);
  const char *message = was_canceled ? "Job canceled; partial output may exist" :
      !ok ? failure :
      app.simulate ? "Simulation capture complete; no physical page printed" : "Job sent to printer";
  papplJobSetMessage(job, "%s", message);
  if (!ok && !was_canceled) papplJobSetReasons(job, PAPPL_JREASON_ERRORS_DETECTED, PAPPL_JREASON_NONE);
  state((unsigned)papplJobGetID(job), was_canceled ? "canceled" : ok ? "completed" : "aborted", pages, bytes, message);
  return ok;
}
/* PAPPL owns a harmless custom placeholder device; transport stays per job. */
static bool device_open(pappl_device_t *device, const char *uri, const char *name) {
  (void)device; (void)uri; (void)name; return true;
}
static void device_close(pappl_device_t *device) { (void)device; }
static ssize_t device_write(pappl_device_t *device, const void *buffer, size_t bytes) {
  (void)device; (void)buffer; (void)bytes; errno = ENOTSUP; return -1;
}
static bool driver(pappl_system_t *system, const char *name, const char *uri, const char *id,
    pappl_pr_driver_data_t *data, ipp_t **attrs, void *context) {
  (void)system; (void)name; (void)uri; (void)id; (void)context;
  *attrs = ippNew();
  data->rstartjob_cb = start_job; data->rstartpage_cb = start_page;
  data->rwriteline_cb = write_line; data->rendpage_cb = end_page; data->rendjob_cb = end_job;
  snprintf(data->make_and_model, sizeof(data->make_and_model), "%s", app.simulate ? "HP LaserJet 1010 FastRaster (simulation)" : "HP LaserJet 1010");
  data->kind = PAPPL_KIND_DOCUMENT;
  data->raster_types = PAPPL_PWG_RASTER_TYPE_BLACK_1 | PAPPL_PWG_RASTER_TYPE_BLACK_8 | PAPPL_PWG_RASTER_TYPE_SGRAY_8;
  data->force_raster_type = PAPPL_PWG_RASTER_TYPE_BLACK_1;
  data->color_supported = data->color_default = PAPPL_COLOR_MODE_MONOCHROME;
  data->orient_default = IPP_ORIENT_PORTRAIT;
  data->quality_default = IPP_QUALITY_NORMAL;
  data->num_resolution = 1; data->x_resolution[0] = data->y_resolution[0] = HP_RESOLUTION;
  data->x_default = data->y_default = HP_RESOLUTION;
  data->sides_supported = data->sides_default = PAPPL_SIDES_ONE_SIDED;
  data->left_right = data->bottom_top = 423;
  data->num_media = 2; data->media[0] = "iso_a4_210x297mm"; data->media[1] = "na_letter_8.5x11in";
  data->num_source = 1; data->source[0] = "main";
  data->num_type = 1; data->type[0] = "stationery";
  data->num_bin = 1; data->bin[0] = "face-down";
  snprintf(data->media_default.size_name, sizeof(data->media_default.size_name), "%s", data->media[0]);
  snprintf(data->media_default.source, sizeof(data->media_default.source), "main");
  snprintf(data->media_default.type, sizeof(data->media_default.type), "stationery");
  data->media_default.size_width = 21000; data->media_default.size_length = 29700;
  data->media_default.left_margin = data->media_default.right_margin = 423;
  data->media_default.top_margin = data->media_default.bottom_margin = 423;
  data->media_ready[0] = data->media_default;
  /* Retain PAPPL's standard clustered/blue-noise gray dither matrices. */
  data->ppm = 0; /* No measured physical performance claim. */
  return true;
}
/* Timer polling runs outside the job lock; event callbacks cannot safely call
 * getters because PAPPL invokes them while holding that same job lock. */
static void job_snapshot(pappl_job_t *job, void *context) {
  (void)context;
  unsigned id = (unsigned)papplJobGetID(job);
  ipp_jstate_t s = papplJobGetState(job);
  pthread_mutex_lock(&app.mutex);
  if (id >= app.job_id) {
    if (id > app.job_id) {
      app.pages = 0; app.bytes = 0;
      snprintf(app.message, sizeof(app.message), "Job pending; no output sent");
    }
    app.job_id = id;
    const char *label = s == IPP_JSTATE_CANCELED ? "canceled" : s == IPP_JSTATE_ABORTED ? "aborted" :
        s == IPP_JSTATE_COMPLETED ? "completed" : s == IPP_JSTATE_PROCESSING ? "processing" : "pending";
    if (strcmp(app.job_state, label)) {
      snprintf(app.job_state, sizeof(app.job_state), "%s", label);
      if (s >= IPP_JSTATE_CANCELED && !app.pages && !app.bytes)
        snprintf(app.message, sizeof(app.message), "%s", s == IPP_JSTATE_CANCELED ? "Job canceled before output" : "Job rejected or failed before output");
      write_state_locked();
    }
  }
  pthread_mutex_unlock(&app.mutex);
}
static bool poll_jobs(pappl_system_t *system, void *context) {
  (void)system; (void)context;
  papplPrinterIterateAllJobs(app.printer, job_snapshot, NULL, 1, 0);
  return true;
}
static uint_fast64_t monotonic_ns(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) _exit(EXIT_FAILURE);
  return (uint_fast64_t)now.tv_sec * 1000000000ULL + (uint_fast64_t)now.tv_nsec;
}
static void request_shutdown(void) {
  uint_fast64_t unset = 0;
  uint_fast64_t deadline = monotonic_ns() + SHUTDOWN_SECONDS * 1000000000ULL;
  /* Arm before PAPPL: its shutdown call can itself wait for a driver-held lock.
   * Subsequent signals or parent events must not extend the original deadline. */
  atomic_compare_exchange_strong(&app.shutdown_deadline_ns, &unset, deadline);
  atomic_store(&app.stopping, true);
  papplSystemShutdown(app.system);
}
static void *shutdown_watchdog(void *context) {
  (void)context;
  while (!atomic_load(&app.shutdown_complete)) {
    uint_fast64_t pause_ns = 250000000ULL;
    uint_fast64_t deadline = atomic_load(&app.shutdown_deadline_ns);
    if (deadline) {
      uint_fast64_t now = monotonic_ns();
      if (now >= deadline) {
        if (atomic_load(&app.shutdown_complete)) return NULL;
        /* No locks, driver close, fsync or retry on this last-resort path.
         * Kernel process teardown closes handles; next startup cleans the
         * private stale spool. Previously transferred output is ambiguous. */
        /* Even a diagnostic pipe write could block, so exit without I/O. */
        _exit(EXIT_FAILURE);
      }
      if (pause_ns > deadline - now) pause_ns = deadline - now;
    }
    struct timespec interval = {.tv_sec = 0, .tv_nsec = (long)pause_ns};
    nanosleep(&interval, NULL);
  }
  return NULL;
}
static void *signal_waiter(void *context) {
  sigset_t *set = context;
  int sig;
  if (!sigwait(set, &sig)) {
    request_shutdown();
  }
  return NULL;
}
static void *parent_waiter(void *context) {
  int queue = *(int *)context;
  while (!atomic_load(&app.stopping)) {
    struct kevent event;
    struct timespec interval = {.tv_sec = 0, .tv_nsec = 500000000};
    int n = kevent(queue, NULL, 0, &event, 1, &interval);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 || (n > 0 && (event.fflags & NOTE_EXIT))) {
      request_shutdown();
      break;
    }
  }
  return NULL;
}
static bool number(const char *text, unsigned long long maximum, unsigned long long *result) {
  char *end;
  if (!*text || *text == '-' || *text == '+') return false;
  errno = 0; unsigned long long n = strtoull(text, &end, 10);
  if (errno || *end || n > maximum) return false;
  *result = n; return true;
}
static http_status_t authorize(pappl_client_t *client, const char *group, gid_t gid, void *context) {
  (void)group; (void)gid; (void)context;
  const char *value = httpGetField(papplClientGetHTTP(client), HTTP_FIELD_AUTHORIZATION);
  size_t size = strlen(app.authorization);
  if (strlen(value) != size) return HTTP_STATUS_UNAUTHORIZED;
  if (timingsafe_bcmp(value, app.authorization, size)) return HTTP_STATUS_UNAUTHORIZED;
  papplClientSetUsername(client, HP_AUTH_USER);
  return HTTP_STATUS_CONTINUE;
}

static void usage(FILE *out) {
  fputs("HP LaserJet 1010 local FastRaster helper\nUsage: hp1010-helper (--simulate | --usb) --state-dir PATH [--port N] [--parent-pid N]\nSimulation faults: --max-write N --fail-after N --delay-ms N --failure io|timeout|disconnect\nIPP defaults: simulation 8631, USB 8632; ipp://127.0.0.1:PORT/ipp/print\nUSB transfer completion does not confirm a printed page.\n", out);
}
int main(int argc, char **argv) {
  if (argc == 4 && !strcmp(argv[1], "--secure-queue") &&
      (!strcmp(argv[3], "enable") || !strcmp(argv[3], "preserve")))
    return hp_secure_queue(argv[2], !strcmp(argv[3], "enable"));
  /* Establish inherited signal ownership before any library creates threads. */
  sigset_t set; sigemptyset(&set); sigaddset(&set, SIGTERM); sigaddset(&set, SIGINT);
  pthread_sigmask(SIG_BLOCK, &set, NULL);
  int port = HP_SIMULATION_PORT, parent_pid = 0; bool selected = false, faults = false, explicit_port = false;
  pthread_t watchdog;
  bool watching_shutdown = false;
  app.sim.fail_after = -1;
  for (int i = 1; i < argc; i ++) {
    const char *arg = argv[i];
    if (!strcmp(arg, "--help")) { usage(stdout); return 0; }
    if (!strcmp(arg, "--simulate") || !strcmp(arg, "--usb")) {
      if (selected) goto invalid;
      selected = true; app.simulate = !strcmp(arg, "--simulate"); continue;
    }
    if (i + 1 >= argc) goto invalid;
    const char *value = argv[++i]; unsigned long long n;
    if (!strcmp(arg, "--state-dir")) {
      if (value[0] != '/' || strlen(value) > PATH_MAX - 100) goto invalid;
      snprintf(app.state_dir, sizeof(app.state_dir), "%s", value);
    } else if (!strcmp(arg, "--port")) {
      if (!number(value, 65535, &n) || n < 1024) goto invalid; port = (int)n; explicit_port = true;
    } else if (!strcmp(arg, "--parent-pid")) {
      if (!number(value, INT_MAX, &n) || n < 2) goto invalid; parent_pid = (int)n;
    } else if (!strcmp(arg, "--max-write")) {
      if (!number(value, 1024 * 1024, &n) || n < 1) goto invalid; app.sim.maximum_write = (size_t)n; faults = true;
    } else if (!strcmp(arg, "--fail-after")) {
      if (!number(value, HP_DOCUMENT_LIMIT * HP_MAX_COPIES, &n)) goto invalid; app.sim.fail_after = (int64_t)n; faults = true;
    } else if (!strcmp(arg, "--failure")) {
      if (!strcmp(value, "io")) app.sim.fail_errno = 0;
      else if (!strcmp(value, "timeout")) app.sim.fail_errno = ETIMEDOUT;
      else if (!strcmp(value, "disconnect")) app.sim.fail_errno = ENODEV;
      else goto invalid;
      faults = true;
    } else if (!strcmp(arg, "--delay-ms")) {
      if (!number(value, 1000, &n)) goto invalid; app.sim.delay_ms = (unsigned)n; faults = true;
    } else goto invalid;
  }
  if (!selected || !app.state_dir[0] || (faults && !app.simulate)) goto invalid;
  if (!app.simulate && !explicit_port) port = HP_USB_PORT;
  int parent_queue = -1;
  if (parent_pid) {
    if (getppid() != parent_pid) { fputs("Parent PID must identify the launching process.\n", stderr); return 2; }
    parent_queue = kqueue();
    struct kevent event;
    EV_SET(&event, (uintptr_t)parent_pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    /* Bind the kernel watch while the process is still our actual parent.
     * Recheck ownership after registration to reject the exit/PID-reuse race. */
    if (parent_queue < 0 || kevent(parent_queue, &event, 1, NULL, 0, NULL) < 0 || getppid() != parent_pid) {
      if (parent_queue >= 0) close(parent_queue);
      fputs("Unable to watch the launching process.\n", stderr); return 1;
    }
    fcntl(parent_queue, F_SETFD, FD_CLOEXEC);
  }
  umask(077);
  if (!private_dir(app.state_dir)) { fputs("State directory must be a private directory owned by this user.\n", stderr); return 1; }
  char lock_path[PATH_MAX]; snprintf(lock_path, sizeof(lock_path), "%s/service.lock", app.state_dir);
  int lock = open(lock_path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) { fputs("State directory already has a running helper.\n", stderr); if (lock >= 0) close(lock); return 1; }
  char token[HP_AUTH_HEX_SIZE + 1], credentials[HP_AUTH_HEX_SIZE + 16], encoded[128];
  if (!hp_auth_load(app.state_dir, true, token)) {
    fputs("Unable to load private service credential. Check state directory permissions.\n", stderr);
    close(lock); return 1;
  }
  snprintf(credentials, sizeof(credentials), "%s:%s", HP_AUTH_USER, token);
  httpEncode64_2(encoded, sizeof(encoded), credentials, (int)strlen(credentials));
  snprintf(app.authorization, sizeof(app.authorization), "Basic %s", encoded);
  DIR *dir = opendir(app.state_dir);
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir))) if (!strncmp(entry->d_name, "spool-", 6)) {
      char old[PATH_MAX]; snprintf(old, sizeof(old), "%s/%s", app.state_dir, entry->d_name);
      struct stat st;
      if (!lstat(old, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid()) clean_spool(old);
    }
    closedir(dir);
  }
  snprintf(app.spool_dir, sizeof(app.spool_dir), "%s/spool-XXXXXX", app.state_dir);
  if (!mkdtemp(app.spool_dir)) { close(lock); return 1; }
  snprintf(app.captures, sizeof(app.captures), "%s/captures", app.state_dir);
  if (app.simulate && !private_dir(app.captures)) { clean_spool(app.spool_dir); close(lock); return 1; }
  app.sim.capture_directory = app.captures;
  snprintf(app.service, sizeof(app.service), "starting");
  state(0, "none", 0, 0, app.simulate ? "Simulation only; no physical printer required" : "Choose HP LaserJet 1010 in your app’s print dialog");
  app.system = papplSystemCreate(PAPPL_SOPTIONS_NO_TLS, "HP LaserJet 1010", port, NULL, app.spool_dir, "/dev/null", PAPPL_LOGLEVEL_FATAL, NULL, false);
  if (!app.system) goto failed;
  papplSystemSetAuthCallback(app.system, "Basic", authorize, NULL);
  papplSystemSetDNSSDName(app.system, NULL);
  papplSystemSetMaxClients(app.system, 4);
  papplSystemSetMaxSubscriptions(app.system, 8);
  papplSystemSetMaxImageSize(app.system, HP_DOCUMENT_LIMIT, HP_MAX_DIMENSION, HP_MAX_DIMENSION);
  if (!papplSystemAddListeners(app.system, "127.0.0.1")) goto failed;
  papplDeviceAddScheme("hp1010", PAPPL_DEVTYPE_CUSTOM_LOCAL, NULL, device_open, device_close, NULL, device_write, NULL, NULL);
  pappl_pr_driver_t drivers[] = {{"hp1010", "HP LaserJet 1010 FastRaster", "MFG:HP;MDL:LaserJet 1010;CMD:FAST_RASTER;", NULL}};
  papplSystemSetPrinterDrivers(app.system, 1, drivers, NULL, NULL, driver, NULL);
  app.printer = papplPrinterCreate(app.system, 1, "HP LaserJet 1010", "hp1010", "MFG:HP;MDL:LaserJet 1010;CMD:FAST_RASTER;", "hp1010://local");
  if (!app.printer) goto failed;
  papplPrinterSetDNSSDName(app.printer, NULL);
  papplPrinterSetMaxActiveJobs(app.printer, 4);
  papplPrinterSetMaxCompletedJobs(app.printer, 20);
  papplPrinterSetMaxPreservedJobs(app.printer, 0);
  papplSystemSetDefaultPrinterID(app.system, 1);
  papplSystemAddTimerCallback(app.system, time(NULL) + 1, 1, poll_jobs, NULL);
  pthread_t waiter, parent_thread;
  bool watching_parent = false;
  if (pthread_create(&watchdog, NULL, shutdown_watchdog, NULL)) goto failed;
  watching_shutdown = true;
  if (parent_queue >= 0) {
    if (pthread_create(&parent_thread, NULL, parent_waiter, &parent_queue)) goto failed;
    watching_parent = true;
  }
  if (pthread_create(&waiter, NULL, signal_waiter, &set)) {
    atomic_store(&app.stopping, true);
    if (watching_parent) pthread_join(parent_thread, NULL);
    goto failed;
  }
  pthread_mutex_lock(&app.mutex); snprintf(app.service, sizeof(app.service), "running"); write_state_locked(); pthread_mutex_unlock(&app.mutex);
  papplSystemRun(app.system);
  request_shutdown();
  pthread_kill(waiter, SIGTERM); pthread_join(waiter, NULL);
  if (watching_parent) pthread_join(parent_thread, NULL);
  if (parent_queue >= 0) close(parent_queue);
  pthread_mutex_lock(&app.mutex); snprintf(app.service, sizeof(app.service), "stopped"); write_state_locked(); pthread_mutex_unlock(&app.mutex);
  papplSystemDelete(app.system);
  clean_spool(app.spool_dir); close(lock);
  atomic_store(&app.shutdown_complete, true);
  pthread_join(watchdog, NULL);
  return 0;
failed:
  if (parent_queue >= 0) close(parent_queue);
  if (app.system) papplSystemDelete(app.system);
  snprintf(app.service, sizeof(app.service), "failed");
  state(0, "none", 0, 0, "Unable to start local IPP service");
  clean_spool(app.spool_dir); close(lock);
  atomic_store(&app.shutdown_complete, true);
  if (watching_shutdown) pthread_join(watchdog, NULL);
  return 1;
invalid:
  usage(stderr); return 2;
}
