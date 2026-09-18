#include "kernel_transport.hpp"
#include "apple_build.hpp"
#include "command_wait.hpp"
#include <libkern/OSByteOrder.h>
#include <libkern/c++/OSString.h>
#include <string.h>
#include <stdio.h>

namespace cx5_native {
IOReturn Buffer::allocate(IOMapper *mapper, uint64_t bytes) {
    if (memory || mapping || !mapper || !bytes || bytes > 64 * 1024 * 1024)
        return kIOReturnBadArgument;
    size = (bytes + 16383) & ~uint64_t(16383);
    memory = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut, size, 16384);
    if (!memory) return kIOReturnNoMemory;
    cpu = static_cast<uint8_t *>(memory->getBytesNoCopy());
    if (!cpu) { release(); return kIOReturnNoMemory; }
    memset(cpu, 0, size);
    mapping = IODMACommand::withSpecification(kIODMACommandOutputHost64, 64, 0,
        IODMACommand::kMapped, size, 4096, mapper);
    if (!mapping) { release(); return kIOReturnNoMemory; }
    auto result = mapping->setMemoryDescriptor(memory, true);
    if (!result) prepared = true;
    IODMACommand::Segment64 segment{};
    UInt64 offset = 0;
    UInt32 count = 1;
    if (!result) result = mapping->gen64IOVMSegments(&offset, &segment, &count);
    if (!result && (count != 1 || segment.fLength < size || offset < size ||
        !segment.fIOVMAddr || (segment.fIOVMAddr & 4095) ||
        segment.fIOVMAddr > UINT64_MAX - size)) result = kIOReturnUnsupported;
    if (!result) dma = segment.fIOVMAddr;
    else release();
    return result;
}

IOReturn Buffer::release() {
    if (mapping) {
        auto result = mapping->clearMemoryDescriptor(prepared);
        if (result) return result;
        prepared = false;
        mapping->release();
        mapping = nullptr;
    }
    if (memory) { memory->release(); memory = nullptr; }
    cpu = nullptr; dma = size = 0;
    return kIOReturnSuccess;
}

IOReturn Transport::attach(IOPCIDevice *device, IOService *owner) {
    if (pci_ || !device || !owner) return kIOReturnBadArgument;
    if (!supported_build()) return kIOReturnUnsupported;
    {   // ConnectX-5 Ex (0x1019) is the validated card; ConnectX-4 Lx (0x1015) shares the mlx5 command path and was
        // brought up on the same code with no other changes (Mac Studio M3 Ultra, Sonnet Echo SE I T5, 2026-09-17).
        const uint16_t did = device->configRead16(2);
        if (device->configRead16(0) != 0x15b3 || (did != 0x1019 && did != 0x1015)) return kIOReturnUnsupported;
    }
    if (!device->open(owner)) return kIOReturnExclusiveAccess;
    pci_ = device; pci_->retain(); owner_ = owner; opened_ = true;
    saved_command_ = pci_->configRead16(4);
    if (saved_command_ == UINT16_MAX) { close(); return kIOReturnNoDevice; }
    command_saved_ = true;
    // Match the working DriverKit path: fresh functions can have BAR memory
    // decoding disabled. Enable it before any MMIO; defer bus mastering until
    // this driver has installed its own mapped command queue.
    pci_->configWrite16(4, saved_command_ | 2);
    if (!(pci_->configRead16(4) & 2)) { close(); return kIOReturnNotReady; }
    bar_memory_ = pci_->getDeviceMemoryWithRegister(0x10);
    if (bar_memory_) {
        bar_memory_->retain(); bar_bytes_=bar_memory_->getLength();
        bar_=map_bar_page(0,false);
    }
    mapper_ = IOMapper::copyMapperForDevice(pci_);
    if (!bar_ || !mapper_) {
        close(); return kIOReturnNotReady;
    }
    return kIOReturnSuccess;
}

