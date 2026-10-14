#ifndef HP_TRANSPORT_H
#define HP_TRANSPORT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* A transport is per job. Never retry a job after any uncertain transfer.
 * begin/end on service job boundaries, write used by the same encoder.
 * write is bounded, short writes allowed, no automatic reconnect mid-job.
 * Simulation captures are private; failed/cancelled captures get .partial. */
typedef struct hp_transport hp_transport;
typedef struct {
  const char *capture_directory;
  size_t maximum_write; /* 0 means unlimited; simulation fault tool */
  int64_t fail_after; /* -1 disabled, fail once bytes >= boundary */
  unsigned delay_ms;
  int fail_errno; /* 0=EIO, ETIMEDOUT/ENODEV allowed for fault simulation */
} hp_sim_options;
hp_transport *hp_transport_sim_open(const hp_sim_options *options, unsigned job_id);
hp_transport *hp_transport_usb_open(char *error, size_t error_size);
ssize_t hp_transport_write(void *transport, const void *bytes, size_t length);
bool hp_transport_finish(hp_transport *transport, bool success);
void hp_transport_close(hp_transport *transport);
const char *hp_transport_error(const hp_transport *transport);
uint64_t hp_transport_bytes(const hp_transport *transport);
bool hp_usb_available(char *message, size_t message_size);
#endif
