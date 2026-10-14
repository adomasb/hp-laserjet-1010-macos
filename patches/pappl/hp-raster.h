#ifndef HP_RASTER_H
#define HP_RASTER_H
#include "pappl-private.h"

typedef struct {
  pappl_client_t *client;
  unsigned char prefix[12], header[sizeof(cups_page_header_t)];
  size_t prefix_size, prefix_offset, header_size, header_offset;
  bool apple, failed;
  unsigned pages, total_pages;
} hp_raster_guard;

bool hp_raster_prepare(hp_raster_guard *g, pappl_client_t *client);
ssize_t hp_raster_read(void *context, unsigned char *buffer, size_t length);
unsigned hp_raster_header(cups_raster_t *ras, cups_page_header_t *header, hp_raster_guard *g);
#endif