namespace {
// Walks the standard capability list for the PCI Express capability (id 0x10).
uint8_t express_capability(IOPCIDevice *device) {
    if (!device || !(device->configRead16(6)&0x10)) return 0; // Status: capabilities list.
    uint8_t at=uint8_t(device->configRead8(0x34)&0xfc);
    for (unsigned hop=0;hop<48 && at>=0x40;++hop) {
        const uint16_t header=device->configRead16(at);
        if ((header&0xff)==0x10) return at;
        at=uint8_t((header>>8)&0xfc);
    }
    return 0;
}
bool read_link(IOPCIDevice *device,PcieLink &link) {
    link=PcieLink{};
    if (auto *name=OSDynamicCast(OSString,device->getProperty("pcidebug")))
        strlcpy(link.name,name->getCStringNoCopy(),sizeof(link.name));
    else strlcpy(link.name,"?",sizeof(link.name));
    const uint8_t cap=express_capability(device);
    if (!cap) return false;
    const uint16_t control=device->configRead16(cap+8), link_control=device->configRead16(cap+0x10);
    if (control==0xffff) return false;
    link.max_payload=128u<<((control>>5)&7); link.max_read_request=128u<<((control>>12)&7);
    link.relaxed_ordering=(control&0x10)!=0; link.aspm_control=uint8_t(link_control&3);
    link.valid=true; return true;
}
}
bool Transport::configure_pcie(uint32_t bytes,char *text,size_t text_bytes) {
    if (!pci_ || !opened_ || !text || !text_bytes) return false;
    text[0]=0;
    express_capability_=express_capability(pci_);
    if (bytes) {
        if (bytes<128 || bytes>4096 || (bytes&(bytes-1)) || !express_capability_) return false;
        const uint16_t control=pci_->configRead16(express_capability_+8);
        if (control==0xffff) return false;
        if (!device_control_saved_) { saved_device_control_=control; device_control_saved_=true; }
        unsigned code=0; while ((128u<<code)<bytes) ++code;
        const uint16_t wanted=uint16_t((control&~uint16_t(7u<<12))|uint16_t(code<<12));
        pci_->configWrite16(express_capability_+8,wanted);
        if (pci_->configRead16(express_capability_+8)!=wanted) return false;
        mrrs_applied_=bytes;
    }
    // Device first, then each bridge up to the host bridge: the tunnel's
    // negotiated sizes are what a read across it is really limited by.
    size_t used=0; unsigned depth=0;
    IOService *node=pci_;
    while (node && depth<8 && used<text_bytes) {
        if (auto *device=OSDynamicCast(IOPCIDevice,node)) {
            PcieLink link{}; const bool ok=read_link(device,link);
            const int n=snprintf(text+used,text_bytes-used,"%s%s mps=%u mrrs=%u ro=%u aspm=%u%s",
                                 used?" | ":"",link.name,link.max_payload,link.max_read_request,
                                 link.relaxed_ordering,link.aspm_control,ok?"":" (no express capability)");
            if (n<0) break;
            used+=size_t(n); ++depth;
        }
        node=node->getProvider();
    }
    return true;
}
IOMemoryMap *Transport::map_bar_page(uint64_t page_offset,bool write_combine) {
    // Only PCI BAR MMIO is mapped physically here; DMA buffers still use IOMapper.
    // A fresh, bounded descriptor avoids changing the PCI descriptor's named
    // entry cache mode. Never create a whole-BAR mapping or overlap UC and WC.
    constexpr uint64_t page_bytes=16384;
    if (!bar_memory_ || (page_offset&(page_bytes-1)) || bar_bytes_<page_bytes ||
        page_offset>bar_bytes_-page_bytes || (write_combine && !page_offset)) return nullptr;
    IOByteCount contiguous=0;
    const uint64_t physical=bar_memory_->getPhysicalSegment(page_offset,&contiguous,kIOMemoryMapperNone);
    if (!physical || (physical&(page_bytes-1)) || contiguous<page_bytes ||
        physical>UINT64_MAX-page_bytes) return nullptr;
    IOAddressRange range{physical,page_bytes};
    auto *memory=IOMemoryDescriptor::withOptions(&range,1,0,nullptr,
        kIOMemoryTypePhysical64|kIODirectionInOut|kIOMemoryMapperNone);
    if (!memory) return nullptr;
    const auto cache=write_combine ? kIOMapWriteCombineCache : kIOMapInhibitCache;
    auto *mapping=memory->createMappingInTask(kernel_task,0,kIOMapAnywhere|cache,0,page_bytes);
    memory->release(); // The successful mapping retains its bounded descriptor.
    if (!mapping) return nullptr;
    const auto options=mapping->getMapOptions();
    if (mapping->getLength()!=page_bytes || !mapping->getVirtualAddress() ||
        (options&kIOMapCacheMask)!=cache || (options&kIOMapReadOnly)) {
        mapping->release(); return nullptr;
    }
    return mapping;
}

