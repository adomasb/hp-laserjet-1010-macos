#include "fastraster.h"
#include "printer_protocol.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wire constants are facts transcribed from HP HPLIP 3.26.6 FastRaster
 * arrays, not an imported implementation. See THIRD_PARTY_NOTICES.md for reference sources. */
static const uint8_t session[] = {
  0xc0,0,0xf8,0x86,0xc0,3,0xf8,0x8f,0xd1,0x58,2,0x58,2,0xf8,0x89,0x41,
  0xc2,0x11,0x20,0x70,0x68,0xf8,0x91,0x46,
  0xc0,0,0xf8,0x88,0xc0,1,0xf8,0x82,0x48
};
static const uint8_t page_start[] = {
  0x43,0xd3,0,0,0,0,0xf8,0x2a,0x75,0xc0,7,0xf8,3,0x6a,
  0xc0,0xcc,0xf8,0x2c,0x7b,0xd3,0,0,0,0,0xf8,0x4c,0x6b
};
static const uint8_t strip_start[] = {0xc2,6,0x20,0x70,0x68,0xf8,0x91,0xc2};

enum fr_state { FR_NEW, FR_JOB, FR_PAGE, FR_DONE };
struct hp_fr {
  hp_write_fn write;
  hp_cancel_fn cancelled;
  void *context;
  enum fr_state state;
  char error[96];
  uint8_t *band;
  uint8_t *compressed;
  unsigned width, height, rows, band_rows, pages;
  size_t packed, stride, compressed_capacity;
};

static bool fail(hp_fr *fr, const char *message) {
  if (fr && !fr->error[0]) snprintf(fr->error, sizeof(fr->error), "%s", message);
  return false;
}

static bool ready(hp_fr *fr) {
  if (!fr || fr->error[0]) return false;
  if (fr->cancelled && fr->cancelled(fr->context)) return fail(fr, "Job cancelled");
  return true;
}

static bool write_all(hp_fr *fr, const void *buffer, size_t length) {
  const uint8_t *p = buffer;
  while (length) {
    if (!ready(fr)) return false;
    ssize_t n = fr->write(fr->context, p, length);
    if (n < 0) {
      if (errno == EINTR) continue;
      return fail(fr, "Output write failed");
    }
    if (!n) return fail(fr, "Output writer made no progress");
    if ((size_t)n > length) return fail(fr, "Output writer returned an invalid count");
    p += (size_t)n;
    length -= (size_t)n;
  }
  return true;
}

