#include "pappl-private.h"
#include "printer_protocol.h"

// Local helper bounded request reader; one counter spans IPP and raster.
ssize_t _papplClientReadInput(void *context, unsigned char *buffer, size_t bytes)
{
  pappl_client_t *client = context;
  if (client->input_bytes >= client->input_limit) { errno = EFBIG; return -1; }
  if (bytes > client->input_limit - client->input_bytes) bytes = client->input_limit - client->input_bytes;
  ssize_t n = httpRead(client->http, (char *)buffer, bytes);
  if (n > 0) client->input_bytes += (size_t)n;
  return n;
}
// IPP callback contract requires requested fields in full, even on fragmented HTTP.
ssize_t _papplClientReadIPP(void *context, unsigned char *buffer, size_t bytes)
{
  size_t offset = 0;
  while (offset < bytes) {
    ssize_t n = _papplClientReadInput(context, buffer + offset, bytes - offset);
    if (n <= 0) return n < 0 ? -1 : (ssize_t)offset;
    offset += (size_t)n;
  }
  return (ssize_t)offset;
}