IOMemoryDescriptor *Transport::bar_page_descriptor(uint64_t page_offset) {
    constexpr uint64_t page_bytes=16384;
    if (!ready() || !bar_memory_ || (page_offset&(page_bytes-1)) || page_offset<page_bytes ||
        bar_bytes_<page_bytes || page_offset>bar_bytes_-page_bytes || page_offset==bf_base_) return nullptr;
    IOByteCount contiguous=0;
    const uint64_t physical=bar_memory_->getPhysicalSegment(page_offset,&contiguous,kIOMemoryMapperNone);
    if (!physical || (physical&(page_bytes-1)) || contiguous<page_bytes ||
        physical>UINT64_MAX-page_bytes) return nullptr;
    IOAddressRange range{physical,page_bytes};
    return IOMemoryDescriptor::withOptions(&range,1,0,nullptr,
        kIOMemoryTypePhysical64|kIODirectionInOut|kIOMemoryMapperNone);
}

bool Transport::map_uar(uint64_t page_offset,bool write_combine) {
    if (bf_map_ || page_offset<16384) return false;
    auto *mapping=map_bar_page(page_offset,write_combine);
    if (!mapping) return false;
    bf_map_=mapping; bf_base_=page_offset; bf_options_=mapping->getMapOptions(); return true;
}

uint32_t Transport::read32(uint64_t offset) const {
    if (!pci_ || pci_->isInactive() || detached() || !bar_ || bar_->getLength()<4 || (offset & 3) || offset > bar_->getLength() - 4)
        return UINT32_MAX;
    auto address = reinterpret_cast<volatile uint32_t *>(bar_->getVirtualAddress() + offset);
    return OSSwapBigToHostInt32(*address);
}

bool Transport::write32(uint64_t offset, uint32_t value) {
    if (!pci_ || pci_->isInactive() || detached() || !bar_ || bar_->getLength()<4 || (offset & 3) || offset > bar_->getLength() - 4)
        return false;
    auto address = reinterpret_cast<volatile uint32_t *>(bar_->getVirtualAddress() + offset);
    *address = OSSwapHostToBigInt32(value);
    publish_dma();
    return true;
}

IOReturn Transport::open() {
    if (detached() || queue_.memory || quarantined) return kIOReturnNotReady;
    const uint32_t high=read32(0x10), low=read32(0x14);
    pci_->setProperty("MCDMANativeCommandQueueHigh",high,32);
    pci_->setProperty("MCDMANativeCommandQueueLow",low,32);
    if (high==UINT32_MAX || low==UINT32_MAX) return kIOReturnNoDevice;
    if (high || (low & 0xfffff000)) return kIOReturnBusy;
    auto result = queue_.allocate(mapper_, 65536);
    if (result) return result;
    publish_dma();
    if (!write32(0x10, uint32_t(queue_.dma >> 32)) ||
        !write32(0x14, uint32_t(queue_.dma))) {
        quarantined = true; return kIOReturnNoDevice;
    }
    bound_ = true;
    if (read32(0x10) != uint32_t(queue_.dma >> 32) ||
        (read32(0x14) & 0xfffff000) != uint32_t(queue_.dma)) return kIOReturnIOError;
    pci_->configWrite16(4, saved_command_ | 2 | 4);
    if (!(pci_->configRead16(4) & 4)) return kIOReturnNotReady;
    for (unsigned i = 0; i < 1000; ++i) {
        if (!(read32(0x1fc) & 0x80000000)) return kIOReturnSuccess;
        IOSleep(1);
    }
    return kIOReturnNotReady;
}

