#pragma once

#ifdef CX5_NATIVE_TEST
// Offline resource-lifetime tests compile the same HCA code against a fake
// command transport; this flag is never set by the kernel build.
#include "kernel_test_types.hpp"
#else
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/pci/IOPCIDevice.h>
#endif
#include "cx5_protocol.hpp"
#include "cx5_status.h"

namespace cx5_native {
// Kernel allocation and IOMMU mapping remain distinct; never use CPU virtual
// or physical addresses as device addresses on Apple Silicon.
struct Buffer {
    static constexpr uint64_t max_allocation_bytes = 64ull * 1024 * 1024;
    IOBufferMemoryDescriptor *memory = nullptr;
    IODMACommand *mapping = nullptr;
    uint8_t *cpu = nullptr;
    uint64_t dma = 0, size = 0;
    bool prepared = false;
    IOReturn allocate(IOMapper *mapper, uint64_t bytes);
    IOReturn release(); // Only after the last hardware reference is destroyed.
};

// The sampler and data path hold different locks. These flags are latched
// across both paths; resets happen only during drained startup/teardown.
class SharedFlag {
    bool value_=false;
public:
    constexpr SharedFlag(bool initial=false):value_(initial) {}
    operator bool() const { return __atomic_load_n(&value_,__ATOMIC_ACQUIRE); }
    bool operator=(bool value) { __atomic_store_n(&value_,value,__ATOMIC_RELEASE); return value; }
    SharedFlag(const SharedFlag &)=delete;
    SharedFlag &operator=(const SharedFlag &)=delete;
};

// One PCIe function or bridge on the path to the host: negotiated maximum
// payload and read-request sizes, relaxed-ordering enable and ASPM control.
struct PcieLink {
    char name[24]{};
    uint32_t max_payload=0, max_read_request=0;
    bool relaxed_ordering=false; uint8_t aspm_control=0; bool valid=false;
};
class Transport {
public:
    IOReturn attach(IOPCIDevice *device, IOService *owner);
    uint16_t vendor_id() const { return vendor_id_; }
    uint16_t device_id() const { return device_id_; }
    // Reads the device's and its ancestors' PCIe control state into `text`
    // (registry diagnostic) and, when bytes is a power of two in 128..4096,
    // sets this device's maximum read request size, restored on close.
    bool configure_pcie(uint32_t max_read_request_bytes, char *text, size_t text_bytes);
    uint32_t max_read_request_applied() const { return mrrs_applied_; }
    IOReturn open();
    bool execute(const uint8_t *input, size_t input_bytes,
                 uint8_t *output, size_t output_bytes);
    // Refuses to unmap uncertain or live DMA, just like our DriverKit path.
    IOReturn close();
    bool detached() const;
    uint32_t read32(uint64_t offset) const;
    bool write32(uint64_t offset, uint32_t value);
    bool write64(uint64_t offset, uint64_t value);
    bool configure_doorbell(uint64_t uar_offset);
    bool configure_blueflame(uint64_t uar_offset,uint32_t buffer_bytes);
    bool write_blueflame(uint64_t offset,const uint8_t *wqe);
    uint32_t blueflame_map_options() const { return bf_options_; }
    // One bounded 16 KiB BAR page as a physical-range descriptor that the
    // Apple core can map into a user task (device memory, never the control
    // page, never overlapping the kernel's own UAR page). Caller releases.
    IOMemoryDescriptor *bar_page_descriptor(uint64_t page_offset);
    IOMapper *mapper() const { return mapper_; }
    // Data callbacks hold the provider lock; willTerminate quiesces them before
    // mappings are released. IOService inactivity is local state, whereas a
    // vendor-ID read synchronously traverses Thunderbolt on every invocation.
    // Keep the stronger detached() check for control operations and teardown.
    bool ready() const { return bound_ && !quarantined && pci_ && !pci_->isInactive(); }
    SharedFlag enabled{}, initialized{}, quarantined{};
    uint32_t resources = 0;
    cx5_command_result last{};
private:
    IOPCIDevice *pci_ = nullptr;
    IOService *owner_ = nullptr;
    IOMemoryDescriptor *bar_memory_ = nullptr;
    uint64_t bar_bytes_=0;
    IOMemoryMap *bar_ = nullptr; // Only the first 16 KiB: control MMIO.
    IOMemoryMap *bf_map_ = nullptr;
    uint64_t bf_base_=0, bf_uar_=0;
    uint32_t bf_buffer_=0, bf_options_=0;
    bool map_uar(uint64_t page_offset,bool write_combine);
    IOMemoryMap *map_bar_page(uint64_t page_offset,bool write_combine);
    IOMapper *mapper_ = nullptr;
    Buffer queue_{};
    bool bound_ = false, opened_ = false, command_saved_ = false;
    uint16_t saved_command_ = 0, vendor_id_ = 0, device_id_ = 0;
    uint8_t express_capability_ = 0;
    uint16_t saved_device_control_ = 0;
    bool device_control_saved_ = false;
    uint32_t mrrs_applied_ = 0;
    uint8_t token_ = 0;
};

inline void publish_dma() { __asm__ volatile("dsb oshst" ::: "memory"); }
inline void acquire_dma() { __asm__ volatile("dmb oshld" ::: "memory"); }
}
