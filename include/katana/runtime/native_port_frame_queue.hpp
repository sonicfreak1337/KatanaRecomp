#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace katana::runtime {

inline constexpr std::uint32_t native_port_frame_queue_contract_version = 2u;
inline constexpr std::size_t native_port_frame_queue_depth = 2u;
inline constexpr std::uint32_t native_port_frame_payload_max_alignment = 64u;

// Zero is reserved as the exhausted-sequence sentinel. The queue admits
// UINT64_MAX as its final frame sequence and then fails further production
// closed instead of wrapping to one.
[[nodiscard]] constexpr std::uint64_t native_port_frame_queue_next_sequence(
    const std::uint64_t position) noexcept {
    return position == std::numeric_limits<std::uint64_t>::max()
               ? 0u
               : position + 1u;
}

[[nodiscard]] constexpr bool
native_port_render_thread_kill_switch_disables(
    const std::string_view value) noexcept {
    return value == "1";
}

// Sampled once per process. The queue remains completely dormant while this
// returns false, leaving the existing serial presentation path authoritative.
[[nodiscard]] bool native_port_render_thread_enabled() noexcept;

enum class NativePortFrameQueueLifecycle : std::uint8_t {
    Disabled,
    Running,
    Draining,
    Stopped,
    Failed,
};

enum class NativePortFrameQueueError : std::uint8_t {
    None,
    ProducerThreadViolation,
    ConsumerThreadViolation,
    ThreadDomainOverlap,
    ProducerLeaseOverlap,
    ConsumerLeaseOverlap,
    ProducerException,
    ConsumerException,
    ConsumerLeaseAbandoned,
    CommandCapacityExceeded,
    PayloadCapacityExceeded,
    InvalidPayloadAlignment,
    InvalidCommandRange,
    SequenceViolation,
    ShutdownWithOutstandingFrame,
};

// Persistent frame commands contain no host pointers. Consumers resolve all
// variable data through payload_offset/payload_size inside the owning arena.
struct NativePortFrameCommand final {
    std::uint32_t kind = 0u;
    std::uint32_t payload_offset = 0u;
    std::uint32_t payload_size = 0u;
    std::uint32_t flags = 0u;
};

static_assert(std::is_trivially_copyable_v<NativePortFrameCommand>);
static_assert(std::is_standard_layout_v<NativePortFrameCommand>);

struct NativePortFrameQueueConfig final {
    enum class ThreadingMode : std::uint8_t {
        ParallelSpsc,
        SerialReference,
    };

    std::uint32_t maximum_commands_per_frame = 32'768u;
    std::uint32_t maximum_payload_bytes_per_frame = 16u * 1024u * 1024u;
    bool enabled = true;
    // SerialReference remains an explicit diagnostic executor. It uses the
    // identical arena/codec/decoder path on one thread even while the process
    // render-thread kill switch is set; ParallelSpsc retains the strict
    // non-overlapping producer/consumer domain contract.
    ThreadingMode threading_mode = ThreadingMode::ParallelSpsc;
};

// A typed, allocation-free producer/consumer mailbox. Every field is a
// monotone or first-error observation; completed_frames never exceeds
// submitted_frames.
struct NativePortFrameQueueSnapshot final {
    std::uint64_t observation_epoch = 0u;
    NativePortFrameQueueLifecycle lifecycle =
        NativePortFrameQueueLifecycle::Disabled;
    NativePortFrameQueueError first_error = NativePortFrameQueueError::None;
    std::uint64_t first_error_sequence = 0u;
    // Process-local opaque identities. Zero means that domain has never been
    // admitted; a valid SPSC parallel session has two nonzero unequal values.
    std::uint64_t producer_thread_identity = 0u;
    std::uint64_t consumer_thread_identity = 0u;
    // Queue positions are the single authoritative submitted/completed
    // sequences. next_* is zero only after the final UINT64_MAX sequence.
    std::uint64_t producer_queue_position = 0u;
    std::uint64_t consumer_queue_position = 0u;
    std::uint64_t next_producer_sequence = 1u;
    std::uint64_t next_consumer_sequence = 1u;
    std::uint64_t submitted_frames = 0u;
    std::uint64_t completed_frames = 0u;
    std::uint64_t queue_full_rejections = 0u;
    std::uint64_t last_submitted_sequence = 0u;
    std::uint64_t last_completed_sequence = 0u;
};

class NativePortFrameQueue;

class NativePortFrameWriteLease final {
  public:
    NativePortFrameWriteLease() noexcept = default;
    ~NativePortFrameWriteLease();

