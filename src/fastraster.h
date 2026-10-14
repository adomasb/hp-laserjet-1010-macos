#ifndef HP_FASTRASTER_H
#define HP_FASTRASTER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Input rows: 600 dpi, top-to-bottom, MSB first, 1=black, padded tail white.
 * Width/height describe the supplied raster; service currently preserves
 * full-page dimensions. Physical imageable origin is pending calibration.
 * Media is HP PCL paper code. Encoder copies must be 1; service replays jobs.
 * Writer can return a short count, -1 with errno, or zero (fatal no progress).
 * Encoder owns no transport; cancellation checked before every write. */
typedef ssize_t (*hp_write_fn)(void *, const void *, size_t);
typedef bool (*hp_cancel_fn)(void *);
typedef struct hp_fr hp_fr;
hp_fr *hp_fr_create(hp_write_fn write, hp_cancel_fn cancelled, void *context);
void hp_fr_destroy(hp_fr *encoder);
bool hp_fr_begin_job(hp_fr *encoder, unsigned copies);
bool hp_fr_begin_page(hp_fr *encoder, unsigned width, unsigned height, unsigned media_code);
bool hp_fr_write_row(hp_fr *encoder, const uint8_t *row, size_t length);
bool hp_fr_end_page(hp_fr *encoder);
bool hp_fr_end_job(hp_fr *encoder);
const char *hp_fr_error(const hp_fr *encoder);
#endif

