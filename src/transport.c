#include "transport.h"
#include "printer_protocol.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HP_WRITE_LIMIT 65536u
#define HP_USB_TIMEOUT_MS 5000u
struct hp_transport {
  bool usb, failed, finished;
  int fd, directory, error_code;
  uint64_t bytes;
  hp_sim_options sim;
  char partial[80], final[80], error[192];
  IOUSBInterfaceInterface300 **interface;
  UInt8 pipe, original_alt;
  bool alternate_changed;
};
static void fail(hp_transport *t, int code, const char *message) {
  if (!t->failed) {
    snprintf(t->error, sizeof(t->error), "%s", message);
    t->error_code = code;
  }
  t->failed = true;
  errno = t->error_code;
}
static hp_transport *allocate(void) {
  hp_transport *t = calloc(1, sizeof(*t));
  if (t) t->fd = t->directory = -1;
  return t;
}
__attribute__((visibility("hidden")))
bool hp_usb_registry_identity_internal(CFDictionaryRef properties) {
  if (!properties) return false;
  const CFStringRef keys[] = { CFSTR(kUSBVendorID), CFSTR(kUSBProductID),
                              CFSTR(kUSBInterfaceClass), CFSTR(kUSBInterfaceSubClass) };
  const int64_t expected[] = { 0x03f0, 0x0c17, 7, 1 };
  for (unsigned n = 0; n < 4; n++) {
    CFTypeRef value = CFDictionaryGetValue(properties, keys[n]); int64_t number = 0;
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID() ||
        !CFNumberGetValue(value, kCFNumberSInt64Type, &number) || number != expected[n]) return false;
  }
  CFTypeRef product = CFDictionaryGetValue(properties, CFSTR(kUSBProductString));
  if (!product) product = CFDictionaryGetValue(properties, CFSTR("USB Product Name"));
  return product && CFGetTypeID(product) == CFStringGetTypeID() &&
    (CFStringCompare(product, CFSTR("hp LaserJet 1010"), kCFCompareCaseInsensitive) == kCFCompareEqualTo ||
     CFStringCompare(product, CFSTR("HP LaserJet 1010 Printer"), kCFCompareCaseInsensitive) == kCFCompareEqualTo);
}
static bool registry_is_1010_interface(io_service_t service) {
  CFMutableDictionaryRef properties = NULL;
  if (IORegistryEntryCreateCFProperties(service, &properties, kCFAllocatorDefault, 0) != kIOReturnSuccess ||
      !properties) return false;
  /* Interface class/subclass must belong to this service. Product identity
   * may be inherited from its device parent without opening any user client. */
  const CFStringRef inherited[] = { CFSTR(kUSBVendorID), CFSTR(kUSBProductID), CFSTR(kUSBProductString) };
  for (unsigned n = 0; n < 3; n++) {
    if (CFDictionaryContainsKey(properties, inherited[n])) continue;
    CFTypeRef value = IORegistryEntrySearchCFProperty(service, kIOServicePlane, inherited[n],
                         kCFAllocatorDefault, kIORegistryIterateParents | kIORegistryIterateRecursively);
    if (value) { CFDictionarySetValue(properties, inherited[n], value); CFRelease(value); }
  }
  bool match = hp_usb_registry_identity_internal(properties);
  CFRelease(properties); return match;
}
static bool registry_original_alternate(io_service_t service, UInt8 *alternate) {
  CFTypeRef property = IORegistryEntryCreateCFProperty(service, CFSTR(kUSBAlternateSetting), kCFAllocatorDefault, 0);
  int64_t value = -1;
  bool valid = property && CFGetTypeID(property) == CFNumberGetTypeID() &&
               CFNumberGetValue(property, kCFNumberSInt64Type, &value) && value >= 0 && value <= 255;
  if (property) CFRelease(property);
  if (valid) *alternate = (UInt8)value;
  return valid;
}
static bool is_1010_product(IOUSBInterfaceInterface300 **interface) {
  io_service_t device = IO_OBJECT_NULL;
  if ((*interface)->GetDevice(interface, &device) != kIOReturnSuccess || !device) return false;
  CFTypeRef product = IORegistryEntryCreateCFProperty(device, CFSTR(kUSBProductString),
                                                     kCFAllocatorDefault, 0);
  /* GetDevice returns the interface plugin's borrowed handle (UUID245+ owns
   * its release); releasing it here invalidates subsequent device queries. */
  bool match = product && CFGetTypeID(product) == CFStringGetTypeID() &&
    (CFStringCompare(product, CFSTR("hp LaserJet 1010"), kCFCompareCaseInsensitive) == kCFCompareEqualTo ||
     CFStringCompare(product, CFSTR("HP LaserJet 1010 Printer"), kCFCompareCaseInsensitive) == kCFCompareEqualTo);
  if (product) CFRelease(product);
  return match;
}
/* Internal pure parser, also linked directly by deterministic transport tests.
 * Walk the entire bounded cached configuration before accepting any candidate. */
