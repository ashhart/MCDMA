#include "kernel_hca.hpp"
#include "cx5_cq_observer.h"
#include <string.h>

namespace cx5_native {
void Hca::header(uint16_t opcode, uint16_t modifier) {
    memset(input_,0,sizeof(input_)); cx5::write_be32(input_,uint32_t(opcode)<<16);
    cx5::write_be32(input_+4,modifier);
}
bool Hca::call(size_t in_bytes,size_t out_bytes) {
    memset(output_,0,sizeof(output_));
    return transport.execute(input_,in_bytes,output_,out_bytes);
}
bool Hca::simple(uint16_t opcode,uint16_t modifier,size_t out_bytes) {
    header(opcode,modifier); return call(16,out_bytes);
}
bool Hca::create(HardwareObject &object,size_t in_bytes) {
    if (object.live || !call(in_bytes)) return false;
    object.id=cx5::read_be32(output_+8)&0xffffff; object.live=true;
    ++transport.resources; return true;
}
bool Hca::destroy(HardwareObject &object,uint16_t opcode) {
    if (!object.live) return true;
    if (!transport.detached()) {
        header(opcode); cx5::write_be32(input_+8,object.id);
        if (!call()) return false;
    }
    object={}; --transport.resources; return true;
}
bool Hca::provide(Pool &p,uint16_t phase) {
    startup_page_phase=phase;
    startup_page_step=1; // QUERY_PAGES
    startup_page_count=0;
    if (!simple(0x107,phase)) return false;
    const int32_t count=int32_t(cx5::read_be32(output_+12));
    startup_page_count=count;
    startup_page_step=2; // Validate the firmware's signed page request.
    if (count<0 || uint32_t(count)>max_pages || p.buffer.memory) {
        startup_error=kIOReturnBadArgument;
        return false;
    }
    p.count=uint32_t(count); p.function=uint16_t(cx5::read_be32(output_+8));
    if (!p.count) { startup_page_step=0; startup_page_phase=0; return true; }
    startup_page_step=3; // Allocate and map the original page pool.
    const IOReturn allocated=p.buffer.allocate(transport.mapper(),uint64_t(p.count)*4096);
    if (allocated) { startup_error=allocated; return false; }
    for (uint32_t offset=0;offset<p.count;) {
        uint32_t batch=p.count-offset; if (batch>256) batch=256;
        startup_page_step=4; // MANAGE_PAGES give command.
        header(0x108,1); cx5::write_be32(input_+8,p.function); cx5::write_be32(input_+12,batch);
        for (uint32_t j=0;j<batch;++j)
            cx5::write_be64(input_+16+j*8,p.buffer.dma+uint64_t(offset+j)*4096);
        if (!call(16+batch*8)) return false;
        for (uint32_t j=0;j<batch;++j) p.mark_given(offset+j);
        p.owned+=batch; offset+=batch;
    }
    startup_page_step=0;
    startup_page_phase=0;
    return true;
}
bool Hca::reclaim(Pool &p) {
    while (p.owned && !transport.quarantined) {
        uint32_t batch=p.owned; if (batch>256) batch=256;
        header(0x108,2); cx5::write_be32(input_+8,p.function); cx5::write_be32(input_+12,batch);
        if (!call(16,16+batch*8)) return false;
        const uint32_t returned=cx5::read_be32(output_+8);
        if (!returned || returned>batch) { transport.quarantined=true; return false; }
        for (uint32_t j=0;j<returned;++j) {
            const uint64_t dma=cx5::read_be64(output_+16+j*8);
            Pool *owner=nullptr;
            Pool *pools[2]={&boot_,&initial_};
            for (Pool *candidate:pools)
                if (candidate->function==p.function && dma>=candidate->buffer.dma &&
                    dma-candidate->buffer.dma<uint64_t(candidate->count)*4096 && !(dma&4095)) owner=candidate;
            if (!owner || !owner->take_given(uint32_t((dma-owner->buffer.dma)/4096))) {
                transport.quarantined=true; return false;
            }
            --owner->owned;
        }
    }
    return p.owned==0;
}
bool Hca::attach_and_start(IOPCIDevice *device, IOService *owner) {
    startup_error=transport.attach(device,owner);
    // start() owns command-queue opening; doing it here too rejects startup.
    return !startup_error && start();
}
bool Hca::start() {
    startup_page_phase=startup_page_step=0;
    startup_page_count=0;
    blueflame_capable=blueflame_enabled=user_blueflame=false;
    blueflame_buffer_bytes=0; blueflame_posts=0;
    startup_error=transport.open();
    if (startup_error) return false;
    // A successful command can still be followed by a local validation or
    // allocation failure; never publish a zero startup error in that case.
    startup_error=kIOReturnIOError;
    if (!simple(0x104) || !simple(0x10a,0,112) || !(output_[111]&2)) return false;
    header(0x10b); cx5::write_be32(input_+8,1);
    if (!call() || !provide(boot_,1) || !simple(0x100,1,4112)) return false;
    uar_shift_=12+uint32_t(cx5::get_bits(output_+16,4096,0x490,16));
    // QUERY_HCA_CAP general fields, NVIDIA PRM: bf and log_bf_reg_size.
    const auto bf_log=uint32_t(cx5::get_bits(output_+16,4096,0x26b,5));
    blueflame_capable=cx5::get_bits(output_+16,4096,0x260,1) && bf_log>=8 && bf_log<=11;
    if (blueflame_capable) blueflame_buffer_bytes=uint32_t(1)<<(bf_log-1);
    if (uar_shift_!=12 && !(uar_shift_==14 && user_queues_requested)) return false;
    // Userspace doorbells need one firmware UAR per mappable 16 KiB host
    // page. This is negotiated before the init pages, as the vendor driver
    // orders it; a refusal keeps 4 KiB pages and only disables user queues.
    // Firmware may retain the negotiated size across a warm driver restart.
    // A current-capability query already confirming 16 KiB needs no rewrite.
    user_queues=user_queues_requested && uar_shift_==14;
    if (user_queues_requested && !user_queues && configure_uar_pages()) { uar_shift_=14; user_queues=true; }
    if (!provide(initial_,2) || !simple(0x102)) return false;
    // UARs are 4 KiB but Apple VM pages are 16 KiB. Hold the first-page IDs
    // until a disjoint UAR is allocated, so WC never aliases control MMIO.
    // With 16 KiB UAR pages only index 0 aliases the control page.
    const unsigned first_disjoint=uar_shift_==14 ? 1 : 4;
    for (unsigned attempt=0;attempt<5;++attempt) {
        header(0x802); if (!create(uar_,16)) return false;
        if (uar_.id>=first_disjoint) break;
        for (const auto &held:skipped_uars_)
            if (held.live && held.id==uar_.id) { transport.quarantined=true; return false; }
        if (attempt==4) return false;
        skipped_uars_[attempt]=uar_; uar_={};
    }
    const uint64_t uar_offset=uint64_t(uar_.id)<<uar_shift_;
    if (blueflame_capable)
        blueflame_enabled=transport.configure_blueflame(uar_offset,blueflame_buffer_bytes);
    if (!blueflame_enabled && !transport.configure_doorbell(uar_offset)) return false;
    user_blueflame=user_queues && blueflame_enabled && user_blueflame_requested;
    if (eq_buffer_.allocate(transport.mapper(),4096)) return false;
    for (unsigned i=0;i<32;++i) eq_buffer_.cpu[i*64+63]=1;
    header(0x301);
    cx5::set_bits(input_+16,64,0x63,5,5); cx5::set_bits(input_+16,64,0x68,24,uar_page_index(uar_));
    cx5::write_be64(input_+0x110,eq_buffer_.dma);
    if (!create(eq_,0x118) || !configure_roce()) return false;
    startup_error=kIOReturnSuccess;
    return true;
}
bool Hca::configure_uar_pages() {
    // output_ holds the current general capabilities just queried. Request
    // log_uar_page_sz=2 (16 KiB) through SET_HCA_CAP, then trust only the
    // value the firmware reports back.
    header(0x109,0); memcpy(input_+16,output_+16,4096);
    cx5::set_bits(input_+16,4096,0x490,16,2);
    if (!call(4112)) return false;
    if (!simple(0x100,1,4112)) return false;
    return cx5::get_bits(output_+16,4096,0x490,16)==2;
}
bool Hca::alloc_uar(HardwareObject &uar) {
    if (!user_queues || uar.live) return false;
    header(0x802); if (!create(uar,16)) return false;
    if (uar.id==0) { // Aliases control MMIO; never expose it.
        destroy(uar,0x803); return false;
    }
    return true;
}
bool Hca::dealloc_uar(HardwareObject &uar) { return destroy(uar,0x803); }
bool Hca::stop() {
    uint32_t owned=uint32_t(uar_.live)+uint32_t(eq_.live);
    for (const auto &held:skipped_uars_) owned+=uint32_t(held.live);
    if (transport.resources>owned) return false;
    if (!destroy(eq_,0x302) || !destroy(uar_,0x803)) return false;
    for (auto &held:skipped_uars_) if (!destroy(held,0x803)) return false;
    if (!transport.detached()) {
        if (transport.quarantined) return false;
        if (transport.initialized && !simple(0x103)) return false;
        if (!reclaim(initial_) || !reclaim(boot_)) return false;
        if (transport.enabled && !simple(0x105)) return false;
    }
    if (eq_buffer_.release() || initial_.buffer.release() || boot_.buffer.release()) return false;
    return transport.close()==kIOReturnSuccess;
}
bool Hca::configure_roce() {
    if (!simple(0x754,0,272)) return false;
    memcpy(mac,output_+262,6);
    bool valid=false; for (auto byte:mac) valid|=byte!=0;
    if (!valid || (mac[0]&1)) return false;
    gid[0]=0xfe; gid[1]=0x80; gid[8]=mac[0]^2; gid[9]=mac[1]; gid[10]=mac[2];
    gid[11]=0xff; gid[12]=0xfe; memcpy(gid+13,mac+3,3);
    header(0x755); cx5::write_be32(input_+12,2); input_[259]=1;
    if (!call(512)) return false;
    if (!source_gid(true)) return false;
    header(0x805); cx5::write_be32(input_+8,0x5006);
    input_[17]=1; input_[18]=1; input_[20]=0x80;
    return call(32,32) && configure_ethernet_mtu(1500);
}
bool Hca::query_mtu(MtuState &state) {
    header(0x805,1); cx5::write_be32(input_+8,0x5003); input_[17]=1;
    if (!call(32,32)) return false;
    state.maximum=uint16_t(cx5::get_bits(output_+16,16,0x20,16));
    state.admin=uint16_t(cx5::get_bits(output_+16,16,0x40,16));
    state.oper=uint16_t(cx5::get_bits(output_+16,16,0x60,16));
    if (!simple(0x754,0,272)) return false;
    state.vport=uint16_t(cx5::get_bits(output_+16,256,0x130,16));
    return state.maximum>=1522 && state.admin>=1280 && state.admin<=state.maximum;
}
bool Hca::write_port_mtu(uint16_t bytes) {
    header(0x805); cx5::write_be32(input_+8,0x5003); input_[17]=1;
    cx5::set_bits(input_+16,16,0x40,16,bytes);
    return call(32,32);
}
bool Hca::write_vport_mtu(uint16_t bytes) {
    header(0x755); cx5::write_be32(input_+12,1u<<6);
    cx5::set_bits(input_+256,256,0x130,16,bytes);
    return call(512);
}
bool Hca::configure_ethernet_mtu(uint16_t bytes) {
    if (!transport.ready() || !transport.initialized || qps_ || bytes<1280 || bytes>9000) return false;
    MtuState before{},after{};
    if (!query_mtu(before)) return false;
    // Ethernet header, optional VLAN and FCS are included in hardware limits.
    const uint16_t frame=uint16_t(bytes+22);
    if (frame>before.maximum) return false;
    if (write_port_mtu(frame) && write_vport_mtu(frame) && query_mtu(after) &&
        after.admin==frame && after.oper>=frame && after.vport==frame) {
        ethernet_mtu=bytes; max_ethernet_mtu=uint16_t(after.maximum-22);
        frame_admin_mtu=after.admin; frame_oper_mtu=after.oper; vport_frame_mtu=after.vport;
        return true;
    }
    // Restore both settings after a rejected command or inconsistent readback.
    // An uncertain firmware timeout retains the transport's quarantine.
    MtuState restored{};
    if (!transport.quarantined &&
        (!write_port_mtu(before.admin) || !write_vport_mtu(before.vport) ||
         !query_mtu(restored) || restored.admin!=before.admin || restored.vport!=before.vport))
        transport.quarantined=true;
    return false;
}
bool Hca::source_gid(bool enable) {
    if (!transport.ready() || !transport.initialized) return false;
    header(0x761); input_[11]=1;
    if (enable) {
        memcpy(input_+16,gid,16); memcpy(input_+34,mac,6); input_[42]=1; input_[43]=2;
    }
    return call(48);
}
bool Hca::port_active(bool &active) {
    active=false;
    if (!transport.ready() || !transport.initialized) return false;
    header(0x805,1); cx5::write_be32(input_+8,0x5006); input_[17]=1;
    if (!call(32,32)) return false;
    active=output_[19]==1;
    if (!simple(0x750)) { active=false; return false; }
    // QUERY_VPORT_STATE packs admin_state and state into separate nibbles.
    // The known-good hardware readback is 0x11, not a scalar state of 2.
    active=active && (output_[15]&0xf)==1 && (output_[15]>>4)==1;
    return true;
}
bool Hca::query_pcie_counters(PcieCounters &counters) {
    counters=PcieCounters{};
    if (!transport.ready() || !transport.initialized) return false;
    // MPCNT register data: 8 bytes selecting PCIe index 0 and group 0 without
    // clearing, then 248 bytes of big-endian 32-bit counters.
    header(0x805,1); cx5::write_be32(input_+8,0x9051);
    if (!call(272,272)) return false;
    const uint8_t *set=output_+24;
    counters.rx_errors=cx5::read_be32(set+8); counters.tx_errors=cx5::read_be32(set+12);
    counters.crc_error_dllp=cx5::read_be32(set+32); counters.crc_error_tlp=cx5::read_be32(set+36);
    counters.stalled_reads=cx5::read_be32(set+48); counters.stalled_writes=cx5::read_be32(set+52);
    counters.stalled_reads_events=cx5::read_be32(set+56); counters.stalled_writes_events=cx5::read_be32(set+60);
    return true;
}
bool Hca::alloc_pd(HardwareObject &pd) { header(0x800); return create(pd,16); }
bool Hca::dealloc_pd(HardwareObject &pd) { return destroy(pd,0x801); }
bool Hca::register_mr(HardwareObject &mr,uint32_t pd,uint64_t address,uint64_t length,
                      const uint64_t *pages,size_t count,uint32_t access,uint32_t &key,unsigned log_page) {
    if (mr.live) return false;
    if (++next_key_==0) ++next_key_;
    bool relaxed=relaxed_ordering_requested && !relaxed_ordering_refused;
    size_t size=cx5::create_user_mkey(input_,sizeof(input_),pd,next_key_,address,length,pages,count,access,log_page,relaxed);
    if (!size) return false;
    if (!create(mr,size)) {
        // A clean firmware refusal of the relaxed-ordering bits (not a transport
        // fault) falls back to strict ordering for this and later keys.
        if (!relaxed || transport.quarantined || !transport.last.completed || !transport.last.firmware_status) return false;
        relaxed_ordering_refused=true; relaxed=false;
        size=cx5::create_user_mkey(input_,sizeof(input_),pd,next_key_,address,length,pages,count,access,log_page,false);
        if (!size || !create(mr,size)) return false;
    }
    if (relaxed) ++relaxed_ordering_keys;
    key=(mr.id<<8)|next_key_; return true;
}
bool Hca::deregister_mr(HardwareObject &mr,uint32_t key) {
    if (!mr.live) return true;
    if ((key>>8)!=mr.id || (!transport.detached() && mr_in_flight(key))) return false;
    return destroy(mr,0x202);
}
bool Hca::create_cq(HardwareCQ &cq) {
    if (cq.object.live || cq.buffer.memory || !eq_.live) return false;
    if (cq.buffer.allocate(transport.mapper(),8192)) return false;
    for (unsigned i=0;i<32;++i) cq.buffer.cpu[i*64+63]=0xf1;
    header(0x400);
    cx5::set_bits(input_+16,64,0x63,5,5); cx5::set_bits(input_+16,64,0x68,24,uar_page_index(uar_));
    cx5::set_bits(input_+16,64,0xa0,32,eq_.id);
    cx5::set_bits(input_+16,64,0x1c0,64,cq.buffer.dma+4096);
    cx5::write_be64(input_+0x110,cq.buffer.dma);
    if (!create(cq.object,0x118)) {
        if (!transport.quarantined) cq.buffer.release();
        return false;
    }
    cq.consumer=0; mcdma_cq_set_live(cq.buffer.cpu,1); return true;
}
bool Hca::destroy_cq(HardwareCQ &cq) {
    if (cq.references || cq.outstanding || !destroy(cq.object,0x401)) return false;
    if (cq.buffer.cpu) mcdma_cq_set_live(cq.buffer.cpu,0);
    return cq.buffer.release()==kIOReturnSuccess;
}
bool Hca::create_qp(HardwareQP &qp,uint32_t pd,HardwareCQ &send,HardwareCQ &recv,uint32_t uar_page) {
    if (qp_index_.size()==qp_index_.limit || qp.object.live || qp.buffer.memory || !send.object.live || !recv.object.live || pd>0xffffff) return false;
    const bool user=uar_page!=kernel_uar;
    if (user && (!user_queues || uar_page>0xffffff || uar_page==uar_page_index(uar_))) return false;
    if (qp.buffer.allocate(transport.mapper(),8192)) return false;
    header(0x500); auto *q=input_+24;
    cx5::set_bits(q,232,0x13,2,3); cx5::set_bits(q,232,0x28,24,pd);
    cx5::set_bits(q,232,0x43,5,30); cx5::set_bits(q,232,0x49,4,5); cx5::set_bits(q,232,0x51,4,5);
    cx5::set_bits(q,232,0x68,24,user ? uar_page : uar_page_index(uar_));
    cx5::set_bits(q,232,0x3e8,24,send.object.id); cx5::set_bits(q,232,0x4e8,24,recv.object.id);
    cx5::set_bits(q,232,0x500,64,qp.buffer.dma+4096);
    cx5::write_be64(input_+0x110,qp.buffer.dma);
    if (!create(qp.object,0x118)) {
        if (!transport.quarantined) qp.buffer.release();
        return false;
    }
    qp.pd=pd; qp.send_cq=&send; qp.recv_cq=&recv;
    ++send.references; ++recv.references;
    qp.user_posted=user;
    if (user) { send.user_mode=true; recv.user_mode=true; }
    qp.next=qps_; qps_=&qp;
    if(!qp_index_.insert(qp.object.id,&qp)) { transport.quarantined=true; return false; }
    return true;
}
bool Hca::reset_qp(HardwareQP &qp) {
    const bool was_live=qp.object.live;
    const bool removed=transport.detached();
    if (was_live && !removed && !qp.user_posted && (qp.send_cq->outstanding<qp.sends.pending() ||
        qp.recv_cq->outstanding<qp.receives.pending() ||
        (qp.send_cq==qp.recv_cq && qp.send_cq->outstanding<qp.sends.pending()+qp.receives.pending()))) {
        transport.quarantined=true; return false;
    }
    if (was_live && qp.state && !removed) {
        header(0x50a); cx5::write_be32(input_+8,qp.object.id);
        if (!call()) return false;
        qp.state=0;
    }
    if (was_live) {
        // A successful firmware RESET stops this QP before memory is released.
        // Remove its CQEs without discarding other QPs sharing either CQ.
        if (!removed &&
            (!purge_qp_completions(*qp.send_cq,qp.object.id) ||
             (qp.recv_cq!=qp.send_cq && !purge_qp_completions(*qp.recv_cq,qp.object.id)))) return false;
        if(!removed) {
            qp.send_cq->outstanding-=qp.sends.pending();
            qp.recv_cq->outstanding-=qp.receives.pending();
        }
        qp.sends.removed(); qp.receives.removed();
        if(removed) {
            // Recover software accounting from retained queues, not from a
            // corrupt shared total. This does not establish DMA-stop proof.
            qp.state=0;
            rebuild_cq_accounting(*qp.send_cq);
            if(qp.recv_cq!=qp.send_cq) rebuild_cq_accounting(*qp.recv_cq);
        }
        qp.producer=qp.recv_producer=0;
        memset(qp.buffer.cpu,0,8192);
        publish_dma();
    }
    return true;
}
bool Hca::destroy_qp(HardwareQP &qp) {
    const bool was_live=qp.object.live;
    const uint32_t qpn=qp.object.id;
    if (!reset_qp(qp)) return false;
    if (!destroy(qp.object,0x501)) return false;
    if (was_live) {
        HardwareQP **link=&qps_;
        while (*link && *link!=&qp) link=&(*link)->next;
        if (*link) *link=qp.next;
        qp_index_.erase(qpn,&qp);
        rebuild_cq_accounting(*qp.send_cq);
        if(qp.recv_cq!=qp.send_cq) rebuild_cq_accounting(*qp.recv_cq);
    }
    if (qp.buffer.release()) return false;
    qp={}; return true;
}
void Hca::rebuild_cq_accounting(HardwareCQ &cq) {
    cq.references=cq.outstanding=0;
    for(auto *qp=qps_;qp;qp=qp->next) {
        if(!qp->object.live) continue;
        if(qp->send_cq==&cq) {++cq.references;cq.outstanding+=qp->sends.pending();}
        if(qp->recv_cq==&cq) {++cq.references;cq.outstanding+=qp->receives.pending();}
    }
}
bool Hca::purge_qp_completions(HardwareCQ &cq,uint32_t qpn) {
    if (!cq.object.live || !transport.ready()) return false;
    unsigned visible=0;
    // Freeze the current published prefix; the hardware cannot reuse these
    // slots until we advance the CQ consumer doorbell below.
    for (;visible<32;++visible) {
        const uint32_t index=cq.consumer+visible;
        auto *entry=cq.buffer.cpu+(index&31)*64;
        const uint8_t owner=static_cast<volatile uint8_t *>(entry)[63];
        if ((owner>>4)==15 || (owner&1)!=((index>>5)&1)) break;
        acquire_dma(); cx5::Completion decoded{};
        if (cx5::decode_cqe(entry,index,5,decoded)==cx5::CQResult::unsupported) {
            transport.quarantined=true; return false;
        }
    }
    // User-posted QPs deliver completions the kernel never counted.
    if (!cq.user_mode && visible>cq.outstanding) { transport.quarantined=true; return false; }
    unsigned discarded=0;
    for (unsigned remaining=visible;remaining;--remaining) {
        const uint32_t index=cq.consumer+remaining-1;
        auto *entry=cq.buffer.cpu+(index&31)*64;
        if ((cx5::read_be32(entry+56)&0xffffff)==qpn) { ++discarded; continue; }
        if (discarded) {
            const uint32_t destination=index+discarded;
            auto *moved=cq.buffer.cpu+(destination&31)*64;
            memcpy(moved,entry,64);
            moved[63]=uint8_t((moved[63]&0xfe)|((destination>>5)&1));
        }
    }
    if (discarded) {
        cq.consumer+=discarded;
        publish_dma(); mcdma_cq_set_consumer(cq.buffer.cpu,cq.consumer); publish_dma();
    }
    return true;
}
bool Hca::transition(HardwareQP &qp,uint16_t opcode,const cx5::RCConnection &requested) {
    cx5::RCConnection connection=requested;
    connection.log_ack_req_freq=ack_request_every_packet ? 0 : 8;
    if (!qp.object.live || connection.qpn!=qp.object.id || connection.pd!=qp.pd ||
        connection.cq!=qp.send_cq->object.id || connection.doorbell!=qp.buffer.dma+4096 ||
        opcode!=0x502+qp.state || !cx5::encode_rc_transition(input_,sizeof(input_),opcode,connection)) return false;
    cx5::set_bits(input_+24,232,0x4e8,24,qp.recv_cq->object.id);
    if (!call(272)) return false;
    ++qp.state; return true;
}
bool Hca::post(HardwareQP &qp,uint64_t work_id,uint8_t opcode,uint64_t local,uint32_t lkey,uint32_t length,
               uint64_t remote,uint32_t rkey) {
    cx5::SendRequest request; request.opcode=opcode; request.remote=remote; request.rkey=rkey;
    request.sge[0]={local,lkey,length}; request.sge_count=1;
    return post(qp,work_id,request);
}
bool Hca::post(HardwareQP &qp,uint64_t work_id,const cx5::SendRequest &request) {
    if (qp.user_posted || !qp.object.live || qp.state!=3 || !transport.ready() || request.inline_data ||
        !qp.sends.can_post(qp.producer) || qp.send_cq->outstanding>=31) return false;
    const uint32_t length=cx5::request_bytes(request);
    if (!length) return false;
    auto *wqe=qp.buffer.cpu+512+(qp.producer&31)*64;
    if (!cx5::encode_send_request(wqe,64,qp.object.id,uint16_t(qp.producer),request)) return false;
    uint32_t lkeys[3]; for (unsigned i=0;i<request.sge_count;++i) lkeys[i]=request.sge[i].lkey;
    if (!qp.sends.post(qp.producer,work_id,request.opcode,length,lkeys,request.sge_count)) return false;
    ++qp.send_cq->outstanding;
    publish_dma(); cx5::write_be32(qp.buffer.cpu+4100,qp.producer+1); publish_dma();
    const uint64_t uar=uint64_t(uar_.id)<<uar_shift_;
    bool posted=false;
    if (blueflame_enabled) {
        // This register belongs to the HCA, not to a QP: alternate across all
        // serialized posts, including different QPs and QP resets.
        posted=transport.write_blueflame(uar+0x800+(blueflame_posts&1)*blueflame_buffer_bytes,wqe);
        if (posted) ++blueflame_posts;
    } else {
        uint64_t doorbell; memcpy(&doorbell,wqe,8);
        posted=transport.write64(uar+0x800,doorbell);
    }
    if (!posted) { transport.quarantined=true; return false; }
    ++qp.producer; return true;
}
bool Hca::receive(HardwareQP &qp,uint64_t work_id,uint64_t address,uint32_t length,uint32_t lkey) {
    if (qp.user_posted || !qp.object.live || qp.state<1 || !transport.ready() || !length || !lkey || address>UINT64_MAX-length ||
        !qp.receives.can_post(qp.recv_producer) || qp.recv_cq->outstanding>=31) return false;
    auto *wqe=qp.buffer.cpu+(qp.recv_producer&31)*16;
    cx5::write_be32(wqe,length); cx5::write_be32(wqe+4,lkey); cx5::write_be64(wqe+8,address);
    if (!qp.receives.post(qp.recv_producer,work_id,0x0a,length,lkey)) return false;
    ++qp.recv_cq->outstanding;
    publish_dma(); cx5::write_be32(qp.buffer.cpu+4096,qp.recv_producer+1); publish_dma();
    ++qp.recv_producer; return true;
}
cx5::CQResult Hca::poll(HardwareCQ &cq,cx5::Completion &completion,cx5::WorkRecord &work,void **client_context,
                        bool *user_posted) {
    if(client_context) *client_context=nullptr;
    if(user_posted) *user_posted=false;
    if (!cq.object.live || !transport.ready()) return cx5::CQResult::unsupported;
    auto *entry=cq.buffer.cpu+(cq.consumer&31)*64;
    const uint8_t owner=static_cast<volatile uint8_t *>(entry)[63];
    if ((owner>>4)==15 || (owner&1)!=((cq.consumer>>5)&1)) return cx5::CQResult::empty;
    acquire_dma();
    auto result=cx5::decode_cqe(entry,cq.consumer,5,completion);
    if (result!=cx5::CQResult::empty && result!=cx5::CQResult::unsupported) {
        auto *qp=qp_index_.find(completion.qpn);
        const bool send=completion.opcode==0 || completion.opcode==13;
        if (!qp || (send ? qp->send_cq : qp->recv_cq)!=&cq) {
            transport.quarantined=true; return cx5::CQResult::unsupported;
        }
        if (qp->user_posted) {
            // No kernel record exists: the hardware counter is the identity
            // and userspace, which wrote the WQE, maps it to its own request.
            work={}; work.id=completion.wqe_counter; work.counter=completion.wqe_counter;
            work.length=completion.bytes; work.occupied=false;
            if(user_posted) *user_posted=true;
        } else if (!cq.outstanding || !(send ? qp->sends : qp->receives).complete(completion.wqe_counter,work)) {
            transport.quarantined=true; return cx5::CQResult::unsupported;
        } else --cq.outstanding;
        if(client_context) *client_context=qp->client_context;
        ++cq.consumer; mcdma_cq_set_consumer(cq.buffer.cpu,cq.consumer); publish_dma();
    }
    return result;
}
bool Hca::mr_in_flight(uint32_t key) const {
    for (auto *qp=qps_;qp;qp=qp->next)
        if (qp->sends.references(key) || qp->receives.references(key)) return true;
    return false;
}
void *Hca::qp_context(uint32_t qpn) const {
    auto *qp=qp_index_.find(qpn);
    return qp && qp->object.live ? qp->client_context : nullptr;
}
}
