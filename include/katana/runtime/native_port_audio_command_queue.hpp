#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace katana::runtime {

inline constexpr std::uint32_t native_port_audio_command_queue_contract_version =
    1u;
inline constexpr std::uint32_t native_port_audio_command_queue_invalid_ack_slot =
    0xFFFF'FFFFu;
inline constexpr std::size_t native_port_audio_command_queue_max_ack_result_bytes =
    512u;
inline constexpr std::uint32_t
    native_port_audio_command_queue_default_payload_bytes = 64u * 1024u * 1024u;
inline constexpr std::string_view native_port_audio_serial_reference_environment =
    "KATANA_PORT_AUDIO_SERIAL_REFERENCE";

enum class NativePortAudioCommandQueueMode : std::uint8_t {
    DedicatedThread,
    SerialReference,
};

enum class NativePortAudioCommandQueueLifecycle : std::uint8_t {
    Disabled,
    Running,
    Draining,
    Stopped,
    Failed,
};

enum class NativePortAudioCommandQueueFailure : std::uint8_t {
    None,
    InvalidConfig,
    Disabled,
    ProducerThreadViolation,
    ConsumerThreadViolation,
    ProducerLeaseOverlap,
    ConsumerLeaseOverlap,
    ProducerLeaseAbandoned,
    ConsumerLeaseAbandoned,
    StampInitialSequence,
    StampRegression,
    StampGap,
    StampOverflow,
    FrameRegression,
    CommandSequenceExhausted,
    PayloadOverflow,
    PayloadReservationOverflow,
    InvalidAckSlot,
    AckSlotBusy,
    AckResultOverflow,
    InvalidAckStatus,
    WorkerFailure,
    QueueClosed,
    QueueFailed,
    ShutdownWithOutstandingLease,
};

class NativePortAudioCommandQueueError final : public std::runtime_error {
  public:
    NativePortAudioCommandQueueError(
        NativePortAudioCommandQueueFailure failure,
        std::uint64_t command_sequence = 0u);

    [[nodiscard]] NativePortAudioCommandQueueFailure failure() const noexcept;
    [[nodiscard]] std::uint64_t command_sequence() const noexcept;

  private:
    NativePortAudioCommandQueueFailure failure_;
    std::uint64_t command_sequence_ = 0u;
};

struct NativePortAudioCommandStamp final {
    std::uint64_t frame_index = 0u;
    std::uint64_t guest_sequence = 0u;

    [[nodiscard]] constexpr bool operator==(
        const NativePortAudioCommandStamp&) const noexcept = default;
};

enum class NativePortAudioCommandTarget : std::uint8_t {
    AudioEngine,
    SoundBank,
    HostOutput,
    Movie,
    Lifecycle,
};

// This is the only command representation crossing the simulation/audio
// boundary. It contains no pointers, strings or callbacks.
struct NativePortAudioPodCommand final {
    std::uint64_t command_sequence = 0u;
    NativePortAudioCommandStamp stamp{};
    NativePortAudioCommandTarget target =
        NativePortAudioCommandTarget::Lifecycle;
    std::uint16_t opcode = 0u;
    std::uint32_t payload_offset = 0u;
    std::uint32_t payload_size = 0u;
    std::uint32_t ack_slot = native_port_audio_command_queue_invalid_ack_slot;
    std::uint32_t flags = 0u;
    // Opaque domain routing identity. The queue preserves these values; the
    // owning audio facade validates target slot/generation before execution.
    std::uint32_t target_slot = 0u;
    std::uint32_t target_generation = 0u;
};

static_assert(std::is_trivially_copyable_v<NativePortAudioCommandStamp>);
static_assert(std::is_standard_layout_v<NativePortAudioCommandStamp>);
static_assert(std::is_trivially_copyable_v<NativePortAudioPodCommand>);
static_assert(std::is_standard_layout_v<NativePortAudioPodCommand>);

enum class NativePortAudioCommandAckStatus : std::uint8_t {
    Pending,
    Completed,
    Cancelled,
    Failed,
};

struct NativePortAudioCommandAckResult final {
    NativePortAudioCommandAckStatus status =
        NativePortAudioCommandAckStatus::Completed;
    std::array<std::uint8_t, 3u> reserved{};
    std::uint32_t error_code = 0u;
    std::uint32_t result_size = 0u;
    std::array<std::byte, native_port_audio_command_queue_max_ack_result_bytes>
        bytes{};
};

