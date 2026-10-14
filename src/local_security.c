#include "local_security.h"
#include "printer_protocol.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <unistd.h>

static bool private_object(int fd, bool directory) {
  struct stat st;
  if (fstat(fd, &st) || st.st_uid != getuid() || (st.st_mode & 077) ||
      (directory ? !S_ISDIR(st.st_mode) : (!S_ISREG(st.st_mode) || st.st_nlink != 1))) return false;
  acl_t acl = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
  if (!acl) return errno == ENOENT; // macOS: open inode has no extended ACL.
  acl_entry_t entry;
  int result = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
  int error = errno;
  acl_free(acl);
  return result == -1 && error == EINVAL; // No extended ACL may grant another account access.
}

bool hp_auth_load(const char *directory, bool create, char token[HP_AUTH_HEX_SIZE + 1]) {
  memset(token, 0, HP_AUTH_HEX_SIZE + 1);
  int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dir < 0) return false;
  if (!private_object(dir, true)) { close(dir); return false; }
  int fd = openat(dir, HP_AUTH_FILE, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0 && errno == ENOENT && create) {
    unsigned char random[HP_AUTH_HEX_SIZE / 2];
    char value[HP_AUTH_HEX_SIZE];
    arc4random_buf(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); i++) {
      value[2*i] = "0123456789abcdef"[random[i] >> 4];
      value[2*i+1] = "0123456789abcdef"[random[i] & 15];
    }
    fd = openat(dir, HP_AUTH_FILE, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
      bool ok = private_object(fd, false) && write(fd, value, sizeof(value)) == sizeof(value) && !fsync(fd);
      close(fd);
      if (!ok) { close(dir); return false; }
    } else if (errno != EEXIST) { close(dir); return false; }
    fd = openat(dir, HP_AUTH_FILE, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  }
  close(dir);
  if (fd < 0) return false;
  char value[HP_AUTH_HEX_SIZE + 1];
  bool ok = private_object(fd, false) && read(fd, value, sizeof(value)) == HP_AUTH_HEX_SIZE;
  close(fd);
  for (size_t i = 0; ok && i < HP_AUTH_HEX_SIZE; i++)
    if (!strchr("0123456789abcdef", value[i]) || !value[i]) ok = false;
  if (ok) memcpy(token, value, HP_AUTH_HEX_SIZE);
  return ok;
}

// A public digest identifies the installed credential without revealing it.
// CUPS persists custom defaults and returns them through Get-Printer-Attributes.
#define AUTH_MARKER "hp1010-auth-id-default"
static void credential_id(const char *token, char id[65]) {
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(token, HP_AUTH_HEX_SIZE, digest);
  for (size_t i = 0; i < sizeof(digest); i++) {
    id[2*i] = "0123456789abcdef"[digest[i] >> 4];
    id[2*i+1] = "0123456789abcdef"[digest[i] & 15];
  }
  id[64] = '\0';
}

ipp_t *hp_queue_auth_request(const char *token, bool enable) {
  char uri[256];
  snprintf(uri, sizeof(uri), "ipp://%s:%s@127.0.0.1:%d/ipp/print", HP_AUTH_USER, token, HP_USB_PORT);
  struct passwd storage, *owner;
  char buffer[16384];
  if (getpwuid_r(getuid(), &storage, buffer, sizeof(buffer), &owner) || !owner) return NULL;
  ipp_t *request = ippNewRequest(IPP_OP_CUPS_ADD_MODIFY_PRINTER);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL,
               "ipp://localhost/printers/HP_LaserJet_1010_USB");
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, owner->pw_name);
  ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_URI, "device-uri", NULL, uri);
  char id[65]; credential_id(token, id);
  ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_NAME, AUTH_MARKER, NULL, id);
  // Complete setup in the same update that installs credentials and enables a
  // new queue. A failed/interrupted update leaves the pending marker retryable.
  ippAddString(request, IPP_TAG_PRINTER, IPP_TAG_NAME, HP_SETUP_STATE_ATTRIBUTE, NULL, "complete");
  // The system queue remains local; access to it is governed by CUPS policy.
  ippAddBoolean(request, IPP_TAG_PRINTER, "printer-is-shared", false);
  if (enable) {
    ippAddInteger(request, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state", IPP_PSTATE_IDLE);
    ippAddBoolean(request, IPP_TAG_PRINTER, "printer-is-accepting-jobs", true);
  }
  return request;
}

static const char *no_password(const char *prompt, http_t *http, const char *method,
                               const char *resource, void *context) {
  (void)prompt; (void)http; (void)method; (void)resource; (void)context;
  return NULL; // Local CUPS peer authentication only; never prompt or read stdin.
}

int hp_secure_queue(const char *directory, bool enable) {
  char token[HP_AUTH_HEX_SIZE + 1];
  if (!hp_auth_load(directory, false, token)) return 1;
  ipp_t *request = hp_queue_auth_request(token, enable);
  if (!request) return 1;
  cupsSetPasswordCB2(no_password, NULL);
  // Explicit UNIX socket: CUPS_SERVER, IPP_PORT and network redirects cannot
  // choose the destination that receives the private credential.
  http_t *http = httpConnect2("/private/var/run/cupsd", 631, NULL, AF_UNIX,
                              HTTP_ENCRYPTION_IF_REQUESTED, 1, 3000, NULL);
  if (!http) { ippDelete(request); return 1; }
  httpSetTimeout(http, 5.0, NULL, NULL);
  // Rewriting even the same device URI can restart an active CUPS job. Check
  // the public installation marker first, and never migrate a busy queue.
  ipp_t *query = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
  ippAddString(query, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL,
               "ipp://localhost/printers/HP_LaserJet_1010_USB");
  const char *names[] = {AUTH_MARKER, "queued-job-count", "printer-is-shared", HP_SETUP_STATE_ATTRIBUTE};
  ippAddStrings(query, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", 4, NULL, names);
  ipp_t *response = cupsDoRequest(http, query, "/printers/HP_LaserJet_1010_USB");
  int result = 1;
  if (response && ippGetStatusCode(response) <= IPP_STATUS_OK_CONFLICTING) {
    ipp_attribute_t *count = ippFindAttribute(response, "queued-job-count", IPP_TAG_INTEGER);
    ipp_attribute_t *shared = ippFindAttribute(response, "printer-is-shared", IPP_TAG_BOOLEAN);
    const char *installed = ippGetString(ippFindAttribute(response, AUTH_MARKER, IPP_TAG_ZERO), 0, NULL);
    const char *setup = ippGetString(ippFindAttribute(response, HP_SETUP_STATE_ATTRIBUTE, IPP_TAG_NAME), 0, NULL);
    bool pending = setup && !strcmp(setup, "pending");
    if (pending && !enable) {
      ippAddInteger(request, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state", IPP_PSTATE_IDLE);
      ippAddBoolean(request, IPP_TAG_PRINTER, "printer-is-accepting-jobs", true);
    }
    char id[65]; credential_id(token, id);
    if (!enable && !pending && installed && !strcmp(installed, id) && shared && !ippGetBoolean(shared, 0)) result = 0;
    else if (count && ippGetCount(count) == 1) result = ippGetInteger(count, 0) == 0 ? -1 : 3;
  }
  ippDelete(response);
  if (result != -1) { ippDelete(request); httpClose(http); return result; }
  response = cupsDoRequest(http, request, "/admin/");
  bool ok = response && ippGetStatusCode(response) <= IPP_STATUS_OK_CONFLICTING;
  ippDelete(response);
  httpClose(http);
  return ok ? 0 : 1;
}