__attribute__((visibility("hidden")))
bool hp_usb_choose_alternate_internal(const uint8_t *data, size_t available,
                                     uint8_t number, uint8_t *alternate) {
  if (!data || !alternate || available < 9 || data[0] != 9 || data[1] != 2) return false;
  size_t total = data[2] | ((size_t)data[3] << 8);
  if (total < 9 || total > available || total > 4096) return false;
  bool candidate = false, bulk_out = false, found = false;
  uint8_t candidate_alt = 0, candidate_protocol = 0, selected = 0, priority = 0;
  for (size_t offset = 9; offset < total;) {
    if (total - offset < 2) return false;
    size_t length = data[offset]; uint8_t type = data[offset + 1];
    if (length < 2 || length > total - offset) return false;
    if (type == 4) {
      if (length != 9) return false;
      if (candidate && bulk_out && candidate_protocol > priority) {
        selected = candidate_alt; priority = candidate_protocol; found = true;
      }
      candidate = data[offset + 2] == number && data[offset + 5] == 7 &&
                  data[offset + 6] == 1 && (data[offset + 7] == 1 || data[offset + 7] == 2);
      candidate_alt = data[offset + 3]; candidate_protocol = data[offset + 7]; bulk_out = false;
    } else if (type == 5) {
      if (length < 7) return false;
      if (candidate && !(data[offset + 2] & 0x80) && (data[offset + 2] & 0x0f) &&
          (data[offset + 3] & 3) == 2 && (data[offset + 4] || data[offset + 5])) bulk_out = true;
    }
    offset += length;
  }
  if (candidate && bulk_out && candidate_protocol > priority) {
    selected = candidate_alt; found = true;
  }
  if (found) *alternate = selected;
  return found;
}
static bool choose_alternate(IOUSBInterfaceInterface300 **interface, UInt8 number,
                             UInt8 *alternate) {
  io_service_t device = IO_OBJECT_NULL;
  if ((*interface)->GetDevice(interface, &device) != kIOReturnSuccess || !device) return false;
  IOCFPlugInInterface **plugin = NULL; IOUSBDeviceInterface **dev = NULL; SInt32 score = 0;
  IOReturn result = IOCreatePlugInInterfaceForService(device, kIOUSBDeviceUserClientTypeID,
                                      kIOCFPlugInInterfaceID, &plugin, &score);
  /* The interface plugin retains ownership of GetDevice's borrowed handle. */
  if (result != kIOReturnSuccess || !plugin) return false;
  HRESULT query = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&dev);
  IODestroyPlugInInterface(plugin);
  if (query || !dev) return false;
  UInt8 active = 0, count = 0;
  bool found = false;
  if ((*dev)->GetConfiguration(dev, &active) == kIOReturnSuccess &&
      (*dev)->GetNumberOfConfigurations(dev, &count) == kIOReturnSuccess) {
    for (unsigned index = 0; index < count; index++) {
      IOUSBConfigurationDescriptorPtr config = NULL;
      if ((*dev)->GetConfigurationDescriptorPtr(dev, (UInt8)index, &config) != kIOReturnSuccess || !config) continue;
      const uint8_t *bytes = (const uint8_t *)config;
      if (bytes[0] != 9 || bytes[1] != 2 || bytes[5] != active) continue;
      size_t length = bytes[2] | ((size_t)bytes[3] << 8);
      /* IOKit owns this cached complete configuration; never copy unbounded data. */
      found = hp_usb_choose_alternate_internal(bytes, length, number, alternate);
      break;
    }
  }
  (*dev)->Release(dev);
  return found;
}
static void release_interface(IOUSBInterfaceInterface300 **interface, bool changed, UInt8 original) {
  if (changed) (void)(*interface)->SetAlternateInterface(interface, original);
  (void)(*interface)->USBInterfaceClose(interface);
  (*interface)->Release(interface);
}
hp_transport *hp_transport_sim_open(const hp_sim_options *o, unsigned job_id) {
  if (!o || !o->capture_directory || o->fail_after < -1 || o->delay_ms > 30000 ||
      (o->fail_errno != 0 && o->fail_errno != ETIMEDOUT && o->fail_errno != ENODEV)) {
    errno = EINVAL; return NULL;
  }
  if (mkdir(o->capture_directory, 0700) < 0 && errno != EEXIST) return NULL;
  int d = open(o->capture_directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (d < 0) return NULL;
  struct stat st;
  if (fstat(d, &st) < 0 || st.st_uid != geteuid() || (st.st_mode & 0777) != 0700) {
    close(d); errno = EACCES; return NULL;
  }
  hp_transport *t = allocate();
  if (!t) { close(d); return NULL; }
  t->directory = d; t->sim = *o;
  /* Random per-job suffix avoids collisions even across service restarts. */
  for (unsigned attempt = 0; attempt < 16; attempt++) {
    snprintf(t->partial, sizeof(t->partial), "job-%u-%08x%08x.partial", job_id,
             arc4random(), arc4random());
    t->fd = openat(d, t->partial, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (t->fd >= 0 || errno != EEXIST) break;
  }
  if (t->fd < 0) { int e = errno; close(d); free(t); errno = e; return NULL; }
  snprintf(t->final, sizeof(t->final), "%.*s.fr", (int)(strlen(t->partial) - 8), t->partial);
  return t;
}

hp_transport *hp_transport_usb_open(char *error, size_t error_size) {
  io_iterator_t iterator = IO_OBJECT_NULL;
  IOReturn result = IOServiceGetMatchingServices(kIOMainPortDefault,
                     IOServiceMatching(kIOUSBInterfaceClassName), &iterator);
  const char *message = "HP LaserJet 1010 USB interface is disconnected or unavailable";
  char plugin_error[192];
  int error_code = ENODEV;
  hp_transport *t = NULL;
  if (result == kIOReturnSuccess) {
    io_service_t service;
    while ((service = IOIteratorNext(iterator))) {
      /* Reject unrelated devices before IOCreatePlugInInterfaceForService,
       * which opens a kernel user client and can block independently of IO. */
      if (!registry_is_1010_interface(service)) { IOObjectRelease(service); continue; }
      IOCFPlugInInterface **plugin = NULL;
      IOUSBInterfaceInterface300 **interface = NULL;
      SInt32 score = 0;
      result = IOCreatePlugInInterfaceForService(service, kIOUSBInterfaceUserClientTypeID,
                                     kIOCFPlugInInterfaceID, &plugin, &score);
      if (result != kIOReturnSuccess || !plugin) {
        snprintf(plugin_error, sizeof(plugin_error),
                 "HP LaserJet 1010 is connected but macOS USB plugin access failed (0x%08x)", result);
        message = plugin_error;
        error_code = result == kIOReturnNoResources ? ENOMEM : EACCES;
        IOObjectRelease(service); continue;
      }
      HRESULT query = (*plugin)->QueryInterface(plugin,
                         CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&interface);
      IODestroyPlugInInterface(plugin);
      if (query || !interface) {
        message = "HP LaserJet 1010 macOS USB interface UUID300 is unavailable";
        error_code = EIO; IOObjectRelease(service); continue;
      }
      UInt16 vendor = 0, product = 0;
      UInt8 class = 0, subclass = 0, number = 0, original = 0, alternate = 0;
      bool match = (*interface)->GetDeviceVendor(interface, &vendor) == kIOReturnSuccess &&
                   (*interface)->GetDeviceProduct(interface, &product) == kIOReturnSuccess &&
                   (*interface)->GetInterfaceClass(interface, &class) == kIOReturnSuccess &&
                   (*interface)->GetInterfaceSubClass(interface, &subclass) == kIOReturnSuccess &&
                   (*interface)->GetInterfaceNumber(interface, &number) == kIOReturnSuccess &&
                   (*interface)->GetAlternateSetting(interface, &original) == kIOReturnSuccess &&
                   vendor == 0x03f0 && product == 0x0c17 && class == 7 && subclass == 1 &&
                   is_1010_product(interface);
      if (!match) { (*interface)->Release(interface); IOObjectRelease(service); continue; }
      if (!choose_alternate(interface, number, &alternate)) {
        message = "HP LaserJet 1010 has no validated protocol 1/2 bulk OUT alternate";
        error_code = ENODEV; (*interface)->Release(interface); IOObjectRelease(service); continue;
      }
      result = (*interface)->USBInterfaceOpen(interface);
      if (result != kIOReturnSuccess) {
        if (result == kIOReturnExclusiveAccess || result == kIOReturnBusy) {
          message = "HP LaserJet 1010 USB interface is busy; exclusive access unavailable";
          error_code = EBUSY;
        } else if (result == kIOReturnNotPrivileged || result == kIOReturnNotPermitted) {
          message = "HP LaserJet 1010 USB interface access denied";
          error_code = EACCES;
        } else if (result == kIOReturnNoDevice) {
          message = "HP LaserJet 1010 USB interface disconnected during open";
          error_code = ENODEV;
        } else {
          message = "HP LaserJet 1010 USB interface cannot be opened exclusively";
          error_code = EIO;
        }
        (*interface)->Release(interface); IOObjectRelease(service); continue;
      }
      /* Another owner may have changed the alternate before our exclusive
       * open; restoration must use the setting observed after ownership. */
      if (!registry_original_alternate(service, &original)) {
        message = "HP LaserJet 1010 original USB alternate cannot be read after exclusive open";
        error_code = EIO; release_interface(interface, false, 0); IOObjectRelease(service); continue;
      }
      bool changed = alternate != original;
      if (changed && (*interface)->SetAlternateInterface(interface, alternate) != kIOReturnSuccess) {
        message = "HP LaserJet 1010 supported USB alternate selection failed";
        error_code = EIO; release_interface(interface, true, original); IOObjectRelease(service); continue;
      }
      UInt8 actual = 0, protocol = 0;
      if ((*interface)->GetAlternateSetting(interface, &actual) != kIOReturnSuccess || actual != alternate ||
          (*interface)->GetInterfaceProtocol(interface, &protocol) != kIOReturnSuccess ||
          (protocol != 1 && protocol != 2)) {
        message = "HP LaserJet 1010 selected alternate cannot be validated";
        error_code = EIO; release_interface(interface, changed, original); IOObjectRelease(service); continue;
      }
      UInt8 endpoints = 0, selected = 0;
      if ((*interface)->GetNumEndpoints(interface, &endpoints) == kIOReturnSuccess) {
        for (unsigned p = 1; p <= endpoints; p++) {
          UInt8 direction, number, type, interval; UInt16 packet;
          if ((*interface)->GetPipeProperties(interface, (UInt8)p, &direction, &number,
                        &type, &packet, &interval) == kIOReturnSuccess &&
              direction == kUSBOut && type == kUSBBulk && packet > 0) { selected = (UInt8)p; break; }
        }
      }
      if (selected && (t = allocate())) {
        t->usb = true; t->interface = interface; t->pipe = selected;
        t->alternate_changed = changed; t->original_alt = original; IOObjectRelease(service); break;
      }
      message = selected ? "Cannot allocate USB transport" : "HP LaserJet 1010 has no usable bulk OUT endpoint";
      error_code = selected ? ENOMEM : ENODEV;
      release_interface(interface, changed, original);
      IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
  } else { message = "Cannot enumerate macOS USB interfaces"; error_code = EIO; }
  if (error && error_size) snprintf(error, error_size, "%s", t ? "" : message);
  if (!t) errno = error_code;
  return t;
}

ssize_t hp_transport_write(void *context, const void *data, size_t length) {
  hp_transport *t = context;
  if (!t) { errno = EINVAL; return -1; }
  if (t->failed || t->finished) { errno = t->failed ? t->error_code : EIO; return -1; }
  if (!data && length) { fail(t, EINVAL, "Invalid transport buffer"); return -1; }
  if (!length) return 0;
  if ((uint64_t)length > HP_TRANSPORT_LIMIT - t->bytes) {
    fail(t, EFBIG, "Transport job size limit exceeded"); return -1;
  }
  size_t count = length > HP_WRITE_LIMIT ? HP_WRITE_LIMIT : length;
  if (t->usb) {
    IOReturn result = (*t->interface)->WritePipeTO(t->interface, t->pipe, (void *)data,
                                 (UInt32)count, HP_USB_TIMEOUT_MS, HP_USB_TIMEOUT_MS);
    if (result != kIOReturnSuccess) {
      char message[160];
      snprintf(message, sizeof(message), "USB transfer failed (0x%08x); transferred amount uncertain; do not resubmit", result);
      int code = result == kIOUSBTransactionTimeout || result == kIOReturnTimeout ? ETIMEDOUT :
                 result == kIOReturnNoDevice ? ENODEV : EIO;
      fail(t, code, message); return -1;
    }
  } else {
    if (t->sim.fail_after >= 0 && t->bytes >= (uint64_t)t->sim.fail_after) {
      int code = t->sim.fail_errno ? t->sim.fail_errno : EIO;
      const char *message = code == ETIMEDOUT ? "Simulated transport timeout; do not resubmit" :
                            code == ENODEV ? "Simulated transport disconnect; do not resubmit" :
                            "Simulated transport failure; do not resubmit";
      fail(t, code, message); return -1;
    }
    if (t->sim.maximum_write && count > t->sim.maximum_write) count = t->sim.maximum_write;
    if (t->sim.fail_after >= 0 && count > (uint64_t)t->sim.fail_after - t->bytes)
      count = (size_t)((uint64_t)t->sim.fail_after - t->bytes);
    if (t->sim.delay_ms) {
      struct timespec delay = { t->sim.delay_ms / 1000, (long)(t->sim.delay_ms % 1000) * 1000000 };
      /* Interrupted delays stop promptly; no automatic transport retry. */
      if (nanosleep(&delay, NULL) < 0) { fail(t, EINTR, "Transport delay interrupted"); return -1; }
    }
    ssize_t written = write(t->fd, data, count);
    if (written <= 0) { fail(t, written < 0 ? errno : EIO, "Capture write failed"); return -1; }
    count = (size_t)written;
  }
  t->bytes += count;
  return (ssize_t)count;
}
bool hp_transport_finish(hp_transport *t, bool success) {
  if (!t || t->finished) { errno = EINVAL; return false; }
  t->finished = true;
  if (!success && !t->failed) fail(t, ECANCELED, "Transport job cancelled or incomplete");
  if (!t->usb) {
    if (fsync(t->fd) < 0) fail(t, errno, "Capture flush failed");
    if (close(t->fd) < 0) fail(t, errno, "Capture close failed");
    t->fd = -1;
    if (!t->failed && renameatx_np(t->directory, t->partial, t->directory,
                                  t->final, RENAME_EXCL) < 0)
      fail(t, errno, "Capture finalization failed");
  }
  return !t->failed;
}
void hp_transport_close(hp_transport *t) {
  if (!t) return;
  if (!t->finished) (void)hp_transport_finish(t, false);
  if (t->interface) {
    release_interface(t->interface, t->alternate_changed, t->original_alt);
  }
  if (t->directory >= 0) close(t->directory);
  free(t);
}
const char *hp_transport_error(const hp_transport *t) { return t ? t->error : "Transport unavailable"; }
uint64_t hp_transport_bytes(const hp_transport *t) { return t ? t->bytes : 0; }
bool hp_usb_available(char *message, size_t size) {
  hp_transport *t = hp_transport_usb_open(message, size);
  if (!t) return false;
  if (message && size) snprintf(message, size, "HP LaserJet 1010 interface is accessible; printing remains unverified");
  hp_transport_close(t); return true;
}
