// Original callback/resource tests with fake Apple allocation and fake hardware.
// These do not demonstrate native discovery, a loaded kext or actual DMA.
#include "apple_provider.hpp"
#include "fake_kernel_transport.hpp"
#include "cx5_cq_observer.h"
#include "cx5_user_post.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <thread>
#include <vector>
using namespace cx5_native;
using namespace cx5_test;
namespace {
bool build_supported=true;
unsigned pins=0;
void *last_pin_context=nullptr;
void *bound_network=nullptr;
void *default_gid_context=nullptr;
unsigned port_event=0;
struct Pin { uint64_t dma; };
template<class T> void put(void *o,size_t offset,T value) { memcpy(static_cast<uint8_t *>(o)+offset,&value,sizeof(value)); }
template<class T> T get(const void *o,size_t offset) { T v; memcpy(&v,static_cast<const uint8_t *>(o)+offset,sizeof(v)); return v; }
}
extern "C" ib_device *_ib_alloc_device(size_t size) {
    assert(size==AppleProvider::device_bytes);
    auto *device=static_cast<ib_device *>(calloc(1,size));
    // Exact 26A5425a allocation default: legacy post-send/recv are absent.
    put<uint64_t>(device,0x690,0x1efcf1f6b3full);
    return device;
}
extern "C" void ib_dealloc_device(ib_device *p) { free(p); }
extern "C" void ib_set_device_ops(ib_device *p,const void *ops) {
    memcpy(static_cast<uint8_t *>(static_cast<void *>(p))+8,ops,AppleProvider::ops_bytes);
}
extern "C" void *alloc_netdev(const char *name) {
    if (!strcmp(name,"missing")) return nullptr;
    auto *network=static_cast<uint8_t *>(calloc(1,0x50));
    network[0x10]=!strcmp(name,"wrong")?4:2; network[0x15]=0x10;
    return network;
}
extern "C" void free_netdev(void *network) { free(network); }
extern "C" int ib_device_set_netdev(ib_device *,void *network,uint32_t port) {
    assert(port==1); bound_network=network; return 0;
}
extern "C" void ib_cache_gid_set_default_gid(ib_device *device,uint32_t port,void *network,uint64_t types,int mode) {
    assert(port==1 && types==4 && network==bound_network);
    alignas(8) uint8_t attr[0x30]{};
    put(attr,0,network); put(attr,8,device);
    assert(AppleProvider::query_gid(device,1,0,attr+0x10)==0);
    put<uint32_t>(attr,0x20,2); put<uint32_t>(attr,0x28,1);
    if (!mode) assert(AppleProvider::add_gid(attr,&default_gid_context)==0);
    else {
        assert(mode==1);
        // 26A5425a _del_gid clears attr.ndev before invoking the provider.
        put<void *>(attr,0,nullptr);
        assert(AppleProvider::del_gid(attr,&default_gid_context)==0);
    }
}
extern "C" void ib_dispatch_event(const void *event) {
    assert(get<ib_device *>(event,0) && get<uint64_t>(event,8)==1);
    port_event=get<uint32_t>(event,16); assert(port_event==9 || port_event==10);
}
namespace cx5_native {
bool supported_build() { return build_supported; }
IOReturn UserMemory::pin(ib_ucontext *context,uint64_t start,uint64_t length,uint32_t access) {
    if (!context || umem_ || !length || start>UINT64_MAX-length || (access&~7u) || ((access&2)&&!(access&1))) return kIOReturnBadArgument;
    auto *pin=new Pin{0x90000000+uint64_t(pins)*0x400000};
    umem_=reinterpret_cast<ib_umem *>(pin); start_=start; length_=length;
    ++pins; last_pin_context=context; return 0;
}
IOReturn UserMemory::translate(uint64_t alias,uint64_t *pages,size_t capacity,size_t &count,unsigned &log_page) const {
    // One device-contiguous mapping starting at the first 4 KiB page, exactly
    // what the IOMMU hands the kernel for one pinned range.
    assert(umem_);
    const cx5::Segment segment{start_&~uint64_t(4095),reinterpret_cast<Pin *>(umem_)->dma,
                               ((start_&4095)+length_+4095)&~uint64_t(4095)};
    log_page=cx5::choose_log_page(&segment,1,start_,length_,alias,capacity);
    if (!log_page) { count=0; return kIOReturnNoSpace; }
    count=cx5::page_list(&segment,1,start_,length_,log_page,pages,capacity);
    return count ? kIOReturnSuccess : kIOReturnUnsupported;
}
void UserMemory::release() {
    assert(umem_ && pins); delete reinterpret_cast<Pin *>(umem_); umem_=nullptr; --pins;
}
}
namespace {
struct Session {
    alignas(8) uint8_t context[0x68]{}, udata[0x50]{}, pd[0x58]{}, cq[0xc8]{}, qp[0x120]{}, cq_object[0x38]{};
    void *mr=nullptr;
    void open(AppleProvider &provider) {
        put(context,0,provider.device()); put<void *>(udata,0x48,context);
        put(pd,8,provider.device()); put(cq,0,provider.device()); put(qp,0,provider.device());
        put<void *>(qp,8,pd);
        put<void *>(cq,8,cq_object); put<uint32_t>(cq_object,0x30,17);
        assert(AppleProvider::alloc_context(context,udata)==0);
    }
    void resources() {
        assert(AppleProvider::alloc_pd(pd,udata)==0);
        uint32_t attr[3]={31,0,0}; assert(AppleProvider::create_cq(cq,attr,udata)==0);
        alignas(8) uint8_t qa[0x58]{};
        put<void *>(qa,0x10,cq); put<void *>(qa,0x18,cq);
        put<uint32_t>(qa,0x30,31); put<uint32_t>(qa,0x34,31);
        put<uint32_t>(qa,0x38,1); put<uint32_t>(qa,0x3c,1);
        put<uint32_t>(qa,0x4c,2); // RC, signal all sends
        assert(AppleProvider::create_qp(qp,qa,udata)==0);
        assert(AppleProvider::register_mr(pd,0x1000000,8192,0x1000000,7,udata,&mr)==0);
        assert(mr && last_pin_context==context);
    }
    void connect(Hca &hca,uint8_t access=6,uint8_t mtu=3,uint8_t resolved_hop_limit=64) {
        alignas(8) uint8_t attr[0xc8]{},gid[0x30]{};
        put<uint32_t>(attr,0,1); put<uint32_t>(attr,0x20,access); put<uint32_t>(attr,0xbc,1);
        assert(AppleProvider::modify_qp(qp,attr,1|8|16|32,udata)==0);
        assert(cx5::get_bits(sim.command.data()+24,232,0x490,2)==unsigned(access>>1));
        put<uint32_t>(attr,0,2); put<uint32_t>(attr,8,mtu);
        put<uint32_t>(attr,0x14,0x123456); put<uint32_t>(attr,0x1c,17);
        memcpy(gid+0x10,hca.gid,16); put<uint32_t>(gid,0x20,2); put<uint32_t>(gid,0x28,1);
        put<void *>(attr,0x40,gid); attr[0x48]=0xfe; attr[0x49]=0x80;
        attr[0x5d]=resolved_hop_limit; put<uint32_t>(attr,0x64,1); attr[0x68]=1;
        put<uint32_t>(attr,0x6c,2); attr[0x70]=2; attr[0xb7]=1; attr[0xc0]=9;
        assert(AppleProvider::modify_qp(qp,attr,1|128|256|1048576|4096|131072|32768,udata)==0);
        assert(cx5::get_bits(sim.command.data()+24,232,0x40,3)==mtu);
        assert(cx5::get_bits(sim.command.data()+24,232,0x4a3,5)==9);
        assert(cx5::get_bits(sim.command.data()+24+24,44,0x58,8)==
               (resolved_hop_limit?resolved_hop_limit:64));
        put<uint32_t>(attr,0,3); put<uint32_t>(attr,0x18,0x654321);
        attr[0xb6]=1; attr[0xb8]=6; attr[0xc1]=13; attr[0xc2]=5;
        assert(AppleProvider::modify_qp(qp,attr,1|512|1024|2048|65536|8192,udata)==0);
        assert(cx5::get_bits(sim.command.data()+24,232,0x38d,3)==5);
        assert(cx5::get_bits(sim.command.data()+24,232,0x390,3)==6);
        assert(cx5::get_bits(sim.command.data()+24,232,0x100,5)==13);
    }
    void close() {
        assert(AppleProvider::destroy_qp(qp,udata)==0);
        assert(AppleProvider::deregister_mr(mr,udata)==0); mr=nullptr;
        assert(AppleProvider::destroy_cq(cq,udata)==0);
        assert(AppleProvider::dealloc_pd(pd,udata)==0);
        AppleProvider::dealloc_context(context);
    }
};
void reset() { assert(!sim.buffers && !pins); sim=Simulation{}; }
void emit(Session &s,uint32_t consumer,uint16_t counter,uint8_t opcode=0,uint8_t syndrome=0) {
    // Each fresh test creates one CQ first; choose its actual allocated mapping.
    const auto qpn=get<uint32_t>(s.qp,0xc0);
    auto *base=sim.maps.at(sim.cq_dma.begin()->second);
    auto *entry=base+(consumer&31)*64; memset(entry,0,64);
    cx5::write_be32(entry+56,qpn); cx5::write_be32(entry+44,4096);
    entry[60]=uint8_t(counter>>8); entry[61]=uint8_t(counter);
    entry[55]=syndrome; entry[63]=uint8_t((opcode<<4)|((consumer>>5)&1));
}
void device_identity() {
    for (const uint16_t part : {uint16_t(0x1019),uint16_t(0x1015)}) {
        reset(); sim.device_id=part; Hca hca; AppleProvider provider;
        assert(hca.start() && provider.prepare(hca));
        alignas(8) uint8_t attr[0x130]{};
        assert(AppleProvider::query_device(provider.device(),attr,nullptr)==0);
        assert(get<uint32_t>(attr,0x20)==sim.vendor_id && get<uint32_t>(attr,0x24)==part);
        assert(provider.dispose() && hca.stop());
        assert(hca.transport.vendor_id()==0 && hca.transport.device_id()==0);
    }
    reset(); sim.device_id=0x1017; Hca unsupported; assert(!unsupported.start());
    reset();
}
void callbacks_and_protection() {
    reset(); Hca hca; AppleProvider provider;
    assert(!provider.prepare(hca)); assert(hca.start());
    build_supported=false; assert(!provider.prepare(hca)); build_supported=true;
    assert(provider.prepare(hca));
    // An empty completion queue must be observable without a kernel polling call.
    assert(get<void *>(provider.operations(),0xf8)!=nullptr);
    // The core filters commands before reaching any provider callbacks.
    const auto commands=get<uint64_t>(provider.device(),0x690);
    // Apple ibv_cmd_poll_cq emits command21 (observed at 0x2b2c1cbb4),
    // separately from post-send28 and post-recv29.
    assert((commands&(uint64_t(1)<<21)) && (commands&(uint64_t(1)<<28)) && (commands&(uint64_t(1)<<29)));
    assert((commands&~((uint64_t(1)<<21)|(uint64_t(1)<<28)|(uint64_t(1)<<29)))==0x1efcf1f6b3full);
    assert(!provider.bind_network("missing") && !provider.bind_network("wrong"));
    // Exercise the installed operation table, not just direct helper calls.
    auto query=get<int(*)(void *,void *,void *)>(provider.operations(),0x70);
    alignas(8) uint8_t attr[0x130]{};
    assert(query(provider.device(),attr,nullptr)==0);
    assert(get<uint32_t>(attr,0x20)==0x15b3 && get<uint32_t>(attr,0x24)==0x1019);
    assert(get<uint32_t>(attr,0x70)==1 && get<uint32_t>(attr,0x68)==0);
    for (const auto offset:{0x28,0x30,0x48,0x58,0x70,0x90,0xa0,0x110,0x118,0x168,0x170,0x180,0x188,0x198,0x1a8,0x1b0,0x1c8})
        assert(get<void *>(provider.operations(),offset));
    // query_port serves the timer-refreshed cache; populate it first, exactly
    // as MCDMACX5Native samples port state before publishing the interface.
    // No network is bound yet, so the sampled port reports down.
    bool sampled=false; assert(provider.sample_port(sampled) && !sampled);
    assert(AppleProvider::query_port(provider.device(),1,attr)==0 && get<uint32_t>(attr,8)==1);
    assert(provider.bind_network("test-only-interface"));
    assert(AppleProvider::query_port(provider.device(),1,attr)==0 && get<uint32_t>(attr,8)==1);
    alignas(8) uint8_t gid_attr[0x30]{}; void *gid_context=nullptr;
    put(gid_attr,0,bound_network); put(gid_attr,8,provider.device());
    memcpy(gid_attr+0x10,hca.gid,16); put<uint32_t>(gid_attr,0x20,2); put<uint32_t>(gid_attr,0x28,1);
    gid_attr[0x10]^=1; assert(AppleProvider::add_gid(gid_attr,&gid_context)==-EINVAL && !gid_context);
    gid_attr[0x10]^=1; assert(AppleProvider::add_gid(gid_attr,&gid_context)==0 && gid_context);
    assert(AppleProvider::query_port(provider.device(),1,attr)==0 && get<uint32_t>(attr,8)==4);
    uint16_t pkey=0;
    assert(AppleProvider::query_pkey(provider.device(),1,0,&pkey)==0 && pkey==0xffff);
    assert(AppleProvider::query_pkey(provider.device(),1,1,&pkey)==-EINVAL);
    // A vport down transition is published by the sampler, not by query_port.
    sim.vport=0x10; assert(provider.sample_port(sampled) && !sampled);
    assert(AppleProvider::query_port(provider.device(),1,attr)==0 && get<uint32_t>(attr,8)==1);
    sim.vport=0x11; assert(provider.sample_port(sampled) && sampled);
    assert(AppleProvider::query_port(provider.device(),1,attr)==0 && get<uint32_t>(attr,8)==4);
    provider.dispatch_port_event(sampled); assert(port_event==9);
    Session s,other; s.open(provider); s.resources(); other.open(provider);
    assert(AppleProvider::alloc_pd(other.pd,s.udata)==0);
    void *wrong=nullptr;
    assert(AppleProvider::register_mr(s.pd,0x1000000,4096,0x1000000,7,other.udata,&wrong)==-EINVAL && !wrong);
    assert(AppleProvider::register_mr(s.pd,0x1000000,4096,0x1000000,2,s.udata,&wrong)==-EINVAL && !wrong);
    assert(AppleProvider::register_mr(s.pd,UINT64_MAX-4095,8192,UINT64_MAX-4095,7,s.udata,&wrong)==-EINVAL && !wrong);
    assert(!provider.dispose()); assert(AppleProvider::dealloc_pd(s.pd,s.udata)==-EBUSY);
    assert(AppleProvider::destroy_cq(s.cq,s.udata)==-EBUSY);
    // Apple's live unicast resolver returns hop_limit=0 after resolving the
    // destination MAC, despite userspace supplying 64 (captured kernel trace).
    s.connect(hca,2,2,0);
    AppleSGE sge{0x1000000,4096,get<uint32_t>(s.mr,0x10)};
    AppleRDMAWR wr{{nullptr,0x100000005ull,&sge,1,0,0,0},0x88880000,0x123400,0};
    AppleSGE bad_sge{0x1002000,1,sge.lkey}; AppleSendWR bad_wr{nullptr,99,&bad_sge,1,2,0,0};
    wr.base.next=&bad_wr; const AppleSendWR *bad=nullptr;
    assert(AppleProvider::post_send(s.qp,&wr.base,&bad)==-EACCES && bad==&bad_wr);
    assert(sim.doorbells==1); assert(AppleProvider::deregister_mr(s.mr,s.udata)==-EBUSY);
    emit(s,0,0); AppleWC wc{};
    assert(AppleProvider::poll_cq(s.cq,1,&wc)==1 && wc.id==wr.base.id && wc.qp==s.qp && wc.status==0);
    wr.base.next=nullptr; wr.base.opcode=4; wr.base.id=0x200000006ull;
    assert(AppleProvider::post_send(s.qp,&wr.base,&bad)==0 && !bad);
    emit(s,1,1,13,0x13); assert(AppleProvider::poll_cq(s.cq,1,&wc)==1 && wc.status==10 && wc.id==wr.base.id);
    // Exact PD ownership: a valid key from one PD is not valid in another QP,
    // and every entry of a scatter list is checked.
    void *other_mr=nullptr;
    assert(AppleProvider::register_mr(other.pd,0x1000000,4096,0x1000000,7,s.udata,&other_mr)==0);
    sge.lkey=get<uint32_t>(other_mr,0x10);
    assert(AppleProvider::post_send(s.qp,&wr.base,&bad)==-EACCES);
    {
        AppleSGE pair[2]={{0x1000000,2048,get<uint32_t>(s.mr,0x10)},{0x1001000,2048,get<uint32_t>(other_mr,0x10)}};
        AppleRDMAWR two{{nullptr,0x300000009ull,pair,2,1,2,0x0d0c0b0a},0x88880000,0x123400,0};
        assert(AppleProvider::post_send(s.qp,&two.base,&bad)==-EACCES && bad==&two.base);
        pair[1].lkey=pair[0].lkey; pair[1].address=0x1000800; pair[1].length=7000; // Past the registration.
        assert(AppleProvider::post_send(s.qp,&two.base,&bad)==-EACCES);
        pair[1].length=2048; pair[1].address=0x1001000;
        assert(AppleProvider::post_send(s.qp,&two.base,&bad)==0 && !bad);
        emit(s,2,2); assert(AppleProvider::poll_cq(s.cq,1,&wc)==1 && wc.id==two.base.id && wc.opcode==1 && wc.status==0);
    }
    assert(AppleProvider::deregister_mr(other_mr,s.udata)==0);
    // A cached/indexed lkey must never retain a freed registration node.
    assert(AppleProvider::post_send(s.qp,&wr.base,&bad)==-EACCES);
    sge.lkey=get<uint32_t>(s.mr,0x10);
    AppleRecvWR recv{nullptr,0x400000007ull,&sge,1}; const AppleRecvWR *bad_recv=nullptr;
    assert(AppleProvider::post_recv(s.qp,&recv,&bad_recv)==0 && !bad_recv);
    emit(s,3,0,2); assert(AppleProvider::poll_cq(s.cq,1,&wc)==1 && wc.id==recv.id && wc.flags==0);
    // A SEND with immediate reaches the application with the raw bits.
    recv.id=0x400000008ull;
    assert(AppleProvider::post_recv(s.qp,&recv,&bad_recv)==0);
    emit(s,4,1,3); memcpy(sim.maps.at(sim.cq_dma.begin()->second)+(4&31)*64+40,"\x0a\x0b\x0c\x0d",4);
    assert(AppleProvider::poll_cq(s.cq,1,&wc)==1 && wc.id==recv.id && wc.opcode==128 && wc.flags==2 && wc.bytes==4096);
    assert(!memcmp(&wc.immediate,"\x0a\x0b\x0c\x0d",4));
    assert(AppleProvider::notify_cq(s.cq,0)==-EOPNOTSUPP);
    assert(AppleProvider::dma_mr(s.pd,7,&wrong)==-EOPNOTSUPP && !wrong);
    // RESET stops DMA and permits reuse of the same QP, including WR counter 0.
    alignas(8) uint8_t reset_attr[0xc8]{};
    assert(AppleProvider::modify_qp(s.qp,reset_attr,1,s.udata)==0);
    s.connect(hca,6,3,37);
    wr.base.opcode=2; assert(AppleProvider::post_send(s.qp,&wr.base,&bad)==0);
    emit(s,5,0); assert(AppleProvider::poll_cq(s.cq,1,&wc)==1 && wc.id==wr.base.id);
    assert(AppleProvider::dealloc_pd(other.pd,s.udata)==0);
    AppleProvider::dealloc_context(other.context);
    s.close();
    put<void *>(gid_attr,0,gid_attr); // A different non-null network is invalid.
    assert(AppleProvider::del_gid(gid_attr,&gid_context)==-EINVAL && gid_context);
    put<void *>(gid_attr,0,nullptr);
    assert(AppleProvider::del_gid(gid_attr,&gid_context)==0 && !gid_context);
    assert(AppleProvider::query_port(provider.device(),1,attr)==0 && get<uint32_t>(attr,8)==1);
    assert(provider.install_default_gid());
    provider.remove_default_gid(); assert(!default_gid_context);
    assert(!provider.gid_status().live && provider.gid_status().deletes==2);
    assert(provider.install_default_gid());
    assert(provider.gid_status().live && provider.gid_status().adds==3);
    assert(provider.sample_port(sampled) && sampled);
    provider.quiesce(); assert(provider.sample_port(sampled) && !sampled);
    provider.dispatch_port_event(sampled); assert(port_event==10);
    provider.remove_default_gid(); assert(!default_gid_context);
    assert(provider.dispose()); assert(hca.stop());
    assert(!sim.buffers && !pins);
}
void registration_index_reuse() {
    reset(); Hca hca; AppleProvider provider; assert(hca.start() && provider.prepare(hca));
    Session s; s.open(provider); s.resources(); s.connect(hca);
    AppleSGE sge{0x1000000,64,0}; AppleSendWR wr{nullptr,77,&sge,1,2,0,0};
    const AppleSendWR *bad=nullptr; AppleWC wc{};
    for(unsigned iteration=0;iteration<160;++iteration) {
        sge.lkey=get<uint32_t>(s.mr,0x10);
        assert(AppleProvider::post_send(s.qp,&wr,&bad)==0);
        emit(s,iteration,uint16_t(iteration));
        assert(AppleProvider::poll_cq(s.cq,1,&wc)==1 && wc.id==77);
        assert(AppleProvider::deregister_mr(s.mr,s.udata)==0); s.mr=nullptr;
        assert(AppleProvider::post_send(s.qp,&wr,&bad)==-EACCES);
        assert(AppleProvider::register_mr(s.pd,0x1000000,8192,0x1000000,7,s.udata,&s.mr)==0);
    }
    s.close(); assert(provider.dispose() && hca.stop() && !pins && !sim.buffers);
    puts("PASS MR index invalidation, stale-key denial and registration churn beyond table size");
}
void failed_creates() {
    for (uint16_t opcode:{uint16_t(0x800),uint16_t(0x400),uint16_t(0x500),uint16_t(0x200)}) {
        for (bool timeout:{false,true}) {
            reset(); Hca hca; AppleProvider provider; assert(hca.start() && provider.prepare(hca));
            Session s; s.open(provider);
            bool pd=false,cq=false;
            if (opcode!=0x800) { assert(AppleProvider::alloc_pd(s.pd,s.udata)==0); pd=true; }
            if (opcode==0x500) {
                uint32_t ca[3]={31,0,0}; assert(AppleProvider::create_cq(s.cq,ca,s.udata)==0); cq=true;
            }
            sim.fail_opcode=opcode; sim.timeout=timeout;
            int result=0;
            if (opcode==0x800) result=AppleProvider::alloc_pd(s.pd,s.udata);
            if (opcode==0x400) { uint32_t ca[3]={31,0,0}; result=AppleProvider::create_cq(s.cq,ca,s.udata); }
            if (opcode==0x500) {
                alignas(8) uint8_t qa[0x58]{}; put<void *>(qa,0x10,s.cq); put<void *>(qa,0x18,s.cq);
                put<uint32_t>(qa,0x30,31); put<uint32_t>(qa,0x34,31);
                put<uint32_t>(qa,0x38,1); put<uint32_t>(qa,0x3c,1); put<uint32_t>(qa,0x4c,2);
                result=AppleProvider::create_qp(s.qp,qa,s.udata);
            }
            if (opcode==0x200) result=AppleProvider::register_mr(s.pd,0x1000000,4096,0x1000000,7,s.udata,&s.mr);
            assert(result<0 && !s.mr);
            assert(provider.orphan_count()==unsigned(timeout));
            if (timeout) {
                const auto held=sim.buffers; assert(!provider.reclaim_orphans() && sim.buffers==held);
                if (opcode==0x200) assert(pins==1);
                sim.removed=true;
            }
            sim.fail_opcode=0; assert(provider.reclaim_orphans() && !pins);
            if (cq) assert(AppleProvider::destroy_cq(s.cq,s.udata)==0);
            if (pd) assert(AppleProvider::dealloc_pd(s.pd,s.udata)==0);
            AppleProvider::dealloc_context(s.context);
            assert(provider.dispose()); assert(hca.stop()); assert(!sim.buffers);
        }
    }
}
void concurrent_allocations() {
    reset(); Hca hca; AppleProvider provider; assert(hca.start() && provider.prepare(hca));
    Session s; s.open(provider);
    std::vector<std::thread> threads;
    for (unsigned t=0;t<4;++t) threads.emplace_back([&]{
        for (unsigned n=0;n<100;++n) {
            alignas(8) uint8_t pd[0x58]{}; put(pd,8,provider.device());
            assert(AppleProvider::alloc_pd(pd,s.udata)==0);
            assert(AppleProvider::dealloc_pd(pd,s.udata)==0);
        }
    });
    for (auto &thread:threads) thread.join();
    AppleProvider::dealloc_context(s.context); assert(provider.dispose() && hca.stop());
}
}
static void jumbo_provider() {
    assert(!sim.buffers); sim=Simulation{};
    Hca hca; AppleProvider provider;
    assert(hca.start() && hca.configure_ethernet_mtu(9000) && provider.prepare(hca));
    bool active=false; assert(provider.sample_port(active)); // No network bound yet.
    alignas(8) uint8_t attr[0x48]{};
    assert(!AppleProvider::query_port(provider.device(),1,attr));
    assert(get<uint32_t>(attr,0xc)==5 && get<uint32_t>(attr,0x10)==5);
    Session s; s.open(provider); s.resources(); s.connect(hca,6,5); s.close();
    assert(provider.dispose() && hca.stop() && !sim.buffers);
}
// R2 regression: a stalled firmware port query (health timer path) must not
// prevent post/poll on a healthy CQ; both still serialize against concurrent
// object-lifecycle firmware commands through the command lock.
static thread_local bool tracked_crud=false;
static void command_lock_decoupling() {
    reset(); Hca hca; AppleProvider provider;
    assert(hca.start() && provider.prepare(hca));
    Session s; s.open(provider); s.resources(); s.connect(hca);
    // Establish a real ready/port-query baseline before injecting the blocked
    // sampler, so an unrelated startup failure cannot leave the gate unsignaled.
    assert(hca.transport.ready() && hca.transport.initialized);
    bool baseline_active=false; assert(provider.sample_port(baseline_active));
    sim.port_gate=true;
    std::thread sampler([&]{ bool active=false; assert(provider.sample_port(active)); });
    sim.port_arrived.get_future().wait();
    // A CRUD callback waiting for the sampler must not monopolize the data
    // mutex. The hook identifies the real lock attempt, not a timed guess.
    std::promise<void> crud_waiting; bool reported=false;
    test_lock_attempt=[&](IOLock *lock) {
        if (tracked_crud && lock==sim.blocked_command_lock && !reported) {
            reported=true; crud_waiting.set_value();
        }
    };
    std::thread crud([&]{
        tracked_crud=true; alignas(8) uint8_t pd[0x58]{};
        put(pd,8,provider.device());
        assert(!AppleProvider::alloc_pd(pd,s.udata));
        assert(!AppleProvider::dealloc_pd(pd,s.udata));
    });
    crud_waiting.get_future().wait();
    auto data=std::async(std::launch::async,[&]{
        AppleSGE sge{0x1000000,4096,get<uint32_t>(s.mr,0x10)};
        AppleRDMAWR wr{{nullptr,77,&sge,1,0,0,0},0x8000000,9,0};
        const AppleSendWR *bad=nullptr; AppleWC completion{};
        assert(!AppleProvider::post_send(s.qp,&wr.base,&bad)); emit(s,0,0);
        assert(AppleProvider::poll_cq(s.cq,1,&completion)==1 && completion.id==77);
    });
    const bool progressed=data.wait_for(std::chrono::milliseconds(100))==std::future_status::ready;
    sim.port_continue.set_value(); sampler.join(); crud.join(); data.get();
    test_lock_attempt=nullptr; sim.port_gate=false;
    assert(progressed && "CRUD waiting on firmware must not hold the data lock");
    s.close(); assert(provider.dispose() && hca.stop() && !sim.buffers);
    puts("PASS sampler plus waiting CRUD preserve post/poll progress and command serialization");
}
static void cq_observation_mapping() {
    reset(); Hca hca; AppleProvider provider;
    assert(hca.start() && provider.prepare(hca));
    Session s,other; s.open(provider); other.open(provider); s.resources(); s.connect(hca);
    alignas(8) uint8_t vma[0xb0]{};
    put<uint64_t>(vma,8,MCDMA_CQ_MAP_BYTES); put<uint64_t>(vma,0x10,18);
    put<uint64_t>(vma,0x18,1); put<uint64_t>(vma,0x20,0x20000);
    auto map=get<int(*)(void *,void *)>(provider.operations(),0xf8);
    assert(map(other.context,vma)==-ENOENT); // No foreign-context CQ mapping.
    for (uint64_t protection: {0ull,2ull,3ull,4ull,5ull,7ull}) {
        put(vma,0x18,protection); assert(map(s.context,vma)==-EPERM);
    }
    put<uint64_t>(vma,0x18,1); put<uint64_t>(vma,0x20,0);
    assert(map(s.context,vma)==-EPERM); put<uint64_t>(vma,0x20,0x20000);
    put<uint64_t>(vma,8,32768); assert(map(s.context,vma)==-EPERM);
    put<uint64_t>(vma,8,MCDMA_CQ_MAP_BYTES); put<uint64_t>(vma,0,16384);
    assert(map(s.context,vma)==-EPERM); put<uint64_t>(vma,0,0);
    for (uint64_t page: {0ull,0x100000002ull}) {
        put(vma,0x10,page); assert(map(s.context,vma)==-EINVAL);
    }
    // The write-combined UAR page is a defined name even without user queues:
    // read-only is a protection error, writable is an unsupported feature.
    put<uint64_t>(vma,0x10,AppleProvider::uar_wc_page_number); assert(map(s.context,vma)==-EPERM);
    put<uint64_t>(vma,0x18,3); assert(map(s.context,vma)==-EOPNOTSUPP); put<uint64_t>(vma,0x18,1);
    put<uint64_t>(vma,0x10,18); assert(map(s.context,vma)==0);
    auto *held=get<IOMemoryDescriptor *>(vma,0x28);
    assert(held && held->references==1 && mcdma_cq_observe(held->bytes)==0);
    assert(map(s.context,vma)==-EPERM); // Never overwrite an existing descriptor.
    AppleSGE sge{0x1000000,4096,get<uint32_t>(s.mr,0x10)};
    AppleRDMAWR wr{{nullptr,0,&sge,1,0,0,0},0x8000000,9,0};
    const AppleSendWR *bad=nullptr; AppleWC completion{};
    for (unsigned i=0;i<65;++i) {
        wr.base.id=100+i;
        assert(AppleProvider::post_send(s.qp,&wr.base,&bad)==0);
        assert(mcdma_cq_observe(held->bytes)==0);
        emit(s,i,uint16_t(i)); assert(mcdma_cq_observe(held->bytes)==1);
        assert(AppleProvider::poll_cq(s.cq,1,&completion)==1);
        assert(completion.id==wr.base.id && mcdma_cq_observe(held->bytes)==0);
    }
    // Reset purges a queued completion and publishes the new consumer; a
    // userspace-only shadow counter would become stale at this point.
    assert(AppleProvider::post_send(s.qp,&wr.base,&bad)==0); emit(s,65,65);
    alignas(8) uint8_t reset_attr[0xc8]{};
    assert(AppleProvider::modify_qp(s.qp,reset_attr,1,s.udata)==0);
    assert(mcdma_cq_observe(held->bytes)==0);
    provider.quiesce(); assert(mcdma_cq_observe(held->bytes)==-1);
    s.close(); AppleProvider::dealloc_context(other.context);
    assert(provider.dispose() && hca.stop());
    assert(sim.buffers==1 && held->references==1 && mcdma_cq_observe(held->bytes)==-1);
    held->release(); assert(!sim.buffers); // VMA::close drops its reference.

    alignas(16384) uint8_t bytes[MCDMA_CQ_MAP_BYTES]{};
    mcdma_cq_set_live(bytes,1);
    for (uint32_t consumer: {31u,32u,63u,64u,0xffffffu,0x1000000u}) {
        auto &owner=bytes[(consumer&31)*64+63];
        owner=uint8_t((consumer>>5)&1); mcdma_cq_set_consumer(bytes,consumer);
        assert(mcdma_cq_observe(bytes)==1); owner^=1; assert(mcdma_cq_observe(bytes)==0);
        owner=0xf0|uint8_t((consumer>>5)&1); assert(mcdma_cq_observe(bytes)==0);
    }
    puts("PASS read-only CQ mapping contract, context isolation, retained lifetime, reset and wrap observation (fake Apple core)");
}
static void cq_mapping_quota() {
    reset(); Hca hca; AppleProvider provider;
    assert(hca.start() && provider.prepare(hca)); Session s; s.open(provider);
    uint32_t attr[3]={31,0,0};
    std::vector<IOMemoryDescriptor *> retained;
    for (unsigned i=0;i<=AppleProvider::context_mapping_limit;++i) {
        put<uint32_t>(s.cq_object,0x30,i);
        assert(AppleProvider::create_cq(s.cq,attr,s.udata)==0);
        alignas(8) uint8_t vma[0xb0]{};
        put<uint64_t>(vma,8,MCDMA_CQ_MAP_BYTES); put<uint64_t>(vma,0x10,uint64_t(i)+1);
        put<uint64_t>(vma,0x18,1); put<uint64_t>(vma,0x20,0x20000);
        if (i==0) {
            // Failed descriptor initialization must return its quota slot.
            IOSubMemoryDescriptor::fail_init=true;
            assert(AppleProvider::mmap(s.context,vma)==-ENOMEM);
            assert(!get<void *>(vma,0x28)); IOSubMemoryDescriptor::fail_init=false;
        }
        if (i==AppleProvider::context_mapping_limit) {
            assert(AppleProvider::mmap(s.context,vma)==-ENOMEM);
            assert(!get<void *>(vma,0x28));
            retained.back()->release(); retained.pop_back();
        }
        assert(AppleProvider::mmap(s.context,vma)==0);
        retained.push_back(get<IOMemoryDescriptor *>(vma,0x28));
        if (i==AppleProvider::context_mapping_limit) {
            hca.transport.quarantined=true;
            alignas(8) uint8_t port[0x48]{};
            (void)AppleProvider::query_port(provider.device(),1,port);
            assert(mcdma_cq_observe(retained.back()->bytes)==-1);
            hca.transport.quarantined=false; // Only a simulated failure.
        }
        assert(AppleProvider::destroy_cq(s.cq,s.udata)==0);
        assert(mcdma_cq_observe(retained.back()->bytes)==-1);
    }
    AppleProvider::dealloc_context(s.context); assert(provider.dispose() && hca.stop());
    assert(sim.buffers==AppleProvider::context_mapping_limit);
    for (auto *mapping:retained) mapping->release();
    assert(!sim.buffers);
    puts("PASS retired mapping quota, failed-map refund, slot reuse, quarantine publication and provider-independent lifetime");
}
static void context_quota_fairness() {
    reset(); Hca hca; AppleProvider provider;
    assert(hca.start() && provider.prepare(hca));
    Session a,b; a.open(provider); b.open(provider);
    constexpr unsigned limit=AppleProvider::context_resource_limit;
    alignas(8) static uint8_t pds[limit+1][0x58];
    memset(pds,0,sizeof(pds));
    for (unsigned i=0;i<limit;++i) {
        put(pds[i],8,provider.device()); assert(!AppleProvider::alloc_pd(pds[i],a.udata));
    }
    put(pds[limit],8,provider.device());
    assert(AppleProvider::alloc_pd(pds[limit],a.udata)==-ENOMEM);
    assert(!AppleProvider::alloc_pd(pds[limit],b.udata));
    assert(!AppleProvider::dealloc_pd(pds[limit],b.udata));
    for (unsigned i=0;i<limit;++i) assert(!AppleProvider::dealloc_pd(pds[i],a.udata));
    AppleProvider::dealloc_context(a.context); AppleProvider::dealloc_context(b.context);
    assert(provider.dispose() && hca.stop());
    puts("PASS one context cannot consume the full device PD quota");
}
static void remaining_context_quotas() {
    for (unsigned kind=0;kind<3;++kind) {
        reset(); Hca hca; AppleProvider provider;
        assert(hca.start() && provider.prepare(hca));
        Session a,b; a.open(provider); b.open(provider); a.resources(); b.resources();
        constexpr unsigned limit=AppleProvider::context_resource_limit;
        alignas(8) static uint8_t cores[limit][0x120]; static void *mrs[limit];
        memset(cores,0,sizeof(cores)); memset(mrs,0,sizeof(mrs));
        auto create=[&](unsigned index,Session &owner) {
            put(cores[index],0,provider.device());
            if(kind==0) { uint32_t attr[3]={31,0,0}; return AppleProvider::create_cq(cores[index],attr,owner.udata); }
            if(kind==2) return AppleProvider::register_mr(owner.pd,0x1000000,8192,0x1000000,7,owner.udata,&mrs[index]);
            put<void *>(cores[index],8,owner.pd); alignas(8) uint8_t attr[0x58]{};
            put<void *>(attr,0x10,owner.cq); put<void *>(attr,0x18,owner.cq);
            put<uint32_t>(attr,0x30,31); put<uint32_t>(attr,0x34,31);
            put<uint32_t>(attr,0x38,1); put<uint32_t>(attr,0x3c,1); put<uint32_t>(attr,0x4c,2);
            return AppleProvider::create_qp(cores[index],attr,owner.udata);
        };
        auto destroy=[&](unsigned index,Session &owner) {
            return kind==0 ? AppleProvider::destroy_cq(cores[index],owner.udata) :
                   kind==1 ? AppleProvider::destroy_qp(cores[index],owner.udata) :
                             AppleProvider::deregister_mr(mrs[index],owner.udata);
        };
        for(unsigned i=0;i<limit-1;++i) assert(!create(i,a));
        assert(create(limit-1,a)==-ENOMEM && !create(limit-1,b));
        assert(!destroy(limit-1,b)); for(unsigned i=0;i<limit-1;++i) assert(!destroy(i,a));
        a.close(); b.close(); assert(provider.dispose() && hca.stop());
    }
    // Both quotas remain charged through context/provider destruction: full
    // per-context budgets fill the device quota, the next context gets only
    // the remainder, and a released mapping makes room again.
    reset(); Hca hca; AppleProvider provider; assert(hca.start() && provider.prepare(hca));
    constexpr unsigned per_context=AppleProvider::context_mapping_limit, device_quota=CQMappingQuota::limit;
    constexpr unsigned full=device_quota/per_context, remainder=device_quota-full*per_context;
    static_assert(remainder>0 && remainder<per_context);
    static Session sessions[full+1]; std::vector<IOMemoryDescriptor *> held;
    for(unsigned c=0;c<=full;++c) {
        auto &session=sessions[c]; session=Session{}; session.open(provider);
        uint32_t attr[3]={31,0,0}; assert(!AppleProvider::create_cq(session.cq,attr,session.udata));
        const unsigned maps=c<full?per_context:remainder+1;
        for(unsigned n=0;n<maps;++n) {
            alignas(8) uint8_t vma[0xb0]{}; put<uint64_t>(vma,8,MCDMA_CQ_MAP_BYTES);
            put<uint64_t>(vma,0x10,18);put<uint64_t>(vma,0x18,1);put<uint64_t>(vma,0x20,0x20000);
            if(c==full && n==remainder) {
                assert(AppleProvider::mmap(session.context,vma)==-ENOMEM);
                held.front()->release();held.erase(held.begin());
            }
            assert(!AppleProvider::mmap(session.context,vma));held.push_back(get<IOMemoryDescriptor *>(vma,0x28));
        }
    }
    for(auto &session:sessions) {
        assert(!AppleProvider::destroy_cq(session.cq,session.udata));
        AppleProvider::dealloc_context(session.context);
    }
    assert(provider.dispose() && hca.stop() && sim.buffers==full+1);
    for(auto *mapping:held) mapping->release();assert(!sim.buffers);
    puts("PASS per-context CQ/QP/MR fairness and retained per-context/global mapping quotas");
}
static void user_queue_mappings() {
    reset(); Hca hca; hca.user_queues_requested=true; AppleProvider provider;
    assert(hca.start() && hca.user_queues && provider.prepare(hca));
    Session s,other; s.open(provider); other.open(provider);
    alignas(8) uint8_t vma[0xb0]{};
    auto map=[&](Session &who,uint64_t page,uint64_t prot) {
        memset(vma,0,sizeof(vma)); put<uint64_t>(vma,8,MCDMA_CQ_MAP_BYTES); put<uint64_t>(vma,0x10,page);
        put<uint64_t>(vma,0x18,prot); put<uint64_t>(vma,0x20,0x20000);
        return AppleProvider::mmap(who.context,vma);
    };
    // The UAR page: writable only, exactly once, own context only.
    assert(map(s,AppleProvider::uar_page_number,1)==-EPERM && !get<void *>(vma,0x28));
    assert(map(s,AppleProvider::uar_page_number,3)==0);
    auto *uar_map=get<IOSubMemoryDescriptor *>(vma,0x28); assert(uar_map && uar_map->writable);
    assert(map(s,AppleProvider::uar_page_number,3)==-EBUSY);
    // A QP created after the UAR mapping is user-posted; the other context
    // never mapped its UAR, so its QP stays kernel-posted. The user context
    // creates its CQ first so the emit helper targets it.
    s.resources(); s.connect(hca);
    other.resources();
    const auto qpn=get<uint32_t>(s.qp,0xc0), other_qpn=get<uint32_t>(other.qp,0xc0);
    assert(map(other,AppleProvider::queue_page_base+qpn,3)==-ENOENT);   // Not the owner.
    assert(map(s,AppleProvider::queue_page_base+qpn,1)==-EPERM);
    assert(map(other,AppleProvider::queue_page_base+other_qpn,3)==-EPERM); // Kernel-posted queues stay private.
    assert(map(s,AppleProvider::queue_page_base+0xffffff,3)==-ENOENT);
    assert(map(s,AppleProvider::queue_page_base+qpn,3)==0);
    auto *queue_map=get<IOSubMemoryDescriptor *>(vma,0x28); assert(queue_map && queue_map->writable);
    // Kernel posting to the user-posted QP is refused before any MR check.
    AppleSGE sge{0x1000000,4096,get<uint32_t>(s.mr,0x10)};
    AppleRDMAWR wr{{nullptr,5,&sge,1,0,0,0},0x8000000,9,0}; const AppleSendWR *bad=nullptr;
    assert(AppleProvider::post_send(s.qp,&wr.base,&bad)==-EOPNOTSUPP);
    AppleRecvWR recv{nullptr,6,&sge,1}; const AppleRecvWR *bad_recv=nullptr;
    assert(AppleProvider::post_recv(s.qp,&recv,&bad_recv)==-EOPNOTSUPP);
    // A completion the user posted arrives with its counter as the identity.
    emit(s,0,7); AppleWC wc{};
    assert(AppleProvider::poll_cq(s.cq,1,&wc)==1 && wc.id==7 && wc.opcode==0 && wc.status==0 && wc.qp==s.qp);
    emit(s,1,8,2); cx5::write_be32(sim.maps.at(sim.cq_dma.begin()->second)+(1&31)*64+44,4000);
    assert(AppleProvider::poll_cq(s.cq,1,&wc)==1 && wc.id==8 && wc.opcode==128 && wc.bytes==4000);
    // The other context's kernel-posted QP keeps its full kernel path.
    other.connect(hca);
    AppleSGE other_sge{0x1000000,4096,get<uint32_t>(other.mr,0x10)};
    AppleRDMAWR other_wr{{nullptr,77,&other_sge,1,0,0,0},0x8000000,9,0};
    assert(AppleProvider::post_send(other.qp,&other_wr.base,&bad)==0);
    // Lifetime: the context goes away while its UAR mapping is still held.
    s.close(); AppleProvider::dealloc_context(s.context);
    assert(provider.orphan_uar_count()==1);
    uar_map->release();               // VMA close after context close.
    Session third; third.open(provider); // The next context allocation sweeps.
    assert(provider.orphan_uar_count()==0);
    AppleProvider::dealloc_context(third.context);
    queue_map->release();
    other.close();
    assert(provider.dispose() && hca.stop() && !sim.buffers && sim.objects.empty());
    puts("PASS user queue mappings: UAR/queue page namespaces, ownership, kernel post refusal, counter identity, orphaned UAR sweep");
}
static void user_blueflame_mappings() {
    // The write-combined UAR page: refused without the personality grant;
    // with it, the descriptor forwards the write-combined cache attribute
    // (only for that page) and the QP's capability block publishes a bank.
    reset(); Hca hca; hca.user_queues_requested=true; AppleProvider provider;
    assert(hca.start() && hca.user_queues && hca.blueflame_enabled && !hca.user_blueflame && provider.prepare(hca));
    Session s; s.open(provider);
    alignas(8) uint8_t vma[0xb0]{};
    auto map=[&](Session &who,uint64_t page,uint64_t prot) {
        memset(vma,0,sizeof(vma)); put<uint64_t>(vma,8,MCDMA_CQ_MAP_BYTES); put<uint64_t>(vma,0x10,page);
        put<uint64_t>(vma,0x18,prot); put<uint64_t>(vma,0x20,0x20000);
        return AppleProvider::mmap(who.context,vma);
    };
    assert(map(s,AppleProvider::uar_wc_page_number,3)==-EOPNOTSUPP && !get<void *>(vma,0x28));
    assert(map(s,AppleProvider::uar_page_number,3)==0);
    auto *plain=get<IOSubMemoryDescriptor *>(vma,0x28); assert(plain);
    assert(map(s,AppleProvider::uar_wc_page_number,3)==-EOPNOTSUPP && map(s,AppleProvider::uar_page_number,3)==-EBUSY);
    IOSubMemoryDescriptor::map_calls=0;
    assert(!plain->makeMapping(plain,nullptr,0,kIOMapAnywhere,0,0));
    assert(IOSubMemoryDescriptor::map_calls==1 && IOSubMemoryDescriptor::last_map_options==kIOMapAnywhere);
    s.resources();
    auto *queue=sim.maps.at(sim.qp_dma.at(get<uint32_t>(s.qp,0xc0)));
    uint32_t flags=0,bank=0;
    assert(mcdma_info_read(queue,&flags,&bank) && flags==MCDMA_INFO_KERNEL_BLUEFLAME && bank==0);
    s.close(); plain->release();
    assert(provider.reclaim_orphans() && provider.dispose() && hca.stop() && !sim.buffers && sim.objects.empty());

    reset(); Hca granted; granted.user_queues_requested=granted.user_blueflame_requested=true; AppleProvider p2;
    assert(granted.start() && granted.user_blueflame && p2.prepare(granted));
    Session t,other; t.open(p2); other.open(p2);
    auto map2=[&](Session &who,uint64_t page,uint64_t prot) {
        memset(vma,0,sizeof(vma)); put<uint64_t>(vma,8,MCDMA_CQ_MAP_BYTES); put<uint64_t>(vma,0x10,page);
        put<uint64_t>(vma,0x18,prot); put<uint64_t>(vma,0x20,0x20000);
        return AppleProvider::mmap(who.context,vma);
    };
    assert(map2(t,AppleProvider::uar_wc_page_number,1)==-EPERM);
    assert(map2(t,AppleProvider::uar_wc_page_number,3)==0);
    auto *wc=get<IOSubMemoryDescriptor *>(vma,0x28); assert(wc && wc->writable);
    assert(map2(t,AppleProvider::uar_page_number,3)==-EBUSY && map2(t,AppleProvider::uar_wc_page_number,3)==-EBUSY);
    // Before the core maps it, no QP may claim the attribute.
    t.resources(); t.connect(granted);
    auto *early=sim.maps.at(sim.qp_dma.at(get<uint32_t>(t.qp,0xc0)));
    assert(mcdma_info_read(early,&flags,&bank) && flags==MCDMA_INFO_KERNEL_BLUEFLAME && bank==0);
    IOSubMemoryDescriptor::map_calls=0;
    assert(!wc->makeMapping(wc,nullptr,0,kIOMapAnywhere|kIOMapInhibitCache,0,0));
    assert(IOSubMemoryDescriptor::map_calls==1 &&
           IOSubMemoryDescriptor::last_map_options==(kIOMapAnywhere|kIOMapWriteCombineCache));
    // A second QP after the mapping carries the bank; the block survives a
    // reset, which clears only the hardware-visible half of the page.
    alignas(8) uint8_t core[0x120]{}; put(core,0,p2.device()); put<void *>(core,8,t.pd);
    alignas(8) uint8_t attr[0x60]{}; put<void *>(attr,0x10,t.cq); put<void *>(attr,0x18,t.cq);
    put<uint32_t>(attr,0x30,31); put<uint32_t>(attr,0x34,31); put<uint32_t>(attr,0x38,1); put<uint32_t>(attr,0x3c,1);
    put<uint32_t>(attr,0x4c,2);
    assert(AppleProvider::create_qp(core,attr,t.udata)==0);
    auto *late=sim.maps.at(sim.qp_dma.at(get<uint32_t>(core,0xc0)));
    assert(mcdma_info_read(late,&flags,&bank) && flags==(MCDMA_INFO_UAR_WRITE_COMBINED|MCDMA_INFO_KERNEL_BLUEFLAME) && bank==256);
    alignas(8) uint8_t modify[0xd0]{}; put<uint32_t>(modify,0,0);
    assert(AppleProvider::modify_qp(core,modify,1,t.udata)==0);
    assert(mcdma_info_read(late,&flags,&bank) && bank==256);
    assert(AppleProvider::destroy_qp(core,t.udata)==0);
    // The other context's ordinary UAR mapping forwards no cache attribute,
    // and a CQ observation mapping never does either.
    assert(map2(other,AppleProvider::uar_page_number,3)==0);
    auto *other_uar=get<IOSubMemoryDescriptor *>(vma,0x28);
    IOSubMemoryDescriptor::map_calls=0;
    assert(!other_uar->makeMapping(other_uar,nullptr,0,kIOMapAnywhere,0,0) && IOSubMemoryDescriptor::last_map_options==kIOMapAnywhere);
    other.resources();
    const auto *object=get<void *>(other.cq,8);
    assert(map2(other,uint64_t(get<uint32_t>(object,0x30))+1,1)==0);
    auto *cq_map=get<IOSubMemoryDescriptor *>(vma,0x28);
    assert(!cq_map->makeMapping(cq_map,nullptr,0,kIOMapAnywhere|kIOMapReadOnly,0,0) &&
           IOSubMemoryDescriptor::last_map_options==(kIOMapAnywhere|kIOMapReadOnly));
    t.close(); wc->release();
    other.close(); other_uar->release(); cq_map->release();
    assert(p2.reclaim_orphans() && p2.dispose() && granted.stop() && !sim.buffers && sim.objects.empty());
    puts("PASS userspace BlueFlame mappings: personality gate, write-combined attribute forwarded for the UAR page only, capability block per QP");
}
static void large_registration() {
    // Registrations are bounded by what the mapping can describe, not by a
    // fixed byte limit: a device-contiguous 64 MiB range takes four 16 MiB
    // pages, an HCA address that only shares the 16 KiB offset forces 16 KiB
    // pages, and a range that then needs too many entries is refused.
    reset(); Hca hca; AppleProvider provider; assert(hca.start() && provider.prepare(hca));
    Session s; s.open(provider); assert(AppleProvider::alloc_pd(s.pd,s.udata)==0);
    void *big=nullptr;
    assert(AppleProvider::register_mr(s.pd,0x1000000,64<<20,0x1000000,7,s.udata,&big)==0 && big);
    assert(cx5::get_bits(sim.command.data()+16,64,0x1da,6)==24 && cx5::read_be32(sim.command.data()+0x60)==2);
    assert(cx5::read_be64(sim.command.data()+0x110)==0x90000000 && cx5::read_be64(sim.command.data()+0x110+24)==0x93000000);
    assert(cx5::read_be64(sim.command.data()+16+0x80/8)==0x1000000 && cx5::read_be64(sim.command.data()+16+0xc0/8)==(64<<20));
    assert(AppleProvider::deregister_mr(big,s.udata)==0);
    void *aliased=nullptr;
    assert(AppleProvider::register_mr(s.pd,0x1000000,1<<20,0x1004000,7,s.udata,&aliased)==0 && aliased);
    assert(cx5::get_bits(sim.command.data()+16,64,0x1da,6)==14 && cx5::read_be32(sim.command.data()+0x60)==32);
    assert(AppleProvider::deregister_mr(aliased,s.udata)==0);
    void *refused=nullptr;
    assert(AppleProvider::register_mr(s.pd,0x1000000,64<<20,0x1004000,7,s.udata,&refused)==-EIO && !refused);
    assert(AppleProvider::register_mr(s.pd,0x1000000,max_mr_bytes+1,0x1000000,7,s.udata,&refused)==-EINVAL && !refused);
    assert(provider.orphan_count()==0 && pins==0);
    alignas(8) uint8_t attr[0x130]{};
    assert(AppleProvider::query_device(provider.device(),attr,nullptr)==0 && get<uint64_t>(attr,0x10)==max_mr_bytes);
    assert(AppleProvider::dealloc_pd(s.pd,s.udata)==0); AppleProvider::dealloc_context(s.context);
    assert(provider.dispose() && hca.stop() && !sim.buffers);
    puts("PASS large registrations: page size chosen from the mapping, HCA-address alias respected, oversize refused");
}
static void process_death_teardown() {
    // A process dies holding a connected kernel-posted QP whose WRITE is still
    // in flight (so its MR is busy) and a second context with mapped UAR,
    // queue and CQ pages. The core's cleanup rounds are refused, it frees its
    // wrappers regardless and deallocates the contexts. The provider must turn
    // the abandoned objects into orphans, keep the retained mappings valid,
    // let a later process reuse the same wrapper addresses and recover the
    // hardware once the firmware answers again.
    reset(); Hca hca; hca.user_queues_requested=true; AppleProvider provider;
    assert(hca.start() && hca.user_queues && provider.prepare(hca));
    Session k,s; k.open(provider); s.open(provider);
    alignas(8) uint8_t vma[0xb0]{};
    auto map=[&](Session &who,uint64_t page,uint64_t prot) {
        memset(vma,0,sizeof(vma)); put<uint64_t>(vma,8,MCDMA_CQ_MAP_BYTES); put<uint64_t>(vma,0x10,page);
        put<uint64_t>(vma,0x18,prot); put<uint64_t>(vma,0x20,0x20000);
        return AppleProvider::mmap(who.context,vma);
    };
    assert(map(s,AppleProvider::uar_page_number,3)==0);
    auto *uar_map=get<IOMemoryDescriptor *>(vma,0x28); assert(uar_map);
    k.resources(); k.connect(hca);
    s.resources(); s.connect(hca);
    assert(map(s,AppleProvider::queue_page_base+get<uint32_t>(s.qp,0xc0),3)==0);
    auto *queue_map=get<IOMemoryDescriptor *>(vma,0x28); assert(queue_map);
    assert(map(s,18,1)==0);
    auto *cq_map=get<IOMemoryDescriptor *>(vma,0x28); assert(cq_map && mcdma_cq_observe(cq_map->bytes)==0);
    AppleSGE sge{0x1000000,4096,get<uint32_t>(k.mr,0x10)};
    AppleRDMAWR wr{{nullptr,5,&sge,1,0,0,0},0x8000000,9,0}; const AppleSendWR *bad=nullptr;
    assert(AppleProvider::post_send(k.qp,&wr.base,&bad)==0);
    // An ordinary close round: the busy MR and the referenced CQ are refused
    // until the QP is gone, which the core retries.
    assert(AppleProvider::deregister_mr(k.mr,k.udata)==-EBUSY);
    assert(AppleProvider::destroy_cq(k.cq,k.udata)==-EBUSY);
    // The firmware now refuses DESTROY_QP, so the core's forced round fails
    // for everything behind it. It gives up and frees its wrappers anyway.
    sim.fail_opcode=0x501;
    assert(AppleProvider::destroy_qp(k.qp,k.udata)==-EBUSY);
    // The refused destroy still reset the QP, so its MR is no longer busy. A
    // process that never deregistered it leaves it behind as well.
    assert(AppleProvider::destroy_cq(k.cq,k.udata)==-EBUSY);
    assert(AppleProvider::dealloc_pd(k.pd,k.udata)==-EBUSY);
    assert(AppleProvider::destroy_qp(s.qp,s.udata)==-EBUSY);
    assert(AppleProvider::destroy_cq(s.cq,s.udata)==-EBUSY);
    assert(AppleProvider::dealloc_pd(s.pd,s.udata)==-EBUSY);
    assert(AppleProvider::deregister_mr(s.mr,s.udata)==0); s.mr=nullptr;
    AppleProvider::dealloc_context(k.context); k.mr=nullptr; // The provider freed the wrapper.
    AppleProvider::dealloc_context(s.context);
    // Deallocation reclaimed what it could at once: the reset QP freed k's
    // MR, while the refused QPs keep their CQs and PDs, and s's UAR page is
    // still mapped.
    assert(provider.orphan_count()==6 && provider.orphan_uar_count()==1);
    assert(mcdma_cq_observe(cq_map->bytes)==-1); // A retained mapping sees the invalidation.
    // The next process gets the same wrapper addresses from the core; none of
    // them may be mistaken for the dead process's objects.
    k.open(provider); k.resources(); k.connect(hca);
    assert(provider.orphan_count()==6);
    // With the firmware answering again the orphans are reclaimed; the UAR
    // page stays leased while its mapping is still held.
    sim.fail_opcode=0;
    assert(!provider.reclaim_orphans() && provider.orphan_count()==0 && provider.orphan_uar_count()==1);
    uar_map->release();
    assert(provider.reclaim_orphans() && provider.orphan_uar_count()==0);
    AppleSGE live{0x1000000,4096,get<uint32_t>(k.mr,0x10)};
    AppleRDMAWR ok{{nullptr,6,&live,1,0,0,0},0x8000000,9,0};
    assert(AppleProvider::post_send(k.qp,&ok.base,&bad)==0);
    alignas(8) uint8_t reset_attr[0xc8]{};
    assert(AppleProvider::modify_qp(k.qp,reset_attr,1,k.udata)==0);
    k.close(); queue_map->release(); cq_map->release();
    assert(provider.dispose() && hca.stop() && !sim.buffers && !pins && sim.objects.empty());
    puts("PASS process-death teardown: refused destroys become orphans, wrapper addresses are reusable, retained mappings stay valid, hardware is reclaimed");
}
static void review_immediate_orphan_reclaim() {
    // Cleanup can be refused once and then succeed during dealloc_context.
    // Reclaim must not free the context while that callback still uses it.
    for (unsigned kind=0;kind<3;++kind) {
        reset(); Hca hca; AppleProvider provider;
        assert(hca.start() && provider.prepare(hca));
        Session s; s.open(provider);
        if (kind==2) s.resources();
        else if (kind==0) assert(AppleProvider::alloc_pd(s.pd,s.udata)==0);
        else {
            uint32_t attr[3]={31,0,0};
            assert(AppleProvider::create_cq(s.cq,attr,s.udata)==0);
        }
        sim.fail_opcode=kind==0 ? 0x801 : kind==1 ? 0x401 : 0x501;
        if (kind==0) assert(AppleProvider::dealloc_pd(s.pd,s.udata)!=0);
        else if (kind==1) assert(AppleProvider::destroy_cq(s.cq,s.udata)!=0);
        else assert(AppleProvider::destroy_qp(s.qp,s.udata)!=0);
        sim.fail_opcode=0;
        AppleProvider::dealloc_context(s.context); s.mr=nullptr;
        assert(provider.orphan_count()==0);
        assert(provider.dispose() && hca.stop() && !sim.buffers && !pins && sim.objects.empty());
    }
    puts("PASS immediate context reclaim after transient PD/CQ/QP destroy refusal");
}
int main() {
    device_identity();
    review_immediate_orphan_reclaim();
    large_registration();
    process_death_teardown();
    user_blueflame_mappings();
    user_queue_mappings();
    registration_index_reuse();
    context_quota_fairness();
    remaining_context_quotas();
    cq_mapping_quota();
    cq_observation_mapping();
    command_lock_decoupling();
    jumbo_provider();
    callbacks_and_protection(); failed_creates(); concurrent_allocations();
    puts("PASS native callback table, context/PD isolation, MR bounds, transitions, completion IDs, reset reuse, failure retention and serialized concurrency (fake Apple core and transport)");
}
