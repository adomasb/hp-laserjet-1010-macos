/* Credential storage and the actual CUPS request builder; no system queue writes. */
#include "local_security.h"
#include "printer_protocol.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <unistd.h>

static ipp_t *captured;
static int response_status = IPP_STATUS_OK;
static int update_status = IPP_STATUS_OK;
static int connects, updates, queued_jobs;
static bool omit_count, is_shared;
static const char *installed_marker;
static const char *setup_state;
static http_t *test_connect(const char *host, int port, http_addrlist_t *addrlist,
                            int family, http_encryption_t encryption, int blocking, int msec, int *cancel) {
  assert(!strcmp(host, "/private/var/run/cupsd") && port == 631 && !addrlist && family == AF_UNIX);
  assert(encryption == HTTP_ENCRYPTION_IF_REQUESTED && blocking == 1 && msec == 3000 && !cancel);
  connects++;
  return (http_t *)1;
}
static void test_timeout(http_t *http, double timeout, http_timeout_cb_t cb, void *data) {
  assert(http == (http_t *)1 && timeout == 5 && !cb && !data);
}
static void test_close(http_t *http) { assert(http == (http_t *)1); }
static ipp_t *test_request(http_t *http, ipp_t *request, const char *resource) {
  assert(http == (http_t *)1);
  ipp_t *response = ippNewResponse(request);
  ippSetStatusCode(response, response_status);
  if (ippGetOperation(request) == IPP_OP_GET_PRINTER_ATTRIBUTES) {
    assert(!strcmp(resource, "/printers/HP_LaserJet_1010_USB"));
    if (!omit_count) ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "queued-job-count", queued_jobs);
    ippAddBoolean(response, IPP_TAG_PRINTER, "printer-is-shared", is_shared);
    if (installed_marker) ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_NAME, "hp1010-auth-id-default", NULL, installed_marker);
    if (setup_state) ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_NAME, HP_SETUP_STATE_ATTRIBUTE, NULL, setup_state);
    ippDelete(request);
  } else {
    assert(!strcmp(resource, "/admin/"));
    captured = request;
    updates++;
    ippSetStatusCode(response, update_status);
    if (update_status == IPP_STATUS_OK) {
      const char *state = ippGetString(ippFindAttribute(request, HP_SETUP_STATE_ATTRIBUTE, IPP_TAG_NAME), 0, NULL);
      assert(state && !strcmp(state, "complete"));
      setup_state = "complete";
    }
  }
  return response;
}
#define httpConnect2 test_connect
#define httpSetTimeout test_timeout
#define httpClose test_close
#define cupsDoRequest test_request
#include "../src/local_security.c"
#undef httpConnect2
#undef httpSetTimeout
#undef httpClose
#undef cupsDoRequest

