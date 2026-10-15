#pragma once
#include "apple_data_verbs.hpp"
#include "registered_memory.hpp"
#include "cq_mapping.hpp"

struct ib_device;
namespace cx5_native {
// This is our device state, not a reconstruction of Apple's private structs.
// Apple owns each core context/PD/CQ/QP; independent nodes retain DMA even if
// the core frees a wrapper after an unsuccessful create callback.
class AppleProvider {
public:
    static constexpr uint32_t driver_id=0x4d434435, abi_version=2;
    static constexpr size_t device_bytes=0x9b0, ops_bytes=0x400;
    // Per device, per context, and mappings per context (a user-posted QP
    // and an observed CQ take one each, the UAR page one more).
    static constexpr unsigned resource_limit=256, context_resource_limit=64, context_mapping_limit=136;
    // mmap page numbers (16 KiB units): 1..2^32 observe a CQ read-only;
    // uar_page_number maps the requesting context's own UAR page writable;
    // uar_wc_page_number maps the same page write-combined (userspace
    // BlueFlame, only when the HCA granted it); a context maps one of the
    // two, once. queue_page_base+QPN maps that user-posted QP's work-queue
    // page writable.
    static constexpr uint64_t uar_page_number=uint64_t(1)<<32, uar_wc_page_number=(uint64_t(1)<<32)+1,
                              queue_page_base=uint64_t(2)<<32;
    static constexpr uint64_t observe_page_limit=uint64_t(UINT32_MAX)+1;
    size_t orphan_uar_count() const;
    bool prepare(Hca &);
    // Requires a BSD interface with the HCA's actual MAC, never a placeholder
    // net_device or a management interface with an unrelated identity.
    bool bind_network(const char *interface_name);
    // Called after core registration, outside the callback lock: the core
    // invokes add_gid synchronously while installing its default GID.
    bool install_default_gid();
    void remove_default_gid();
    // True when the hardware port is up but Apple's cache has dropped the default GID, as it
    // does when the link goes down. The port only counts as active once that GID is live, so
    // the poll re-installs it; nothing else would.
    bool needs_default_gid();
    // Samples port health on the command lock only, so a slow or failing
    // firmware query cannot delay post/poll on the data-path lock.
    bool sample_port(bool &active);
    // Same lock discipline for the PCIe counters. Returns false only when the
    // firmware query failed; `sampled` stays false while the device is not ready.
    bool sample_pcie_counters(Hca::PcieCounters &counters, bool &sampled);
    struct GidStatus { bool live=false; uint64_t adds=0,deletes=0; };
    GidStatus gid_status();
    void dispatch_port_event(bool active);
    void quiesce();
    // Must be called only AFTER the interface has unregistered and drained all
    // core callbacks. It refuses disposal with any live context or resource.
    bool dispose();
    ib_device *device() const { return device_; }
    const void *operations() const { return operations_; }
    // Only frees failed-create objects, never objects still owned by the core.
    // Uncertain DMA cannot be reclaimed until PCI removal is observed.
    bool reclaim_orphans();
    size_t orphan_count() const;

    static int query_device(void *,void *,void *);
    static int query_port(void *,uint32_t,void *);
    static int immutable(void *,uint32_t,void *);
    static int link_layer(void *,uint32_t);
    static int query_gid(void *,uint32_t,int,void *);
    static int query_pkey(void *,uint32_t,uint16_t,uint16_t *);
    static int add_gid(const void *,void **);
    static int del_gid(const void *,void **);
    static int alloc_context(void *,void *);
    static void dealloc_context(void *);
    static int mmap(void *,void *);
    static int alloc_pd(void *,void *);
    static int dealloc_pd(void *,void *);
    static int create_cq(void *,const void *,void *);
    static int destroy_cq(void *,void *);
    static int create_qp(void *,void *,void *);
    static int modify_qp(void *,void *,int,void *);
    static int destroy_qp(void *,void *);
    static int post_send(void *,const AppleSendWR *,const AppleSendWR **);
    static int post_recv(void *,const AppleRecvWR *,const AppleRecvWR **);
    static int poll_cq(void *,int,AppleWC *);
    static int notify_cq(void *,int);
    static int dma_mr(void *,int,void **);
    static int register_mr(void *,uint64_t,uint64_t,uint64_t,int,void *,void **);
    static int deregister_mr(void *,void *);

private:
    struct Context;
    struct PD;
    struct CQ;
    struct QP;
    struct MR;
    struct Guard;
    struct CommandGuard;
    Hca *hca_=nullptr;
    CQMappingQuota *mapping_quota_=nullptr;
    ib_device *device_=nullptr;
    // lock_ guards provider state and the short data path (post/poll); firmware
    // commands additionally take command_lock_, serializing Hca input/output
    // buffers and the command queue. Always acquire command_lock_ before lock_; never wait for command
    // ownership while holding lock_.
    IOLock *lock_=nullptr, *command_lock_=nullptr;
    void *network_=nullptr;
    bool gid_live_=false;
    uint64_t gid_adds_=0,gid_deletes_=0;
    bool quiescing_=false;
    // Last firmware-confirmed port state, refreshed by sample_port (500 ms
    // timer) and read by query_port without issuing firmware of its own.
    bool port_active_=false, port_valid_=false;
    alignas(8) uint8_t operations_[ops_bytes]{};
    Context *contexts_=nullptr;
    PD *pds_=nullptr;
    CQ *cqs_=nullptr;
    QP *qps_=nullptr;
    MR *mrs_=nullptr;
    cx5::PointerIndex<MR,256> mr_index_{};
    // Context UARs whose mapping descriptors outlived the context, awaiting
    // DEALLOC_UAR once the last descriptor is gone (command lock held).
    UarLease *orphan_uars_=nullptr;
    uint64_t uar_leaks_=0;
    bool ready() const;
    static AppleProvider *owner(const void *device);
    Context *context(const void *udata);
    void put_context(Context *);
    void release_uar(Context *);
    void sweep_orphan_uars();
    // Both require the command lock and the provider lock.
    unsigned orphan_context_objects(Context *);
    bool reclaim_orphans_locked();
    void forget(PD *);
    void forget(CQ *);
    void forget(QP *);
    void forget(MR *);
};
}