struct NativePortAudioCommandAck final {
    std::uint64_t command_sequence = 0u;
    NativePortAudioCommandStamp stamp{};
    NativePortAudioCommandAckStatus status =
        NativePortAudioCommandAckStatus::Pending;
    std::array<std::uint8_t, 3u> reserved{};
    std::uint32_t error_code = 0u;
    std::uint32_t result_size = 0u;
    std::array<std::byte, native_port_audio_command_queue_max_ack_result_bytes>
        bytes{};
};

static_assert(std::is_trivially_copyable_v<NativePortAudioCommandAckResult>);
static_assert(std::is_standard_layout_v<NativePortAudioCommandAckResult>);
static_assert(std::is_trivially_copyable_v<NativePortAudioCommandAck>);
static_assert(std::is_standard_layout_v<NativePortAudioCommandAck>);

struct NativePortAudioCommandQueueConfig final {
    NativePortAudioCommandQueueMode mode =
        NativePortAudioCommandQueueMode::DedicatedThread;
    std::uint32_t maximum_commands = 1'024u;
    std::uint32_t maximum_payload_bytes =
        native_port_audio_command_queue_default_payload_bytes;
    std::uint32_t maximum_ack_slots = 1'024u;
    bool enabled = true;
};

struct NativePortAudioCommandQueueSnapshot final {
    NativePortAudioCommandQueueLifecycle lifecycle =
        NativePortAudioCommandQueueLifecycle::Disabled;
    NativePortAudioCommandQueueMode mode =
        NativePortAudioCommandQueueMode::DedicatedThread;
    NativePortAudioCommandQueueFailure first_error =
        NativePortAudioCommandQueueFailure::None;
    std::uint64_t first_error_command_sequence = 0u;

    std::uint64_t producer_thread_identity = 0u;
    std::uint64_t consumer_thread_identity = 0u;
    std::uint64_t submitted_commands = 0u;
    std::uint64_t completed_commands = 0u;
    std::uint64_t cancelled_commands = 0u;
    std::uint64_t failed_commands = 0u;
    std::uint64_t submitted_payload_bytes = 0u;
    std::uint64_t completed_payload_bytes = 0u;
    std::uint64_t cancelled_payload_bytes = 0u;
    std::uint64_t queued_commands = 0u;
    std::uint64_t queued_payload_bytes = 0u;
    std::uint64_t queue_full_waits = 0u;
    std::uint64_t ack_waits = 0u;
    std::uint64_t published_acks = 0u;
    std::uint64_t consumed_acks = 0u;
    std::uint64_t next_command_sequence = 1u;
    std::uint64_t last_submitted_command_sequence = 0u;
    std::uint64_t last_completed_command_sequence = 0u;
    bool has_last_submitted_stamp = false;
    bool has_last_completed_stamp = false;
    NativePortAudioCommandStamp last_submitted_stamp{};
    NativePortAudioCommandStamp last_completed_stamp{};
};

class NativePortAudioCommandQueue;

