#pragma once

#include "katana/runtime/native_port_audio_command_queue.hpp"

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

// The execution domain is the process-wide owner of the audio transport.  A
// client facade may own a title-specific Core, but it never owns a second
// queue or a second worker.  Keep this contract independent from the public
// product/profile version; the parent runtime owns that version bump.
inline constexpr std::uint32_t
    native_port_audio_execution_domain_contract_version = 1u;

// The queue now has an explicit Movie tag, so the domain deliberately uses
// that same target identity instead of creating a second wire-level enum.
// Lifecycle remains a queue-internal sentinel and is outside the registry.
using NativePortAudioExecutionDomainTarget = NativePortAudioCommandTarget;

// A registration handle is the only identity a facade needs after it has
// registered an endpoint.  It is deliberately a value type: it can be copied
// into a facade, command-builder state, or a bounded teardown record without
// carrying a pointer or owning anything.  Generation is never zero and is
// changed every time a fixed slot is reused, so an old command cannot execute
// against a newly registered object.
struct NativePortAudioExecutionDomainTargetHandle final {
    NativePortAudioExecutionDomainTarget target =
        NativePortAudioExecutionDomainTarget::AudioEngine;
    std::uint32_t slot = 0u;
    std::uint32_t generation = 0u;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return generation != 0u;
    }
};

static_assert(std::is_trivially_copyable_v<
              NativePortAudioExecutionDomainTargetHandle>);
static_assert(std::is_standard_layout_v<
              NativePortAudioExecutionDomainTargetHandle>);

inline constexpr std::size_t native_port_audio_execution_domain_target_count =
    4u;
inline constexpr std::uint32_t
    native_port_audio_execution_domain_max_slots_per_target = 16u;
inline constexpr std::size_t native_port_audio_execution_domain_slot_count =
    native_port_audio_execution_domain_target_count *
    native_port_audio_execution_domain_max_slots_per_target;

enum class NativePortAudioExecutionDomainFailure : std::uint8_t {
    None,
    InvalidConfig,
    ConfigMismatch,
    Disabled,
    InvalidTarget,
    InvalidFlags,
    TargetAlreadyRegistered,
    TargetNotRegistered,
    TargetIdentityMismatch,
    ExecutorMissing,
    ProducerThreadViolation,
    ConsumerThreadViolation,
    QueueFull,
    QueueFailed,
    QueueClosed,
    SequenceBusy,
    InvalidPayload,
    ScatterOnWorker,
    WorkerFailure,
    StampInitialSequence,
    StampRegression,
    StampGap,
    StampOverflow,
    FrameRegression,
    AckSlotBusy,
    AckMissing,
    TargetExecutionFailed,
    Shutdown,
};

class NativePortAudioExecutionDomainError final : public std::runtime_error {
  public:
    NativePortAudioExecutionDomainError(
        NativePortAudioExecutionDomainFailure failure,
        NativePortAudioCommandQueueFailure queue_failure =
            NativePortAudioCommandQueueFailure::None,
        std::uint64_t command_sequence = 0u);

    [[nodiscard]] NativePortAudioExecutionDomainFailure failure() const noexcept;
    [[nodiscard]] NativePortAudioCommandQueueFailure queue_failure() const noexcept;
    [[nodiscard]] std::uint64_t command_sequence() const noexcept;

  private:
    NativePortAudioExecutionDomainFailure failure_;
    NativePortAudioCommandQueueFailure queue_failure_;
    std::uint64_t command_sequence_ = 0u;
};

using NativePortAudioExecutionDomainExecutor = void (*)(
    void* target,
    std::uint16_t opcode,
    std::span<const std::byte> payload,
    NativePortAudioCommandAckResult& result) noexcept;

// If a target cannot acknowledge its Destroy command because the shared
// domain has entered a terminal state, the domain invokes this callback on
// the owning consumer before releasing the target slot.  It is deliberately
// separate from the command executor: cleanup must not depend on another
// queue publication and must never destroy worker-owned state on the
// producer thread.
using NativePortAudioExecutionDomainCleanup = void (*)(void* target) noexcept;

enum class NativePortAudioExecutionDomainStage : std::uint8_t {
    None = 0u,
    AudioDecode = 1u,
    AudioMix = 2u,
    // A single product command may perform bounded codec work and mixing
    // before it submits its owning output buffer.  Keep this as one broad
    // command classification; the worker records both timers around the
    // same executor invocation and never times individual samples.
    AudioDecodeAndMix = 3u,
};