static void be16(uint8_t *p, unsigned v) {
  p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static void be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void wipe(void *memory, size_t size) {
  volatile uint8_t *p = memory;
  while (size--) *p++ = 0;
}

static bool emit_byte(hp_fr *fr, size_t *used, uint8_t value) {
  if (*used >= fr->compressed_capacity) return fail(fr, "Compressed strip exceeds its bound");
  fr->compressed[(*used)++] = value;
  return true;
}

/* Count extensions use the saturated value in the command as their base.
 * The inclusive thresholds and FE/FF encodings are HP protocol facts;
 * this encoder deliberately avoids predictors and horizontal runs. */
static bool emit_count(hp_fr *fr, size_t *used, size_t count, size_t threshold) {
  if (count < threshold) return true;
  size_t remainder = count - threshold;
  if (remainder <= 253) return emit_byte(fr, used, (uint8_t)remainder);
  if (remainder <= 509)
    return emit_byte(fr, used, 0xfe) && emit_byte(fr, used, (uint8_t)(remainder-254));
  remainder -= 255;
  return emit_byte(fr, used, 0xff) && emit_byte(fr, used, (uint8_t)(remainder >> 8)) &&
      emit_byte(fr, used, (uint8_t)remainder);
}

static bool compress_band(hp_fr *fr, size_t *used) {
  *used = 0;
  for (unsigned y = 0; y < fr->band_rows; ++y) {
    const uint8_t *row = fr->band + y * fr->stride;
    const uint8_t *seed = y ? row - fr->stride : NULL;
    size_t prefix = 0;
    while (prefix < fr->stride && row[prefix] == (seed ? seed[prefix] : 0)) ++prefix;
    if (prefix == fr->stride) {
      /* Canonical pure-seed terminator: the row ends after the seed copy,
       * before the otherwise implied predictor/run is evaluated. */
      if (!emit_byte(fr, used, 0xb8) || !emit_count(fr, used, prefix, 3)) return false;
      continue;
    }
    size_t column = prefix;
    while (column < fr->stride) {
      size_t count = fr->stride - column;
      if (count > 512) count = 512;
      size_t replacements = count - 1;
      uint8_t command = (uint8_t)(((prefix < 3 ? prefix : 3) << 3) |
          (replacements < 7 ? replacements : 7));
      if (!emit_byte(fr, used, command) || !emit_count(fr, used, prefix, 3) ||
          !emit_byte(fr, used, row[column]) || !emit_count(fr, used, replacements, 7)) return false;
      for (size_t i = 1; i < count; ++i)
        if (!emit_byte(fr, used, row[column+i])) return false;
      column += count;
      prefix = 0;
    }
  }
  return true;
}

static bool flush_band(hp_fr *fr) {
  if (!fr->band_rows) return true;
  size_t compressed_length;
  if (!compress_band(fr, &compressed_length)) return false;
  uint32_t length = (uint32_t)compressed_length;
  uint32_t embedded = length + 25;
  uint8_t frame[15], header[25] = {0};
  memcpy(frame, strip_start, sizeof(strip_start));
  for (unsigned i = 0; i < 4; ++i) frame[8+i] = (uint8_t)(embedded >> (8*i));
  frame[12] = 0xf8; frame[13] = 0x92; frame[14] = 0x46;
  header[1] = 1;
  be16(header+2, HP_RESOLUTION);
  be16(header+4, 24);
  header[6] = 3;
  be32(header+12, length);
  be16(header+16, (unsigned)fr->stride * 8);
  be16(header+18, fr->band_rows);
  be16(header+22, fr->rows - fr->band_rows);
  header[24] = 1;
  if (!write_all(fr, frame, sizeof(frame)) ||
      !write_all(fr, header, sizeof(header)) ||
      !write_all(fr, fr->compressed, length)) return false;
  memset(fr->band, 0, fr->stride * 128);
  memset(fr->compressed, 0, fr->compressed_capacity);
  fr->band_rows = 0;
  return true;
}

hp_fr *hp_fr_create(hp_write_fn write, hp_cancel_fn cancelled, void *context) {
  if (!write) { errno = EINVAL; return NULL; }
  hp_fr *fr = calloc(1, sizeof(*fr));
  if (fr) { fr->write = write; fr->cancelled = cancelled; fr->context = context; }
  return fr;
}

void hp_fr_destroy(hp_fr *fr) {
  if (fr) {
    if (fr->band) { wipe(fr->band, fr->stride * 128); free(fr->band); }
    if (fr->compressed) { wipe(fr->compressed, fr->compressed_capacity); free(fr->compressed); }
    wipe(fr, sizeof(*fr));
    free(fr);
  }
}

const char *hp_fr_error(const hp_fr *fr) {
  return fr ? fr->error : "Invalid encoder";
}

bool hp_fr_begin_job(hp_fr *fr, unsigned copies) {
  if (!ready(fr)) return false;
  if (fr->state != FR_NEW) return fail(fr, "Job already started");
  if (copies != 1) return fail(fr, "Encoder requires one copy; repeat the captured job for copies");
  char pjl[300];
  int length = snprintf(pjl, sizeof(pjl),
      "\033%%-12345X@PJL SET RESOLUTION=%u\n"
      "@PJL SET BITSPERPIXEL=1\n"
      "@PJL SET ECONOMODE=OFF\r\n@PJL SET TIMEOUT=900\n"
      "@PJL ENTER LANGUAGE=PCLXL\n) HP-PCL XL;2;0;Comment\n", HP_RESOLUTION);
  if (length < 0 || (size_t)length >= sizeof(pjl)) return fail(fr, "Job header overflow");
  if (!write_all(fr, pjl, (size_t)length) || !write_all(fr, session, sizeof(session))) return false;
  fr->state = FR_JOB;
  return true;
}

bool hp_fr_begin_page(hp_fr *fr, unsigned width, unsigned height, unsigned media_code) {
  if (!ready(fr)) return false;
  if (fr->state != FR_JOB) return fail(fr, "Page requires an open job");
  if (!width || !height || width > HP_MAX_DIMENSION || height > HP_MAX_DIMENSION)
    return fail(fr, "Raster dimensions must be between 1 and 7100");
  if (media_code != 26 && media_code != 2) return fail(fr, "Unsupported media code");
  if (fr->pages == UINT_MAX) return fail(fr, "Too many pages");
  fr->width = width; fr->height = height;
  fr->packed = (width+7u)/8u;
  fr->stride = (fr->packed+7u) & ~(size_t)7;
  fr->band = calloc(128, fr->stride);
  /* At most two <=512-byte literal commands per row: <=3 bytes overhead
   * each. Any prefix-count extension costs no more than bytes skipped.
   * A pure-seed row needs <=4 bytes. Thus stride+6 bounds every row. */
  fr->compressed_capacity = 128 * (fr->stride + 6);
  fr->compressed = calloc(1, fr->compressed_capacity);
  if (!fr->band || !fr->compressed) return fail(fr, "Cannot allocate raster strip");
  fr->rows = fr->band_rows = 0;
  /* Input uses PCL paper codes; PCL XL/FastRaster paper enums differ. */
  uint8_t settings[] = {0xc0,0,0xf8,0x28,0xc0,0,0xf8,0x25};
  settings[5] = media_code == 26 ? 2 : 0;
  if (!write_all(fr, settings, sizeof(settings)) || !write_all(fr, page_start, sizeof(page_start))) return false;
  fr->state = FR_PAGE;
  return true;
}

bool hp_fr_write_row(hp_fr *fr, const uint8_t *row, size_t length) {
  if (!ready(fr)) return false;
  if (fr->state != FR_PAGE) return fail(fr, "Raster row requires an open page");
  if (!row || length != fr->packed) return fail(fr, "Raster row has an invalid length");
  if (fr->rows >= fr->height) return fail(fr, "Too many raster rows");
  unsigned remainder = fr->width & 7u;
  if (remainder && (row[length-1] & ((1u << (8u-remainder))-1u)))
    return fail(fr, "Unused raster tail bits must be white");
  memcpy(fr->band + fr->band_rows * fr->stride, row, length);
  ++fr->band_rows; ++fr->rows;
  return fr->band_rows < 128 || flush_band(fr);
}

bool hp_fr_end_page(hp_fr *fr) {
  if (!ready(fr)) return false;
  if (fr->state != FR_PAGE) return fail(fr, "No page is open");
  if (fr->rows != fr->height) return fail(fr, "Page has missing raster rows");
  static const uint8_t end = 0x44;
  if (!flush_band(fr) || !write_all(fr, &end, 1)) return false;
  wipe(fr->band, fr->stride * 128);
  free(fr->band); fr->band = NULL;
  wipe(fr->compressed, fr->compressed_capacity);
  free(fr->compressed); fr->compressed = NULL;
  fr->state = FR_JOB; ++fr->pages;
  return true;
}

bool hp_fr_end_job(hp_fr *fr) {
  if (!ready(fr)) return false;
  if (fr->state != FR_JOB) return fail(fr, "Job end requires all pages closed");
  if (!fr->pages) return fail(fr, "Job contains no pages");
  static const char end[] = "\x49\x42\033%-12345X@PJL EOJ\n\033%-12345X";
  if (!write_all(fr, end, sizeof(end)-1)) return false;
  fr->state = FR_DONE;
  return true;
}