class NativePortAudioCommandProducerLease final {
  public:
    NativePortAudioCommandProducerLease() noexcept = default;
    ~NativePortAudioCommandProducerLease();
    NativePortAudioCommandProducerLease(
        const NativePortAudioCommandProducerLease&) = delete;
    NativePortAudioCommandProducerLease& operator=(
        const NativePortAudioCommandProducerLease&) = delete;
    NativePortAudioCommandProducerLease(
        NativePortAudioCommandProducerLease&& other) noexcept;
    NativePortAudioCommandProducerLease& operator=(
        NativePortAudioCommandProducerLease&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint64_t command_sequence() const noexcept;
    [[nodiscard]] const NativePortAudioPodCommand& command() const noexcept;
    [[nodiscard]] std::span<std::byte> payload() noexcept;
    [[nodiscard]] bool copy_payload(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] bool publish() noexcept;
    void abort() noexcept;

  private:
    friend class NativePortAudioCommandQueue;
    NativePortAudioCommandProducerLease(NativePortAudioCommandQueue& queue,
                                        std::uint32_t slot,
                                        std::uint64_t command_sequence = 0u) noexcept;
    void reset() noexcept;

    NativePortAudioCommandQueue* queue_ = nullptr;
    std::uint32_t slot_ = 0u;
    std::uint64_t command_sequence_ = 0u;
    bool active_ = false;
};

class NativePortAudioCommandConsumerLease final {
  public:
    NativePortAudioCommandConsumerLease() noexcept = default;
    ~NativePortAudioCommandConsumerLease();
    NativePortAudioCommandConsumerLease(
        const NativePortAudioCommandConsumerLease&) = delete;
    NativePortAudioCommandConsumerLease& operator=(
        const NativePortAudioCommandConsumerLease&) = delete;
    NativePortAudioCommandConsumerLease(
        NativePortAudioCommandConsumerLease&& other) noexcept;
    NativePortAudioCommandConsumerLease& operator=(
        NativePortAudioCommandConsumerLease&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool cancelled() const noexcept;
    [[nodiscard]] const NativePortAudioPodCommand& command() const noexcept;
    [[nodiscard]] std::span<const std::byte> payload() const noexcept;
    [[nodiscard]] bool complete(
        const NativePortAudioCommandAckResult& result = {}) noexcept;
    [[nodiscard]] bool cancel(std::uint32_t error_code = 0u) noexcept;
    [[nodiscard]] bool fail(std::uint32_t error_code = 0u) noexcept;

  private:
    friend class NativePortAudioCommandQueue;
    NativePortAudioCommandConsumerLease(NativePortAudioCommandQueue& queue,
                                        std::uint32_t slot,
                                        std::uint64_t command_sequence,
                                        bool cancelled) noexcept;
    void reset() noexcept;

    NativePortAudioCommandQueue* queue_ = nullptr;
    std::uint32_t slot_ = 0u;
    std::uint64_t command_sequence_ = 0u;
    bool cancelled_ = false;
    bool active_ = false;
};

// Fixed-POD one-producer/one-consumer transport. Command slots, the byte
// payload slab and acknowledgement slots are allocated once at construction.
// No worker, callback, mutex, heap operation or command execution occurs in
// the queue. The owning audio facade consumes leases on its chosen thread.
class NativePortAudioCommandQueue final {
  public:
    explicit NativePortAudioCommandQueue(
        const NativePortAudioCommandQueueConfig& config = {});
    ~NativePortAudioCommandQueue();

    NativePortAudioCommandQueue(const NativePortAudioCommandQueue&) = delete;
    NativePortAudioCommandQueue& operator=(
        const NativePortAudioCommandQueue&) = delete;
    NativePortAudioCommandQueue(NativePortAudioCommandQueue&&) = delete;
    NativePortAudioCommandQueue& operator=(NativePortAudioCommandQueue&&) =
        delete;

    [[nodiscard]] NativePortAudioCommandQueueMode mode() const noexcept;
    [[nodiscard]] std::optional<NativePortAudioCommandProducerLease>
    try_begin_produce(const NativePortAudioPodCommand& command);
    [[nodiscard]] std::optional<NativePortAudioCommandProducerLease>
    wait_begin_produce(const NativePortAudioPodCommand& command);
    [[nodiscard]] std::optional<NativePortAudioCommandConsumerLease>
    try_begin_consume();
    [[nodiscard]] std::optional<NativePortAudioCommandConsumerLease>
    wait_begin_consume();
    // The owning consumer reports an unrecoverable worker failure. This never
    // executes or cancels the currently leased command; the owner must finish
    // that lease explicitly so its exact-once accounting remains intact.
    void fail_terminal(NativePortAudioCommandQueueFailure failure,
                       std::uint64_t command_sequence = 0u) noexcept;
    [[nodiscard]] std::optional<NativePortAudioCommandAck>
    try_read_ack(std::uint32_t ack_slot, std::uint64_t command_sequence);
    [[nodiscard]] std::optional<NativePortAudioCommandAck>
    wait_read_ack(std::uint32_t ack_slot, std::uint64_t command_sequence);
    void request_shutdown() noexcept;
    [[nodiscard]] NativePortAudioCommandQueueSnapshot
    snapshot() const noexcept;

  private:
    friend class NativePortAudioCommandProducerLease;
    friend class NativePortAudioCommandConsumerLease;

    enum class SlotState : std::uint8_t {
        Free,
        Writing,
        Ready,
        Reading,
        Cancelled,
    };
    enum class AckSlotState : std::uint8_t {
        Free,
        Pending,
        Completed,
        Cancelled,
        Failed,
    };
    struct CommandSlot;
    struct AckSlot;

