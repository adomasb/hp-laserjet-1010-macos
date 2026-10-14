#include "fastraster.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint8_t *bytes;
  size_t length, capacity, short_count, calls, cancel_after;
  int fault;
  bool interrupted, cancel;
} sink;

static ssize_t write_sink(void *context, const void *bytes, size_t length) {
  sink *s = context;
  ++s->calls;
  if (s->fault == 1) { errno = EIO; return -1; }
  if (s->fault == 2) return 0;
  if (s->fault == 3) return (ssize_t)length + 1;
  if (s->fault == 4 && !s->interrupted) {
    s->interrupted = true; errno = EINTR; return -1;
  }
  if (s->short_count && length > s->short_count) length = s->short_count;
  assert(s->length + length <= 16 * 1024 * 1024);
  if (s->length + length > s->capacity) {
    s->capacity = (s->length + length) * 2 + 1024;
    s->bytes = realloc(s->bytes, s->capacity);
    assert(s->bytes);
  }
  memcpy(s->bytes + s->length, bytes, length);
  s->length += length;
  return (ssize_t)length;
}
static bool cancelled(void *context) {
  sink *s = context;
  return s->cancel || (s->cancel_after && s->calls >= s->cancel_after);
}
static hp_fr *create(sink *s) {
  hp_fr *fr = hp_fr_create(write_sink, cancelled, s);
  assert(fr); return fr;
}
static void dispose(hp_fr *fr, sink *s) {
  hp_fr_destroy(fr); free(s->bytes);
}
static void sticky(hp_fr *fr, sink *s) {
  char message[100];
  snprintf(message, sizeof(message), "%s", hp_fr_error(fr));
  assert(message[0]);
  size_t calls = s->calls;
  assert(!hp_fr_begin_job(fr, 1));
  assert(!hp_fr_begin_page(fr, 64, 1, 26));
  assert(!hp_fr_write_row(fr, NULL, 0));
  assert(!hp_fr_end_page(fr));
  assert(!hp_fr_end_job(fr));
  assert(s->calls == calls);
  assert(!strcmp(message, hp_fr_error(fr)));
}
static size_t find_bytes(const sink *s, const uint8_t *needle, size_t length) {
  for (size_t i = 0; i + length <= s->length; ++i)
    if (!memcmp(s->bytes+i, needle, length)) return i;
  assert(!"Expected wire sequence absent"); return 0;
}

static void fixture_test(void) {
  sink s = {.short_count = 7, .fault = 4}; hp_fr *fr = create(&s);
  assert(!hp_fr_error(fr)[0]);
  assert(hp_fr_begin_job(fr, 1));
  assert(s.interrupted);
  assert(hp_fr_begin_page(fr, 64, 1, 26));
  const uint8_t row[] = {0x80,0,0,0,0,0,0,1};
  assert(hp_fr_write_row(fr, row, sizeof(row)));
  assert(hp_fr_end_page(fr)); assert(hp_fr_end_job(fr));
  /* Source-derived literal command: 8 total bytes => replacement7,
   * saturated7 has explicit extension0 AFTER the first color byte.
   * Payload10, outer35, mode24, width64, actual height1. */
  const uint8_t fixture[] = {
    0xc2,6,0x20,0x70,0x68,0xf8,0x91,0xc2,0x23,0,0,0,0xf8,0x92,0x46,
    0,1,2,0x58,0,24,3,0,0,0,0,0,0,0,0,10,0,0x40,0,1,0,0,0,0,1,
    7,0x80,0,0,0,0,0,0,0,1
  };
  size_t offset = find_bytes(&s, fixture, sizeof(fixture));
  assert(s.bytes[offset+sizeof(fixture)] == 0x44);
  size_t calls = s.calls;
  assert(!hp_fr_begin_job(fr, 1)); sticky(fr, &s); assert(s.calls == calls);
  dispose(fr, &s);
}

static unsigned read_be16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static size_t read_be32(const uint8_t *p) {
  return ((size_t)p[0] << 24) | ((size_t)p[1] << 16) | ((size_t)p[2] << 8) | p[3];
}
static size_t first_strip(const sink *s) {
  const uint8_t start[] = {0xc2,6,0x20,0x70,0x68,0xf8,0x91,0xc2};
  return find_bytes(s, start, sizeof(start));
}

