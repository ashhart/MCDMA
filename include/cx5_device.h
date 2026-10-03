#pragma once
#include <stdint.h>

/* Shared by attachment, verbs discovery and the standalone acceptance client. */
static inline int mcdma_supported_device(uint32_t vendor, uint32_t device) {
    return vendor == 0x15b3 && (device == 0x1019 || device == 0x1017 || device == 0x1015);
}

static inline int mcdma_native_peer(uint32_t vendor, uint32_t device, unsigned link_layer) {
    return mcdma_supported_device(vendor, device) && link_layer == 2;
}