    [[nodiscard]] std::optional<NativePortAudioCommandProducerLease>
    try_begin_produce_impl(const NativePortAudioPodCommand& command);
    [[nodiscard]] std::optional<NativePortAudioCommandConsumerLease>
    try_begin_consume_impl();
    [[nodiscard]] bool publish_producer(std::uint32_t slot,
                                        std::uint64_t command_sequence) noexcept;
    void abort_producer(std::uint32_t slot,
                        std::uint64_t command_sequence) noexcept;
    [[nodiscard]] bool finish_consumer(
        std::uint32_t slot,
        std::uint64_t command_sequence,
        const NativePortAudioCommandAckResult& result) noexcept;
    [[nodiscard]] bool finish_consumer_cancelled(
        std::uint32_t slot,
        std::uint64_t command_sequence,
        NativePortAudioCommandAckStatus status,
        std::uint32_t error_code) noexcept;
    [[nodiscard]] bool finalize_consumer(
        std::uint32_t slot,
        std::uint64_t command_sequence,
        const NativePortAudioCommandAckResult& result) noexcept;
    void abandon_consumer(std::uint32_t slot,
                          std::uint64_t command_sequence) noexcept;
    [[nodiscard]] bool producer_thread_allowed() noexcept;
    [[nodiscard]] bool consumer_thread_allowed() noexcept;
    void publish_error(NativePortAudioCommandQueueFailure failure,
                       std::uint64_t command_sequence) noexcept;
    void cancel_ready_commands() noexcept;
    [[nodiscard]] std::span<std::byte> writable_payload(
        std::uint32_t slot) noexcept;
    [[nodiscard]] std::span<const std::byte> readable_payload(
        std::uint32_t slot) const noexcept;
    [[nodiscard]] const NativePortAudioPodCommand& slot_command(
        std::uint32_t slot) const noexcept;
    [[nodiscard]] std::optional<NativePortAudioCommandAck> read_ack(
        std::uint32_t ack_slot,
        std::uint64_t command_sequence,
        bool wait);

    NativePortAudioCommandQueueConfig config_{};
    std::unique_ptr<CommandSlot[]> commands_;
    std::unique_ptr<std::byte[]> payload_;
    std::unique_ptr<AckSlot[]> acks_;
    std::atomic<NativePortAudioCommandQueueLifecycle> lifecycle_{
        NativePortAudioCommandQueueLifecycle::Disabled};
    std::atomic<std::uint8_t> first_error_{
        static_cast<std::uint8_t>(NativePortAudioCommandQueueFailure::None)};
    std::atomic<std::uint64_t> first_error_command_sequence_{0u};
    std::atomic<std::uint64_t> producer_thread_identity_{0u};
    std::atomic<std::uint64_t> consumer_thread_identity_{0u};
    std::atomic<std::uint64_t> producer_position_{0u};
    std::atomic<std::uint64_t> consumer_position_{0u};
    std::atomic<std::uint64_t> producer_payload_position_{0u};
    std::atomic<std::uint64_t> consumer_payload_position_{0u};
    std::atomic<std::uint64_t> next_command_sequence_{1u};
    std::atomic<std::uint64_t> last_submitted_command_sequence_{0u};
    std::atomic<std::uint64_t> last_completed_command_sequence_{0u};
    std::atomic<std::uint64_t> last_submitted_frame_{0u};
    std::atomic<std::uint64_t> last_submitted_guest_sequence_{0u};
    std::atomic<std::uint64_t> last_completed_frame_{0u};
    std::atomic<std::uint64_t> last_completed_guest_sequence_{0u};
    std::atomic<bool> has_last_submitted_stamp_{false};
    std::atomic<bool> has_last_completed_stamp_{false};
    std::atomic<bool> producer_lease_active_{false};
    std::atomic<bool> consumer_lease_active_{false};
    std::atomic<bool> first_error_claimed_{false};
    std::atomic<bool> first_error_published_{false};
    std::atomic<std::uint64_t> submitted_commands_{0u};
    std::atomic<std::uint64_t> completed_commands_{0u};
    std::atomic<std::uint64_t> cancelled_commands_{0u};
    std::atomic<std::uint64_t> failed_commands_{0u};
    std::atomic<std::uint64_t> submitted_payload_bytes_{0u};
    std::atomic<std::uint64_t> completed_payload_bytes_{0u};
    std::atomic<std::uint64_t> cancelled_payload_bytes_{0u};
    std::atomic<std::uint64_t> queue_full_waits_{0u};
    std::atomic<std::uint64_t> ack_waits_{0u};
    std::atomic<std::uint64_t> published_acks_{0u};
    std::atomic<std::uint64_t> consumed_acks_{0u};
    std::atomic<std::uint64_t> event_epoch_{0u};
};

[[nodiscard]] bool native_port_audio_serial_reference_requested(
    std::string_view value) noexcept;
[[nodiscard]] bool native_port_audio_serial_reference_requested() noexcept;

} // namespace katana::runtime