static void count_and_literal_tests(void) {
  /* Prefixes511/512/513 cross FE/FF boundary, with remaining9/8/7
   * literals crossing the inclusive replacement7 threshold. Stride520.
   * These command prefixes are hand-derived from HP's count formulas. */
  const uint8_t expected[][5] = {
    {0x1f,0xfe,0xfe,0x80,1},
    {0x1f,0xfe,0xff,0x80,0},
    {0x1e,0xff,0,0xff,0x80}
  };
  const size_t expected_lengths[] = {13,12,11};
  for (unsigned i = 0; i < 3; ++i) {
    sink s = {0}; hp_fr *fr = create(&s);
    uint8_t row[520] = {0}; row[511+i] = 0x80;
    assert(hp_fr_begin_job(fr, 1)); assert(hp_fr_begin_page(fr, 4160, 1, 26));
    assert(hp_fr_write_row(fr, row, sizeof(row)));
    assert(hp_fr_end_page(fr)); assert(hp_fr_end_job(fr));
    size_t at = first_strip(&s);
    assert(read_be32(s.bytes+at+15+12) == expected_lengths[i]);
    assert(!memcmp(s.bytes+at+40, expected[i], 5));
    for (size_t j = 5; j < expected_lengths[i]; ++j) assert(s.bytes[at+40+j] == 0);
    dispose(fr, &s);
  }
  /* Seed count remainder253 stays one byte; remainder254 startsFE. */
  const uint8_t seed_escape[][5] = {
    {0x1f,0xfc,0x80,1,0}, {0x1f,0xfd,0x80,0,0}, {0x1e,0xfe,0,0x80,0}
  };
  const size_t seed_escape_lengths[] = {12,11,10};
  for (unsigned i = 0; i < 3; ++i) {
    sink s = {0}; hp_fr *fr = create(&s);
    uint8_t row[264] = {0}; row[255+i] = 0x80;
    assert(hp_fr_begin_job(fr, 1)); assert(hp_fr_begin_page(fr, 2112, 1, 26));
    assert(hp_fr_write_row(fr, row, sizeof(row))); assert(hp_fr_end_page(fr)); assert(hp_fr_end_job(fr));
    size_t at = first_strip(&s);
    assert(read_be32(s.bytes+at+27) == seed_escape_lengths[i]);
    assert(!memcmp(s.bytes+at+40, seed_escape[i], sizeof(seed_escape[i])));
    dispose(fr, &s);
  }
  /* Literal replacement260/261 crosses remainder253/254 too. */
  const uint8_t literal_escape[][4] = {{0x1f,0,0x80,0xfd}, {0x17,0x80,0xfe,0}};
  for (unsigned i = 0; i < 2; ++i) {
    sink s = {0}; hp_fr *fr = create(&s);
    uint8_t row[264] = {0}; row[3-i] = 0x80;
    assert(hp_fr_begin_job(fr, 1)); assert(hp_fr_begin_page(fr, 2112, 1, 26));
    assert(hp_fr_write_row(fr, row, sizeof(row))); assert(hp_fr_end_page(fr)); assert(hp_fr_end_job(fr));
    size_t at = first_strip(&s);
    assert(read_be32(s.bytes+at+27) == 264+i);
    assert(!memcmp(s.bytes+at+40, literal_escape[i], sizeof(literal_escape[i])));
    dispose(fr, &s);
  }
  /* The first literal command contains512total bytes, never513.
   * Input row byte counts511/512/513 align to stride512/512/520. */
  for (unsigned n = 511; n <= 513; ++n) {
    sink s = {0}; hp_fr *fr = create(&s);
    uint8_t row[513]; memset(row, 0xff, sizeof(row));
    assert(hp_fr_begin_job(fr, 1)); assert(hp_fr_begin_page(fr, n*8, 1, 2));
    assert(hp_fr_write_row(fr, row, n)); assert(hp_fr_end_page(fr)); assert(hp_fr_end_job(fr));
    size_t at = first_strip(&s);
    const uint8_t first[] = {7,0xff,0xfe,0xfa};
    assert(!memcmp(s.bytes+at+40, first, sizeof(first)));
    assert(read_be16(s.bytes+at+15+16) == (n <= 512 ? 4096 : 4160));
    assert(read_be32(s.bytes+at+15+12) == (n <= 512 ? 515 : 525));
    if (n == 511) assert(s.bytes[at+40+514] == 0);
    else assert(s.bytes[at+40+514] == 0xff);
    if (n == 513) {
      const uint8_t second[] = {7,0xff,0,0,0,0,0,0,0,0};
      assert(!memcmp(s.bytes+at+40+515, second, sizeof(second)));
    }
    dispose(fr, &s);
  }
  /* A prefix of exactly3 must append a zero count-extension byte. */
  sink s = {0}; hp_fr *fr = create(&s);
  uint8_t row[8] = {0,0,0,0x80,0,0,0,0};
  assert(hp_fr_begin_job(fr, 1)); assert(hp_fr_begin_page(fr, 64, 1, 26));
  assert(hp_fr_write_row(fr, row, sizeof(row))); assert(hp_fr_end_page(fr)); assert(hp_fr_end_job(fr));
  const uint8_t prefix[] = {0x1c,0,0x80,0,0,0,0};
  size_t at = first_strip(&s);
  assert(read_be32(s.bytes+at+27) == sizeof(prefix));
  assert(!memcmp(s.bytes+at+40, prefix, sizeof(prefix)));
  dispose(fr, &s);
}

