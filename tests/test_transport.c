#include "../src/transport.h"
#include <CoreFoundation/CoreFoundation.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static const unsigned char payload[] = { 0, 0x1b, 0xff, 1, 2, 3, 4, 0 };
bool hp_usb_choose_alternate_internal(const uint8_t *, size_t, uint8_t, uint8_t *);
bool hp_usb_registry_identity_internal(CFDictionaryRef);
static void test_registry_identity(void) {
  CFMutableDictionaryRef properties = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  assert(properties);
  const CFStringRef keys[] = { CFSTR("idVendor"), CFSTR("idProduct"),
                              CFSTR("bInterfaceClass"), CFSTR("bInterfaceSubClass") };
  const int64_t expected[] = { 0x03f0, 0x0c17, 7, 1 };
  for (unsigned n = 0; n < 4; n++) {
    CFNumberRef value = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &expected[n]);
    assert(value); CFDictionarySetValue(properties, keys[n], value); CFRelease(value);
  }
  CFDictionarySetValue(properties, CFSTR("USB Product Name"), CFSTR("hp LaserJet 1010"));
  assert(hp_usb_registry_identity_internal(properties));
  for (unsigned n = 0; n < 4; n++) {
    int64_t wrong = expected[n] + 1;
    CFNumberRef value = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &wrong);
    CFDictionarySetValue(properties, keys[n], value); CFRelease(value);
    assert(!hp_usb_registry_identity_internal(properties));
    CFDictionarySetValue(properties, keys[n], CFSTR("7"));
    assert(!hp_usb_registry_identity_internal(properties));
    CFDictionaryRemoveValue(properties, keys[n]);
    assert(!hp_usb_registry_identity_internal(properties));
    value = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &expected[n]);
    CFDictionarySetValue(properties, keys[n], value); CFRelease(value);
  }
  CFDictionarySetValue(properties, CFSTR("USB Product Name"), CFSTR("hp LaserJet 1012"));
  assert(!hp_usb_registry_identity_internal(properties));
  CFDictionarySetValue(properties, CFSTR("USB Product Name"), CFSTR("HP LASERJET 1010 PRINTER"));
  assert(hp_usb_registry_identity_internal(properties));
  CFDictionaryRemoveValue(properties, CFSTR("USB Product Name"));
  assert(!hp_usb_registry_identity_internal(properties));
  assert(!hp_usb_registry_identity_internal(NULL)); CFRelease(properties);
}
static void test_descriptors(void) {
  /* Independent hand-authored fixture using the observed descriptor fields,
   * including default protocol3 and protocol2/protocol1 alternates. */
  const uint8_t config[78] = {
    9,2,78,0,1,1,0,0xc0,1,
    9,4,0,0,3,7,1,3,0,
    7,5,0x01,2,64,0,0, 7,5,0x81,2,64,0,0, 7,5,0x82,3,8,0,255,
    9,4,0,1,2,7,1,2,0,
    7,5,0x01,2,64,0,0, 7,5,0x81,2,64,0,0,
    9,4,0,2,1,7,1,1,0,
    7,5,0x01,2,64,0,0
  };
  uint8_t alt = 99, modified[78];
  assert(hp_usb_choose_alternate_internal(config, sizeof(config), 0, &alt) && alt == 1);
  assert(!hp_usb_choose_alternate_internal(config, sizeof(config), 1, &alt));
  assert(!hp_usb_choose_alternate_internal(config, 77, 0, &alt));
  assert(!hp_usb_choose_alternate_internal(NULL, 78, 0, &alt));
  assert(!hp_usb_choose_alternate_internal(config, 78, 0, NULL));
  memcpy(modified, config, 78); modified[0] = 8;
  assert(!hp_usb_choose_alternate_internal(modified, 78, 0, &alt));
  memcpy(modified, config, 78); modified[2] = 79;
  assert(!hp_usb_choose_alternate_internal(modified, 78, 0, &alt));
  memcpy(modified, config, 78); modified[2] = 8;
  assert(!hp_usb_choose_alternate_internal(modified, 78, 0, &alt));
  memcpy(modified, config, 78); modified[2] = 0; modified[3] = 32;
  assert(!hp_usb_choose_alternate_internal(modified, 8192, 0, &alt));
  memcpy(modified, config, 78); modified[71] = 0;
  assert(!hp_usb_choose_alternate_internal(modified, 78, 0, &alt));
  memcpy(modified, config, 78); modified[71] = 8;
  assert(!hp_usb_choose_alternate_internal(modified, 78, 0, &alt));
  memcpy(modified, config, 78); modified[71] = 6;
  assert(!hp_usb_choose_alternate_internal(modified, 78, 0, &alt));
  memcpy(modified, config, 78); modified[39] = 8;
  assert(!hp_usb_choose_alternate_internal(modified, 78, 0, &alt));
  memcpy(modified, config, 78); modified[2] = 39;
  assert(!hp_usb_choose_alternate_internal(modified, 78, 0, &alt)); /* Only protocol3. */
  memcpy(modified, config, 78); modified[41] = 1;
  assert(hp_usb_choose_alternate_internal(modified, 78, 0, &alt) && alt == 2); /* Same-interface only. */
  memcpy(modified, config, 78); modified[46] = 1; modified[69] = 2;
  assert(hp_usb_choose_alternate_internal(modified, 78, 0, &alt) && alt == 2); /* Prefer protocol2 even later. */
  memcpy(modified, config, 78); modified[51] = 3; modified[74] = 3;
  assert(!hp_usb_choose_alternate_internal(modified, 78, 0, &alt)); /* No bulk OUT. */
  memcpy(modified, config, 78); modified[45] = 2; modified[68] = 2;
  assert(!hp_usb_choose_alternate_internal(modified, 78, 0, &alt)); /* Wrong subclass. */
}
/* Test-only link override forces capture-name collisions without weakening
 * production randomness or exposing transport internals. */