// These bits are encoded in every domain command.  Caller-owned flags must
// stay below bit 24; the domain adds the broad stage classification here so
// the worker can instrument a whole target command, never individual samples.
inline constexpr std::uint32_t
    native_port_audio_execution_domain_stage_shift = 24u;
inline constexpr std::uint32_t
    native_port_audio_execution_domain_stage_mask = 0x0300'0000u;
inline constexpr std::uint32_t
    native_port_audio_execution_domain_user_flags_mask =
        0x00FF'FFFFu;

struct NativePortAudioExecutionDomainPayloadPart final {
    const std::byte* data = nullptr;
    std::uint32_t size = 0u;
};

static_assert(std::is_trivially_copyable_v<
              NativePortAudioExecutionDomainPayloadPart>);
static_assert(std::is_standard_layout_v<
              NativePortAudioExecutionDomainPayloadPart>);

struct NativePortAudioExecutionDomainConfig final {
    NativePortAudioCommandQueueConfig command_queue{};
};

struct NativePortAudioExecutionDomainDispatchResult final {
    NativePortAudioExecutionDomainFailure failure =
        NativePortAudioExecutionDomainFailure::None;
    NativePortAudioCommandQueueFailure queue_failure =
        NativePortAudioCommandQueueFailure::None;
    std::uint64_t command_sequence = 0u;
    NativePortAudioCommandStamp stamp{};
    bool has_ack = false;
    bool inline_execution = false;
    NativePortAudioCommandAck ack{};

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return failure == NativePortAudioExecutionDomainFailure::None;
    }
    [[nodiscard]] constexpr bool completed() const noexcept {
        return accepted() && (!has_ack ||
                              ack.status ==
                                  NativePortAudioCommandAckStatus::Completed);
    }
};

struct NativePortAudioExecutionDomainTargetSnapshot final {
    NativePortAudioExecutionDomainTarget target =
        NativePortAudioExecutionDomainTarget::AudioEngine;
    std::uint32_t target_slot = 0u;
    std::uint32_t target_generation = 0u;
    bool registered = false;
    std::uint32_t active_dispatches = 0u;
    std::uint64_t executed_commands = 0u;
    std::uint64_t failed_commands = 0u;
};

struct NativePortAudioExecutionDomainSnapshot final {
    NativePortAudioCommandQueueSnapshot queue{};
    NativePortAudioExecutionDomainFailure first_error =
        NativePortAudioExecutionDomainFailure::None;
    std::uint64_t first_error_command_sequence = 0u;
    std::uint64_t producer_thread_identity = 0u;
    std::uint64_t consumer_thread_identity = 0u;
    std::uint64_t next_guest_sequence = 0u;
    std::uint64_t last_frame_index = 0u;
    bool has_last_frame_index = false;
    bool worker_alive = false;
    bool shutdown_requested = false;
    std::array<NativePortAudioExecutionDomainTargetSnapshot,
                native_port_audio_execution_domain_slot_count>
        targets{};
};

class NativePortAudioExecutionDomain final {
    class Impl;

  public:
    NativePortAudioExecutionDomain(
        const NativePortAudioExecutionDomain&) = delete;
    NativePortAudioExecutionDomain& operator=(
        const NativePortAudioExecutionDomain&) = delete;
    NativePortAudioExecutionDomain(NativePortAudioExecutionDomain&&) = delete;
    NativePortAudioExecutionDomain& operator=(
        NativePortAudioExecutionDomain&&) = delete;
    ~NativePortAudioExecutionDomain();

    // The returned shared object is process-wide.  All audio facades acquire
    // this same owner; a second queue/worker is never constructed for a
    // compatible acquire.  Config mismatches are rejected before dispatch.
    [[nodiscard]] static std::shared_ptr<NativePortAudioExecutionDomain>
    acquire(const NativePortAudioExecutionDomainConfig& config = {});

    [[nodiscard]] NativePortAudioCommandQueueMode mode() const noexcept;

