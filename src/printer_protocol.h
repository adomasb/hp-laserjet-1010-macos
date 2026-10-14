#ifndef HP_PRINTER_PROTOCOL_H
#define HP_PRINTER_PROTOCOL_H

/* Shared by the helper, PAPPL adapter, and Swift's bridging header. */
#define HP_USB_PORT 8632
#define HP_SETUP_STATE_ATTRIBUTE "hp1010-setup-state-default"
#define HP_SIMULATION_PORT 8631
#define HP_RESOLUTION 600
#define HP_MAX_DIMENSION 7100
#define HP_MAX_PAGES 100
#define HP_MAX_COPIES 99
#define HP_IPP_ATTRIBUTE_LIMIT (1024 * 1024)
#define HP_DOCUMENT_LIMIT (128ULL * 1024 * 1024)
#define HP_TRANSPORT_LIMIT (256ULL * 1024 * 1024)
#define HP_PACKED_ROW_BYTES ((HP_MAX_DIMENSION + 7) / 8)

#endif
