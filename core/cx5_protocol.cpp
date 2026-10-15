#include "cx5_protocol.hpp"
#include <string.h>

namespace cx5 {
uint32_t read_be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | p[3];
}
void write_be32(uint8_t *p, uint32_t v) {
    for (unsigned n = 0; n < 4; ++n) p[n] = uint8_t(v >> (24 - n * 8));
}
uint64_t read_be64(const uint8_t *p) {
    return (uint64_t(read_be32(p)) << 32) | read_be32(p+4);
}
void write_be64(uint8_t *p, uint64_t v) {
    write_be32(p,uint32_t(v>>32)); write_be32(p+4,uint32_t(v));
}
size_t mailbox_count(size_t bytes) {
    return bytes<=16 ? 0 : (bytes-16+mailbox_data_bytes-1)/mailbox_data_bytes;
}
Error encode_command(uint8_t *e,const uint8_t *in,size_t n,size_t out,uint8_t token,
                     uint64_t input_dma,uint64_t output_dma) {
    if (!e || !in || n<8 || out<8 || n>max_command_bytes || out>max_command_bytes)
        return Error::invalid_length;
    if (!token) return Error::invalid_token;
    if ((n>16 && (!input_dma || (input_dma & (mailbox_stride-1)))) ||
        (out>16 && (!output_dma || (output_dma & (mailbox_stride-1)))))
        return Error::invalid_address;
    uint8_t saved[16]{};
    memcpy(saved,in,n<16 ? n : 16);
    memset(e,0,command_bytes);
    e[0]=7; write_be32(e+4,uint32_t(n));
    write_be64(e+8,n>16 ? input_dma : 0);
    memcpy(e+16,saved,16);
    write_be64(e+48,out>16 ? output_dma : 0);
    write_be32(e+56,uint32_t(out)); e[60]=token; e[63]=1;
    uint8_t signature=0xff;
    for (size_t i=0;i<command_bytes;++i) signature^=e[i];
    e[61]=signature;
    return Error::none;
}
static bool mailbox_extent(size_t bytes,size_t capacity,uint64_t dma) {
    if (!bytes || bytes>max_command_bytes-16 || !dma || (dma & (mailbox_stride-1)))
        return false;
    size_t count=(bytes+mailbox_data_bytes-1)/mailbox_data_bytes;
    return count<=max_mailboxes && capacity>=count*mailbox_stride &&
           dma<=UINT64_MAX-count*mailbox_stride;
}
Error prepare_mailboxes(uint8_t *blocks,size_t capacity,uint64_t dma,
                        const uint8_t *payload,size_t bytes,uint8_t token) {
    if (!blocks || !mailbox_extent(bytes,capacity,dma)) return Error::invalid_length;
    if (!token) return Error::invalid_token;
    const size_t count=(bytes+mailbox_data_bytes-1)/mailbox_data_bytes;
    memset(blocks,0,count*mailbox_stride);
    for (size_t i=0;i<count;++i) {
        auto *b=blocks+i*mailbox_stride;
        size_t n=bytes-i*mailbox_data_bytes;
        if (n>mailbox_data_bytes) n=mailbox_data_bytes;
        if (payload) memcpy(b,payload+i*mailbox_data_bytes,n);
        write_be64(b+0x230,i+1<count ? dma+(i+1)*mailbox_stride : 0);
        write_be32(b+0x238,uint32_t(i)); b[0x23d]=token;
        // PRM table 97 places control after the 512-byte data payload;
        // its older prose offsets 0x1c0..0x1ff contradict that table.
        uint8_t control=0xff;
        for (size_t j=0x200;j<0x23e;++j) control^=b[j];
        b[0x23e]=control;
        uint8_t signature=0xff;
        for (size_t j=0;j<0x23f;++j) signature^=b[j];
        b[0x23f]=signature;
    }
    return Error::none;
}
Error collect_mailboxes(const uint8_t *blocks,size_t capacity,uint64_t dma,
                        uint8_t *payload,size_t bytes,uint8_t token) {
    if (!blocks || !payload || !mailbox_extent(bytes,capacity,dma)) return Error::invalid_length;
    if (!token) return Error::invalid_token;
    const size_t count=(bytes+mailbox_data_bytes-1)/mailbox_data_bytes;
    // Validate the complete chain before copying any result to the caller.
    for (size_t i=0;i<count;++i) {
        const auto *b=blocks+i*mailbox_stride;
        if (read_be64(b+0x230)!=(i+1<count ? dma+(i+1)*mailbox_stride : 0) ||
            read_be32(b+0x238)!=i || b[0x23d]!=token) return Error::bad_mailbox;
    }
    for (size_t i=0;i<count;++i) {
        size_t n=bytes-i*mailbox_data_bytes;
        if (n>mailbox_data_bytes) n=mailbox_data_bytes;
        memcpy(payload+i*mailbox_data_bytes,blocks+i*mailbox_stride,n);
    }
    // Output checksums are not assumed enabled before HCA capabilities are
    // negotiated; pointer, sequence, token and command ownership are checked.
    return Error::none;
}
Error inspect(const Registers &r, Device &d) {
    d = {};
    if (r.identity == UINT32_MAX || r.firmware == UINT32_MAX ||
        r.interface_version == UINT32_MAX || r.queue_geometry == UINT32_MAX ||
        r.initializing == UINT32_MAX) return Error::removed;
    if (r.identity != pci_identity && r.identity != pci_identity_non_ex) return Error::wrong_device;
    if (r.initializing & 0x80000000u) return Error::initializing;
    if ((r.interface_version >> 16) != 5) return Error::command_revision;
    const unsigned slots = (r.queue_geometry >> 4) & 15;
    const unsigned stride = r.queue_geometry & 15;
    // One 4 KiB command page; no more than the 32 doorbell bits, each entry
    // must accommodate the 64-byte command record from the PRM.
    if (slots > 5 || stride < 6 || stride > 12 || slots + stride > 12)
        return Error::queue_geometry;
    d = {uint16_t(r.firmware), uint16_t(r.firmware >> 16),
         uint16_t(r.interface_version), uint16_t(r.interface_version >> 16),
         uint8_t(r.revision), uint8_t(slots), uint8_t(stride)};
    return Error::none;
}
Error encode_inline(uint8_t *e, const uint8_t *in, size_t n, size_t out, uint8_t t) {
    if (!e || !in || n < 8 || n > 16 || out < 8 || out > 16)
        return Error::invalid_length;
    if (!t) return Error::invalid_token;
    // Permit a caller's inline input to overlap the destination record.
    uint8_t saved[16] = {};
    memcpy(saved, in, n);
    memset(e, 0, command_bytes);
    e[0] = 7;
    write_be32(e + 4, uint32_t(n));
    memcpy(e + 16, saved, n);
    write_be32(e + 56, uint32_t(out));
    e[60] = t;
    e[63] = 1;
    uint8_t checksum = 0xff;
    for (size_t i = 0; i < command_bytes; ++i) checksum ^= e[i];
    e[61] = checksum;
    return Error::none;
}
Error decode_inline(const uint8_t *e, uint8_t t, uint8_t *out, size_t n,
                    uint8_t &delivery, uint8_t &fw) {
    delivery = fw = 0;
    if (!e || !out || n < 8 || n > 16) return Error::invalid_length;
    if (!t) return Error::invalid_token;
    if (e[63] & 1) return Error::busy;
    if (e[60] != t) return Error::token_mismatch;
    delivery = uint8_t(e[63] >> 1);
    if (delivery) return Error::delivery;
    if (read_be32(e + 56) != n) return Error::invalid_length;
    fw = e[32];
    if (fw) return Error::firmware;
    memmove(out, e + 32, n);
    return Error::none;
}
Error CommandLease::begin(uint8_t t) {
    if (state_ == State::quarantined) return Error::poisoned;
    if (state_ == State::submitted) return Error::busy;
    if (!t) return Error::invalid_token;
    state_ = State::submitted;
    token_ = t;
    return Error::none;
}
Error CommandLease::finish(uint8_t t) {
    if (state_ == State::quarantined) return Error::poisoned;
    if (state_ != State::submitted) return Error::busy;
    if (t != token_) { state_ = State::quarantined; return Error::token_mismatch; }
    state_ = State::idle;
    return Error::none;
}
void CommandLease::timeout() {
    if (state_ == State::submitted) state_ = State::quarantined;
}
}
