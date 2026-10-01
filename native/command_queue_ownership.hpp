#pragma once
#include <stdint.h>

namespace cx5_native {
// The initialization segment contains a command queue address in bits 63:12.
// A zero address is the ordinary unbound state. On the reported cold CX5 Ex,
// firmware presents 0x80000156: NIC interface mode 1 (disabled), an apparent
// 0x80000000 address, and queue size/stride 5/6. Linux writes its new DMA
// address over the entire low word during startup. Restrict this exception
// to the observed disabled signature; a queue in full-driver mode stays busy.
constexpr bool command_queue_available(uint32_t high, uint32_t low) {
    return high == 0 && ((low & 0xfffff000u) == 0 || low == 0x80000156u);
}
}
