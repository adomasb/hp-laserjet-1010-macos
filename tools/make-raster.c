/* Public deterministic raster fixtures; no user documents. */
#include <cups/raster.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  unsigned width = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 4960;
  unsigned height = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 7016;
  unsigned pages = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10) : 2;
  int bits = argc > 5 ? atoi(argv[5]) : 1;
  if (width > 20000 || height > 20000 || !width || !height || !pages || pages > 101 || (bits != 1 && bits != 8)) return 2;
  int fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return 1;
  cups_mode_t mode = argc > 6 && !strcmp(argv[6], "urf") ? CUPS_RASTER_WRITE_APPLE : CUPS_RASTER_WRITE_PWG;
  bool noise = argc > 7 && !strcmp(argv[7], "noise");
  uint32_t noise_state = 0x1010600u;
  cups_raster_t *raster = cupsRasterOpen(fd, mode);
  if (!raster) { close(fd); return 1; }
  cups_page_header2_t header;
  memset(&header, 0, sizeof(header));
  header.HWResolution[0] = header.HWResolution[1] = 600;
  header.PageSize[0] = 595; header.PageSize[1] = 842;
  header.cupsPageSize[0] = 595.276f; header.cupsPageSize[1] = 841.89f;
  header.cupsWidth = width; header.cupsHeight = height;
  header.cupsBitsPerColor = header.cupsBitsPerPixel = bits;
  header.cupsBytesPerLine = bits == 1 ? (width + 7) / 8 : width;
  header.cupsColorOrder = CUPS_ORDER_CHUNKED;
  header.cupsColorSpace = bits == 1 ? CUPS_CSPACE_K : CUPS_CSPACE_SW;
  header.cupsNumColors = 1;
  header.NumCopies = 1;
  header.cupsInteger[CUPS_RASTER_PWG_TotalPageCount] = pages;
  snprintf(header.cupsPageSizeName, sizeof(header.cupsPageSizeName), "iso_a4_210x297mm");
  unsigned char *row = malloc(header.cupsBytesPerLine);
  if (!row) { cupsRasterClose(raster); close(fd); return 1; }
  for (unsigned page = 0; page < pages; page++) {
    if (!cupsRasterWriteHeader2(raster, &header)) return 1;
    for (unsigned y = 0; y < height; y++) {
      memset(row, bits == 1 ? 0 : 255, header.cupsBytesPerLine);
      if (noise) {
        /* High-entropy public fixture keeps aggregate output-bound tests
         * meaningful when white rows are compressed to seed-copy commands. */
        for (unsigned i = 0; i < header.cupsBytesPerLine; i++) {
          noise_state ^= noise_state << 13;
          noise_state ^= noise_state >> 17;
          noise_state ^= noise_state << 5;
          row[i] = (unsigned char)noise_state;
        }
        if (bits == 1 && (width & 7u)) row[header.cupsBytesPerLine - 1] &= (unsigned char)(255u << (8u - (width & 7u)));
      } else if (y == 50 + page * 50 && width > 120) {
        for (unsigned x = 40; x < 120; x++) {
          if (bits == 1) row[x / 8] |= 128u >> (x & 7); else row[x] = 0;
        }
      }
      if (cupsRasterWritePixels(raster, row, header.cupsBytesPerLine) != header.cupsBytesPerLine) return 1;
    }
  }
  free(row); cupsRasterClose(raster); close(fd);
  return 0;
}
