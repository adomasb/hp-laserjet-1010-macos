/* Test-only HTTP endpoint: exercise the patched parser with sanitizers. */
#include <pappl/pappl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static bool form(pappl_client_t *client, void *context) {
  (void)context;
  cups_option_t *options = NULL;
  int count = papplClientGetForm(client, &options);
  cupsFreeOptions(count, options);
  // Use PAPPL's built-in fixed responses so no test response writer is needed.
  // These test-only statuses encode exactly one parsed field versus none.
  return papplClientRespond(client, count == 1 ? HTTP_STATUS_BAD_REQUEST : HTTP_STATUS_NOT_FOUND,
                            NULL, NULL, 0, 0);
}

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  umask(077);
  pappl_system_t *system = papplSystemCreate(PAPPL_SOPTIONS_NO_TLS, "Parser test",
      atoi(argv[1]), NULL, argv[2], "-", PAPPL_LOGLEVEL_DEBUG, NULL, false);
  if (!system || !papplSystemAddListeners(system, "127.0.0.1")) return 1;
  papplSystemSetDNSSDName(system, NULL);
  papplSystemAddResourceCallback(system, "/form", "text/plain", form, NULL);
  papplSystemRun(system);
  papplSystemDelete(system);
  return 0;
}
