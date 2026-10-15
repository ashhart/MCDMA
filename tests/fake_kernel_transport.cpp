#include "fake_kernel_transport.hpp"
#include "cx5_device.h"
#include "command_queue_ownership.hpp"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
namespace cx5_test { Simulation sim; }
using namespace cx5_test;
namespace cx5_native {
IOReturn Buffer::allocate(IOMapper *,uint64_t bytes) {
    assert(!cpu && bytes);
    if (bytes>max_allocation_bytes) return kIOReturnBadArgument;
    if (bytes==sim.fail_buffer_allocate_bytes) return kIOReturnNoMemory;
    size=(bytes+16383)&~uint64_t(16383);
    cpu=static_cast<uint8_t *>(calloc(1,size)); assert(cpu);
    memory=new IOBufferMemoryDescriptor;
    memory->bytes=cpu; memory->live_buffers=&sim.buffers;
    dma=sim.next_dma; sim.next_dma+=size; prepared=true; ++sim.buffers;
    // Deliberately different from CPU VA, exposing accidental identity mapping.
    assert(dma!=reinterpret_cast<uint64_t>(cpu)); sim.maps[dma]=cpu;
    return kIOReturnSuccess;
}
IOReturn Buffer::release() {
    if (cpu) sim.maps.erase(dma);
    if (memory) memory->release();
    *this={}; return kIOReturnSuccess;
}
IOReturn Transport::attach(IOPCIDevice *,IOService *) { return kIOReturnSuccess; }
IOReturn Transport::open() {
    // Match the real transport: a bound command queue cannot be opened twice.
    if (bound_ || !command_queue_available(sim.command_queue_high,sim.command_queue_low)) return kIOReturnBusy;
    if (!mcdma_supported_device(sim.vendor_id,sim.device_id)) return kIOReturnUnsupported;
    vendor_id_=sim.vendor_id; device_id_=sim.device_id;
    pci_=&sim.pci; sim.pci.inactive=&sim.removed;
    bar_=&sim.bar; bar_bytes_=sim.bar.length; sim.uar.writes=&sim.doorbells;
    bound_=true; return kIOReturnSuccess;
}
IOReturn Transport::close() {
    if (resources || (!sim.removed && (enabled || initialized || quarantined))) return kIOReturnBusy;
    vendor_id_=device_id_=0;
    bound_=false; pci_=nullptr; bar_=bf_map_=nullptr; bf_base_=bf_uar_=0; bf_buffer_=bf_options_=0; bar_bytes_=0; return kIOReturnSuccess;
}
bool Transport::map_uar(uint64_t offset,bool wc) {
    if (bf_map_ || offset<16384 || (offset&16383) || bar_bytes_<16384 ||
        offset>bar_bytes_-16384 || (wc ? sim.bf_map_fail : sim.uc_map_fail)) return false;
    bf_map_=&sim.uar; bf_base_=offset; bf_options_=wc ? 0x400 : 0x100;
    sim.uar.length=16384; return true;
}
IOMemoryDescriptor *Transport::bar_page_descriptor(uint64_t offset) {
    if (!ready() || offset<16384 || (offset&16383) || bar_bytes_<16384 || offset>bar_bytes_-16384 || offset==bf_base_) return nullptr;
    auto &page=sim.user_pages[offset];
    if (page.empty()) page.assign(16384,0);
    auto *descriptor=new IOMemoryDescriptor; descriptor->bytes=page.data(); return descriptor;
}
bool Transport::configure_pcie(uint32_t bytes,char *text,size_t text_bytes) {
    if (!text || !text_bytes) return false;
    if (bytes && (bytes<128 || bytes>4096 || (bytes&(bytes-1)))) return false;
    ++sim.pcie_configs; sim.mrrs_requested=bytes; mrrs_applied_=bytes;
    snprintf(text,text_bytes,"3:0:0 mps=512 mrrs=%u ro=1 aspm=0 | 2:1:0 mps=512 mrrs=512 ro=1 aspm=0",bytes?bytes:512u);
    return true;
}
uint32_t Transport::read32(uint64_t) const { return 0; }
bool Transport::write32(uint64_t,uint32_t) { return !sim.removed; }
bool Transport::execute(const uint8_t *in,size_t in_bytes,uint8_t *out,size_t out_bytes) {
    assert(in_bytes>=16 && out_bytes>=16); ++sim.calls;
    sim.command.assign(in,in+in_bytes);
    memset(out,0,out_bytes); last={};
    const uint16_t op=uint16_t(cx5::read_be32(in)>>16); last.opcode=op;
    if (!ready()) return false;
    if (op==0x802 && ++sim.uar_allocations==sim.fail_uar_allocation) { last.firmware_status=1; return false; }
    if (op==sim.fail_opcode) {
        if (sim.timeout) quarantined=true;
        else last.firmware_status=1;
        return false;
    }
    switch (op) {
    case 0x104: enabled=true; break;
    case 0x105: assert(!initialized && sim.objects.empty() && sim.pages.empty()); enabled=false; break;
    case 0x102: assert(enabled); initialized=true; break;
    case 0x103: assert(sim.objects.empty()); initialized=false; break;
    case 0x10a: assert(out_bytes==112); out[111]=2; break;
    case 0x10b: assert(cx5::read_be32(in+8)==1); break;
    case 0x100: assert(out_bytes==4112); cx5::set_bits(out+16,4096,0x260,1,sim.bf_capable); cx5::set_bits(out+16,4096,0x26b,5,sim.bf_log);
        cx5::set_bits(out+16,4096,0x490,16,sim.uar_page_log); break;
    case 0x109: // SET_HCA_CAP, general capabilities, before the init pages.
        assert(in_bytes==4112 && cx5::read_be32(in+4)==0 && !initialized); ++sim.set_caps;
        if (!sim.uar_pages_16k_supported) { last.firmware_status=1; return false; }
        sim.uar_page_log=unsigned(cx5::get_bits(in+16,4096,0x490,16)); break;
    case 0x107:
        cx5::write_be32(out+8,1);
        cx5::write_be32(out+12,uint32_t(cx5::read_be32(in+4)==2 ?
            sim.query_initial_pages_count : sim.query_pages_count));
        break;
    case 0x108: {
        const auto count=cx5::read_be32(in+12);
        if (cx5::read_be32(in+4)==1) {
            assert(in_bytes==16+8*count);
            for (unsigned i=0;i<count;++i) sim.pages.push_back(cx5::read_be64(in+16+8*i));
        } else {
            assert(out_bytes==16+8*count && count<=sim.pages.size());
            cx5::write_be32(out+8,count);
            for (unsigned i=0;i<count;++i) {
                cx5::write_be64(out+16+8*i,sim.bad_reclaim ? 0xdead0000 : sim.pages.back());
                sim.pages.pop_back();
            }
        }
        break;
    }
    case 0x800: case 0x802: case 0x301: case 0x400: case 0x500: case 0x200: {
        if (op==0x200) {
            const bool relaxed=cx5::get_bits(in+16,64,0xd,1)==1;
            if (relaxed && sim.refuse_relaxed_ordering) { last.completed=1; last.firmware_status=3; return false; }
            if (relaxed) ++sim.relaxed_keys; else ++sim.strict_keys;
        }
        const uint32_t id=sim.next_id++; sim.objects.insert(id);
        if (op==0x400) sim.cq_dma[id]=cx5::read_be64(in+0x110);
        if (op==0x500) sim.qp_dma[id]=cx5::read_be64(in+0x110);
        cx5::write_be32(out+8,id); break;
    }
    case 0x801: case 0x803: case 0x302: case 0x401: case 0x501: case 0x202:
        assert(sim.objects.erase(cx5::read_be32(in+8))==1); break;
    case 0x502: case 0x503: case 0x504: case 0x50a:
        assert(sim.objects.count(cx5::read_be32(in+8))); break;
    case 0x754:
        assert(out_bytes==272); out[262]=0x02; out[267]=0x10;
        cx5::set_bits(out+16,256,0x130,16,sim.vport_frame); break;
    case 0x755:
        if (cx5::read_be32(in+12)==2) { assert(in[259]==1); break; }
        assert(cx5::read_be32(in+12)==64 && in_bytes==512);
        if (sim.reject_jumbo_vport_once && cx5::get_bits(in+256,256,0x130,16)==9022) {
            sim.reject_jumbo_vport_once=false; last.firmware_status=1; return false;
        }
        sim.vport_frame=uint16_t(cx5::get_bits(in+256,256,0x130,16)); break;
    case 0x761: assert(in[11]==1 && (in[43]==2 || in[43]==0)); break;
    case 0x805:
        if (cx5::read_be32(in+8)==0x9051) {
            // Query only, PCIe index 0, group 0, no clear.
            assert(in_bytes==272 && out_bytes==272 && cx5::read_be32(in+4)==1);
            for (size_t i=16;i<24;++i) assert(in[i]==0);
            ++sim.mpcnt_queries;
            if (!sim.mpcnt_supported) { last.completed=1; last.firmware_status=2; return false; }
            for (unsigned i=0;i<16;++i) cx5::write_be32(out+24+4*i,sim.mpcnt[i]);
            break;
        }
        assert(in_bytes==32 && out_bytes==32 && in[17]==1);
        if (cx5::read_be32(in+8)==0x5006) {
            if (sim.port_gate) {
                sim.blocked_command_lock=test_held_locks.back(); sim.port_arrived.set_value();
                sim.port_continue.get_future().wait();
            }
            out[19]=1; break;
        }
        assert(cx5::read_be32(in+8)==0x5003);
        if (cx5::read_be32(in+4)==0) {
            const auto requested=uint16_t(cx5::get_bits(in+16,16,0x40,16));
            assert(requested<=sim.max_frame);
            if (sim.ignore_jumbo_port_once && requested==9022) sim.ignore_jumbo_port_once=false;
            else sim.admin_frame=sim.oper_frame=requested;
        } else assert(cx5::read_be32(in+4)==1);
        cx5::set_bits(out+16,16,0x20,16,sim.max_frame);
        cx5::set_bits(out+16,16,0x40,16,sim.admin_frame);
        cx5::set_bits(out+16,16,0x60,16,sim.oper_frame); break;
    case 0x750: out[15]=sim.vport; break;
    default: assert(false);
    }
    last.completed=1; return true;
}
}
