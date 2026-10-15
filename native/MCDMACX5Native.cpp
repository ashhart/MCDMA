#include "MCDMACX5Native.hpp"
#include "apple_registration.hpp"
#include "apple_build.hpp"
#include <IOKit/IOMessage.h>
#include <IOKit/IOUserClient.h>
#include <kern/task.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>

OSDefineMetaClassAndStructors(MCDMACX5Native,IOService)
struct MCDMACX5Native::State {
    cx5_native::Hca hca{};
    cx5_native::AppleProvider verbs{};
    cx5_native::RdmaNetwork network{};
    IOService *interface=nullptr;
    bool active=false;
    cx5_native::AppleProvider::GidStatus gid{};
    unsigned pcie_polls=0;
    bool pcie_counters_refused=false;
};
IOWorkLoop *MCDMACX5Native::getWorkLoop() const { return workloop_; }
bool MCDMACX5Native::start(IOService *parent) {
    // The bundle also requires explicit lab enablement; merely loading it
    // must not take the currently working DriverKit device away.
    const char *verified_build=cx5_native::verified_build_name();
    if (!verified_build || getProperty("MCDMALabEnabled")!=kOSBooleanTrue ||
        !IOService::start(parent)) return false;
    auto *pci=OSDynamicCast(IOPCIDevice,parent);
    if (!pci) { IOService::stop(parent); return false; }
    state_=new State{};
    if (!state_) { IOService::stop(parent); return false; }
    auto &s=*state_;
    const char *stage="workloop";
    parent->setProperty("MCDMANativeVersion","0.1.17");
    workloop_=IOWorkLoop::workLoop();
    timer_=IOTimerEventSource::timerEventSource(this,poll);
    if (!workloop_ || !timer_ || workloop_->addEventSource(timer_)) goto failure;
    timer_attached_=true;
    stage="hca-start";
    // Userspace queues are an explicit lab option: the firmware must grant
    // 16 KiB UAR pages first, and the property only requests the attempt.
    s.hca.user_queues_requested=getProperty("MCDMAUserQueues")==kOSBooleanTrue;
    // Userspace BlueFlame additionally lets a user-post context map its UAR
    // page write-combined; granted only once kernel BlueFlame is enabled.
    s.hca.user_blueflame_requested=getProperty("MCDMAUserBlueFlame")==kOSBooleanTrue;
    // Lab knobs for the tunnel-read cost: relaxed-ordering memory keys and
    // per-packet acknowledgement requests; both default off.
    s.hca.relaxed_ordering_requested=getProperty("MCDMARelaxedOrdering")==kOSBooleanTrue;
    s.hca.ack_request_every_packet=getProperty("MCDMAAckRequestEveryPacket")==kOSBooleanTrue;
    if (!s.hca.attach_and_start(pci,this)) goto failure;
    setProperty("MCDMAUserQueues",s.hca.user_queues);
    setProperty("MCDMAUserBlueFlame",s.hca.user_blueflame);
    setProperty("MCDMARelaxedOrdering",s.hca.relaxed_ordering_requested);
    setProperty("MCDMAAckRequestEveryPacket",s.hca.ack_request_every_packet);
    {
        // PCIe control state of the device and the tunnel path, and the
        // optional maximum read request size (0 leaves the platform value).
        uint32_t mrrs=0;
        if (auto *number=OSDynamicCast(OSNumber,getProperty("MCDMAMaxReadRequestBytes"))) mrrs=number->unsigned32BitValue();
        char path[512];
        const bool configured=s.hca.transport.configure_pcie(mrrs,path,sizeof(path));
        setProperty("MCDMAPCIePath",configured ? path : "unavailable");
        setProperty("MCDMAMaxReadRequestRequested",mrrs,32);
        setProperty("MCDMAMaxReadRequestApplied",s.hca.transport.max_read_request_applied(),32);
    }
    setProperty("MCDMAUarPageBytes",uint32_t(s.hca.user_queues?16384:4096),32);
    setProperty("MCDMABlueFlameCapable",s.hca.blueflame_capable);
    setProperty("MCDMABlueFlameEnabled",s.hca.blueflame_enabled);
    setProperty("MCDMABlueFlameBufferBytes",s.hca.blueflame_buffer_bytes,32);
    setProperty("MCDMABlueFlameMapOptions",s.hca.transport.blueflame_map_options(),32);
    stage="jumbo-mtu";
    if (!s.hca.configure_ethernet_mtu(9000)) goto failure;
    setProperty("MCDMAEthernetMTU",s.hca.ethernet_mtu,32);
    setProperty("MCDMAFrameAdminMTU",s.hca.frame_admin_mtu,32);
    setProperty("MCDMAFrameOperMTU",s.hca.frame_oper_mtu,32);
    setProperty("MCDMAVportFrameMTU",s.hca.vport_frame_mtu,32);
    stage="address-interface";
    if (!s.network.attach(this,s.hca.mac,s.hca.ethernet_mtu)) goto failure;
    stage="verbs-prepare";
    if (!s.verbs.prepare(s.hca)) goto failure;
    stage="network-bind";
    if (!s.verbs.bind_network(s.network.name())) goto failure;
    stage="native-publish";
    s.interface=cx5_native::publish(this,s.verbs.device(),s.network.name(),false);
    if (!s.interface) goto failure;
    stage="default-gid";
    if (!s.verbs.install_default_gid()) goto failure;
    s.gid=s.verbs.gid_status();
    setProperty("MCDMAGidLive",s.gid.live);
    setProperty("MCDMAGidAdds",s.gid.adds,64);
    setProperty("MCDMAGidDeletes",s.gid.deletes,64);
    setProperty("MCDMAAddressInterface",s.network.name());
    setProperty("MCDMATransport","hardware RoCE v2; polled RC; static IPv6 neighbours");
    setProperty("MCDMANativeBuild",verified_build);
    stage="port-state";
    if (!s.verbs.sample_port(s.active) || !s.network.link(s.active)) goto failure;
    setProperty("MCDMAPortActive",s.active);
    s.interface->registerService();
    s.verbs.dispatch_port_event(s.active);
    timer_->setTimeoutMS(500);
    registerService();
    parent->setProperty("MCDMANativeStartStage","registered");
    parent->removeProperty("MCDMANativeStartError");
    IOLog("MCDMA native: registered %s, physical/GID port %s\n",s.network.name(),s.active?"active":"down");
    return true;
failure:
    // The provider outlives a failed driver instance, preserving the actual
    // startup result even when IOLog is not retained in the kernel log.
    parent->setProperty("MCDMANativeStartStage",stage);
    parent->setProperty("MCDMANativeStartError",uint32_t(s.hca.startup_error),32);
    parent->setProperty("MCDMANativeLastOpcode",s.hca.transport.last.opcode,16);
    parent->setProperty("MCDMANativeTransportError",s.hca.transport.last.transport_error,32);
    parent->setProperty("MCDMANativeFirmwareStatus",s.hca.transport.last.firmware_status,32);
    parent->setProperty("MCDMANativeFirmwareSyndrome",s.hca.transport.last.syndrome,32);
    IOLog("MCDMA native: start failed, command=%04x transport=%u firmware=%u syndrome=%08x\n",
          s.hca.transport.last.opcode,s.hca.transport.last.transport_error,
          s.hca.transport.last.firmware_status,s.hca.transport.last.syndrome);
    cancel_poll();
    if (!cleanup()) retain_failed_state();
    IOService::stop(parent); return false;
}
IOReturn MCDMACX5Native::setProperties(OSObject *properties) {
    // Registry writes arrive on the calling process's thread. Every knob changes
    // behaviour for all users of the device (one rewrites the PCIe Device Control
    // register), so only an administrator (root) process may change them.
    const IOReturn privilege=IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator);
    if (privilege!=kIOReturnSuccess) return kIOReturnNotPrivileged;
    auto *dictionary=OSDynamicCast(OSDictionary,properties);
    if (!dictionary || !state_ || stopping_) return kIOReturnBadArgument;
    auto &s=*state_;
    bool handled=false;
    if (auto *value=dictionary->getObject("MCDMARelaxedOrdering")) {
        if (value!=kOSBooleanTrue && value!=kOSBooleanFalse) return kIOReturnBadArgument;
        s.hca.relaxed_ordering_requested=value==kOSBooleanTrue;
        s.hca.relaxed_ordering_refused=false; s.hca.relaxed_ordering_keys=0;
        setProperty("MCDMARelaxedOrdering",s.hca.relaxed_ordering_requested);
        setProperty("MCDMARelaxedOrderingRefused",false); setProperty("MCDMARelaxedOrderingKeys",uint64_t(0),64);
        handled=true;
    }
    if (auto *value=dictionary->getObject("MCDMAAckRequestEveryPacket")) {
        if (value!=kOSBooleanTrue && value!=kOSBooleanFalse) return kIOReturnBadArgument;
        s.hca.ack_request_every_packet=value==kOSBooleanTrue;
        setProperty("MCDMAAckRequestEveryPacket",s.hca.ack_request_every_packet);
        handled=true;
    }
    if (auto *value=OSDynamicCast(OSNumber,dictionary->getObject("MCDMAMaxReadRequestBytes"))) {
        const uint32_t mrrs=value->unsigned32BitValue();
        char path[512];
        if (!s.hca.transport.configure_pcie(mrrs,path,sizeof(path))) return kIOReturnUnsupported;
        setProperty("MCDMAPCIePath",path);
        setProperty("MCDMAMaxReadRequestRequested",mrrs,32);
        setProperty("MCDMAMaxReadRequestApplied",s.hca.transport.max_read_request_applied(),32);
        handled=true;
    }
    return handled ? kIOReturnSuccess : kIOReturnUnsupported;
}
void MCDMACX5Native::poll(OSObject *owner,IOTimerEventSource *timer) {
    auto *self=OSDynamicCast(MCDMACX5Native,owner);
    if (!self || self->stopping_ || !self->state_) return;
    auto &s=*self->state_; bool active=false;
    if (!s.verbs.sample_port(active)) active=false;
    // After a link flap the port is up again but its default GID is gone; re-install it, and
    // the next poll reports the port active. Outside the provider locks, as at start.
    if (!active && s.verbs.needs_default_gid()) s.verbs.install_default_gid();
    const auto gid=s.verbs.gid_status();
    if (gid.live!=s.gid.live || gid.adds!=s.gid.adds || gid.deletes!=s.gid.deletes) {
        s.gid=gid;
        self->setProperty("MCDMAGidLive",gid.live);
        self->setProperty("MCDMAGidAdds",gid.adds,64);
        self->setProperty("MCDMAGidDeletes",gid.deletes,64);
    }
    if (active!=s.active) {
        s.active=active; s.network.link(active);
        self->setProperty("MCDMAPortActive",active);
        s.verbs.dispatch_port_event(active);
    }
    if (s.hca.relaxed_ordering_requested) {
        self->setProperty("MCDMARelaxedOrderingRefused",s.hca.relaxed_ordering_refused);
        self->setProperty("MCDMARelaxedOrderingKeys",s.hca.relaxed_ordering_keys,64);
    }
    // PCIe counters once a second. A failed query is not retried: firmware
    // without the register would otherwise refuse a command every second.
    if (!s.pcie_counters_refused && (++s.pcie_polls&1)==0) {
        cx5_native::Hca::PcieCounters counters{};
        bool sampled=false;
        if (!s.verbs.sample_pcie_counters(counters,sampled)) {
            s.pcie_counters_refused=true;
            char text[96];
            snprintf(text,sizeof(text),"refused: status %u syndrome %08x",
                     s.hca.transport.last.firmware_status,s.hca.transport.last.syndrome);
            self->setProperty("MCDMAPcieCounters",text);
        } else if (sampled) {
            self->setProperty("MCDMAPcieCounters","sampled every second");
            self->setProperty("MCDMAPcieOutboundStalledReads",counters.stalled_reads,32);
            self->setProperty("MCDMAPcieOutboundStalledWrites",counters.stalled_writes,32);
            self->setProperty("MCDMAPcieOutboundStalledReadSeconds",counters.stalled_reads_events,32);
            self->setProperty("MCDMAPcieOutboundStalledWriteSeconds",counters.stalled_writes_events,32);
            self->setProperty("MCDMAPcieRxErrors",counters.rx_errors,32);
            self->setProperty("MCDMAPcieTxErrors",counters.tx_errors,32);
            self->setProperty("MCDMAPcieCrcErrorsDllp",counters.crc_error_dllp,32);
            self->setProperty("MCDMAPcieCrcErrorsTlp",counters.crc_error_tlp,32);
        }
    }
    timer->setTimeoutMS(500);
}
IOReturn MCDMACX5Native::cancel(OSObject *owner,void *,void *,void *,void *) {
    auto *self=OSDynamicCast(MCDMACX5Native,owner);
    if (!self) return kIOReturnBadArgument;
    self->stopping_=true;
    if (self->timer_) { self->timer_->cancelTimeout(); self->timer_->disable(); }
    return kIOReturnSuccess;
}
void MCDMACX5Native::cancel_poll() {
    if (workloop_) workloop_->runAction(cancel,this);
    else stopping_=true;
    if (timer_attached_) { workloop_->removeEventSource(timer_); timer_attached_=false; }
}
bool MCDMACX5Native::willTerminate(IOService *parent,IOOptionBits options) {
    setProperty("MCDMANativeStopStage","will-terminate");
    cancel_poll();
    if (state_ && state_->verbs.device()) {
        state_->verbs.quiesce(); state_->network.link(false);
        if (state_->interface) state_->verbs.dispatch_port_event(false);
        setProperty("MCDMAPortActive",false);
    }
    return IOService::willTerminate(parent,options);
}
bool MCDMACX5Native::didTerminate(IOService *parent,IOOptionBits options,bool *defer) {
    // IOService schedules stop only after the client closes its provider.
    // Closing solely from stop leaves a removed PCI tunnel waiting forever.
    // Finish core/DMA teardown first: an exclusive-open release must never
    // hand a live HCA or an uncertain DMA address to the next driver.
    cancel_poll();
    setProperty("MCDMANativeStopStage","did-terminate");
    if (!cleanup()) {
        retain_failed_state();
        *defer=true;
    } else {
        setProperty("MCDMANativeStopStage","provider-closed");
    }
    return IOService::didTerminate(parent,options,defer);
}
bool MCDMACX5Native::cleanup() {
    if (!state_) return true;
    auto &s=*state_;
    if (s.verbs.device()) s.verbs.quiesce();
    if (s.interface) {
        setProperty("MCDMANativeStopStage","unpublish");
        // Apple's quiesce stops netdev callbacks, then unregister waits for
        // core users; never deallocate while clients still hold the device.
        if (!cx5_native::unpublish(s.interface)) return false;
        s.interface=nullptr;
    }
    setProperty("MCDMANativeStopStage","verbs-dispose");
    if (s.verbs.device() && (!s.verbs.reclaim_orphans() || !s.verbs.dispose())) return false;
    setProperty("MCDMANativeStopStage","network-detach");
    if (!s.network.detach()) return false;
    setProperty("MCDMANativeStopStage","hca-stop");
    if (!s.hca.stop() || s.hca.transport.close()) return false;
    delete state_; state_=nullptr; return true;
}
void MCDMACX5Native::retain_failed_state() {
    // A failed firmware command can leave a live DMA address. Keep the
    // driver, its executable and every mapping resident rather than guessing
    // that the device stopped. Recovery requires removal or reboot.
    if (!retained_) { retain(); retained_=true; }
    setProperty("MCDMAQuarantined",true);
    IOLog("MCDMA native: retained unresolved core/DMA ownership; unload is blocked\n");
}
void MCDMACX5Native::stop(IOService *parent) {
    cancel_poll();
    if (!cleanup()) retain_failed_state();
    else setProperty("MCDMANativeStopStage","stopped");
    IOService::stop(parent);
}
void MCDMACX5Native::free() {
    if (timer_) { timer_->release(); timer_=nullptr; }
    if (workloop_) { workloop_->release(); workloop_=nullptr; }
    IOService::free();
}
