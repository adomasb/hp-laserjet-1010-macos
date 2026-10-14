#include "hp-raster.h"
#include "printer_protocol.h"
#include <stddef.h>

/* Guard only fixed standard headers and I/O bounds; CUPS alone decodes raster.
 * Source architecture: Apple CUPS raster-stream.c cups_raster_read() fills
 * 65536-byte buffers for small pixel reads. Returning one byte for that request
 * prevents hidden read-ahead across a page boundary. Valid mono rows <=HP_MAX_DIMENSION.
 */

static unsigned hp_be32(const unsigned char *p) {
  return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}
static bool hp_exact(hp_raster_guard *g, unsigned char *p, size_t length, bool boundary) {
  size_t offset = 0;
  while (offset < length) {
    ssize_t n = _papplClientReadInput(g->client, p + offset, length - offset);
    if (n <= 0) {
      if (n < 0 || offset || !boundary) g->failed = true;
      return false;
    }
    offset += (size_t)n;
  }
  return true;
}
bool hp_raster_prepare(hp_raster_guard *g, pappl_client_t *client) {
  memset(g, 0, sizeof(*g));
  g->client = client;
  if (!hp_exact(g, g->prefix, 4, false)) return false;
  g->prefix_size = 4;
  if (!memcmp(g->prefix, "RaS2", 4)) return true;
  if (memcmp(g->prefix, "UNIR", 4)) { g->failed = true; return false; }
  g->apple = true;
  if (!hp_exact(g, g->prefix + 4, 8, false) || memcmp(g->prefix + 4, "AST\0", 4)) { g->failed = true; return false; }
  g->prefix_size = 12;
  g->total_pages = hp_be32(g->prefix + 8);
  if (g->total_pages > HP_MAX_PAGES) { g->failed = true; return false; }
  return true;
}
ssize_t hp_raster_read(void *context, unsigned char *buffer, size_t length) {
  hp_raster_guard *g = context;
  if (g->failed) { errno = EINVAL; return -1; }
  if (g->prefix_offset < g->prefix_size) {
    if (length > g->prefix_size - g->prefix_offset) length = g->prefix_size - g->prefix_offset;
    memcpy(buffer, g->prefix + g->prefix_offset, length);
    g->prefix_offset += length;
    return (ssize_t)length;
  }
  if (g->header_offset < g->header_size) {
    if (length > g->header_size - g->header_offset) length = g->header_size - g->header_offset;
    memcpy(buffer, g->header + g->header_offset, length);
    g->header_offset += length;
    return (ssize_t)length;
  }
  if (length == 65536) length = 1; /* CUPS pixel opcode buffer fill, no read-ahead. */
  else if (length > HP_MAX_DIMENSION) { g->failed = true; errno = EINVAL; return -1; }
  ssize_t n = _papplClientReadInput(g->client, buffer, length);
  if (n <= 0) g->failed = true; /* EOF inside page pixels is always truncation. */
  return n;
}
unsigned hp_raster_header(cups_raster_t *ras, cups_page_header_t *header, hp_raster_guard *g) {
  unsigned width, height, bits, line, dpi_x, dpi_y, color, duplex, count;
  g->header_size = g->apple ? 32 : sizeof(cups_page_header_t);
  g->header_offset = 0;
  if (!hp_exact(g, g->header, g->header_size, true)) return 0;
  if (g->apple) {
    bits = g->header[0]; color = g->header[1]; duplex = g->header[2];
    width = hp_be32(g->header + 12); height = hp_be32(g->header + 16);
    dpi_x = dpi_y = hp_be32(g->header + 20);
    line = width; count = g->total_pages;
    if (bits != 8 || (color != 0 && color != 4) || duplex > 1) goto invalid;
  } else {
#define HP_FIELD(name) hp_be32(g->header + offsetof(cups_page_header_t, name))
    width = HP_FIELD(cupsWidth); height = HP_FIELD(cupsHeight);
    bits = HP_FIELD(cupsBitsPerPixel); line = HP_FIELD(cupsBytesPerLine);
    dpi_x = HP_FIELD(HWResolution[0]); dpi_y = HP_FIELD(HWResolution[1]);
    color = HP_FIELD(cupsColorSpace); duplex = HP_FIELD(Duplex);
    count = HP_FIELD(cupsInteger[CUPS_RASTER_PWG_TotalPageCount]);
    if ((bits != 1 && bits != 8) || HP_FIELD(cupsBitsPerColor) != bits || HP_FIELD(cupsColorOrder) != CUPS_ORDER_CHUNKED || HP_FIELD(cupsNumColors) != 1 ||
        (color != CUPS_CSPACE_K && color != CUPS_CSPACE_W && color != CUPS_CSPACE_SW) || duplex) goto invalid;
#undef HP_FIELD
  }
  if (!width || width > HP_MAX_DIMENSION || !height || height > HP_MAX_DIMENSION || line != (width * bits + 7) / 8 || dpi_x != HP_RESOLUTION || dpi_y != HP_RESOLUTION || count > HP_MAX_PAGES || g->pages >= HP_MAX_PAGES) goto invalid;
  if (!g->pages && !g->apple) g->total_pages = count;
  if (!cupsRasterReadHeader(ras, header) || g->header_offset != g->header_size) goto invalid;
  header->cupsInteger[CUPS_RASTER_PWG_TotalPageCount] = g->total_pages;
  g->pages ++;
  return 1;
invalid:
  g->failed = true;
  return 0;
}