static void seed_and_final_height_tests(void) {
  const unsigned heights[] = {129,231};
  for (unsigned i = 0; i < 2; ++i) {
    sink s = {0}; hp_fr *fr = create(&s);
    uint8_t row[8]; memset(row, 0xff, sizeof(row));
    assert(hp_fr_begin_job(fr, 1)); assert(hp_fr_begin_page(fr, 64, heights[i], 26));
    for (unsigned y = 0; y < heights[i]; ++y) assert(hp_fr_write_row(fr, row, 8));
    assert(hp_fr_end_page(fr)); assert(hp_fr_end_job(fr));
    size_t at = first_strip(&s);
    assert(read_be32(s.bytes+at+27) == 264); /* first literal10+127 seed commands2 */
    const uint8_t literal[] = {7,0xff,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    assert(!memcmp(s.bytes+at+40, literal, sizeof(literal)));
    for (unsigned y = 1; y < 128; ++y) {
      assert(s.bytes[at+40+10+(y-1)*2] == 0xb8);
      assert(s.bytes[at+40+11+(y-1)*2] == 5);
    }
    at += 40 + 264;
    unsigned final_height = heights[i] - 128;
    assert(read_be16(s.bytes+at+15+18) == final_height);
    assert(read_be16(s.bytes+at+15+22) == 128);
    assert(read_be32(s.bytes+at+27) == 10+(final_height-1)*2);
    /* The first black row is literal again: seed MUST reset to white,
     * rather than copying the previous strip's final black row. */
    assert(!memcmp(s.bytes+at+40, literal, sizeof(literal)));
    assert(s.bytes[at+40+10+(final_height-1)*2] == 0x44);
    dispose(fr, &s);
  }
}

static void state_and_bounds_tests(void) {
  assert(!hp_fr_create(NULL, NULL, NULL));
  assert(!hp_fr_begin_job(NULL, 1));
  assert(!hp_fr_end_page(NULL)); hp_fr_destroy(NULL);
  const unsigned copies[] = {0,2,99,100};
  for (size_t i = 0; i < sizeof(copies)/sizeof(copies[0]); ++i) {
    sink s = {0}; hp_fr *fr = create(&s);
    assert(!hp_fr_begin_job(fr, copies[i])); assert(s.calls == 0);
    sticky(fr, &s); dispose(fr, &s);
  }
  for (unsigned i = 0; i < 7; ++i) {
    sink s = {0}; hp_fr *fr = create(&s);
    if (i == 0) assert(!hp_fr_begin_page(fr, 64, 1, 26));
    else if (i == 1) assert(!hp_fr_write_row(fr, (const uint8_t *)"x", 1));
    else if (i == 2) assert(!hp_fr_end_page(fr));
    else if (i == 3) assert(!hp_fr_end_job(fr));
    else {
      assert(hp_fr_begin_job(fr, 1));
      if (i == 4) assert(!hp_fr_begin_job(fr, 1));
      else if (i == 5) assert(!hp_fr_end_job(fr));
      else { assert(hp_fr_begin_page(fr, 64, 1, 26)); assert(!hp_fr_end_job(fr)); }
    }
    sticky(fr, &s); dispose(fr, &s);
  }
  const unsigned dimensions[][3] = {
    {0,1,26}, {1,0,26}, {7101,1,26}, {1,7101,26}, {~0u,~0u,26}, {64,1,0}
  };
  for (size_t i = 0; i < sizeof(dimensions)/sizeof(dimensions[0]); ++i) {
    sink s = {0}; hp_fr *fr = create(&s); assert(hp_fr_begin_job(fr, 1));
    assert(!hp_fr_begin_page(fr, dimensions[i][0], dimensions[i][1], dimensions[i][2]));
    sticky(fr, &s); dispose(fr, &s);
  }
  for (unsigned i = 0; i < 6; ++i) {
    sink s = {0}; hp_fr *fr = create(&s);
    assert(hp_fr_begin_job(fr, 1)); assert(hp_fr_begin_page(fr, 13, 1, 26));
    uint8_t row[2] = {0x80,0x08};
    if (i == 0) assert(!hp_fr_write_row(fr, row, 1));
    else if (i == 1) assert(!hp_fr_write_row(fr, NULL, 2));
    else if (i == 2) { row[1] = 1; assert(!hp_fr_write_row(fr, row, 2)); }
    else if (i == 3) assert(!hp_fr_end_page(fr));
    else if (i == 4) {
      assert(hp_fr_write_row(fr, row, 2)); assert(!hp_fr_write_row(fr, row, 2));
    } else assert(!hp_fr_begin_page(fr, 13, 1, 26));
    sticky(fr, &s); dispose(fr, &s);
  }
  sink s = {0}; hp_fr *fr = create(&s);
  assert(hp_fr_begin_job(fr, 1)); assert(hp_fr_begin_page(fr, 7100, 7100, 2));
  uint8_t row[888];
  for (unsigned i = 0; i < 7100; ++i) {
    memset(row, i % 2 ? 0 : 0xff, sizeof(row));
    row[887] &= 0xf0;
    assert(hp_fr_write_row(fr, row, sizeof(row)));
  }
  assert(hp_fr_end_page(fr)); assert(hp_fr_end_job(fr));
  size_t at = first_strip(&s);
  assert(read_be32(s.bytes+at+27) == 128*(888+6)); /* attain allocation bound */
  assert(s.length < 7 * 1024 * 1024); dispose(fr, &s);
}

static void fault_tests(void) {
  for (int fault = 1; fault <= 3; ++fault) {
    sink s = {.fault = fault}; hp_fr *fr = create(&s);
    assert(!hp_fr_begin_job(fr, 1)); sticky(fr, &s); dispose(fr, &s);
  }
  sink s = {.cancel = true}; hp_fr *fr = create(&s);
  assert(!hp_fr_begin_job(fr, 1)); assert(s.calls == 0); sticky(fr, &s); dispose(fr, &s);
  s = (sink){.short_count = 1, .cancel_after = 5}; fr = create(&s);
  assert(!hp_fr_begin_job(fr, 1)); assert(s.calls == 5); sticky(fr, &s); dispose(fr, &s);
  s = (sink){0}; fr = create(&s);
  assert(hp_fr_begin_job(fr, 1)); assert(hp_fr_begin_page(fr, 64, 128, 26));
  uint8_t row[8] = {0};
  for (unsigned i = 0; i < 127; ++i) assert(hp_fr_write_row(fr, row, 8));
  s.fault = 1; assert(!hp_fr_write_row(fr, row, 8)); sticky(fr, &s); dispose(fr, &s);
  s = (sink){0}; fr = create(&s);
  assert(hp_fr_begin_job(fr, 1)); assert(hp_fr_begin_page(fr, 64, 1, 26));
  s.cancel = true; assert(!hp_fr_write_row(fr, row, 8)); sticky(fr, &s); dispose(fr, &s);
}

static void emit_patterns(const char *path) {
  sink s = {.short_count = 29}; hp_fr *fr = create(&s);
  assert(hp_fr_begin_job(fr, 1)); assert(hp_fr_begin_page(fr, 13, 129, 26));
  for (unsigned y = 0; y < 129; ++y) {
    uint8_t row[2] = {0,0};
    if (y == 0 || y == 128) { row[0] = 0x80; row[1] = 0x08; }
    if (y == 64) { row[0] = 0xaa; row[1] = 0xa8; }
    assert(hp_fr_write_row(fr, row, 2));
  }
  assert(hp_fr_end_page(fr)); assert(hp_fr_begin_page(fr, 64, 1, 2));
  uint8_t row[8] = {0x80,0,0,0,0,0,0,1};
  assert(hp_fr_write_row(fr, row, 8)); assert(hp_fr_end_page(fr)); assert(hp_fr_end_job(fr));
  if (path) {
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(s.bytes, 1, s.length, file) == s.length); assert(!fclose(file));
  }
  dispose(fr, &s);
}

int main(int argc, char **argv) {
  assert(argc <= 2);
  fixture_test(); count_and_literal_tests(); seed_and_final_height_tests();
  state_and_bounds_tests(); fault_tests();
  emit_patterns(argc == 2 ? argv[1] : NULL);
  puts("DeltaPlus source commands/counts, seed resets, final bands, bounds and sticky faults passed");
  return 0;
}