    // A move-only producer reservation writes directly into the queue's
    // preallocated payload slab.  It is intended for bounded AICA snapshots
    // and other bulk PCM producers: no temporary buffer, callback, mutex, or
    // allocation is introduced between begin and publish.  Abort abandons
    // the reservation and does not consume a guest sequence.
    class ProducerLease final {
      public:
        ProducerLease() noexcept = default;
        ~ProducerLease();
        ProducerLease(const ProducerLease&) = delete;
        ProducerLease& operator=(const ProducerLease&) = delete;
        ProducerLease(ProducerLease&& other) noexcept;
        ProducerLease& operator=(ProducerLease&& other) noexcept;

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] std::span<std::byte> payload() noexcept;
        [[nodiscard]] const NativePortAudioCommandStamp& stamp() const noexcept;
        [[nodiscard]] std::uint64_t command_sequence() const noexcept;
        [[nodiscard]] const NativePortAudioExecutionDomainDispatchResult&
        result() const noexcept;
        [[nodiscard]] bool publish() noexcept;
        void abort() noexcept;

      private:
        friend class NativePortAudioExecutionDomain;
        friend class NativePortAudioExecutionDomain::Impl;
        ProducerLease(
            NativePortAudioExecutionDomain* domain,
            NativePortAudioCommandProducerLease&& queue_lease,
            NativePortAudioCommandStamp stamp,
            NativePortAudioExecutionDomainDispatchResult result) noexcept;

