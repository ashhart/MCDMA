#include "apple_provider.hpp"
#include "apple_build.hpp"
#include "cx5_cq_observer.h"
#include "cx5_user_post.h"
#include <string.h>
#include <sys/errno.h>
#ifndef CX5_NATIVE_TEST
#include <ptrauth.h>
#define mcdma_log IOLog
#else
#define mcdma_log(...) ((void)0)
#endif

extern "C" ib_device *_ib_alloc_device(size_t);
extern "C" void ib_dealloc_device(ib_device *);
extern "C" void ib_set_device_ops(ib_device *,const void *);
extern "C" void *alloc_netdev(const char *);
extern "C" void free_netdev(void *);
extern "C" int ib_device_set_netdev(ib_device *,void *,uint32_t);
extern "C" void ib_cache_gid_set_default_gid(ib_device *,uint32_t,void *,uint64_t,int);
extern "C" void ib_dispatch_event(const void *);

namespace cx5_native {
namespace {
// Offsets are from our inspection of the owner's 26A5425a core, not imported
// private headers. These helpers are NEVER used on raw userspace addresses.
template<class T> T read(const void *object,size_t offset) {
    T value{}; if (object) memcpy(&value,static_cast<const uint8_t *>(object)+offset,sizeof(value));
    return value;
}
template<class T> void write(void *object,size_t offset,T value) {
    memcpy(static_cast<uint8_t *>(object)+offset,&value,sizeof(value));
}
template<uint16_t discriminator,class F> void callback(void *table,size_t offset,F function) {
#ifdef CX5_NATIVE_TEST
    (void)discriminator;
    const auto pointer=reinterpret_cast<void *>(function);
#else
    // ib_set_device_ops copies pointer bits; call sites use these observed
    // type discriminators without storage-address diversity.
    const auto pointer=ptrauth_sign_unauthenticated(
        ptrauth_strip(reinterpret_cast<void *>(function),ptrauth_key_function_pointer),
        ptrauth_key_asia,discriminator);
#endif
    write(table,offset,pointer);
}
template<class T> T *find(T *head,const void *core) {
    if (!core) return nullptr;
    for (;head;head=head->next) if (head->core==core) return head;
    return nullptr;
}
template<class T> unsigned count(T *head) {
    unsigned n=0; for (;head;head=head->next) ++n; return n;
}
template<class T,class Predicate> unsigned count_if(T *head,Predicate matches) {
    unsigned n=0; for(;head;head=head->next) n+=matches(head); return n;
}
template<class T> void unlink(T *&head,T *node) {
    T **at=&head; while (*at && *at!=node) at=&(*at)->next;
    if (*at) *at=node->next;
}
bool range(uint64_t address,uint32_t length,uint64_t base,uint64_t size) {
    return length && address>=base && uint64_t(length)<=size && address-base<=size-length;
}
int io_error(IOReturn error) {
    if (error==kIOReturnBadArgument) return -EINVAL;
    if (error==kIOReturnNoMemory) return -ENOMEM;
    if (error==kIOReturnBusy) return -EBUSY;
    return error ? -EIO : 0;
}
}
struct AppleProvider::Context {
    void *core=nullptr; unsigned references=0; Context *next=nullptr;
    CQMappingQuota *mapping_quota=nullptr;
    // Context-owned UAR for userspace doorbells; QPs created after its page
    // has been mapped are user-posted on it.
    UarLease *uar=nullptr; bool uar_mapped=false, uar_wc=false;
    ~Context() { if(mapping_quota) mapping_quota->release(); }
};
struct AppleProvider::PD {
    void *core=nullptr; Context *context=nullptr; HardwareObject hardware{};
    unsigned references=0; PD *next=nullptr;
};
struct AppleProvider::CQ {
    void *core=nullptr; Context *context=nullptr; HardwareCQ hardware{}; CQ *next=nullptr;
};
struct AppleProvider::QP {
    void *core=nullptr; PD *pd=nullptr; HardwareQP hardware{};
    cx5::RCConnection connection{}; bool signal_all=false; QP *next=nullptr;
};
struct AppleProvider::MR {
    void *core=nullptr; PD *pd=nullptr; RegisteredMemory hardware{};
    uint64_t address=0,length=0; uint32_t access=0,index_key=0; MR *next=nullptr;
};
struct AppleProvider::Guard {
    AppleProvider *provider;
    explicit Guard(AppleProvider *p):provider(p) { if (p) IOLockLock(p->lock_); }
    ~Guard() {
        if (!provider) return;
        if (provider->hca_ && !provider->ready()) {
            for (auto *cq=provider->cqs_;cq;cq=cq->next)
                if (cq->hardware.buffer.cpu) mcdma_cq_set_live(cq->hardware.buffer.cpu,0);
        }
        IOLockUnlock(provider->lock_);
    }
};
// Serializes firmware commands (Hca input_/output_ scratch, the command queue
// and next_key_). Acquired before Guard in resource callbacks and by the sampler.
// Waiting for firmware ownership must never hold the data-path Guard.
struct AppleProvider::CommandGuard {
    AppleProvider *provider;
    explicit CommandGuard(AppleProvider *p):provider(p) { if (p) IOLockLock(p->command_lock_); }
    ~CommandGuard() { if (provider) IOLockUnlock(provider->command_lock_); }
};
bool AppleProvider::ready() const { return !quiescing_ && hca_ && hca_->transport.ready() && hca_->transport.initialized; }
AppleProvider *AppleProvider::owner(const void *device) { return read<AppleProvider *>(device,0x9a8); }
AppleProvider::Context *AppleProvider::context(const void *udata) {
    // The core passes its uverbs bundle; +0x48 is the context established by
    // ib_alloc_ucontext, also used by its alloc_pd and create_cq paths.
    return find(contexts_,read<void *>(udata,0x48));
}
void AppleProvider::put_context(Context *node) {
    if (node->references) --node->references;
    if (!node->core && !node->references) { unlink(contexts_,node); delete node; }
}
// Command lock held. A UAR still referenced by a mapping descriptor cannot be
// returned to firmware yet: park it and retry when the descriptor is gone.
void AppleProvider::release_uar(Context *node) {
    auto *lease=node->uar; node->uar=nullptr; node->uar_mapped=false;
    if (!lease) return;
    if (lease->count()==1 && hca_ && hca_->dealloc_uar(lease->uar)) { delete lease; return; }
    lease->orphan=true; lease->next=orphan_uars_; orphan_uars_=lease;
}
void AppleProvider::sweep_orphan_uars() {
    UarLease **at=&orphan_uars_;
    while (*at) {
        auto *lease=*at;
        if (lease->count()==1 && hca_ && hca_->dealloc_uar(lease->uar)) { *at=lease->next; delete lease; continue; }
        if (lease->count()==1) ++uar_leaks_; // Firmware refused; retried on later sweeps.
        at=&lease->next;
    }
}
size_t AppleProvider::orphan_uar_count() const { return count(orphan_uars_); }
void AppleProvider::forget(PD *node) { unlink(pds_,node); put_context(node->context); delete node; }
void AppleProvider::forget(CQ *node) { unlink(cqs_,node); put_context(node->context); delete node; }
void AppleProvider::forget(QP *node) { unlink(qps_,node); --node->pd->references; delete node; }
void AppleProvider::forget(MR *node) {
    mr_index_.erase(node->index_key,node);
    unlink(mrs_,node); --node->pd->references; delete node;
}

bool AppleProvider::prepare(Hca &hca) {
    if (device_ || lock_ || command_lock_ || !supported_build() || !hca.transport.ready() || !hca.transport.initialized) return false;
    lock_=IOLockAlloc(); if (!lock_) return false;
    command_lock_=IOLockAlloc();
    if (!command_lock_) { IOLockFree(lock_); lock_=nullptr; return false; }
    device_=_ib_alloc_device(device_bytes);
    if (!device_) {
        IOLockFree(command_lock_); command_lock_=nullptr;
        IOLockFree(lock_); lock_=nullptr; return false;
    }
    mapping_quota_=new CQMappingQuota;
    if (!mapping_quota_) {
        ib_dealloc_device(device_); device_=nullptr;
        IOLockFree(command_lock_); command_lock_=nullptr;
        IOLockFree(lock_); lock_=nullptr; return false;
    }
    hca_=&hca;
    quiescing_=false;
    port_active_=false; port_valid_=false;
    // 26A5425a _ib_alloc_device supplies a legacy command mask that omits
    // POLL_CQ (21), POST_SEND (28) and POST_RECV (29); uapi_merge_def filters
    // them before our real callbacks can run. These command IDs are observed
    // in Apple's ibv_cmd_* helpers, not inferred from callback-table slots.
    // Preserve the core's other command bits.
    write<uint64_t>(device_,0x690,read<uint64_t>(device_,0x690)|
                    (uint64_t(1)<<21)|(uint64_t(1)<<28)|(uint64_t(1)<<29));
    write(device_,0x9a8,this);
    write(operations_,0x08,driver_id); write(operations_,0x0c,abi_version);
    callback<0xbcad>(operations_,0x070,query_device);
    callback<0x392e>(operations_,0x090,query_port);
    callback<0x392e>(operations_,0x0a0,immutable);
    callback<0x50c7>(operations_,0x0a8,link_layer);
    callback<0x54f7>(operations_,0x0c8,query_gid);
    callback<0xe4fe>(operations_,0x0d0,add_gid);
    callback<0xe4fe>(operations_,0x0d8,del_gid);
    callback<0x54f7>(operations_,0x0e0,query_pkey);
    callback<0xe4fe>(operations_,0x0e8,alloc_context);
    callback<0x2abe>(operations_,0x0f0,dealloc_context);
    callback<0xe4fe>(operations_,0x0f8,mmap);
    callback<0xe4fe>(operations_,0x110,alloc_pd);
    callback<0xe4fe>(operations_,0x118,dealloc_pd);
    callback<0xbcad>(operations_,0x168,create_qp);
    callback<0x0e2e>(operations_,0x170,modify_qp);
    callback<0xe4fe>(operations_,0x180,destroy_qp);
    callback<0xbcad>(operations_,0x028,post_send);
    callback<0xbcad>(operations_,0x030,post_recv);
    callback<0xbcad>(operations_,0x188,create_cq);
    callback<0xe4fe>(operations_,0x198,destroy_cq);
    callback<0x392e>(operations_,0x048,poll_cq);
    callback<0x50c7>(operations_,0x058,notify_cq);
    callback<0x392e>(operations_,0x1a8,dma_mr);
    callback<0xd725>(operations_,0x1b0,register_mr);
    callback<0xe4fe>(operations_,0x1c8,deregister_mr);
    write<uint64_t>(operations_,0x3c0,0xc8); // CQ core bytes
    write<uint64_t>(operations_,0x3d0,0x58); // PD
    write<uint64_t>(operations_,0x3d8,0x120); // QP
    write<uint64_t>(operations_,0x3f0,0x68); // context
    ib_set_device_ops(device_,operations_);
    constexpr char description[]="MCDMA ConnectX-5 Ex polled RC provider";
    memcpy(static_cast<uint8_t *>(static_cast<void *>(device_))+0x698,description,sizeof(description));
    memcpy(static_cast<uint8_t *>(static_cast<void *>(device_))+0x6d8,hca.gid+8,8);
    write<uint8_t>(device_,0x6e5,1); // IB_NODE_CA, Ethernet link layer below
    write<uint32_t>(device_,0x6e8,1); write<uint32_t>(device_,0x4e8,1);
    return true;
}
bool AppleProvider::bind_network(const char *name) {
    if (!device_ || network_ || !name || !*name || !supported_build()) return false;
    auto *network=alloc_netdev(name);
    if (!network) return false;
    if (memcmp(static_cast<uint8_t *>(network)+0x10,hca_->mac,6) ||
        ib_device_set_netdev(device_,network,1)) { free_netdev(network); return false; }
    network_=network; return true;
}
bool AppleProvider::install_default_gid() {
    if (!device_ || !network_) return false;
    ib_cache_gid_set_default_gid(device_,1,network_,uint64_t(1)<<2,0);
    Guard guard(this); return gid_live_;
}
void AppleProvider::remove_default_gid() {
    if (device_ && network_) ib_cache_gid_set_default_gid(device_,1,network_,uint64_t(1)<<2,1);
}
bool AppleProvider::sample_port(bool &active) {
    active=false;
    // Command -> state is the sole nested order. Keep command ownership until
    // publishing this sample so another sampler cannot publish out of order.
    CommandGuard command(this);
    Hca *hca=nullptr;
    {
        Guard guard(this);
        if (!ready()) return true;
        hca=hca_;
    }
    bool raw=false;
    const bool failed=!hca->port_active(raw);
    Guard guard(this);
    if (failed) { port_valid_=false; port_active_=false; return false; }
    if (!ready()) return true;
    port_active_=raw; port_valid_=true;
    active=raw && network_ && gid_live_; return true;
}
bool AppleProvider::sample_pcie_counters(Hca::PcieCounters &counters,bool &sampled) {
    counters=Hca::PcieCounters{}; sampled=false;
    CommandGuard command(this);
    Hca *hca=nullptr;
    {
        Guard guard(this);
        if (!ready()) return true;
        hca=hca_;
    }
    if (!hca->query_pcie_counters(counters)) return false;
    sampled=true; return true;
}
AppleProvider::GidStatus AppleProvider::gid_status() {
    Guard guard(this); return {gid_live_,gid_adds_,gid_deletes_};
}
void AppleProvider::dispatch_port_event(bool active) {
    // ib_dispatch_event copies 24 bytes and reads the event at +0x10;
    // its cache task reads the element's port at +8.
    struct Event { ib_device *device; uint64_t port; uint32_t event,pad; };
    static_assert(sizeof(Event)==24,"Apple event ABI");
    if (device_) { Event event{device_,1,active?9u:10u,0}; ib_dispatch_event(&event); }
}
void AppleProvider::quiesce() {
    Guard guard(this); quiescing_=true;
    for (auto *cq=cqs_;cq;cq=cq->next)
        if (cq->hardware.buffer.cpu) mcdma_cq_set_live(cq->hardware.buffer.cpu,0);
}
bool AppleProvider::dispose() {
    if (!device_) return true;
    if (contexts_ || pds_ || cqs_ || qps_ || mrs_ || orphan_uars_) return false;
    if (network_) {
        if (ib_device_set_netdev(device_,nullptr,1)) return false;
        free_netdev(network_); network_=nullptr; gid_live_=false;
    }
    ib_dealloc_device(device_); device_=nullptr; hca_=nullptr;
    mapping_quota_->release(); mapping_quota_=nullptr;
    port_active_=false; port_valid_=false;
    IOLockFree(command_lock_); command_lock_=nullptr;
    IOLockFree(lock_); lock_=nullptr; memset(operations_,0,sizeof(operations_)); return true;
}
size_t AppleProvider::orphan_count() const {
    size_t n=0;
    for (auto *p=pds_;p;p=p->next) n+=!p->core;
    for (auto *p=cqs_;p;p=p->next) n+=!p->core;
    for (auto *p=qps_;p;p=p->next) n+=!p->core;
    for (auto *p=mrs_;p;p=p->next) n+=!p->core;
    return n;
}
bool AppleProvider::reclaim_orphans() {
    CommandGuard command(this); Guard guard(this);
    return reclaim_orphans_locked();
}
bool AppleProvider::reclaim_orphans_locked() {
    if (!hca_ || (hca_->transport.quarantined && !hca_->transport.detached())) return false;
    sweep_orphan_uars();
    for (auto *p=qps_,*next=p;p;p=next) { next=p->next; if (!p->core && hca_->destroy_qp(p->hardware)) forget(p); }
    for (auto *p=mrs_,*next=p;p;p=next) { next=p->next; if (!p->core && !p->hardware.destroy(*hca_)) forget(p); }
    for (auto *p=cqs_,*next=p;p;p=next) { next=p->next; if (!p->core && hca_->destroy_cq(p->hardware)) forget(p); }
    for (auto *p=pds_,*next=p;p;p=next) {
        next=p->next; if (!p->core && !p->references && hca_->dealloc_pd(p->hardware)) forget(p);
    }
    return orphan_count()==0 && !orphan_uars_;
}
int AppleProvider::query_device(void *device,void *attr,void *) {
    auto *p=owner(device); Guard guard(p); if (!p || !attr) return -EINVAL;
    memset(attr,0,0x130);
    memcpy(static_cast<uint8_t *>(attr)+8,p->hca_->gid+8,8);
    write<uint64_t>(attr,0x10,max_mr_bytes); write<uint64_t>(attr,0x18,4096);
    write<uint32_t>(attr,0x20,p->hca_->transport.vendor_id());
    write<uint32_t>(attr,0x24,p->hca_->transport.device_id());
    write<uint32_t>(attr,0x2c,resource_limit); write<uint32_t>(attr,0x30,31);
    write<uint32_t>(attr,0x48,cx5::max_rdma_sge); write<uint32_t>(attr,0x4c,cx5::max_rdma_sge);
    write<uint32_t>(attr,0x54,resource_limit); write<uint32_t>(attr,0x58,31);
    write<uint32_t>(attr,0x5c,resource_limit); write<uint32_t>(attr,0x60,resource_limit);
    write<uint32_t>(attr,0x50,1); write<uint32_t>(attr,0x64,1);
    write<uint32_t>(attr,0x6c,resource_limit); write<uint32_t>(attr,0x70,1);
    write<uint16_t>(attr,0xb8,1);
    // No atomics, on-demand paging, completion events or GPU memory advertised.
    return 0;
}
int AppleProvider::query_port(void *device,uint32_t port,void *attr) {
    auto *p=owner(device); Guard guard(p); if (!p || !attr || port!=1) return -EINVAL;
    memset(attr,0,0x48);
    // Serves the sample_port-confirmed cache; client port queries never issue
    // firmware commands and never wait behind one on the data-path lock.
    const bool active=p->ready() && p->port_valid_ && p->port_active_ && p->network_ && p->gid_live_;
    write<uint32_t>(attr,8,active?4:1);
    write<uint32_t>(attr,0xc,Hca::roce_mtu(p->hca_->max_ethernet_mtu));
    write<uint32_t>(attr,0x10,Hca::roce_mtu(p->hca_->ethernet_mtu));
    write<uint32_t>(attr,0x18,1); write<uint8_t>(attr,0x1c,1);
    write<uint32_t>(attr,0x24,1u<<30); write<uint16_t>(attr,0x30,1);
    write<uint8_t>(attr,0x44,active?5:3);
    // Width/speed remain unspecified until the port status decoder is wired.
    return 0;
}
int AppleProvider::immutable(void *device,uint32_t port,void *attr) {
    if (!owner(device) || port!=1 || !attr) return -EINVAL;
    memset(attr,0,16); write<uint32_t>(attr,0,1); write<uint32_t>(attr,4,1);
    write<uint32_t>(attr,8,(1u<<23)|(1u<<13)); // RoCE v2, Ethernet AH
    return 0;
}
int AppleProvider::link_layer(void *device,uint32_t port) { return owner(device) && port==1 ? 2 : 0; }
int AppleProvider::query_gid(void *device,uint32_t port,int index,void *gid) {
    auto *p=owner(device); Guard guard(p);
    if (!p || !gid || port!=1 || index!=0 || !p->network_) return -EINVAL;
    memcpy(gid,p->hca_->gid,16); return 0;
}
int AppleProvider::query_pkey(void *device,uint32_t port,uint16_t index,uint16_t *key) {
    if (!owner(device) || port!=1 || index || !key) return -EINVAL;
    *key=0xffff; return 0; // RoCE's fixed full-membership P_Key.
}
int AppleProvider::add_gid(const void *attr,void **context) {
    auto *p=owner(read<void *>(attr,8)); CommandGuard command(p); Guard guard(p);
    if (!p || !context || !p->ready() || !p->network_ || p->gid_live_ ||
        read<void *>(attr,0)!=p->network_ || read<uint32_t>(attr,0x20)!=2 ||
        read<uint16_t>(attr,0x24)!=0 || read<uint32_t>(attr,0x28)!=1 ||
        memcmp(static_cast<const uint8_t *>(attr)+0x10,p->hca_->gid,16)) return -EINVAL;
    bool sourced=false;
    {
        sourced=p->hca_->source_gid(true);
    }
    if (!sourced) return -EIO;
    p->gid_live_=true; ++p->gid_adds_; *context=p; return 0;
}
int AppleProvider::del_gid(const void *attr,void **context) {
    auto *p=owner(read<void *>(attr,8)); CommandGuard command(p); Guard guard(p);
    // Apple's cache clears ndev before this callback. The retained provider
    // context, device, GID and slot still establish ownership during deletion.
    auto *network=read<void *>(attr,0);
    if (!p || !context || *context!=p || read<uint16_t>(attr,0x24)!=0 ||
        (network && network!=p->network_) || read<uint32_t>(attr,0x20)!=2 ||
        read<uint32_t>(attr,0x28)!=1 ||
        memcmp(static_cast<const uint8_t *>(attr)+0x10,p->hca_->gid,16)) return -EINVAL;
    bool unsourced=true;
    if (!p->hca_->transport.detached()) {
        unsourced=p->hca_->source_gid(false);
    }
    if (!unsourced) return -EIO;
    p->gid_live_=false; ++p->gid_deletes_; *context=nullptr; return 0;
}
int AppleProvider::alloc_context(void *core,void *udata) {
    auto *p=owner(read<void *>(core,0)); CommandGuard command(p); Guard guard(p);
    if (!p || !udata || read<void *>(udata,0x48)!=core || !p->ready()) return -EINVAL;
    if (find(p->contexts_,core)) return -EINVAL;
    if (count(p->contexts_)>=resource_limit) return -ENOMEM;
    p->sweep_orphan_uars();
    // Objects abandoned by an earlier dirty exit must not consume this
    // process's quota; a refused reclaim simply leaves them for the next try.
    (void)p->reclaim_orphans_locked();
    auto *node=new Context{}; if (!node) return -ENOMEM;
    node->mapping_quota=new CQMappingQuota(context_mapping_limit);
    if (!node->mapping_quota) { delete node; return -ENOMEM; }
    if (p->hca_->user_queues) {
        // Best effort: without a UAR the context simply keeps kernel posting.
        auto *lease=new UarLease{};
        if (lease && p->hca_->alloc_uar(lease->uar)) node->uar=lease; else delete lease;
    }
    node->core=core; node->next=p->contexts_; p->contexts_=node; return 0;
}
void AppleProvider::dealloc_context(void *core) {
    auto *p=owner(read<void *>(core,0)); CommandGuard command(p); Guard guard(p); if (!p) return;
    auto *node=find(p->contexts_,core); if (!node) return;
    // Hold the context through reclamation: forgetting its last PD/CQ can
    // drop the final child reference and otherwise delete it before we return.
    ++node->references;
    node->core=nullptr;
    // A dying process reaches here after the core's cleanup rounds. The core
    // frees every PD/CQ/QP wrapper of the context even when a destroy
    // callback was refused (a busy MR, an out-of-order request or a failed
    // firmware command) and never calls this provider about them again. Drop
    // their core identity now, so a later process whose wrappers land at the
    // same addresses is not mistaken for them, and keep the hardware for
    // reclaim. The MR wrapper is ours to free.
    const unsigned abandoned=p->orphan_context_objects(node);
    p->release_uar(node);
    p->sweep_orphan_uars();
    if (abandoned) {
        const bool reclaimed=p->reclaim_orphans_locked();
        (void)reclaimed;
        mcdma_log("MCDMA native: context closed with %u undestroyed objects; %s\n",abandoned,
                  reclaimed ? "reclaimed" : "retained for a later reclaim");
    }
    p->put_context(node);
}
unsigned AppleProvider::orphan_context_objects(Context *context) {
    unsigned n=0;
    for (auto *qp=qps_;qp;qp=qp->next)
        if (qp->core && qp->pd->context==context) { qp->core=nullptr; qp->hardware.client_context=nullptr; ++n; }
    for (auto *mr=mrs_;mr;mr=mr->next)
        if (mr->core && mr->pd->context==context) {
            mr_index_.erase(mr->index_key,mr);
            IOFreeData(mr->core,0x88); mr->core=nullptr; ++n;
        }
    for (auto *cq=cqs_;cq;cq=cq->next)
        if (cq->core && cq->context==context) {
            cq->core=nullptr;
            if (cq->hardware.buffer.cpu) mcdma_cq_set_live(cq->hardware.buffer.cpu,0);
            ++n;
        }
    for (auto *pd=pds_;pd;pd=pd->next)
        if (pd->core && pd->context==context) { pd->core=nullptr; ++n; }
    return n;
}
int AppleProvider::mmap(void *core,void *vma) {
    auto *p=owner(read<void *>(core,0)); Guard guard(p);
    auto *context=p?find(p->contexts_,core):nullptr;
    if (!context || !vma || !p->ready()) return -EINVAL;
    // Observed 26A5425a MemoryMap VMA: start/end/pgoff/prot/flags/mdesc.
    // Only a fresh, anywhere, one-host-page, shared mapping is accepted; the
    // page number selects the object and fixes the permitted protection.
    // In this core prot=1 produces kIOMapReadOnly, and VMA::close releases
    // the descriptor after unmapping it. No physical or IOMMU address export.
    if (read<uint64_t>(vma,0) || read<uint64_t>(vma,8)!=MCDMA_CQ_MAP_BYTES ||
        read<uint64_t>(vma,0x20)!=0x20000 ||
        read<void *>(vma,0x28) || read<void *>(vma,0x30)) return -EPERM;
    const auto page=read<uint64_t>(vma,0x10), prot=read<uint64_t>(vma,0x18);
    if (page==uar_page_number || page==uar_wc_page_number) {
        // The requesting context's own UAR page, writable, once per context.
        // The write-combined variant exists for userspace BlueFlame and is
        // granted only when the kernel's own BlueFlame path already proved
        // that attribute on this HCA and the personality allows it.
        const bool write_combine=page==uar_wc_page_number;
        if (prot!=3) return -EPERM;
        if (write_combine && !p->hca_->user_blueflame) return -EOPNOTSUPP;
        if (!context->uar) return -ENOENT;
        if (context->uar_mapped) return -EBUSY;
        auto *bar=p->hca_->transport.bar_page_descriptor(p->hca_->uar_page_offset(context->uar->uar));
        if (!bar) return -EIO;
        auto *mapping=uar_mapping(bar,*context->uar,*p->mapping_quota_,context->mapping_quota,write_combine);
        bar->release(); // The subrange retains it.
        if (!mapping) return -ENOMEM;
        context->uar_mapped=true; context->uar_wc=write_combine; write(vma,0x28,mapping); return 0;
    }
    if (page>=queue_page_base && page<queue_page_base+0x1000000) {
        // One user-posted QP's work-queue page, writable, owner context only.
        if (prot!=3) return -EPERM;
        const auto qpn=uint32_t(page-queue_page_base);
        for (auto *qp=p->qps_;qp;qp=qp->next) {
            if (!qp->core || qp->pd->context!=context || !qp->hardware.object.live || qp->hardware.object.id!=qpn) continue;
            if (!qp->hardware.user_posted) return -EPERM; // Kernel-posted queues are never exposed.
            auto &buffer=qp->hardware.buffer;
            if (!buffer.memory || buffer.size!=MCDMA_CQ_MAP_BYTES) return -EINVAL;
            auto *mapping=queue_write_mapping(buffer.memory,*p->mapping_quota_,context->mapping_quota);
            if (!mapping) return -ENOMEM;
            write(vma,0x28,mapping); return 0;
        }
        return -ENOENT;
    }
    if (prot!=1) return -EPERM;
    if (!page || page>observe_page_limit) return -EINVAL;
    for (auto *cq=p->cqs_;cq;cq=cq->next) {
        if (cq->context!=context || !cq->core || !cq->hardware.object.live) continue;
        // _create_cq stores the core-owned uobject at cq+8 and returns its
        // uint32 handle from uobject+0x30. It is not a userspace pointer.
        const auto *object=read<void *>(cq->core,8);
        if (!object || uint64_t(read<uint32_t>(object,0x30))+1!=page) continue;
        auto &buffer=cq->hardware.buffer;
        if (!buffer.memory || buffer.size!=MCDMA_CQ_MAP_BYTES) return -EINVAL;
        // A mapping retains its own reference. Destroying a CQ stops DMA and
        // invalidates the live word, but cannot recycle its mapped storage.
        auto *mapping=cq_read_mapping(buffer.memory,*p->mapping_quota_,context->mapping_quota);
        if (!mapping) return -ENOMEM;
        write(vma,0x28,mapping); return 0;
    }
    return -ENOENT;
}
int AppleProvider::alloc_pd(void *core,void *udata) {
    auto *p=owner(read<void *>(core,8)); CommandGuard command(p); Guard guard(p);
    auto *context=p?p->context(udata):nullptr;
    if (!context || !p->ready() || find(p->pds_,core)) return -EINVAL;
    if (count(p->pds_)>=resource_limit ||
        count_if(p->pds_,[&](auto *n){return n->context==context;})>=context_resource_limit) return -ENOMEM;
    auto *node=new PD{}; if (!node) return -ENOMEM;
    node->context=context; ++context->references;
    node->next=p->pds_; p->pds_=node;
    bool allocated=false;
    {
        allocated=p->hca_->alloc_pd(node->hardware);
    }
    if (!allocated) {
        if (!p->hca_->transport.quarantined) p->forget(node);
        return -EIO;
    }
    node->core=core; return 0;
}
int AppleProvider::dealloc_pd(void *core,void *) {
    auto *p=owner(read<void *>(core,8)); CommandGuard command(p); Guard guard(p); auto *node=p?find(p->pds_,core):nullptr;
    if (!node) return -EINVAL;
    bool deallocated=false;
    {
        deallocated=!node->references && p->hca_->dealloc_pd(node->hardware);
    }
    if (!deallocated) return -EBUSY;
    p->forget(node); return 0;
}
int AppleProvider::create_cq(void *core,const void *attr,void *udata) {
    auto *p=owner(read<void *>(core,0)); CommandGuard command(p); Guard guard(p); auto *context=p?p->context(udata):nullptr;
    if (!context || !p->ready() || !attr || find(p->cqs_,core)) return -EINVAL;
    if (!read<uint32_t>(attr,0) || read<uint32_t>(attr,0)>31 || read<uint32_t>(attr,4) || read<uint32_t>(attr,8)) return -EOPNOTSUPP;
    if (count(p->cqs_)>=resource_limit ||
        count_if(p->cqs_,[&](auto *n){return n->context==context;})>=context_resource_limit) return -ENOMEM;
    auto *node=new CQ{}; if (!node) return -ENOMEM;
    node->context=context; ++context->references; node->next=p->cqs_; p->cqs_=node;
    bool created=false;
    {
        created=p->hca_->create_cq(node->hardware);
    }
    if (!created) {
        if (!p->hca_->transport.quarantined) p->forget(node);
        return -EIO;
    }
    node->core=core; write<uint32_t>(core,0x28,31); return 0;
}
int AppleProvider::destroy_cq(void *core,void *) {
    auto *p=owner(read<void *>(core,0)); CommandGuard command(p); Guard guard(p); auto *node=p?find(p->cqs_,core):nullptr;
    if (!node) return -EINVAL;
    bool destroyed=false;
    {
        destroyed=p->hca_->destroy_cq(node->hardware);
    }
    if (!destroyed) return -EBUSY;
    p->forget(node); return 0;
}
int AppleProvider::create_qp(void *core,void *attr,void *udata) {
    auto *p=owner(read<void *>(core,0)); CommandGuard command(p); Guard guard(p);
    auto *context=p?p->context(udata):nullptr; auto *pd=p?find(p->pds_,read<void *>(core,8)):nullptr;
    auto *send=p?find(p->cqs_,read<void *>(attr,0x10)):nullptr;
    auto *recv=p?find(p->cqs_,read<void *>(attr,0x18)):nullptr;
    if (!context || !pd || !send || !recv || pd->context!=context ||
        send->context!=context || recv->context!=context || !p->ready() || find(p->qps_,core)) return -EINVAL;
    // RC only, no SRQ, at most 31 requests each way, up to two send scatter
    // entries, one receive entry, and inline data up to the WQEBB's room
    // (honoured by userspace posting; kernel posting refuses inline).
    if (read<uint32_t>(attr,0x4c)!=2 || read<void *>(attr,0x20) || read<uint32_t>(attr,0x50) ||
        read<uint32_t>(attr,0x48)>1 || read<uint32_t>(attr,0x40)>cx5::max_inline_rdma ||
        !read<uint32_t>(attr,0x30) || read<uint32_t>(attr,0x30)>31 ||
        !read<uint32_t>(attr,0x34) || read<uint32_t>(attr,0x34)>31 ||
        !read<uint32_t>(attr,0x38) || read<uint32_t>(attr,0x38)>cx5::max_rdma_sge ||
        read<uint32_t>(attr,0x3c)!=1) return -EOPNOTSUPP;
    if (count(p->qps_)>=resource_limit ||
        count_if(p->qps_,[&](auto *n){return n->pd->context==context;})>=context_resource_limit) return -ENOMEM;
    auto *node=new QP{}; if (!node) return -ENOMEM;
    node->pd=pd; ++pd->references; node->next=p->qps_; p->qps_=node;
    // Once a context has mapped its UAR page, its new QPs are user-posted on
    // that UAR: the kernel refuses to post to them and only consumes CQEs.
    const uint32_t uar_page=context->uar && context->uar_mapped
        ? p->hca_->uar_page_index(context->uar->uar) : Hca::kernel_uar;
    bool created=false;
    {
        created=p->hca_->create_qp(node->hardware,pd->hardware.id,send->hardware,recv->hardware,uar_page);
    }
    if (!created) {
        if (!p->hca_->transport.quarantined) p->forget(node);
        return -EIO;
    }
    node->core=core; node->hardware.client_context=core;
    if (node->hardware.user_posted) {
        // Capability block for the owner's mapped queue page. Bank bytes are
        // published only when this context's UAR page was actually mapped
        // write-combined (the descriptor forwarded the attribute), so a
        // context holding an ordinary device-memory mapping never attempts a
        // BlueFlame push. Beyond the hardware-visible queues and the reset.
        uint32_t flags=0;
        if (context->uar_wc && context->uar && context->uar->write_combined) flags|=MCDMA_INFO_UAR_WRITE_COMBINED;
        if (p->hca_->blueflame_enabled) flags|=MCDMA_INFO_KERNEL_BLUEFLAME;
        mcdma_info_write(node->hardware.buffer.cpu,flags,p->hca_->blueflame_buffer_bytes);
    }
    // IB_SIGNAL_ALL_WR is zero; IB_SIGNAL_REQ_WR is one.
    node->signal_all=read<uint32_t>(attr,0x48)==0;
    node->connection.qpn=node->hardware.object.id; node->connection.pd=pd->hardware.id;
    node->connection.cq=send->hardware.object.id; node->connection.doorbell=node->hardware.buffer.dma+4096;
    write<uint32_t>(core,0xc0,node->hardware.object.id);
    write<uint32_t>(attr,0x30,31); write<uint32_t>(attr,0x34,31); return 0;
}
int AppleProvider::modify_qp(void *core,void *attr,int mask,void *udata) {
    auto *p=owner(read<void *>(core,0)); CommandGuard command(p); Guard guard(p); auto *node=p?find(p->qps_,core):nullptr;
    if (!node || !attr || !udata || !p->ready() || p->context(udata)!=node->pd->context) return -EINVAL;
    constexpr uint32_t state=1, current=2, access=8, pkey=16, port=32, av=128, mtu=256;
    constexpr uint32_t timeout=512,retry=1024,rnr=2048,rqpsn=4096,rdatomic=8192;
    constexpr uint32_t minrnr=32768,sqpsn=65536,destatomic=131072,destqpn=1048576;
    const auto next=read<uint32_t>(attr,0), bits=uint32_t(mask);
    if (!(bits&state) || ((bits&current) && read<uint32_t>(attr,4)!=node->hardware.state)) return -EINVAL;
    const auto flags=bits&~current;
    if (next==0 && flags==state) {
        bool reset=false;
        {
            reset=p->hca_->reset_qp(node->hardware);
        }
        return reset?0:-EIO;
    }
    if (next!=node->hardware.state+1 || next>3) return -EINVAL;
    auto connection=node->connection;
    if (next==1) {
        if (flags!=(state|pkey|port|access) || read<uint16_t>(attr,0xb0) || read<uint32_t>(attr,0xbc)!=1) return -EINVAL;
        const auto rights=read<uint32_t>(attr,0x20); if (rights&~6u) return -EOPNOTSUPP;
        connection.access=uint8_t(rights);
    } else if (next==2) {
        if (flags!=(state|av|mtu|destqpn|rqpsn|destatomic|minrnr)) return -EOPNOTSUPP;
        if (read<uint32_t>(attr,0x6c)!=2 || read<uint32_t>(attr,0x64)!=1 ||
            read<uint8_t>(attr,0x68)!=1 || read<uint8_t>(attr,0x5c)!=0 ||
            read<uint8_t>(attr,0xb7)!=1 || read<uint8_t>(attr,0x5e) || read<uint32_t>(attr,0x58)) return -EOPNOTSUPP;
        const auto path_mtu=read<uint32_t>(attr,8);
        // ib_qp_attr: min_rnr_timer 0xb8, port_num(u32) 0xbc, timeout 0xc0, retry_cnt 0xc1, rnr_retry 0xc2.
        if (!path_mtu || path_mtu>Hca::roce_mtu(p->hca_->ethernet_mtu) || read<uint8_t>(attr,0xb8)>31) return -EINVAL;
        connection.path_mtu=uint8_t(path_mtu); connection.min_rnr_timer=read<uint8_t>(attr,0xb8);
        // 26A5425a's unicast resolver overwrites the caller's hop limit with
        // zero on this directly connected route. Preserve our RC default (64)
        // for that unspecified result; retain any nonzero resolved value.
        if (const auto hops=read<uint8_t>(attr,0x5d)) connection.hop_limit=hops;
        connection.remote_qpn=read<uint32_t>(attr,0x1c); connection.receive_psn=read<uint32_t>(attr,0x14);
        memcpy(connection.remote_gid,static_cast<uint8_t *>(attr)+0x48,16);
        memcpy(connection.remote_mac,static_cast<uint8_t *>(attr)+0x70,6);
        // Only the programmed source GID is accepted, including GID type.
        auto *sgid=read<void *>(attr,0x40);
        if (!sgid || read<uint32_t>(sgid,0x20)!=2 || read<uint32_t>(sgid,0x28)!=1 ||
            memcmp(static_cast<uint8_t *>(sgid)+0x10,p->hca_->gid,16) ||
            (connection.remote_mac[0]&1) || !memcmp(connection.remote_mac,"\0\0\0\0\0\0",6)) return -EINVAL;
    } else {
        if (flags!=(state|timeout|retry|rnr|sqpsn|rdatomic) || read<uint8_t>(attr,0xb6)!=1) return -EOPNOTSUPP;
        connection.send_psn=read<uint32_t>(attr,0x18); connection.timeout=read<uint8_t>(attr,0xc0);
        connection.retry_count=read<uint8_t>(attr,0xc1); connection.rnr_retry=read<uint8_t>(attr,0xc2);
    }
    bool transitioned=false;
    {
        transitioned=p->hca_->transition(node->hardware,uint16_t(0x501+next),connection);
    }
    if (!transitioned) return -EIO;
    node->connection=connection; return 0;
}
int AppleProvider::destroy_qp(void *core,void *) {
    auto *p=owner(read<void *>(core,0)); CommandGuard command(p); Guard guard(p); auto *node=p?find(p->qps_,core):nullptr;
    if (!node) return -EINVAL;
    bool destroyed=false;
    {
        destroyed=p->hca_->destroy_qp(node->hardware);
    }
    if (!destroyed) return -EBUSY;
    p->forget(node); return 0;
}
int AppleProvider::post_send(void *core,const AppleSendWR *wr,const AppleSendWR **bad) {
    if (bad) *bad=wr;
    auto *p=owner(read<void *>(core,0)); Guard guard(p); auto *node=p?find(p->qps_,core):nullptr;
    if (!node || !bad || !p->ready()) return -EINVAL;
    if (node->hardware.user_posted) return -EOPNOTSUPP; // Userspace owns this queue.
    // Check ownership and exact bounds of every scatter entry before each
    // post; hardware also enforces the MKey's PD, bounds and permission bits.
    // Earlier WRs remain committed.
    for (auto *item=wr;item;item=item->next) {
        cx5::SendRequest request; int error=0;
        if (!apple_send_request(item,node->signal_all,request,error)) { *bad=item; return error; }
        for (unsigned i=0;i<request.sge_count;++i) {
            const auto &sge=request.sge[i];
            auto *mr=p->mr_index_.find(sge.lkey);
            if (!mr || !mr->core || mr->pd!=node->pd || !range(sge.address,sge.length,mr->address,mr->length) ||
                (request.opcode==cx5::wqe_read && !(mr->access&1))) { *bad=item; return -EACCES; }
        }
        AppleRDMAWR one{}; one.base=*item; one.base.next=nullptr;
        if (cx5::rdma_opcode(request.opcode)) { one.remote=request.remote; one.rkey=request.rkey; }
        const AppleSendWR *failed=nullptr;
        const int result=apple_post_send(*p->hca_,node->hardware,node->signal_all,&one.base,&failed);
        if (result) { *bad=item; return result; }
    }
    *bad=nullptr; return 0;
}
int AppleProvider::post_recv(void *core,const AppleRecvWR *wr,const AppleRecvWR **bad) {
    if (bad) *bad=wr;
    auto *p=owner(read<void *>(core,0)); Guard guard(p); auto *node=p?find(p->qps_,core):nullptr;
    if (!node || !bad || !p->ready()) return -EINVAL;
    if (node->hardware.user_posted) return -EOPNOTSUPP;
    for (auto *item=wr;item;item=item->next) {
        if (item->sge_count!=1 || !item->sge) { *bad=item; return -EOPNOTSUPP; }
        auto *mr=p->mr_index_.find(item->sge->lkey);
        if (!mr || !mr->core || mr->pd!=node->pd || !(mr->access&1) || !range(item->sge->address,item->sge->length,mr->address,mr->length)) { *bad=item; return -EACCES; }
        AppleRecvWR one=*item; one.next=nullptr; const AppleRecvWR *failed=nullptr;
        const int result=apple_post_recv(*p->hca_,node->hardware,&one,&failed);
        if (result) { *bad=item; return result; }
    }
    *bad=nullptr; return 0;
}
int AppleProvider::poll_cq(void *core,int maximum,AppleWC *completions) {
    auto *p=owner(read<void *>(core,0)); Guard guard(p); auto *node=p?find(p->cqs_,core):nullptr;
    return node && p->ready() ? apple_poll_cq(*p->hca_,node->hardware,maximum,completions) : -EINVAL;
}
int AppleProvider::notify_cq(void *,int) { return -EOPNOTSUPP; }
int AppleProvider::dma_mr(void *,int,void **result) { if (result) *result=nullptr; return -EOPNOTSUPP; }
int AppleProvider::register_mr(void *core,uint64_t start,uint64_t length,uint64_t iova,int access,void *udata,void **result) {
    if (!result) return -EINVAL;
    *result=nullptr;
    auto *p=owner(read<void *>(core,8)); CommandGuard command(p); Guard guard(p); auto *pd=p?find(p->pds_,core):nullptr;
    if (!pd || !p->ready() || p->context(udata)!=pd->context) return -EINVAL;
    if (count(p->mrs_)>=resource_limit ||
        count_if(p->mrs_,[&](auto *n){return n->pd->context==pd->context;})>=context_resource_limit) return -ENOMEM;
    auto *node=new MR{}; if (!node) return -ENOMEM;
    // Apple delegates allocation of ib_mr to the provider (unlike PD/CQ/QP).
    void *wrapper=IOMallocZeroData(0x88); if (!wrapper) { delete node; return -ENOMEM; }
    node->pd=pd; ++pd->references; node->next=p->mrs_; p->mrs_=node;
    int error=0;
    {
        error=io_error(node->hardware.create(*p->hca_,static_cast<ib_ucontext *>(pd->context->core),
            pd->hardware.id,start,length,iova,uint32_t(access)));
    }
    if (error) {
        IOFreeData(wrapper,0x88);
        if (!node->hardware.needs_retention()) p->forget(node);
        return error;
    }
    if(!p->mr_index_.insert(node->hardware.key(),node)) {
        p->hca_->transport.quarantined=true;
        IOFreeData(wrapper,0x88); return -EIO;
    }
    node->index_key=node->hardware.key();
    node->core=wrapper; node->address=iova; node->length=length; node->access=uint32_t(access);
    write(wrapper,0,p->device_); write(wrapper,8,core);
    write<uint32_t>(wrapper,0x10,node->hardware.key()); write<uint32_t>(wrapper,0x14,node->hardware.key());
    *result=wrapper; return 0;
}
int AppleProvider::deregister_mr(void *core,void *) {
    auto *p=owner(read<void *>(core,0)); CommandGuard command(p); Guard guard(p); auto *node=p?find(p->mrs_,core):nullptr;
    if (!node) return -EINVAL;
    int error=0;
    {
        error=io_error(node->hardware.destroy(*p->hca_));
    }
    if (error) return error;
    p->forget(node); IOFreeData(core,0x88); return 0;
}
}
