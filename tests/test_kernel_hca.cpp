// Exercises the actual kernel_hca.cpp with a deterministic fake transport.
// This is not a hardware, Apple ABI, or native discovery test.
#include "kernel_hca.hpp"
#include "apple_data_verbs.hpp"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <set>
#include <vector>
#include <sys/errno.h>

#include "fake_kernel_transport.hpp"
using namespace cx5_test;

namespace {
using namespace cx5_native;
void completion(HardwareCQ &cq,HardwareQP &qp,uint16_t counter,uint8_t opcode=0,uint8_t syndrome=0,unsigned ahead=0) {
    auto *entry=cq.buffer.cpu+((cq.consumer+ahead)&31)*64;
    memset(entry,0,64); cx5::write_be32(entry+56,qp.object.id);
    entry[60]=uint8_t(counter>>8); entry[61]=uint8_t(counter);
    entry[55]=syndrome; entry[63]=uint8_t((opcode<<4)|(((cq.consumer+ahead)>>5)&1));
}
void reset() { assert(!sim.buffers); sim=Simulation{}; }
void connect(Hca &hca,HardwareQP &qp) {
    cx5::RCConnection c{qp.object.id,qp.pd,qp.send_cq->object.id,17,0x123456,0x654321,
        qp.buffer.dma+4096,{}, {}};
    c.remote_gid[0]=0xfe; c.remote_gid[1]=0x80; c.remote_mac[0]=2;
    for (unsigned op=0x502;op<=0x504;++op) assert(hca.transition(qp,uint16_t(op),c));
}
void driver_startup() {
    reset(); Hca hca;
    // Same composition called by the real IOService, with fake PCI/firmware.
    assert(hca.attach_and_start(nullptr,nullptr) && hca.startup_error==kIOReturnSuccess);
    assert(hca.transport.initialized && !sim.objects.empty());
    assert(hca.stop() && !sim.buffers);
}
void issue_9_page_query_without_firmware_error() {
    reset(); sim.command_queue_high=1; sim.command_queue_low=0x156;
    Hca boot_owned;
    assert(!boot_owned.start());
    assert(boot_owned.startup_error==kIOReturnBusy &&
           boot_owned.startup_page_phase==0 && boot_owned.startup_page_step==0 &&
           !sim.calls && !sim.buffers);
    assert(boot_owned.stop());

    // Both functions report this exact low word after the enclosure is
    // power-cycled. They must be able to reach QUERY_PAGES on first attach.
    reset(); sim.command_queue_low=0x80000156;
    Hca cold_boot;
    assert(cold_boot.start());
    assert(cold_boot.transport.last.opcode!=0 && sim.calls>0);
    assert(cold_boot.stop() && !sim.buffers);

    // An apparent host address remains protected unless the entire observed
    // disabled-mode signature matches. A high word always keeps it busy.
    for (const auto low : {0x80000056u,0x80001156u,0x80000157u}) {
        reset(); sim.command_queue_low=low;
        Hca occupied;
        assert(!occupied.start() && occupied.startup_error==kIOReturnBusy && !sim.calls);
        assert(occupied.stop() && !sim.buffers);
    }
    reset(); sim.command_queue_high=1; sim.command_queue_low=0x80000156;
    Hca high_address;
    assert(!high_address.start() && high_address.startup_error==kIOReturnBusy && !sim.calls);
    assert(high_address.stop() && !sim.buffers);

    // QUERY_PAGES succeeded, then a local validation or allocation failed.
    // The issue's last-opcode/firmware fields alone cannot distinguish them.
    reset(); sim.query_pages_count=16385;
    Hca invalid_count;
    assert(!invalid_count.start());
    assert(invalid_count.transport.last.opcode==0x107 &&
           invalid_count.transport.last.firmware_status==0);
    assert(invalid_count.startup_error==kIOReturnBadArgument);
    assert(invalid_count.startup_page_phase==1 &&
           invalid_count.startup_page_step==2 &&
           invalid_count.startup_page_count==16385);
    assert(invalid_count.stop() && !sim.buffers);

    reset(); sim.query_pages_count=-1;
    Hca negative_count;
    assert(!negative_count.start());
    assert(negative_count.startup_error==kIOReturnBadArgument &&
           negative_count.startup_page_step==2 && negative_count.startup_page_count==-1);
    assert(negative_count.stop() && !sim.buffers);

    reset(); sim.fail_buffer_allocate_bytes=8192;
    Hca no_memory;
    assert(!no_memory.start());
    assert(no_memory.transport.last.opcode==0x107 &&
           no_memory.transport.last.firmware_status==0);
    assert(no_memory.startup_error==kIOReturnNoMemory);
    assert(no_memory.startup_page_phase==1 &&
           no_memory.startup_page_step==3 &&
           no_memory.startup_page_count==2);
    assert(no_memory.stop() && !sim.buffers);
}
void issue_9_large_initial_page_request() {
    reset(); sim.query_initial_pages_count=8828;
    Hca hca;
    assert(hca.start());
    assert(hca.startup_error==kIOReturnSuccess && hca.startup_page_count==8828);
    assert(sim.pages.size()==8830);
    assert(hca.stop() && sim.pages.empty() && !sim.buffers);

    reset(); sim.query_initial_pages_count=8828;
    sim.fail_buffer_allocate_bytes=uint64_t(8828)*4096;
    Hca allocation_failure;
    assert(!allocation_failure.start());
    assert(allocation_failure.startup_error==kIOReturnNoMemory &&
           allocation_failure.startup_page_phase==2 &&
           allocation_failure.startup_page_step==3 &&
           allocation_failure.startup_page_count==8828);
    assert(allocation_failure.stop() && sim.pages.empty() && !sim.buffers);

    reset(); sim.query_initial_pages_count=16385;
    Hca over_limit;
    assert(!over_limit.start());
    assert(over_limit.startup_error==kIOReturnBadArgument &&
           over_limit.startup_page_phase==2 && over_limit.startup_page_step==2 &&
           over_limit.startup_page_count==16385);
    assert(over_limit.stop() && sim.pages.empty() && !sim.buffers);
}
void transport_access_guards() {
    reset(); Transport transport;
    assert(!transport.ready() && !transport.write64(0x4800,1) && transport.detached());
    assert(!transport.open() && transport.ready());
    assert(!transport.write64(0x4800,1));
    assert(!transport.configure_doorbell(4096));
    assert(transport.configure_doorbell(16384));
    assert(!transport.configure_blueflame(16384,256));
    const unsigned reads=sim.pci.config_reads;
    assert(transport.write64(0x4800,0x123456789abcdef0ull));
    assert(sim.uar.words[0x800/8]==0x123456789abcdef0ull && sim.doorbells==1);
    assert(!transport.write64(1,1) && !transport.write64(0x4900,1));
    const auto length=sim.uar.length; sim.uar.length=4;
    assert(!transport.write64(0x4800,1)); sim.uar.length=length;
    transport.quarantined=true;
    assert(!transport.ready() && !transport.write64(0x4800,1));
    transport.quarantined=false;
    sim.pci.logically_inactive=true;
    assert(!transport.ready() && !transport.write64(0x4800,1));
    assert(!transport.detached()); // Logical service termination cannot prove DMA stopped.
    sim.pci.logically_inactive=false;
    sim.removed=true;
    assert(!transport.ready() && !transport.write64(0x4800,1) && transport.detached());
    assert(sim.doorbells==1 && sim.pci.config_reads==reads+2);
    sim.removed=false; sim.pci.vendor=0xffff;
    assert(transport.detached() && sim.pci.config_reads==reads+3);
    sim.pci.vendor=0x15b3;
    assert(!transport.detached() && sim.pci.config_reads==reads+4);
    assert(!transport.close() && !transport.ready() && !transport.write64(0x4800,1));
}
void blueflame_full_wqe_and_shared_alternation() {
    reset(); Hca hca; assert(hca.start());
    HardwareObject pd; HardwareCQ cq; HardwareQP a,b;
    assert(hca.alloc_pd(pd) && hca.create_cq(cq));
    assert(hca.create_qp(a,pd.id,cq,cq) && hca.create_qp(b,pd.id,cq,cq));
    connect(hca,a); connect(hca,b);
    // One UAR is shared by both QPs: two first posts must use opposite banks.
    assert(hca.post(a,71,0x08,0x3000,7,4096,0x9000,9));
    assert(hca.post(b,72,0x10,0x5000,8,1024,0xa000,10));
    const auto *bar=reinterpret_cast<const uint8_t *>(sim.uar.words.data());
    assert(!memcmp(bar+0x800,a.buffer.cpu+512,64));
    assert(!memcmp(bar+0x900,b.buffer.cpu+512,64));
    for (unsigned i=64;i<128;++i) assert(!bar[0x800+i] && !bar[0x900+i]);
    completion(cq,a,0); completion(cq,b,0,0,0,1);
    cx5::Completion c; cx5::WorkRecord w;
    assert(hca.poll(cq,c,w)==cx5::CQResult::success && w.id==71);
    assert(hca.poll(cq,c,w)==cx5::CQResult::success && w.id==72);
    assert(hca.reset_qp(a)); connect(hca,a);
    assert(hca.post(a,73,0x08,0x7000,7,4096,0xb000,9));
    assert(hca.blueflame_posts==3 && !memcmp(bar+0x800,a.buffer.cpu+512,64));
    assert(!memcmp(bar+0x900,b.buffer.cpu+512,64));
    completion(cq,a,0); assert(hca.poll(cq,c,w)==cx5::CQResult::success && w.id==73);
    assert(hca.destroy_qp(a) && hca.destroy_qp(b) && hca.destroy_cq(cq));
    assert(hca.dealloc_pd(pd) && hca.stop() && !sim.buffers);
}
void blueflame_guards_and_fallback() {
    reset(); Transport t; uint8_t wqe[64]{};
    assert(!t.write_blueflame(0x4800,wqe)); assert(!t.open());
    assert(!t.configure_blueflame(1,256) && !t.configure_blueflame(4096,256));
    assert(!t.configure_blueflame(16384,64) && !t.configure_blueflame(16384,129));
    assert(!t.configure_blueflame(sim.bar.length,256));
    assert(t.configure_blueflame(16384,256));
    assert(!t.configure_blueflame(16384,256) && !t.configure_doorbell(16384));
    assert(!t.write64(0x4800,1)); // Never create/access a UC alias of WC UAR.
    assert(!t.write_blueflame(0x4808,wqe) && !t.write_blueflame(0x4a00,wqe));
    assert(!t.write_blueflame(0x4800,nullptr));
    const auto length=sim.uar.length; sim.uar.length=0x87f;
    assert(!t.write_blueflame(0x4800,wqe)); sim.uar.length=length;
    t.quarantined=true; assert(!t.write_blueflame(0x4800,wqe)); t.quarantined=false;
    sim.removed=true; assert(!t.write_blueflame(0x4800,wqe));sim.removed=false;
    assert(!t.close() && !t.write_blueflame(0x4800,wqe));
    for(unsigned mode=0;mode<4;++mode) {
        reset(); sim.bf_capable=mode!=0; sim.bf_log=mode==1 ? 7 : mode==2 ? 12 : 9;
        sim.bf_map_fail=mode==3;
        Hca hca; assert(hca.start() && !hca.blueflame_enabled);
        HardwareObject pd; HardwareCQ cq; HardwareQP qp;
        assert(hca.alloc_pd(pd) && hca.create_cq(cq) && hca.create_qp(qp,pd.id,cq,cq));connect(hca,qp);
        for(unsigned n=0;n<3;++n) {
            assert(hca.post(qp,80+n,0x08,0x3000+4096*n,7,4096,0x9000,9));
            const auto *bar=reinterpret_cast<const uint8_t *>(sim.uar.words.data());
            assert(!memcmp(bar+0x800,qp.buffer.cpu+512+n*64,8));
            for(unsigned i=8;i<128;++i) assert(bar[0x800+i]==0);
            completion(cq,qp,n); cx5::Completion c;cx5::WorkRecord w;
            assert(hca.poll(cq,c,w)==cx5::CQResult::success && w.id==80+n);
        }
        assert(hca.destroy_qp(qp) && hca.destroy_cq(cq) && hca.dealloc_pd(pd) && hca.stop());
    }
    // Holding all UARs from page0 prevents allocator reuse; failure at each
    // allocation and after mapping must reclaim every owned UAR and DMA page.
    for(unsigned failure=0;failure<7;++failure) {
        reset(); sim.next_id=0;
        if (failure<5) sim.fail_uar_allocation=failure+1;
        if (failure==5) sim.bf_map_fail=sim.uc_map_fail=true;
        if (failure==6) sim.fail_opcode=0x301;
        Hca hca; assert(!hca.start());
        sim.fail_opcode=0; assert(hca.stop() && !sim.buffers && sim.objects.empty());
    }
    reset(); sim.next_id=0; Hca hca;
    assert(hca.start() && hca.blueflame_enabled && sim.uar_allocations==5);
    assert(hca.stop() && sim.objects.empty());
}
void mtu_configuration() {
    reset(); Hca hca; assert(hca.start());
    assert(hca.ethernet_mtu==1500 && Hca::roce_mtu(hca.ethernet_mtu)==3);
    assert(Hca::roce_mtu(0)==0 && Hca::roce_mtu(4191)==4 && Hca::roce_mtu(4192)==5);
    unsigned before=sim.calls;
    assert(!hca.configure_ethernet_mtu(9001) && !hca.configure_ethernet_mtu(1279));
    assert(sim.calls==before);
    sim.max_frame=9000; assert(!hca.configure_ethernet_mtu(9000));
    assert(sim.admin_frame==1522 && sim.vport_frame==1522 && hca.ethernet_mtu==1500);
    sim.max_frame=10000;
    sim.reject_jumbo_vport_once=true;
    assert(!hca.configure_ethernet_mtu(9000) && !hca.transport.quarantined);
    assert(sim.admin_frame==1522 && sim.vport_frame==1522 && hca.ethernet_mtu==1500);
    sim.ignore_jumbo_port_once=true;
    assert(!hca.configure_ethernet_mtu(9000) && !hca.transport.quarantined);
    assert(sim.admin_frame==1522 && sim.vport_frame==1522 && hca.ethernet_mtu==1500);
    assert(hca.configure_ethernet_mtu(9000));
    assert(hca.ethernet_mtu==9000 && hca.max_ethernet_mtu==9978);
    assert(hca.frame_admin_mtu==9022 && hca.frame_oper_mtu==9022 && hca.vport_frame_mtu==9022);
    assert(Hca::roce_mtu(hca.ethernet_mtu)==5);
    HardwareObject pd; HardwareCQ cq; HardwareQP qp;
    assert(hca.alloc_pd(pd) && hca.create_cq(cq) && hca.create_qp(qp,pd.id,cq,cq));
    before=sim.calls; assert(!hca.configure_ethernet_mtu(1500) && sim.calls==before);
    assert(hca.destroy_qp(qp) && hca.destroy_cq(cq) && hca.dealloc_pd(pd));
    assert(hca.configure_ethernet_mtu(1500) && Hca::roce_mtu(hca.ethernet_mtu)==3);
    assert(hca.stop() && !sim.buffers);
}
void pcie_counters() {
    reset(); Hca hca; Hca::PcieCounters counters;
    assert(!hca.query_pcie_counters(counters) && !sim.mpcnt_queries);
    assert(hca.start());
    for (unsigned i=0;i<16;++i) sim.mpcnt[i]=0x01000000u+i;
    assert(hca.query_pcie_counters(counters) && sim.mpcnt_queries==1);
    assert(counters.rx_errors==0x01000002u && counters.tx_errors==0x01000003u);
    assert(counters.crc_error_dllp==0x01000008u && counters.crc_error_tlp==0x01000009u);
    assert(counters.stalled_reads==0x0100000cu && counters.stalled_writes==0x0100000du);
    assert(counters.stalled_reads_events==0x0100000eu && counters.stalled_writes_events==0x0100000fu);
    // Firmware without the register: the query fails cleanly and the command
    // path stays usable for the port sampler.
    sim.mpcnt_supported=false;
    assert(!hca.query_pcie_counters(counters) && sim.mpcnt_queries==2 && !counters.stalled_reads);
    bool active=false;
    assert(hca.port_active(active) && active);
    assert(hca.stop() && !sim.buffers);
}
void lifecycle() {
    reset(); Hca hca;
    assert(hca.start() && sim.pages.size()==4 && sim.buffers==3);
    bool active=false;
    assert(hca.port_active(active) && active);
    for (uint8_t state: {uint8_t(0),uint8_t(1),uint8_t(2),uint8_t(0x10),uint8_t(0x12)}) {
        sim.vport=state; assert(hca.port_active(active) && !active);
    }
    sim.vport=0x11;
    HardwareObject pd,mr; HardwareCQ cq; HardwareQP qp;
    assert(hca.alloc_pd(pd) && hca.create_cq(cq) && hca.create_qp(qp,pd.id,cq,cq));
    uint64_t pages[2]={0x800000,0x900000}; uint32_t key=0;
    assert(hca.register_mr(mr,pd.id,0x12345001,8191,pages,2,7,key));
    assert(!hca.stop() && !hca.destroy_cq(cq)); connect(hca,qp);
    const auto config_before_data=sim.pci.config_reads;
    for (unsigned i=0;i<31;++i) assert(hca.post(qp,0x100000000ull+i,0x08,0x12345001,key,4096,0x55550000,55));
    assert(sim.doorbells==31 && !hca.post(qp,9,0x08,0x12345001,key,4096,0x55550000,55));
    assert(sim.pci.config_reads==config_before_data);
    assert(hca.mr_in_flight(key) && !hca.deregister_mr(mr,key));
    const auto config_before_poll=sim.pci.config_reads;
    for (unsigned i=0;i<31;++i) {
        completion(cq,qp,uint16_t(i)); cx5::Completion c; cx5::WorkRecord work;
        assert(hca.poll(cq,c,work)==cx5::CQResult::success && work.id==0x100000000ull+i);
    }
    // Posting and reading DMA completion memory must not synchronously query PCI.
    assert(sim.pci.config_reads==config_before_poll);
    assert(!hca.mr_in_flight(key) && !cq.outstanding);
    // Error CQEs must preserve the originating work ID and hardware syndrome.
    assert(hca.post(qp,987654321,0x10,0x12345001,key,4096,0x55550000,55));
    completion(cq,qp,31,13,0x13); cx5::Completion c; cx5::WorkRecord work;
    assert(hca.poll(cq,c,work)==cx5::CQResult::error && c.syndrome==0x13 && work.id==987654321);
    assert(hca.receive(qp,99,0x12345001,4096,key));
    completion(cq,qp,0,2); assert(hca.poll(cq,c,work)==cx5::CQResult::success && work.id==99);
    assert(hca.deregister_mr(mr,key)); assert(hca.destroy_qp(qp)); assert(hca.destroy_cq(cq));
    assert(hca.dealloc_pd(pd)); assert(hca.stop());
    assert(!sim.buffers && sim.objects.empty() && sim.pages.empty());
}
void failed_create(bool timeout) {
    reset(); Hca hca; assert(hca.start()); HardwareCQ cq;
    sim.fail_opcode=0x400; sim.timeout=timeout;
    assert(!hca.create_cq(cq));
    if (!timeout) { assert(!cq.buffer.memory && sim.buffers==3); sim.fail_opcode=0; assert(hca.stop()); }
    else {
        assert(cq.buffer.memory && sim.buffers==4 && !hca.stop());
        sim.removed=true; assert(hca.destroy_cq(cq)); assert(hca.stop());
    }
    assert(!sim.buffers);
}
void corrupt_completion() {
    reset(); Hca hca; assert(hca.start()); HardwareObject pd; HardwareCQ cq; HardwareQP qp;
    assert(hca.alloc_pd(pd) && hca.create_cq(cq) && hca.create_qp(qp,pd.id,cq,cq)); connect(hca,qp);
    assert(hca.post(qp,1,0x08,0x10000000,7,4096,0x20000000,8));
    completion(cq,qp,99); cx5::Completion c; cx5::WorkRecord work;
    assert(hca.poll(cq,c,work)==cx5::CQResult::unsupported && hca.transport.quarantined);
    assert(qp.sends.pending()==1 && cq.outstanding==1 && !hca.destroy_qp(qp));
    sim.removed=true; assert(hca.destroy_qp(qp)); assert(!cq.outstanding);
    assert(hca.destroy_cq(cq) && hca.dealloc_pd(pd) && hca.stop()); assert(!sim.buffers);
}
void corrupt_page_return() {
    reset(); Hca hca; assert(hca.start()); sim.bad_reclaim=true;
    assert(!hca.stop() && hca.transport.quarantined && sim.buffers==3);
    sim.removed=true; assert(hca.stop() && !sim.buffers);
}
void native_data_callbacks() {
    reset(); Hca hca; assert(hca.start()); HardwareObject pd; HardwareCQ cq; HardwareQP a,b;
    assert(hca.alloc_pd(pd) && hca.create_cq(cq));
    assert(hca.create_qp(a,pd.id,cq,cq) && hca.create_qp(b,pd.id,cq,cq));
    connect(hca,a); connect(hca,b);
    int native_a=0,native_b=0; a.client_context=&native_a; b.client_context=&native_b;
    AppleSGE sge{0x10000000,64,7};
    AppleRDMAWR write{{nullptr,0x100000001,&sge,1,0,2,0},0x20000000,8,0};
    AppleRDMAWR read=write; read.base.id=0x100000002; read.base.opcode=4;
    const AppleSendWR *bad=nullptr;
    assert(!apple_post_send(hca,a,false,&write.base,&bad) && !bad);
    assert(!apple_post_send(hca,b,false,&read.base,&bad) && !bad);
    completion(cq,b,0); completion(cq,a,0,0,0,1);
    AppleWC wc[4]; memset(wc,0xa5,sizeof(wc));
    assert(apple_poll_cq(hca,cq,4,wc)==2);
    assert(wc[0].id==read.base.id && wc[0].opcode==2 && wc[0].bytes==64 && wc[0].qp==&native_b);
    assert(wc[1].id==write.base.id && wc[1].opcode==1 && wc[1].qp==&native_a);
    assert(!wc[0].status && !wc[1].status && wc[0].port==1 && wc[1].port==1);
    for (auto byte:wc[0].remaining) assert(!byte);
    for (auto byte:wc[1].remaining) assert(!byte);
    const auto *untouched=reinterpret_cast<const uint8_t *>(&wc[2]);
    for (size_t i=0;i<2*sizeof(AppleWC);++i) assert(untouched[i]==0xa5);
    assert(!apple_poll_cq(hca,cq,0,nullptr));
    assert(apple_poll_cq(hca,cq,1,nullptr)==-EINVAL && apple_poll_cq(hca,cq,-1,wc)==-EINVAL);

    // Only the valid prefix is posted; bad_wr names the rejected inline WR.
    AppleSendWR invalid{nullptr,4,&sge,1,2,10,0};
    AppleSendWR send{&invalid,3,&sge,1,2,2,0};
    assert(apple_post_send(hca,a,false,&send,&bad)==-EOPNOTSUPP && bad==&invalid);
    assert(a.sends.pending()==1);
    completion(cq,a,1); assert(apple_poll_cq(hca,cq,1,wc)==1 && wc[0].id==3 && wc[0].opcode==0);
    send.next=nullptr; send.flags=0;
    assert(apple_post_send(hca,a,false,&send,&bad)==-EOPNOTSUPP && bad==&send);
    assert(!apple_post_send(hca,a,true,&send,&bad));
    completion(cq,a,2,13,0x13);
    assert(apple_poll_cq(hca,cq,1,wc)==1 && wc[0].status==10 && wc[0].vendor==0x13 && wc[0].id==3);
    // Scatter lists: up to three entries for SEND and two for RDMA, every
    // entry checked; a WRITE with immediate carries the raw bits and fence.
    AppleSGE list[4]={{0x10000000,16,7},{0x10001000,16,7},{0x10002000,16,9},{0x10003000,16,7}};
    send.flags=2; send.sge=list; send.sge_count=4;
    assert(apple_post_send(hca,a,false,&send,&bad)==-EINVAL && !a.sends.pending());
    send.sge_count=3;
    assert(!apple_post_send(hca,a,false,&send,&bad) && a.sends.pending()==1 && a.sends.references(9));
    {
        const uint8_t *wqe=a.buffer.cpu+512+((a.producer-1)&31)*64;
        assert(wqe[3]==0x0a && (cx5::read_be32(wqe+4)&0xff)==4 && cx5::read_be32(wqe+48)==16 && cx5::read_be32(wqe+52)==9);
    }
    completion(cq,a,uint16_t(a.producer-1)); assert(apple_poll_cq(hca,cq,1,wc)==1 && wc[0].id==3 && wc[0].opcode==0);
    assert(!a.sends.references(9));
    AppleRDMAWR write_imm=write; write_imm.base.opcode=1; write_imm.base.flags=2|1; write_imm.base.immediate=0x11223344;
    write_imm.base.sge=list; write_imm.base.sge_count=3;
    assert(apple_post_send(hca,a,false,&write_imm.base,&bad)==-EINVAL && !a.sends.pending());
    write_imm.base.sge_count=2;
    assert(!apple_post_send(hca,a,false,&write_imm.base,&bad));
    {
        const uint8_t *wqe=a.buffer.cpu+512+((a.producer-1)&31)*64; uint32_t imm=0; memcpy(&imm,wqe+12,4);
        assert(wqe[3]==0x09 && wqe[11]==0x88 && imm==0x11223344 && (cx5::read_be32(wqe+4)&0xff)==4);
        assert(cx5::read_be64(wqe+16)==0x20000000 && cx5::read_be32(wqe+32)==16 && cx5::read_be32(wqe+48)==16);
    }
    completion(cq,a,uint16_t(a.producer-1)); assert(apple_poll_cq(hca,cq,1,wc)==1 && wc[0].opcode==1 && wc[0].id==write.base.id);
    write_imm.base.flags=2|4|8; write_imm.base.sge_count=1;
    assert(apple_post_send(hca,a,false,&write_imm.base,&bad)==-EOPNOTSUPP); // Kernel posting never inlines.
    send.sge=&sge; send.sge_count=1;
    write.remote=UINT64_MAX;
    assert(apple_post_send(hca,a,false,&write.base,&bad)==-EINVAL && !a.sends.pending());
    write.remote=0x20000000;
    for (unsigned i=0;i<31;++i) assert(!apple_post_send(hca,a,false,&write.base,&bad));
    assert(apple_post_send(hca,a,false,&write.base,&bad)==-ENOMEM && bad==&write.base);
    for (unsigned i=0;i<31;++i) {
        completion(cq,a,uint16_t(i+5)); assert(apple_poll_cq(hca,cq,1,wc)==1); // Two extra posts precede these.
    }
    AppleRecvWR recv{nullptr,0x200000001,&sge,1}; const AppleRecvWR *bad_recv=nullptr;
    assert(!apple_post_recv(hca,b,&recv,&bad_recv)); completion(cq,b,0,2);
    cx5::write_be32(cq.buffer.cpu+(cq.consumer&31)*64+44,32);
    assert(apple_poll_cq(hca,cq,1,wc)==1 && wc[0].id==recv.id && wc[0].opcode==128 && wc[0].bytes==32);
    assert(!apple_post_recv(hca,b,&recv,&bad_recv)); completion(cq,b,1,2);
    cx5::write_be32(cq.buffer.cpu+(cq.consumer&31)*64+44,65);
    assert(apple_poll_cq(hca,cq,1,wc)==1 && wc[0].status==1 && !wc[0].bytes);
    // Receive formats: SEND with immediate (3) and WRITE with immediate (1)
    // report the raw immediate with wc flag 2; SEND with invalidate (4) is
    // still unsupported and never reported as plain success.
    assert(!apple_post_recv(hca,b,&recv,&bad_recv)); completion(cq,b,2,3);
    memcpy(cq.buffer.cpu+(cq.consumer&31)*64+40,"\x0a\x0b\x0c\x0d",4);
    cx5::write_be32(cq.buffer.cpu+(cq.consumer&31)*64+44,16);
    assert(apple_poll_cq(hca,cq,1,wc)==1 && wc[0].status==0 && wc[0].opcode==128 && wc[0].bytes==16 && wc[0].flags==2);
    assert(!memcmp(&wc[0].immediate,"\x0a\x0b\x0c\x0d",4));
    assert(!apple_post_recv(hca,b,&recv,&bad_recv)); completion(cq,b,3,1);
    cx5::write_be32(cq.buffer.cpu+(cq.consumer&31)*64+44,4096);
    assert(apple_poll_cq(hca,cq,1,wc)==1 && wc[0].status==0 && wc[0].opcode==129 && wc[0].bytes==4096 && wc[0].flags==2);
    assert(!apple_post_recv(hca,b,&recv,&bad_recv)); completion(cq,b,4,4);
    assert(apple_poll_cq(hca,cq,1,wc)==1 && wc[0].status==21 && wc[0].vendor==4);
    assert(hca.destroy_qp(a) && hca.destroy_qp(b) && hca.destroy_cq(cq) && hca.dealloc_pd(pd) && hca.stop());
    assert(!sim.buffers);
}
void detached_credit_recovery(bool separate) {
    reset(); Hca hca; assert(hca.start()); HardwareObject pd; HardwareCQ send,recv; HardwareQP a,b;
    assert(hca.alloc_pd(pd) && hca.create_cq(send));
    HardwareCQ *receive=&send;
    if(separate) {assert(hca.create_cq(recv));receive=&recv;}
    assert(hca.create_qp(a,pd.id,send,*receive) && hca.create_qp(b,pd.id,send,*receive));
    connect(hca,a);connect(hca,b);
    assert(hca.post(a,1,0x08,0x10000000,7,64,0x20000000,8));
    assert(hca.post(b,2,0x08,0x10000000,7,64,0x20000000,8));
    assert(hca.receive(a,3,0x10000000,64,7));
    assert(hca.receive(b,4,0x10000000,64,7));
    send.outstanding=0;send.references=0;receive->outstanding=0;receive->references=0;
    // Corruption while attached must quarantine without dropping work/storage.
    const auto buffers=sim.buffers;
    assert(!hca.reset_qp(a) && hca.transport.quarantined && a.sends.pending()==1 && sim.buffers==buffers);
    sim.removed=true;const auto calls=sim.calls;
    assert(hca.destroy_qp(a));
    assert(send.references==(separate?1u:2u) && send.outstanding==(separate?1u:2u));
    assert(receive->references==(separate?1u:2u) && receive->outstanding==(separate?1u:2u));
    assert(b.sends.pending()==1 && b.receives.pending()==1);
    assert(hca.destroy_qp(b) && !send.outstanding && !send.references && !receive->outstanding && !receive->references);
    assert(hca.destroy_cq(send) && (!separate || hca.destroy_cq(recv)) && hca.dealloc_pd(pd) && hca.stop());
    assert(sim.calls==calls && !sim.buffers);
}
void tunnel_knobs() {
    // Relaxed-ordering keys: requested bits on every registration until the
    // firmware refuses once, then strict keys and a reported refusal; the
    // acknowledgement-request frequency follows the flag on every transition.
    reset(); Hca hca; hca.relaxed_ordering_requested=true; assert(hca.start());
    HardwareObject pd,mr,mr2; uint32_t key=0,key2=0; uint64_t pages[2]={0x40000000,0x40001000};
    assert(hca.alloc_pd(pd) && hca.register_mr(mr,pd.id,0x12345000,8192,pages,2,7,key));
    assert(sim.relaxed_keys==1 && sim.strict_keys==0 && hca.relaxed_ordering_keys==1 && !hca.relaxed_ordering_refused);
    sim.refuse_relaxed_ordering=true;
    assert(hca.register_mr(mr2,pd.id,0x12345000,8192,pages,2,7,key2));
    assert(sim.relaxed_keys==1 && sim.strict_keys==1 && hca.relaxed_ordering_refused && hca.relaxed_ordering_keys==1 && !hca.transport.quarantined);
    HardwareObject mr3; uint32_t key3=0; sim.refuse_relaxed_ordering=false;
    assert(hca.register_mr(mr3,pd.id,0x12345000,8192,pages,2,7,key3) && sim.strict_keys==2 && sim.relaxed_keys==1);
    assert(hca.deregister_mr(mr,key) && hca.deregister_mr(mr2,key2) && hca.deregister_mr(mr3,key3));
    HardwareCQ cq; HardwareQP qp;
    assert(hca.create_cq(cq) && hca.create_qp(qp,pd.id,cq,cq));
    cx5::RCConnection c{qp.object.id,qp.pd,cq.object.id,17,0x123456,0x654321,qp.buffer.dma+4096,{},{}};
    c.remote_gid[0]=0xfe; c.remote_gid[1]=0x80; c.remote_mac[0]=2;
    assert(hca.transition(qp,0x502,c) && cx5::get_bits(sim.command.data()+24,232,0x380,4)==8);
    hca.ack_request_every_packet=true;
    assert(hca.transition(qp,0x503,c) && cx5::get_bits(sim.command.data()+24,232,0x380,4)==0);
    hca.ack_request_every_packet=false;
    assert(hca.transition(qp,0x504,c) && cx5::get_bits(sim.command.data()+24,232,0x380,4)==8);
    char text[256];
    assert(hca.transport.configure_pcie(0,text,sizeof(text)) && strstr(text,"mrrs=512") && hca.transport.max_read_request_applied()==0);
    assert(!hca.transport.configure_pcie(192,text,sizeof(text)) && !hca.transport.configure_pcie(8192,text,sizeof(text)));
    assert(hca.transport.configure_pcie(4096,text,sizeof(text)) && hca.transport.max_read_request_applied()==4096 && sim.mrrs_requested==4096);
    assert(hca.destroy_qp(qp) && hca.destroy_cq(cq) && hca.dealloc_pd(pd) && hca.stop() && sim.objects.empty());
    reset();
    { Hca plain; assert(plain.start()); HardwareObject pd2,m; uint32_t k=0;
      assert(plain.alloc_pd(pd2) && plain.register_mr(m,pd2.id,0x12345000,8192,pages,2,7,k) && sim.relaxed_keys==0 && sim.strict_keys==1);
      assert(plain.deregister_mr(m,k) && plain.dealloc_pd(pd2) && plain.stop()); }
    puts("PASS tunnel knobs: relaxed-ordering keys with firmware-refusal fallback, per-packet ACK requests, PCIe probe and read-request size");
}
void user_queues() {
    // Negotiated 16 KiB UAR pages: SET_HCA_CAP before the init pages, then a
    // context UAR, a user-posted QP whose completions carry only the counter,
    // kernel posting refused, reset without kernel credit accounting.
    reset(); Hca hca; hca.user_queues_requested=true;
    assert(hca.start() && hca.user_queues && sim.set_caps==1 && sim.uar_page_log==2);
    HardwareObject pd,uar; HardwareCQ cq; HardwareQP kernel_qp,user_qp;
    assert(hca.alloc_pd(pd) && hca.create_cq(cq));
    // ALLOC_UAR's firmware ID goes into EQ/CQ/QP contexts unchanged. Page
    // size changes the BAR byte offset, not the allocated resource identity.
    assert(cx5::get_bits(sim.command.data()+16,64,0x68,24)==1);
    assert(hca.alloc_uar(uar) && uar.live && uar.id>=1 && hca.uar_page_index(uar)==uar.id);
    assert(hca.uar_page_offset(uar)==uint64_t(uar.id)<<14);
    assert(hca.create_qp(kernel_qp,pd.id,cq,cq) && !kernel_qp.user_posted && !cq.user_mode);
    assert(hca.create_qp(user_qp,pd.id,cq,cq,hca.uar_page_index(uar)) && user_qp.user_posted && cq.user_mode);
    assert(cx5::get_bits(sim.command.data()+24,232,0x68,24)==uar.id);
    HardwareQP rejected;
    assert(!hca.create_qp(rejected,pd.id,cq,cq,0x1000000) && !rejected.object.live);
    connect(hca,kernel_qp); connect(hca,user_qp);
    assert(!hca.post(user_qp,1,0x08,0x1000,7,64,0x2000,8) && !hca.receive(user_qp,2,0x1000,64,7));
    assert(hca.post(kernel_qp,3,0x08,0x1000,7,64,0x2000,8) && cq.outstanding==1);
    cx5::Completion c; cx5::WorkRecord w; void *context=nullptr; bool user=false;
    completion(cq,user_qp,5,2); cx5::write_be32(cq.buffer.cpu+(cq.consumer&31)*64+44,4000);
    assert(hca.poll(cq,c,w,&context,&user)==cx5::CQResult::success && user && w.id==5 && w.length==4000 && cq.outstanding==1);
    completion(cq,kernel_qp,0);
    assert(hca.poll(cq,c,w,&context,&user)==cx5::CQResult::success && !user && w.id==3 && cq.outstanding==0);
    completion(cq,user_qp,9);
    assert(hca.poll(cq,c,w,&context,&user)==cx5::CQResult::success && user && w.id==9 && w.counter==9);
    completion(cq,user_qp,10,13,0x13);
    assert(hca.poll(cq,c,w,&context,&user)==cx5::CQResult::error && user && w.id==10 && c.syndrome==0x13);
    // Visible completions of a user-posted QP are purged on reset even though
    // the kernel never counted them.
    completion(cq,user_qp,11); completion(cq,user_qp,12,0,0,1); completion(cq,kernel_qp,1,0,0,2);
    assert(hca.post(kernel_qp,4,0x08,0x1000,7,64,0x2000,8));
    assert(hca.reset_qp(user_qp) && user_qp.state==0);
    assert(hca.poll(cq,c,w,&context,&user)==cx5::CQResult::success && !user && w.id==4);
    assert(hca.poll(cq,c,w)==cx5::CQResult::empty);
    assert(hca.destroy_qp(user_qp) && hca.destroy_qp(kernel_qp) && hca.destroy_cq(cq));
    assert(!hca.stop()); // The context UAR is still allocated.
    assert(hca.dealloc_uar(uar) && !uar.live && hca.dealloc_pd(pd));
    assert(hca.stop() && !sim.buffers && sim.objects.empty());
    // Firmware refusing 16 KiB pages keeps 4 KiB pages and only disables the feature.
    reset(); sim.uar_pages_16k_supported=false;
    { Hca refused; refused.user_queues_requested=true;
      assert(refused.start() && !refused.user_queues && sim.set_caps==1);
      HardwareObject none; assert(!refused.alloc_uar(none) && !none.live);
      HardwareObject pd2; HardwareCQ cq2; HardwareQP qp2;
      assert(refused.alloc_pd(pd2) && refused.create_cq(cq2) && !refused.create_qp(qp2,pd2.id,cq2,cq2,4));
      assert(refused.destroy_cq(cq2) && refused.dealloc_pd(pd2) && refused.stop()); }
    reset();
    { Hca plain; assert(plain.start() && !plain.user_queues && sim.set_caps==0 && plain.stop()); }
    reset(); sim.uar_page_log=2;
    { Hca already_16k; already_16k.user_queues_requested=true;
      assert(already_16k.start() && already_16k.user_queues && sim.set_caps==0);
      assert(already_16k.stop() && !sim.buffers); }
    // Userspace BlueFlame needs the request, user queues and working kernel BlueFlame.
    reset();
    { Hca bf; bf.user_queues_requested=bf.user_blueflame_requested=true;
      assert(bf.start() && bf.user_queues && bf.blueflame_enabled && bf.user_blueflame && bf.stop()); }
    reset(); sim.bf_capable=false;
    { Hca nobf; nobf.user_queues_requested=nobf.user_blueflame_requested=true;
      assert(nobf.start() && nobf.user_queues && !nobf.blueflame_enabled && !nobf.user_blueflame && nobf.stop()); }
    reset(); sim.uar_pages_16k_supported=false;
    { Hca noqueues; noqueues.user_queues_requested=noqueues.user_blueflame_requested=true;
      assert(noqueues.start() && !noqueues.user_queues && noqueues.blueflame_enabled && !noqueues.user_blueflame && noqueues.stop()); }
    reset();
    { Hca unrequested; unrequested.user_blueflame_requested=true;
      assert(unrequested.start() && !unrequested.user_queues && !unrequested.user_blueflame && unrequested.stop()); }
    puts("PASS user queues: 16 KiB UAR negotiation, context UAR, user-posted QP identity, kernel post refusal, reset purge");
}
void destroy_pending_qp(uint32_t initial_consumer,bool separate_receive_cq) {
    reset(); Hca hca; assert(hca.start()); HardwareObject pd; HardwareCQ cq,receive_cq; HardwareQP a,b;
    assert(hca.alloc_pd(pd) && hca.create_cq(cq));
    auto *rcq=&cq;
    if (separate_receive_cq) { assert(hca.create_cq(receive_cq)); rcq=&receive_cq; }
    assert(hca.create_qp(a,pd.id,cq,*rcq) && hca.create_qp(b,pd.id,cq,*rcq));
    connect(hca,a); connect(hca,b); cq.consumer=initial_consumer; rcq->consumer=initial_consumer;
    int native_b=0; b.client_context=&native_b;
    for (unsigned i=0;i<3;++i) {
        assert(hca.post(a,i,0x08,0x10000000,7,64,0x20000000,8));
        assert(hca.post(b,0x100000000ull+i,0x08,0x10000000,9,64,0x20000000,8));
    }
    assert(hca.receive(a,4,0x10000000,64,7));
    completion(cq,b,0,0,0,0); completion(cq,a,0,0,0,1);
    completion(cq,b,1,0,0,2); completion(cq,a,1,0,0,3);
    completion(cq,a,2,0,0,4); completion(cq,b,2,0,0,5);
    if (separate_receive_cq) completion(*rcq,a,0,2);
    // Failed RESET must preserve all work and DMA ownership for a retry.
    sim.fail_opcode=0x50a;
    assert(!hca.destroy_qp(a) && hca.mr_in_flight(7) && cq.consumer==initial_consumer);
    sim.fail_opcode=0;
    assert(hca.destroy_qp(a) && !hca.mr_in_flight(7) && hca.mr_in_flight(9));
    assert(cq.outstanding==3 && cq.consumer==initial_consumer+3);
    AppleWC wc[4]; assert(apple_poll_cq(hca,cq,4,wc)==3);
    for (unsigned i=0;i<3;++i) assert(wc[i].id==0x100000000ull+i && wc[i].qp==&native_b && !wc[i].status);
    assert(!cq.outstanding && !rcq->outstanding);
    assert(hca.destroy_qp(b) && hca.destroy_cq(cq));
    if (separate_receive_cq) assert(hca.destroy_cq(receive_cq));
    assert(hca.dealloc_pd(pd) && hca.stop() && !sim.buffers);
}
}
int main() {
    tunnel_knobs();
    user_queues();
    detached_credit_recovery(false);detached_credit_recovery(true);
    transport_access_guards();
    blueflame_full_wqe_and_shared_alternation();
    blueflame_guards_and_fallback();
    mtu_configuration();
    driver_startup();
    issue_9_page_query_without_firmware_error();
    issue_9_large_initial_page_request();
    pcie_counters();
    lifecycle(); failed_create(false); failed_create(true); corrupt_completion(); corrupt_page_return();
    native_data_callbacks();
    for (uint32_t initial: {0u,29u,31u,32u,63u,0xfffffffeu}) {
        destroy_pending_qp(initial,false); destroy_pending_qp(initial,true);
    }
    puts("PASS kernel HCA lifecycle, Apple data callback translation, credits, errors, port state and failure retention (fake transport)");
}