int main(void) {
  char path[] = "/tmp/hp1010-auth-test-XXXXXX";
  assert(mkdtemp(path));
  char filename[1024], other[1024], token[65], again[65];
  snprintf(filename, sizeof(filename), "%s/access-token", path);
  snprintf(other, sizeof(other), "%s/other", path);
  assert(!hp_auth_load(path, false, token));
  assert(hp_auth_load(path, true, token));
  assert(strlen(token) == 64 && hp_auth_load(path, true, again) && !strcmp(token, again));
  struct stat st; assert(!stat(filename, &st) && (st.st_mode & 0777) == 0600);
  setenv("CUPS_SERVER", "attacker.invalid", 1); setenv("IPP_PORT", "12345", 1);
  assert(!hp_secure_queue(path, false) && connects == 1);
  assert(ippGetOperation(captured) == IPP_OP_CUPS_ADD_MODIFY_PRINTER);
  assert(!ippFindAttribute(captured, "printer-state", IPP_TAG_ZERO));
  assert(!ippFindAttribute(captured, "printer-is-accepting-jobs", IPP_TAG_ZERO));
  const char *uri = ippGetString(ippFindAttribute(captured, "device-uri", IPP_TAG_URI), 0, NULL);
  assert(strstr(uri, token) && strstr(uri, "@127.0.0.1:8632/ipp/print"));
  assert(!ippGetBoolean(ippFindAttribute(captured, "printer-is-shared", IPP_TAG_BOOLEAN), 0));
  ippDelete(captured);
  assert(!hp_secure_queue(path, true));
  assert(ippGetInteger(ippFindAttribute(captured, "printer-state", IPP_TAG_ENUM), 0) == IPP_PSTATE_IDLE);
  assert(ippGetBoolean(ippFindAttribute(captured, "printer-is-accepting-jobs", IPP_TAG_BOOLEAN), 0));
  ippDelete(captured);
  response_status = IPP_STATUS_ERROR_FORBIDDEN;
  assert(hp_secure_queue(path, false) == 1 && updates == 2);
  response_status = IPP_STATUS_OK;
  queued_jobs = 1;
  assert(hp_secure_queue(path, false) == 3 && updates == 2);
  char marker[65]; credential_id(token, marker); installed_marker = marker;
  assert(!hp_secure_queue(path, false) && updates == 2); // Idempotent even while printing.
  is_shared = true;
  assert(hp_secure_queue(path, false) == 3 && updates == 2);
  is_shared = false; installed_marker = "old";
  queued_jobs = 0; omit_count = true;
  assert(hp_secure_queue(path, false) == 1 && updates == 2);
  omit_count = false;
  assert(!hp_secure_queue(path, false) && updates == 3); ippDelete(captured);
  // A new queue survives a failed credential update. Retry/relaunch must enable
  // it from persisted intent, even though Swift now sees an existing queue.
  setup_state = "pending"; installed_marker = "unconfigured";
  update_status = IPP_STATUS_ERROR_FORBIDDEN;
  assert(hp_secure_queue(path, true) == 1 && !strcmp(setup_state, "pending"));
  ippDelete(captured);
  update_status = IPP_STATUS_OK;
  assert(!hp_secure_queue(path, false) && !strcmp(setup_state, "complete"));
  assert(ippGetInteger(ippFindAttribute(captured, "printer-state", IPP_TAG_ENUM), 0) == IPP_PSTATE_IDLE);
  assert(ippGetBoolean(ippFindAttribute(captured, "printer-is-accepting-jobs", IPP_TAG_BOOLEAN), 0));
  ippDelete(captured);
  // After successful recovery, retries do not resume a user-paused queue or
  // rewrite its URI. Repair may reinstall credentials but must retain its pause.
  installed_marker = marker;
  int saved_updates = updates;
  assert(!hp_secure_queue(path, false) && updates == saved_updates);
  installed_marker = "unconfigured";
  assert(!hp_secure_queue(path, false) && updates == saved_updates + 1);
  assert(!ippFindAttribute(captured, "printer-state", IPP_TAG_ZERO));
  assert(!ippFindAttribute(captured, "printer-is-accepting-jobs", IPP_TAG_ZERO));
  ippDelete(captured);
  // Pending setup must still fail closed when the queue is busy or uninspectable.
  setup_state = "pending"; installed_marker = marker; queued_jobs = 1;
  saved_updates = updates;
  assert(hp_secure_queue(path, false) == 3 && updates == saved_updates);
  queued_jobs = 0; omit_count = true;
  assert(hp_secure_queue(path, false) == 1 && updates == saved_updates);
  omit_count = false;
  assert(!hp_secure_queue(path, false) && updates == saved_updates + 1);
  assert(ippGetInteger(ippFindAttribute(captured, "printer-state", IPP_TAG_ENUM), 0) == IPP_PSTATE_IDLE);
  ippDelete(captured);
  assert(!chmod(filename, 0644) && !hp_auth_load(path, true, again));
  assert(!chmod(filename, 0600));
  assert(!link(filename, other) && !hp_auth_load(path, true, again)); unlink(other);
  assert(!rename(filename, other) && !symlink(other, filename) && !hp_auth_load(path, true, again));
  unlink(filename); assert(!rename(other, filename));
  assert(!chmod(path, 0755) && !hp_auth_load(path, true, again)); chmod(path, 0700);
  // Explicit ACLs are rejected even when POSIX mode bits are private.
  acl_t acl = acl_from_text("!#acl 1\nuser:FFFFEEEE-DDDD-CCCC-BBBB-AAAA00000000:0:root:allow:read\n");
  assert(acl && !acl_set_file(filename, ACL_TYPE_EXTENDED, acl)); acl_free(acl);
  assert(!hp_auth_load(path, true, again));
  unlink(filename);
  int fd = open(filename, O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd >= 0);
  assert(write(fd, "broken", 6) == 6); close(fd);
  assert(!hp_auth_load(path, true, again)); // Never silently rotate a corrupt credential.
  unlink(filename); assert(!mkfifo(filename, 0600));
  assert(!hp_auth_load(path, true, again)); unlink(filename);
  int saved_connects = connects;
  assert(hp_secure_queue(path, false) == 1 && connects == saved_connects);
  rmdir(path);
  puts("PASS private credential persistence, permissions, ACL, symlink/hardlink/FIFO/corruption rejection, bounded local CUPS request and preserved queue state");
}
