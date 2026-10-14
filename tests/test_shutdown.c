/* Controlled shutdown test: compile the production service in this translation
 * unit, replace only PAPPL shutdown with a blocking stand-in, never open USB. */
#define main service_program_main
#define papplSystemShutdown controlled_shutdown
#include "../src/service.c"
#undef main
#undef papplSystemShutdown

static bool block_shutdown;
static atomic_uint calls;
static atomic_uint_fast64_t first_deadline;
void controlled_shutdown(pappl_system_t *system) {
  (void)system;
  uint_fast64_t deadline = atomic_load(&app.shutdown_deadline_ns);
  unsigned call = atomic_fetch_add(&calls, 1);
  if (!call) atomic_store(&first_deadline, deadline);
  else if (deadline != atomic_load(&first_deadline)) _exit(90);
  if (block_shutdown) for (;;) {
    struct timespec interval = {.tv_sec = 0, .tv_nsec = 200000000};
    nanosleep(&interval, NULL);
  }
}
static void *second_request(void *unused) {
  (void)unused;
  struct timespec interval = {.tv_sec = 2, .tv_nsec = 0};
  nanosleep(&interval, NULL);
  request_shutdown();
  return NULL;
}
int main(int argc, char **argv) {
  block_shutdown = argc > 1 && !strcmp(argv[1], "hang");
  sigset_t set;
  sigemptyset(&set); sigaddset(&set, SIGTERM); sigaddset(&set, SIGINT);
  if (pthread_sigmask(SIG_BLOCK, &set, NULL)) return 91;
  pthread_t watchdog, waiter, repeat;
  if (pthread_create(&watchdog, NULL, shutdown_watchdog, NULL) ||
      pthread_create(&waiter, NULL, signal_waiter, &set)) return 92;
  if (block_shutdown && pthread_create(&repeat, NULL, second_request, NULL)) return 93;
  puts("ready"); fflush(stdout);
  pthread_join(waiter, NULL);
  atomic_store(&app.shutdown_complete, true);
  pthread_join(watchdog, NULL);
  return atomic_load(&calls) == 1 ? 0 : 94;
}
