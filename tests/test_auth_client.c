/* Exercise macOS libcups challenge/retry and document upload against simulation. */
#include "local_security.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char token[HP_AUTH_HEX_SIZE + 1];
static int challenges;
static const char *password(const char *prompt, http_t *http, const char *method,
                            const char *resource, void *context) {
  (void)prompt; (void)http; (void)context;
  assert(!strcmp(method, "POST") && !strcmp(resource, "/ipp/print"));
  return ++challenges == 1 ? token : NULL;
}
int main(int argc, char **argv) {
  assert(argc == 4 && hp_auth_load(argv[2], false, token));
  int port = atoi(argv[1]);
  char uri[256]; snprintf(uri, sizeof(uri), "ipp://127.0.0.1:%d/ipp/print", port);
  cupsSetUser(HP_AUTH_USER);
  cupsSetPasswordCB2(password, NULL);
  http_t *http = httpConnect2("127.0.0.1", port, NULL, AF_INET, HTTP_ENCRYPTION_NEVER, 1, 3000, NULL);
  assert(http); httpSetTimeout(http, 8, NULL, NULL);
  ipp_t *request = ippNewRequest(IPP_OP_CREATE_JOB);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, uri);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, "cups-test-user");
  ipp_t *response = cupsDoRequest(http, request, "/ipp/print");
  assert(response && ippGetStatusCode(response) < IPP_STATUS_ERROR_BAD_REQUEST);
  int job = ippGetInteger(ippFindAttribute(response, "job-id", IPP_TAG_INTEGER), 0);
  assert(job > 0); ippDelete(response);
  request = ippNewRequest(IPP_OP_SEND_DOCUMENT);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, uri);
  ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", job);
  ippAddBoolean(request, IPP_TAG_OPERATION, "last-document", true);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", NULL, "image/pwg-raster");
  response = cupsDoFileRequest(http, request, "/ipp/print", argv[3]);
  assert(response && ippGetStatusCode(response) < IPP_STATUS_ERROR_BAD_REQUEST);
  assert(challenges == 1);
  ippDelete(response); httpClose(http);
  puts("PASS macOS libcups authentication challenge, retry, Create-Job and Send-Document");
}