    NativePortFrameWriteLease(const NativePortFrameWriteLease&) = delete;
    NativePortFrameWriteLease& operator=(const NativePortFrameWriteLease&) =
        delete;
    NativePortFrameWriteLease(NativePortFrameWriteLease&& other) noexcept;
    NativePortFrameWriteLease& operator=(
        NativePortFrameWriteLease&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;

    [[nodiscard]] std::optional<std::uint32_t> append_payload(
        std::span<const std::byte> bytes,
        std::uint32_t alignment = 1u) noexcept;
    [[nodiscard]] bool append_command_reference(
        std::uint32_t kind,
        std::uint32_t payload_offset,
        std::uint32_t payload_size,
        std::uint32_t flags = 0u) noexcept;
    [[nodiscard]] bool append_command(
        std::uint32_t kind,
        std::span<const std::byte> payload,
        std::uint32_t alignment = 1u,
        std::uint32_t flags = 0u) noexcept;

    template <typename Payload>
        requires(std::is_trivially_copyable_v<Payload> &&
                 std::is_standard_layout_v<Payload>)
    [[nodiscard]] bool append_pod_command(
        const std::uint32_t kind,
        const Payload& payload,
        const std::uint32_t flags = 0u) noexcept {
        return append_command(
            kind,
            std::as_bytes(std::span<const Payload>(&payload, 1u)),
            static_cast<std::uint32_t>(alignof(Payload)),
            flags);
    }

    // publish() is the sole visibility boundary. Until it succeeds, consumer
    // code cannot observe any command or payload byte in this lease.
    [[nodiscard]] bool publish() noexcept;
    void abort() noexcept;
    void fail(NativePortFrameQueueError error) noexcept;

  private:
    friend class NativePortFrameQueue;
    NativePortFrameWriteLease(NativePortFrameQueue& queue,
                              std::size_t arena,
                              std::uint64_t sequence) noexcept;
    [[nodiscard]] bool ensure_owner_thread() const noexcept;
    void reset() noexcept;

    NativePortFrameQueue* queue_ = nullptr;
    std::size_t arena_ = 0u;
    std::uint64_t sequence_ = 0u;
    int uncaught_exceptions_ = 0;
    bool active_ = false;
};

class NativePortFrameReadLease final {
  public:
    NativePortFrameReadLease() noexcept = default;
    ~NativePortFrameReadLease();

    NativePortFrameReadLease(const NativePortFrameReadLease&) = delete;
    NativePortFrameReadLease& operator=(const NativePortFrameReadLease&) =
        delete;
    NativePortFrameReadLease(NativePortFrameReadLease&& other) noexcept;
    NativePortFrameReadLease& operator=(
        NativePortFrameReadLease&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;
    [[nodiscard]] std::span<const NativePortFrameCommand> commands()
        const noexcept;
    [[nodiscard]] std::span<const std::byte> payload() const noexcept;
    [[nodiscard]] std::span<const std::byte> command_payload(
        const NativePortFrameCommand& command) const noexcept;

    // The arena becomes producer-reusable only after complete(). A consumer
    // exception or abandoned lease publishes a typed terminal failure instead.
    [[nodiscard]] bool complete() noexcept;
    void fail(NativePortFrameQueueError error =
                  NativePortFrameQueueError::ConsumerException) noexcept;

  private:
    friend class NativePortFrameQueue;
    NativePortFrameReadLease(NativePortFrameQueue& queue,
                             std::size_t arena,
                             std::uint64_t sequence) noexcept;
    [[nodiscard]] bool ensure_owner_thread() const noexcept;
    void reset() noexcept;

    NativePortFrameQueue* queue_ = nullptr;
    std::size_t arena_ = 0u;
    std::uint64_t sequence_ = 0u;
    int uncaught_exceptions_ = 0;
    bool active_ = false;
};

class NativePortFrameQueue final {
  public:
    explicit NativePortFrameQueue(
        const NativePortFrameQueueConfig& config = {});

    // Lifetime contract: the facade owner must request shutdown, let the
    // consumer observe the terminal lifecycle, and join that thread before
    // destroying the queue. The destructor can diagnose outstanding work but
    // cannot make concurrent arena access safe and is never a join substitute.
    ~NativePortFrameQueue();

    NativePortFrameQueue(const NativePortFrameQueue&) = delete;
    NativePortFrameQueue& operator=(const NativePortFrameQueue&) = delete;
    NativePortFrameQueue(NativePortFrameQueue&&) = delete;
    NativePortFrameQueue& operator=(NativePortFrameQueue&&) = delete;

    // Reports whether the parallel facility was configured, including after a
    // terminal lifecycle. Admission is represented by snapshot().lifecycle.
    [[nodiscard]] bool enabled() const noexcept;