static uint32_t forced_random;
uint32_t arc4random(void) {
  if (forced_random) return forced_random;
  uint32_t value; arc4random_buf(&value, sizeof(value)); return value;
}
static void verify_directory(const char *directory, unsigned finals, unsigned partials,
                             size_t expected_final_bytes, size_t expected_partial_bytes) {
  DIR *d = opendir(directory); assert(d);
  struct dirent *entry;
  unsigned got_final = 0, got_partial = 0;
  size_t final_bytes = 0, partial_bytes = 0;
  while ((entry = readdir(d))) {
    if (entry->d_name[0] == '.') continue;
    char path[1024]; snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
    struct stat st; assert(lstat(path, &st) == 0);
    assert(S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
    if (strstr(entry->d_name, ".partial")) { got_partial++; partial_bytes += st.st_size; }
    else { assert(strstr(entry->d_name, ".fr")); got_final++; final_bytes += st.st_size; }
    int fd = open(path, O_RDONLY); assert(fd >= 0);
    unsigned char bytes[32]; ssize_t n = read(fd, bytes, sizeof(bytes)); close(fd);
    assert(n == st.st_size && n <= (ssize_t)sizeof(payload));
    assert(memcmp(bytes, payload, (size_t)n) == 0);
  }
  closedir(d);
  assert(got_final == finals && got_partial == partials);
  assert(final_bytes == expected_final_bytes && partial_bytes == expected_partial_bytes);
}
static void ignore_signal(int signal_number) { (void)signal_number; }
int main(void) {
  test_descriptors();
  test_registry_identity();
  char root[] = "/tmp/hp-transport-tests-XXXXXX"; assert(mkdtemp(root));
  char path[1024]; snprintf(path, sizeof(path), "%s/captures", root);
  hp_sim_options o = { path, 3, -1, 0, 0 };
  hp_transport *a = hp_transport_sim_open(&o, 7);
  hp_transport *b = hp_transport_sim_open(&o, 7); assert(a && b);
  /* Both live transports have the same job ID, but distinct exclusive files. */
  size_t offset = 0;
  while (offset < sizeof(payload)) {
    ssize_t n = hp_transport_write(a, payload + offset, sizeof(payload) - offset);
    assert(n > 0 && n <= 3); offset += (size_t)n;
  }
  assert(hp_transport_bytes(a) == sizeof(payload));
  assert(hp_transport_write(a, NULL, 0) == 0);
  assert(hp_transport_finish(a, true));
  assert(!hp_transport_finish(a, true));
  assert(hp_transport_write(a, payload, 1) == -1);
  hp_transport_close(a);
  assert(hp_transport_write(b, payload, sizeof(payload)) == 3);
  assert(!hp_transport_finish(b, false));
  assert(strstr(hp_transport_error(b), "cancelled")); hp_transport_close(b);
  verify_directory(path, 1, 1, 8, 3);

  o.maximum_write = 0; o.fail_after = 5;
  a = hp_transport_sim_open(&o, 8); assert(a);
  assert(hp_transport_write(a, payload, 8) == 5);
  assert(hp_transport_write(a, payload + 5, 3) == -1 && errno == EIO);
  assert(hp_transport_bytes(a) == 5);
  assert(strstr(hp_transport_error(a), "failure"));
  assert(!hp_transport_finish(a, true)); hp_transport_close(a);
  verify_directory(path, 1, 2, 8, 8);

  /* Failure before any data and implicit cancellation on close. */
  o.fail_after = 0; a = hp_transport_sim_open(&o, 9); assert(a);
  assert(hp_transport_write(a, payload, 8) == -1);
  assert(hp_transport_bytes(a) == 0); hp_transport_close(a);
  o.fail_after = -1; a = hp_transport_sim_open(&o, 10); assert(a); hp_transport_close(a);
  verify_directory(path, 1, 4, 8, 8);

  a = hp_transport_sim_open(&o, 11); assert(a);
  assert(hp_transport_write(a, payload, 256ull * 1024 * 1024 + 1) == -1 && errno == EFBIG);
  assert(strstr(hp_transport_error(a), "limit")); hp_transport_close(a);
  a = hp_transport_sim_open(&o, 12); assert(a);
  assert(hp_transport_write(a, NULL, 1) == -1 && errno == EINVAL); hp_transport_close(a);

  /* Fault-tool delay interruption is a terminal failure, not a retry. */
  struct sigaction action = {0}; action.sa_handler = ignore_signal;
  assert(sigaction(SIGALRM, &action, NULL) == 0);
  o.delay_ms = 500; a = hp_transport_sim_open(&o, 13); assert(a);
  struct itimerval timer = { .it_value = {0, 10000} };
  assert(setitimer(ITIMER_REAL, &timer, NULL) == 0);
  assert(hp_transport_write(a, payload, 1) == -1 && errno == EINTR);
  assert(hp_transport_bytes(a) == 0 && strstr(hp_transport_error(a), "interrupted"));
  assert(!hp_transport_finish(a, true)); hp_transport_close(a);
  o.delay_ms = 30001; assert(!hp_transport_sim_open(&o, 14) && errno == EINVAL);
  o.delay_ms = 0; o.fail_after = -2; assert(!hp_transport_sim_open(&o, 14));
  o.fail_after = -1;

  /* Timeout/disconnect faults after a confirmed prefix are terminal. A new
   * job may recover independently, while the old job must never resume. */
  const int fault_codes[] = { ETIMEDOUT, ENODEV };
  for (unsigned fault = 0; fault < 2; fault++) {
    o.fail_errno = fault_codes[fault]; o.fail_after = 3;
    a = hp_transport_sim_open(&o, 20 + fault); assert(a);
    assert(hp_transport_write(a, payload, 8) == 3);
    assert(hp_transport_write(a, payload + 3, 5) == -1 && errno == fault_codes[fault]);
    assert(hp_transport_bytes(a) == 3);
    char original_error[192]; snprintf(original_error, sizeof(original_error), "%s", hp_transport_error(a));
    assert(strstr(original_error, fault == 0 ? "timeout" : "disconnect"));
    o.fail_after = -1; o.fail_errno = 0;
    b = hp_transport_sim_open(&o, 30 + fault); assert(b);
    assert(hp_transport_write(b, payload, 8) == 8);
    assert(hp_transport_finish(b, true)); hp_transport_close(b);
    assert(hp_transport_write(a, payload + 3, 5) == -1 && errno == fault_codes[fault]);
    assert(hp_transport_bytes(a) == 3 && strcmp(original_error, hp_transport_error(a)) == 0);
    assert(!hp_transport_finish(a, true)); hp_transport_close(a);
  }
  verify_directory(path, 3, 9, 24, 14);
  o.fail_after = 0; o.fail_errno = ENODEV;
  a = hp_transport_sim_open(&o, 40); assert(a);
  assert(hp_transport_write(a, payload, 8) == -1 && errno == ENODEV);
  assert(hp_transport_bytes(a) == 0);
  assert(!hp_transport_finish(a, true)); hp_transport_close(a);
  verify_directory(path, 3, 10, 24, 14);
  o.fail_after = -1;
  o.fail_errno = EACCES; assert(!hp_transport_sim_open(&o, 14) && errno == EINVAL);
  o.fail_errno = 0;

  /* Existing insecure directories and symlinks cannot be capture targets. */
  char insecure[1024], link[1024];
  snprintf(insecure, sizeof(insecure), "%s/insecure", root); assert(mkdir(insecure, 0755) == 0);
  o.capture_directory = insecure; assert(!hp_transport_sim_open(&o, 15) && errno == EACCES);
  snprintf(link, sizeof(link), "%s/link", root); assert(symlink(path, link) == 0);
  o.capture_directory = link; assert(!hp_transport_sim_open(&o, 15));
  /* Force every attempted name to collide. Existing insecure regular files
   * and symlink targets must be left byte-for-byte and mode-for-mode intact. */
  char collision[2048], target[1024];
  snprintf(collision, sizeof(collision), "%s/job-15-0000000700000007.partial", path);
  int fd = open(collision, O_WRONLY | O_CREAT | O_EXCL, 0644); assert(fd >= 0);
  assert(write(fd, "sentinel", 8) == 8); assert(close(fd) == 0);
  o.capture_directory = path; forced_random = 7;
  assert(!hp_transport_sim_open(&o, 15) && errno == EEXIST);
  struct stat existing; assert(lstat(collision, &existing) == 0);
  assert((existing.st_mode & 0777) == 0644 && existing.st_size == 8);
  char sentinel[8]; fd = open(collision, O_RDONLY); assert(fd >= 0);
  assert(read(fd, sentinel, 8) == 8 && memcmp(sentinel, "sentinel", 8) == 0); close(fd);
  assert(unlink(collision) == 0);
  snprintf(target, sizeof(target), "%s/target", root);
  fd = open(target, O_WRONLY | O_CREAT | O_EXCL, 0644); assert(fd >= 0);
  assert(write(fd, "sentinel", 8) == 8); close(fd);
  assert(symlink(target, collision) == 0);
  assert(!hp_transport_sim_open(&o, 15) && errno == EEXIST);
  assert(lstat(collision, &existing) == 0 && S_ISLNK(existing.st_mode));
  assert(stat(target, &existing) == 0 && (existing.st_mode & 0777) == 0644 && existing.st_size == 8);
  fd = open(target, O_RDONLY); assert(fd >= 0);
  assert(read(fd, sentinel, 8) == 8 && memcmp(sentinel, "sentinel", 8) == 0); close(fd);
  assert(unlink(collision) == 0 && unlink(target) == 0); forced_random = 0;
  assert(hp_transport_write(NULL, payload, 1) == -1);
  assert(hp_transport_bytes(NULL) == 0);
  assert(strlen(hp_transport_error(NULL)) > 0); hp_transport_close(NULL);

  /* The aggregate software suite never opens physical USB interfaces. */
  if (!getenv("HP1010_SKIP_USB_PROBE")) {
    /* Read-only hardware absence check: this must never submit bytes. */
    char error[192]; a = hp_transport_usb_open(error, sizeof(error));
    if (a) {
      hp_transport_close(a);
      puts("USB absence check SKIPPED: an interface is attached; no writes issued");
    } else {
      assert(strlen(error) > 0);
      printf("USB transport open unavailable: %s\n", error);
      assert(!hp_usb_available(error, sizeof(error)) && strlen(error) > 0);
    }
  }
  /* All diagnostics are temporary and tests clean them after verification. */
  DIR *d = opendir(path); assert(d); struct dirent *entry;
  while ((entry = readdir(d))) {
    if (entry->d_name[0] == '.') continue;
    char file[2048]; snprintf(file, sizeof(file), "%s/%s", path, entry->d_name); assert(unlink(file) == 0);
  }
  closedir(d); assert(rmdir(path) == 0); assert(unlink(link) == 0);
  assert(rmdir(insecure) == 0); assert(rmdir(root) == 0);
  puts("transport tests passed");
  return 0;
}