bool Transport::execute(const uint8_t *in, size_t in_bytes, uint8_t *out, size_t out_bytes) {
    last = {};
    if (!in || !out || in_bytes < 16 || out_bytes < 16 || !ready()) return false;
    last.opcode = uint16_t(cx5::read_be32(in) >> 16);
    if (++token_ == 0) ++token_;
    constexpr size_t chain = cx5::max_mailboxes * cx5::mailbox_stride;
    auto error = cx5::Error::none;
    if (in_bytes > 16)
        error = cx5::prepare_mailboxes(queue_.cpu + 0x1000, chain, queue_.dma + 0x1000,
            in + 16, in_bytes - 16, token_);
    if (out_bytes > 16 && error == cx5::Error::none)
        error = cx5::prepare_mailboxes(queue_.cpu + 0x5000, chain, queue_.dma + 0x5000,
            nullptr, out_bytes - 16, token_);
    if (error == cx5::Error::none)
        error = cx5::encode_command(queue_.cpu, in, in_bytes, out_bytes, token_,
            in_bytes > 16 ? queue_.dma + 0x1000 : 0, out_bytes > 16 ? queue_.dma + 0x5000 : 0);
    if (error != cx5::Error::none) { last.transport_error = uint32_t(error); return false; }
    publish_dma();
    if (!write32(0x18, 1) || read32(0) == UINT32_MAX) {
        last.transport_error = uint32_t(cx5::Error::removed); quarantined = true; return false;
    }
    const auto waited=cx5::wait_command(
        [&]{return !(static_cast<volatile uint8_t *>(queue_.cpu)[63]&1);},
        [&]{return !pci_ || pci_->isInactive();},
        [](unsigned us){IODelay(us);},[](unsigned ms){IOSleep(ms);},last.polls);
    if (waited==cx5::CommandWait::complete) {
            acquire_dma(); last.completed = 1;
            if (queue_.cpu[60] != token_) error = cx5::Error::token_mismatch;
            else if ((last.delivery_status = queue_.cpu[63] >> 1)) error = cx5::Error::delivery;
            else if (cx5::read_be32(queue_.cpu + 56) != out_bytes) error = cx5::Error::invalid_length;
            if (error == cx5::Error::none) {
                last.firmware_status = queue_.cpu[32]; last.syndrome = cx5::read_be32(queue_.cpu + 36);
                memcpy(out, queue_.cpu + 32, 16);
                if (out_bytes > 16 && !last.firmware_status)
                    error = cx5::collect_mailboxes(queue_.cpu + 0x5000, chain, queue_.dma + 0x5000,
                        out + 16, out_bytes - 16, token_);
            }
            last.transport_error = uint32_t(error);
            if (error != cx5::Error::none) quarantined = true;
            const bool ok = error == cx5::Error::none && !last.firmware_status;
            if (ok && last.opcode == 0x104) enabled = true;
            if (ok && last.opcode == 0x105) enabled = false;
            if (ok && last.opcode == 0x102) initialized = true;
            if (ok && last.opcode == 0x103) initialized = false;
            return ok;
    }
    last.transport_error=uint32_t(waited==cx5::CommandWait::removed ? cx5::Error::removed : cx5::Error::timeout);
    quarantined=true; // Preserve all possibly live DMA until confirmed teardown.
    return false;
}

IOReturn Transport::close() {
    if (resources) return kIOReturnBusy;
    if (!detached()) {
        if (quarantined || enabled || initialized || resources) return kIOReturnBusy;
        if (bound_) {
            if (!write32(0x10, 0) || !write32(0x14, 0) || read32(0x10) ||
                (read32(0x14) & 0xfffff000)) return kIOReturnNotReady;
        }
        // Also undo memory decoding when startup failed before binding our
        // queue; never overwrite or clear a foreign command queue.
        if (device_control_saved_) {
            pci_->configWrite16(express_capability_+8, saved_device_control_);
            device_control_saved_=false; mrrs_applied_=0;
        }
        if (command_saved_) {
            pci_->configWrite16(4, saved_command_);
            if (pci_->configRead16(4) != saved_command_) return kIOReturnNotReady;
        }
    }
    auto result = queue_.release();
    if (result) return result;
    if (mapper_) { mapper_->release(); mapper_ = nullptr; }
    if (bf_map_) { bf_map_->release(); bf_map_=nullptr; }
    bf_base_=bf_uar_=0; bf_buffer_=bf_options_=0;
    if (bar_) { bar_->release(); bar_ = nullptr; }
    if (bar_memory_) { bar_memory_->release(); bar_memory_=nullptr; }
    bar_bytes_=0;
    if (pci_) {
        if (opened_) pci_->close(owner_);
        pci_->release(); pci_ = nullptr;
    }
    owner_ = nullptr; bound_ = opened_ = command_saved_ = false;
    enabled=false; initialized=false; quarantined=false;
    return kIOReturnSuccess;
}
}
