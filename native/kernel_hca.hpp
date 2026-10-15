#pragma once
#include "kernel_transport.hpp"
#include "cx5_verbs.hpp"
#include "cx5_work_queue.hpp"
#include "cx5_pointer_index.hpp"

namespace cx5_native {
struct HardwareObject { uint32_t id = 0; bool live = false; };
struct HardwareCQ {
    HardwareObject object{};
    Buffer buffer{};
    uint32_t consumer = 0, references = 0, outstanding = 0;
    // A user-posted QP delivers completions here whose requests the kernel
    // never recorded; credit accounting then covers kernel-posted QPs only.
    bool user_mode = false;
};
struct HardwareQP {
    HardwareObject object{};
    Buffer buffer{};
    HardwareCQ *send_cq = nullptr, *recv_cq = nullptr;
    uint32_t pd = 0, state = 0, producer = 0, recv_producer = 0;
    cx5::WorkQueue sends{}, receives{};
    void *client_context = nullptr;
    HardwareQP *next = nullptr;
    // Created on a context-owned UAR: userspace writes its own WQEs and
    // doorbells, the kernel refuses to post, and completions carry only the
    // hardware WQE counter as their identity.
    bool user_posted = false;
};
// The caller serializes commands and resource changes; no fixed diagnostic MR
// or queue is allocated here. Each object belongs to its requesting verbs call.
class Hca {
public:
    Transport transport{};
    bool start();
    bool attach_and_start(IOPCIDevice *device, IOService *owner);
    IOReturn startup_error = kIOReturnSuccess;
    uint16_t startup_page_phase = 0;
    uint16_t startup_page_step = 0;
    int32_t startup_page_count = 0;
    bool stop();
    bool port_active(bool &active);
    // PCIe performance counters: an ACCESS_REG query of MPCNT group 0. The
    // stall fields are the share (0-100) of the last second in which the device
    // held outbound reads or writes it could not send for lack of PCIe credits,
    // and how many seconds that share exceeded 30, when the firmware supports
    // those fields. An accepted MPCNT query alone does not prove stall support.
    struct PcieCounters {
        uint32_t rx_errors=0, tx_errors=0, crc_error_dllp=0, crc_error_tlp=0;
        uint32_t stalled_reads=0, stalled_writes=0, stalled_reads_events=0, stalled_writes_events=0;
    };
    bool query_pcie_counters(PcieCounters &counters);
    // Startup-only MTU configuration; refuses changes while QPs exist.
    bool configure_ethernet_mtu(uint16_t bytes);
    uint16_t ethernet_mtu=0, max_ethernet_mtu=0;
    uint16_t frame_admin_mtu=0, frame_oper_mtu=0, vport_frame_mtu=0;
    static uint8_t roce_mtu(uint16_t ethernet_bytes) {
        // Reserve 96 bytes for RoCE v2 headers, including IPv6 and extensions.
        uint8_t result=0;
        for (uint8_t n=1;n<=5;++n) if (uint32_t(128u<<n)+96<=ethernet_bytes) result=n;
        return result;
    }
    bool source_gid(bool enable);
    bool alloc_pd(HardwareObject &pd);
    bool dealloc_pd(HardwareObject &pd);
    bool register_mr(HardwareObject &mr, uint32_t pd, uint64_t address, uint64_t length,
                     const uint64_t *pages, size_t count, uint32_t access, uint32_t &key,
                     unsigned log_page=12);
    bool deregister_mr(HardwareObject &mr, uint32_t key);
    bool create_cq(HardwareCQ &cq);
    bool destroy_cq(HardwareCQ &cq);
    // A user-posted QP passes the allocated firmware UAR ID its doorbells use;
    // the kernel's own UAR is used when the index is left unspecified.
    static constexpr uint32_t kernel_uar=UINT32_MAX;
    bool create_qp(HardwareQP &qp, uint32_t pd, HardwareCQ &send, HardwareCQ &recv, uint32_t uar_page=kernel_uar);
    bool destroy_qp(HardwareQP &qp);
    bool reset_qp(HardwareQP &qp);
    bool transition(HardwareQP &qp, uint16_t opcode, const cx5::RCConnection &connection);
    // Kernel posting never inlines: the request's bytes live in the posting
    // process, which this path cannot read. Inline requests are refused.
    bool post(HardwareQP &qp, uint64_t work_id, const cx5::SendRequest &request);
    bool post(HardwareQP &qp, uint64_t work_id, uint8_t opcode, uint64_t local, uint32_t lkey,
              uint32_t length, uint64_t remote, uint32_t rkey);
    bool receive(HardwareQP &qp, uint64_t work_id, uint64_t address, uint32_t length, uint32_t lkey);
    cx5::CQResult poll(HardwareCQ &cq, cx5::Completion &completion, cx5::WorkRecord &work, void **client_context=nullptr,
                       bool *user_posted=nullptr);
    bool mr_in_flight(uint32_t key) const;
    void *qp_context(uint32_t qpn) const;
    // Context-owned UARs for userspace doorbells; only with 16 KiB UAR pages,
    // so one firmware UAR is exactly one mappable host page.
    bool alloc_uar(HardwareObject &uar);
    bool dealloc_uar(HardwareObject &uar);
    // EQ/CQ/QP contexts take the ALLOC_UAR resource ID without scaling.
    // Only the BAR byte offset below depends on the negotiated page size.
    uint32_t uar_page_index(const HardwareObject &uar) const { return uar.id; }
    uint64_t uar_page_offset(const HardwareObject &uar) const { return uint64_t(uar.id)<<uar_shift_; }
    uint8_t mac[6]{}, gid[16]{};
    bool blueflame_capable=false, blueflame_enabled=false;
    uint32_t blueflame_buffer_bytes=0;
    uint64_t blueflame_posts=0;
    // Requested by the personality before start(); granted only after the
    // firmware confirms 16 KiB UAR pages via SET_HCA_CAP and a re-query.
    bool user_queues_requested=false, user_queues=false;
    // Userspace BlueFlame: a context may map its UAR page write-combined and
    // push WQEs itself. Granted only with user queues and once the kernel's
    // own BlueFlame path is enabled on this HCA (capability, bank size and
    // a working write-combined mapping of the same kind of page).
    bool user_blueflame_requested=false, user_blueflame=false;
    // PCIe relaxed ordering on memory keys (tried per registration; a firmware
    // refusal falls back to strict ordering and is reported) and acknowledgement
    // requests on every packet instead of the vendor default of every 256.
    bool relaxed_ordering_requested=false, relaxed_ordering_refused=false;
    uint64_t relaxed_ordering_keys=0;
    bool ack_request_every_packet=false;
private:
    static constexpr uint32_t max_pages = uint32_t(Buffer::max_allocation_bytes / 4096);
    struct Pool {
        Buffer buffer{};
        uint32_t count = 0, owned = 0;
        uint16_t function = 0;
        uint64_t given[(max_pages + 63) / 64]{};
        void mark_given(uint32_t page) { given[page / 64] |= uint64_t(1) << (page % 64); }
        bool take_given(uint32_t page) {
            if (page >= count) return false;
            const uint64_t mask = uint64_t(1) << (page % 64);
            auto &word = given[page / 64];
            if (!(word & mask)) return false;
            word &= ~mask;
            return true;
        }
    } boot_, initial_;
    HardwareObject uar_{}, eq_{}, skipped_uars_[4]{};
    Buffer eq_buffer_{};
    uint8_t input_[8192]{}, output_[8192]{};
    uint8_t next_key_ = 1;
    uint32_t uar_shift_ = 12;
    HardwareQP *qps_ = nullptr;
    cx5::PointerIndex<HardwareQP,256> qp_index_{};
    void header(uint16_t opcode, uint16_t modifier = 0);
    bool call(size_t in_bytes = 16, size_t out_bytes = 16);
    bool simple(uint16_t opcode, uint16_t modifier = 0, size_t out_bytes = 16);
    bool create(HardwareObject &object, size_t in_bytes);
    bool destroy(HardwareObject &object, uint16_t opcode);
    bool provide(Pool &pool, uint16_t phase);
    bool reclaim(Pool &pool);
    bool configure_roce();
    struct MtuState { uint16_t maximum=0,admin=0,oper=0,vport=0; };
    bool query_mtu(MtuState &state);
    bool write_port_mtu(uint16_t frame_bytes);
    bool write_vport_mtu(uint16_t frame_bytes);
    bool purge_qp_completions(HardwareCQ &cq, uint32_t qpn);
    void rebuild_cq_accounting(HardwareCQ &cq);
    bool configure_uar_pages();
};
}