    // try_* never blocks. wait_* uses atomic wait/notify at frame granularity;
    // no mutex, callback, allocation, or per-draw synchronization is involved.
    [[nodiscard]] std::optional<NativePortFrameWriteLease>
    try_begin_produce() noexcept;
    [[nodiscard]] std::optional<NativePortFrameWriteLease>
    wait_begin_produce() noexcept;
    [[nodiscard]] std::optional<NativePortFrameReadLease>
    try_begin_consume() noexcept;
    [[nodiscard]] std::optional<NativePortFrameReadLease>
    wait_begin_consume() noexcept;

    // Shutdown is deterministic FIFO drain: no new producer lease is admitted,
    // already published frames remain consumable, and Stopped is published only
    // after the final consumer completion fence.
    void request_shutdown() noexcept;
    void report_producer_error(NativePortFrameQueueError error,
                               std::uint64_t sequence = 0u) noexcept;
    void report_consumer_error(NativePortFrameQueueError error,
                               std::uint64_t sequence = 0u) noexcept;
    [[nodiscard]] NativePortFrameQueueSnapshot snapshot() const noexcept;

  private:
    friend class NativePortFrameWriteLease;
    friend class NativePortFrameReadLease;

    enum class ArenaState : std::uint8_t { Free, Writing, Ready, Reading };
    struct PayloadDeleter final {
        void operator()(std::byte* payload) const noexcept;
    };
    struct Arena final {
        std::unique_ptr<NativePortFrameCommand[]> commands;
        std::unique_ptr<std::byte[], PayloadDeleter> payload;
        std::atomic<ArenaState> state{ArenaState::Free};
        std::uint32_t command_count = 0u;
        std::uint32_t payload_size = 0u;
        std::uint64_t sequence = 0u;
    };

    [[nodiscard]] bool bind_producer_thread() noexcept;
    [[nodiscard]] bool bind_consumer_thread() noexcept;
    [[nodiscard]] bool is_producer_thread() const noexcept;
    [[nodiscard]] bool is_consumer_thread() const noexcept;
    void reject_foreign_write(std::size_t arena,
                              std::uint64_t sequence) noexcept;
    void reject_foreign_read(std::size_t arena,
                             std::uint64_t sequence) noexcept;
    [[nodiscard]] bool has_producer_capacity() const noexcept;
    [[nodiscard]] bool has_consumer_work() const noexcept;
    void notify_state_change() noexcept;
    void maybe_publish_stopped() noexcept;
    void publish_error(NativePortFrameQueueError error,
                       std::uint64_t sequence) noexcept;

    [[nodiscard]] std::optional<std::uint32_t> append_payload(
        std::size_t arena,
        std::uint64_t sequence,
        std::span<const std::byte> bytes,
        std::uint32_t alignment) noexcept;
    [[nodiscard]] bool append_command_reference(
        std::size_t arena,
        std::uint64_t sequence,
        std::uint32_t kind,
        std::uint32_t payload_offset,
        std::uint32_t payload_size,
        std::uint32_t flags) noexcept;
    [[nodiscard]] bool publish_write(std::size_t arena,
                                     std::uint64_t sequence) noexcept;
    void abort_write(std::size_t arena, std::uint64_t sequence) noexcept;
    void fail_write(std::size_t arena,
                    std::uint64_t sequence,
                    NativePortFrameQueueError error) noexcept;

    [[nodiscard]] std::span<const NativePortFrameCommand> commands(
        std::size_t arena,
        std::uint64_t sequence) noexcept;
    [[nodiscard]] std::span<const std::byte> payload(
        std::size_t arena,
        std::uint64_t sequence) noexcept;
    [[nodiscard]] bool complete_read(std::size_t arena,
                                     std::uint64_t sequence) noexcept;
    void fail_read(std::size_t arena,
                   std::uint64_t sequence,
                   NativePortFrameQueueError error) noexcept;

    NativePortFrameQueueConfig config_;
    std::array<Arena, native_port_frame_queue_depth> arenas_;
    std::atomic<NativePortFrameQueueLifecycle> lifecycle_{
        NativePortFrameQueueLifecycle::Disabled};
    std::atomic<std::uint64_t> producer_position_{0u};
    std::atomic<std::uint64_t> consumer_position_{0u};
    std::atomic<std::uint64_t> event_epoch_{0u};
    std::atomic<std::uint64_t> producer_thread_token_{0u};
    std::atomic<std::uint64_t> consumer_thread_token_{0u};
    std::atomic<bool> producer_lease_active_{false};
    std::atomic<bool> consumer_lease_active_{false};
    std::atomic<std::uint64_t> queue_full_rejections_{0u};
    std::atomic<bool> first_error_claimed_{false};
    std::atomic<bool> first_error_ready_{false};
    NativePortFrameQueueError first_error_ =
        NativePortFrameQueueError::None;
    std::uint64_t first_error_sequence_ = 0u;
};

} // namespace katana::runtime
