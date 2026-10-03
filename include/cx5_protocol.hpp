// Original project implementation; hardware references are in PROVENANCE.md.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace cx5 {
constexpr uint32_t pci_identity = 0x101915b3;
constexpr uint32_t pci_identity_non_ex = 0x101715b3;   // ConnectX-5 without Ex: the same command interface
constexpr size_t command_bytes = 64;
constexpr size_t mailbox_data_bytes = 512;
constexpr size_t mailbox_record_bytes = 576;
constexpr size_t mailbox_stride = 1024;
constexpr size_t max_command_bytes = 8192;
constexpr size_t max_mailboxes = 16;

enum class Error : uint32_t {
    none, wrong_device, removed, initializing, command_revision,
    queue_geometry, invalid_length, invalid_token, busy, poisoned,
    delivery, firmware, token_mismatch, invalid_address, bad_mailbox, timeout
};
struct Registers {
    uint32_t identity, revision, firmware, interface_version, queue_geometry, initializing;
};
struct Device {
    uint16_t major, minor, patch, command_revision;
    uint8_t pci_revision, log_slots, log_stride;
};
uint32_t read_be32(const uint8_t *p);
void write_be32(uint8_t *p, uint32_t value);
uint64_t read_be64(const uint8_t *p);
void write_be64(uint8_t *p, uint64_t value);
size_t mailbox_count(size_t bytes);
Error encode_command(uint8_t *entry, const uint8_t *input, size_t input_size,
                     size_t output_size, uint8_t token, uint64_t input_dma,
                     uint64_t output_dma);
Error prepare_mailboxes(uint8_t *blocks, size_t capacity, uint64_t dma,
                        const uint8_t *payload, size_t bytes, uint8_t token);
Error collect_mailboxes(const uint8_t *blocks, size_t capacity, uint64_t dma,
                        uint8_t *payload, size_t bytes, uint8_t token);
Error inspect(const Registers &raw, Device &device);
// This first codec deliberately handles only commands whose input and output
// fit entirely in the entry; mailbox chaining is a separate implementation step.
Error encode_inline(uint8_t *entry, const uint8_t *input, size_t input_size,
                    size_t output_size, uint8_t token);
Error decode_inline(const uint8_t *entry, uint8_t token, uint8_t *output,
                    size_t output_size, uint8_t &delivery, uint8_t &firmware);

class CommandLease {
public:
    Error begin(uint8_t token);
    Error finish(uint8_t token);
    void timeout();
    bool releasable() const { return state_ == State::idle; }
    bool poisoned() const { return state_ == State::quarantined; }
private:
    enum class State { idle, submitted, quarantined };
    State state_ = State::idle;
    uint8_t token_ = 0;
};
}