        NativePortAudioExecutionDomain* domain_ = nullptr;
        NativePortAudioCommandProducerLease queue_lease_;
        NativePortAudioCommandStamp stamp_{};
        NativePortAudioExecutionDomainDispatchResult result_{};
        bool active_ = false;
    };

    using NativePortAudioExecutionDomainProducerLease = ProducerLease;

    [[nodiscard]] ProducerLease begin_async_payload(
        const NativePortAudioExecutionDomainTargetHandle& handle,
        std::uint16_t opcode,
        std::uint32_t payload_size,
        std::uint64_t frame_index,
        std::uint32_t flags = 0u,
        NativePortAudioExecutionDomainStage stage =
            NativePortAudioExecutionDomainStage::None) noexcept;

    // Executor pointers are lifecycle state, deliberately kept outside POD
    // command slots.  Register before sending a target's explicit construct
    // command and unregister only after its explicit destroy command has been
    // synchronously acknowledged.
    [[nodiscard]] bool register_target(
        NativePortAudioExecutionDomainTarget target,
        std::uint32_t target_slot,
        std::uint32_t target_generation,
        void* object,
        NativePortAudioExecutionDomainExecutor executor,
        NativePortAudioExecutionDomainCleanup cleanup = nullptr) noexcept;
    // Select a free fixed slot and allocate its next non-zero generation.
    // std::optional is allocation-free; registration is a lifecycle
    // operation and is never performed by the command/sample hotpath.
    [[nodiscard]] std::optional<
        NativePortAudioExecutionDomainTargetHandle>
    register_target(
        NativePortAudioExecutionDomainTarget target,
        void* object,
        NativePortAudioExecutionDomainExecutor executor,
        NativePortAudioExecutionDomainCleanup cleanup = nullptr) noexcept;
    [[nodiscard]] bool unregister_target(
        const NativePortAudioExecutionDomainTargetHandle& handle,
        void* object) noexcept;
    [[nodiscard]] bool unregister_target(
        NativePortAudioExecutionDomainTarget target,
        std::uint32_t target_slot,
        std::uint32_t target_generation,
        void* object) noexcept;
    [[nodiscard]] bool unregister_target(
        NativePortAudioExecutionDomainTarget target,
        void* object) noexcept;

    [[nodiscard]] bool configuration_matches(
        const NativePortAudioExecutionDomainConfig& config) const noexcept;

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult dispatch_async(
        const NativePortAudioExecutionDomainTargetHandle& handle,
        std::uint16_t opcode,
        std::span<const std::byte> payload,
        std::uint64_t frame_index,
        std::uint32_t flags = 0u,
        NativePortAudioExecutionDomainStage stage =
            NativePortAudioExecutionDomainStage::None) noexcept;

    // frame_index is a producer hint and is required to be nondecreasing.
    // guest_sequence is generated here, exactly 0,1,2,..., independently of
    // guest cycles or title-specific timestamps.  target_slot and
    // target_generation bind the command to one registered object instance.
    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult dispatch_async(
        NativePortAudioExecutionDomainTarget target,
        std::uint32_t target_slot,
        std::uint32_t target_generation,
        std::uint16_t opcode,
        std::span<const std::byte> payload,
        std::uint64_t frame_index,
        std::uint32_t flags = 0u,
        NativePortAudioExecutionDomainStage stage =
            NativePortAudioExecutionDomainStage::None) noexcept;

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult dispatch_async(
        NativePortAudioExecutionDomainTarget target,
        std::uint16_t opcode,
        std::span<const std::byte> payload,
        std::uint64_t frame_index,
        std::uint32_t flags = 0u,
        NativePortAudioExecutionDomainStage stage =
            NativePortAudioExecutionDomainStage::None) noexcept;

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult
    dispatch_async_scatter(
        NativePortAudioExecutionDomainTarget target,
        std::uint32_t target_slot,
        std::uint32_t target_generation,
        std::uint16_t opcode,
        std::span<const NativePortAudioExecutionDomainPayloadPart> payload,
        std::uint64_t frame_index,
        std::uint32_t flags = 0u,
        NativePortAudioExecutionDomainStage stage =
            NativePortAudioExecutionDomainStage::None) noexcept;

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult
    dispatch_async_scatter(
        const NativePortAudioExecutionDomainTargetHandle& handle,
        std::uint16_t opcode,
        std::span<const NativePortAudioExecutionDomainPayloadPart> payload,
        std::uint64_t frame_index,
        std::uint32_t flags = 0u,
        NativePortAudioExecutionDomainStage stage =
            NativePortAudioExecutionDomainStage::None) noexcept;

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult dispatch_sync(
        const NativePortAudioExecutionDomainTargetHandle& handle,
        std::uint16_t opcode,
        std::span<const std::byte> payload,
        std::uint64_t frame_index,
        std::uint32_t flags = 0u,
        NativePortAudioExecutionDomainStage stage =
            NativePortAudioExecutionDomainStage::None) noexcept;

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult dispatch_sync(
        NativePortAudioExecutionDomainTarget target,
        std::uint32_t target_slot,
        std::uint32_t target_generation,
        std::uint16_t opcode,
        std::span<const std::byte> payload,
        std::uint64_t frame_index,
        std::uint32_t flags = 0u,
        NativePortAudioExecutionDomainStage stage =
            NativePortAudioExecutionDomainStage::None) noexcept;

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult dispatch_sync(
        NativePortAudioExecutionDomainTarget target,
        std::uint16_t opcode,
        std::span<const std::byte> payload,
        std::uint64_t frame_index,
        std::uint32_t flags = 0u,
        NativePortAudioExecutionDomainStage stage =
            NativePortAudioExecutionDomainStage::None) noexcept;

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult
    dispatch_sync_scatter(
        NativePortAudioExecutionDomainTarget target,
        std::uint32_t target_slot,
        std::uint32_t target_generation,
        std::uint16_t opcode,
        std::span<const NativePortAudioExecutionDomainPayloadPart> payload,
        std::uint64_t frame_index,
        std::uint32_t flags = 0u,
        NativePortAudioExecutionDomainStage stage =
            NativePortAudioExecutionDomainStage::None) noexcept;

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult
    dispatch_sync_scatter(
        const NativePortAudioExecutionDomainTargetHandle& handle,
        std::uint16_t opcode,
        std::span<const NativePortAudioExecutionDomainPayloadPart> payload,
        std::uint64_t frame_index,
        std::uint32_t flags = 0u,
        NativePortAudioExecutionDomainStage stage =
            NativePortAudioExecutionDomainStage::None) noexcept;

    // Dedicated mode is continuously consumed by its one worker.  Serial
    // Reference mode has no worker and pump() executes pending commands on
    // the producer thread; it is intended only for the explicit pre-
    // construction KATANA_PORT_AUDIO_SERIAL_REFERENCE=1 gate.
    void pump() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool on_audio_thread() const noexcept;
    [[nodiscard]] NativePortAudioExecutionDomainSnapshot
    snapshot() const noexcept;

    [[nodiscard]] bool bind_telemetry(
        class NativePortTelemetry* telemetry) noexcept;
    [[nodiscard]] bool unbind_telemetry(
        class NativePortTelemetry* telemetry) noexcept;

  private:
    friend std::shared_ptr<NativePortAudioExecutionDomain>
    acquire_native_port_audio_execution_domain(
        const NativePortAudioExecutionDomainConfig& config);

    explicit NativePortAudioExecutionDomain(
        const NativePortAudioExecutionDomainConfig& config);

    std::unique_ptr<Impl> impl_;
};

using NativePortAudioExecutionDomainProducerLease =
    NativePortAudioExecutionDomain::ProducerLease;

[[nodiscard]] std::shared_ptr<NativePortAudioExecutionDomain>
acquire_native_port_audio_execution_domain(
    const NativePortAudioExecutionDomainConfig& config = {});

} // namespace katana::runtime
