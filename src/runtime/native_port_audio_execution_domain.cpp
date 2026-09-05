#include "native_port_audio_execution_domain.hpp"

#include "katana/runtime/native_port_telemetry.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace katana::runtime {
namespace {

std::atomic<std::uint64_t> next_audio_domain_thread_token{1u};

// Request validation is performed by the submitting thread and can also be
// performed by the worker for a nested direct dispatch.  Keep the transient
// diagnostic pair thread-local; these values are never shared state and are
// only read immediately by the same call path.
thread_local NativePortAudioExecutionDomainFailure last_request_failure_ =
    NativePortAudioExecutionDomainFailure::None;
thread_local NativePortAudioCommandQueueFailure last_queue_failure_ =
    NativePortAudioCommandQueueFailure::None;

inline constexpr std::uint16_t telemetry_barrier_opcode = 1u;

[[nodiscard]] std::uint64_t audio_domain_thread_token() noexcept {
    static thread_local const std::uint64_t token = [] {
        const auto value = next_audio_domain_thread_token.fetch_add(
            1u, std::memory_order_relaxed);
        return value == 0u
                   ? next_audio_domain_thread_token.fetch_add(
                         1u, std::memory_order_relaxed)
                   : value;
    }();
    return token;
}

[[nodiscard]] bool valid_target(
    const NativePortAudioExecutionDomainTarget target) noexcept {
    return static_cast<std::size_t>(target) <
           native_port_audio_execution_domain_target_count;
}

[[nodiscard]] std::size_t target_slot_index(
    const NativePortAudioExecutionDomainTarget target,
    const std::uint32_t slot) noexcept {
    return static_cast<std::size_t>(target) *
               native_port_audio_execution_domain_max_slots_per_target +
           static_cast<std::size_t>(slot);
}

[[nodiscard]] NativePortAudioCommandTarget queue_target(
    const NativePortAudioExecutionDomainTarget target) noexcept {
    switch (target) {
    case NativePortAudioExecutionDomainTarget::AudioEngine:
        return NativePortAudioCommandTarget::AudioEngine;
    case NativePortAudioExecutionDomainTarget::SoundBank:
        return NativePortAudioCommandTarget::SoundBank;
    case NativePortAudioExecutionDomainTarget::HostOutput:
        return NativePortAudioCommandTarget::HostOutput;
    case NativePortAudioExecutionDomainTarget::Movie:
        return NativePortAudioCommandTarget::Movie;
    case NativePortAudioExecutionDomainTarget::Lifecycle:
        return NativePortAudioCommandTarget::Lifecycle;
    }
    return NativePortAudioCommandTarget::Lifecycle;
}

[[nodiscard]] bool queue_target_matches(
    const NativePortAudioExecutionDomainTarget target,
    const NativePortAudioCommandTarget queue_value) noexcept {
    return queue_value == queue_target(target);
}

// A zero next-generation value is a permanent fail-closed exhaustion marker.
// Reusing generation 1 after uint32 wrap could make an old bounded command
// indistinguishable from a new registration, so a slot is retired forever at
// that boundary rather than reopening the identity domain.
[[nodiscard]] std::uint32_t next_target_generation(
    const std::uint32_t generation) noexcept {
    return generation == std::numeric_limits<std::uint32_t>::max()
               ? 0u
               : generation + 1u;
}

[[nodiscard]] NativePortAudioExecutionDomainFailure map_queue_failure(
    const NativePortAudioCommandQueueFailure failure) noexcept {
    switch (failure) {
    case NativePortAudioCommandQueueFailure::None:
        return NativePortAudioExecutionDomainFailure::None;
    case NativePortAudioCommandQueueFailure::Disabled:
        return NativePortAudioExecutionDomainFailure::Disabled;
    case NativePortAudioCommandQueueFailure::ProducerThreadViolation:
        return NativePortAudioExecutionDomainFailure::ProducerThreadViolation;
    case NativePortAudioCommandQueueFailure::ConsumerThreadViolation:
        return NativePortAudioExecutionDomainFailure::ConsumerThreadViolation;
    case NativePortAudioCommandQueueFailure::ProducerLeaseOverlap:
        return NativePortAudioExecutionDomainFailure::SequenceBusy;
    case NativePortAudioCommandQueueFailure::StampInitialSequence:
        return NativePortAudioExecutionDomainFailure::StampInitialSequence;
    case NativePortAudioCommandQueueFailure::StampRegression:
        return NativePortAudioExecutionDomainFailure::StampRegression;
    case NativePortAudioCommandQueueFailure::StampGap:
        return NativePortAudioExecutionDomainFailure::StampGap;
    case NativePortAudioCommandQueueFailure::StampOverflow:
        return NativePortAudioExecutionDomainFailure::StampOverflow;
    case NativePortAudioCommandQueueFailure::FrameRegression:
        return NativePortAudioExecutionDomainFailure::FrameRegression;
    case NativePortAudioCommandQueueFailure::InvalidAckSlot:
    case NativePortAudioCommandQueueFailure::AckSlotBusy:
        return NativePortAudioExecutionDomainFailure::AckSlotBusy;
    case NativePortAudioCommandQueueFailure::QueueClosed:
        return NativePortAudioExecutionDomainFailure::QueueClosed;
    case NativePortAudioCommandQueueFailure::QueueFailed:
        return NativePortAudioExecutionDomainFailure::QueueFailed;
    case NativePortAudioCommandQueueFailure::WorkerFailure:
        return NativePortAudioExecutionDomainFailure::WorkerFailure;
    default:
        return NativePortAudioExecutionDomainFailure::QueueFailed;
    }
}

[[nodiscard]] const char* domain_failure_name(
    const NativePortAudioExecutionDomainFailure failure) noexcept {
    switch (failure) {
    case NativePortAudioExecutionDomainFailure::None:
        return "none";
    case NativePortAudioExecutionDomainFailure::InvalidConfig:
        return "invalid-config";
    case NativePortAudioExecutionDomainFailure::ConfigMismatch:
        return "config-mismatch";
    case NativePortAudioExecutionDomainFailure::Disabled:
        return "disabled";
    case NativePortAudioExecutionDomainFailure::InvalidTarget:
        return "invalid-target";
    case NativePortAudioExecutionDomainFailure::InvalidFlags:
        return "invalid-flags";
    case NativePortAudioExecutionDomainFailure::TargetAlreadyRegistered:
        return "target-already-registered";
    case NativePortAudioExecutionDomainFailure::TargetNotRegistered:
        return "target-not-registered";
    case NativePortAudioExecutionDomainFailure::TargetIdentityMismatch:
        return "target-identity-mismatch";
    case NativePortAudioExecutionDomainFailure::ExecutorMissing:
        return "executor-missing";
    case NativePortAudioExecutionDomainFailure::ProducerThreadViolation:
        return "producer-thread";
    case NativePortAudioExecutionDomainFailure::ConsumerThreadViolation:
        return "consumer-thread";
    case NativePortAudioExecutionDomainFailure::QueueFull:
        return "queue-full";
    case NativePortAudioExecutionDomainFailure::QueueFailed:
        return "queue-failed";
    case NativePortAudioExecutionDomainFailure::QueueClosed:
        return "queue-closed";
    case NativePortAudioExecutionDomainFailure::SequenceBusy:
        return "sequence-busy";
    case NativePortAudioExecutionDomainFailure::InvalidPayload:
        return "invalid-payload";
    case NativePortAudioExecutionDomainFailure::ScatterOnWorker:
        return "scatter-on-worker";
    case NativePortAudioExecutionDomainFailure::WorkerFailure:
        return "worker-failure";
    case NativePortAudioExecutionDomainFailure::StampInitialSequence:
        return "stamp-initial-sequence";
    case NativePortAudioExecutionDomainFailure::StampRegression:
        return "stamp-regression";
    case NativePortAudioExecutionDomainFailure::StampGap:
        return "stamp-gap";
    case NativePortAudioExecutionDomainFailure::StampOverflow:
        return "stamp-overflow";
    case NativePortAudioExecutionDomainFailure::FrameRegression:
        return "frame-regression";
    case NativePortAudioExecutionDomainFailure::AckSlotBusy:
        return "ack-slot-busy";
    case NativePortAudioExecutionDomainFailure::AckMissing:
        return "ack-missing";
    case NativePortAudioExecutionDomainFailure::TargetExecutionFailed:
        return "target-execution-failed";
    case NativePortAudioExecutionDomainFailure::Shutdown:
        return "shutdown";
    }
    return "unknown";
}

[[nodiscard]] NativePortAudioCommandQueueConfig effective_queue_config(
    const NativePortAudioExecutionDomainConfig& config) noexcept {
    auto result = config.command_queue;
    if (native_port_audio_serial_reference_requested())
        result.mode = NativePortAudioCommandQueueMode::SerialReference;
    return result;
}

[[nodiscard]] NativePortAudioExecutionDomainDispatchResult failed_result(
    const NativePortAudioExecutionDomainFailure failure,
    const NativePortAudioCommandQueueFailure queue_failure =
        NativePortAudioCommandQueueFailure::None,
    const std::uint64_t sequence = 0u,
    const NativePortAudioCommandStamp stamp = {}) noexcept {
    NativePortAudioExecutionDomainDispatchResult result;
    result.failure = failure;
    result.queue_failure = queue_failure;
    result.command_sequence = sequence;
    result.stamp = stamp;
    return result;
}

} // namespace

NativePortAudioExecutionDomainError::NativePortAudioExecutionDomainError(
    const NativePortAudioExecutionDomainFailure failure,
    const NativePortAudioCommandQueueFailure queue_failure,
    const std::uint64_t command_sequence)
    : std::runtime_error(domain_failure_name(failure)), failure_(failure),
      queue_failure_(queue_failure), command_sequence_(command_sequence) {}

NativePortAudioExecutionDomainFailure
NativePortAudioExecutionDomainError::failure() const noexcept {
    return failure_;
}

NativePortAudioCommandQueueFailure
NativePortAudioExecutionDomainError::queue_failure() const noexcept {
    return queue_failure_;
}

std::uint64_t NativePortAudioExecutionDomainError::command_sequence()
    const noexcept {
    return command_sequence_;
}

class NativePortAudioExecutionDomain::Impl final {
    friend class NativePortAudioExecutionDomain::ProducerLease;

  private:
    static constexpr std::uint8_t slot_free = 0u;
    static constexpr std::uint8_t slot_initializing = 1u;
    static constexpr std::uint8_t slot_active = 2u;
    static constexpr std::uint8_t slot_retiring = 3u;
    static constexpr std::uint32_t active_dispatches_closed_bit =
        std::uint32_t{1u} << 31u;
    static constexpr std::uint32_t active_dispatches_count_mask =
        ~active_dispatches_closed_bit;

    struct TargetSlot final {
        std::atomic<std::uint8_t> state{slot_free};
        std::atomic<void*> object{nullptr};
        std::atomic<std::uint32_t> generation{0u};
        // Registration is lifecycle-only.  The next value is kept even
        // while a slot is free so a stale command cannot become valid after
        // slot reuse.  A zero value is never issued.
        std::atomic<std::uint32_t> next_generation{1u};
        std::atomic<std::uint32_t> last_generation{0u};
        std::atomic<NativePortAudioExecutionDomainExecutor> executor{nullptr};
        std::atomic<NativePortAudioExecutionDomainCleanup> cleanup{nullptr};
        std::atomic<NativePortAudioExecutionDomainConsumerService>
            consumer_service{nullptr};
        // The high bit closes acquisition while a slot is free, initializing,
        // or retiring.  The low bits are the live dispatch references.
        std::atomic<std::uint32_t> active_dispatches{
            active_dispatches_closed_bit};
        std::atomic<std::uint64_t> executed_commands{0u};
        std::atomic<std::uint64_t> failed_commands{0u};
    };

    struct PinnedTarget final {
        TargetSlot* slot = nullptr;
        void* object = nullptr;
        NativePortAudioExecutionDomainExecutor executor = nullptr;
        NativePortAudioExecutionDomainConsumerService consumer_service =
            nullptr;
        bool valid = false;
    };

  public:
    explicit Impl(NativePortAudioExecutionDomain* const owner,
                  const NativePortAudioExecutionDomainConfig& requested)
        : owner_(owner), queue_config_(effective_queue_config(requested)),
          queue_(queue_config_) {
        if (queue_config_.maximum_ack_slots == 0u)
            throw NativePortAudioExecutionDomainError(
                NativePortAudioExecutionDomainFailure::InvalidConfig);
        try {
            ack_slots_ = std::make_unique<std::atomic<std::uint8_t>[]>(
                queue_config_.maximum_ack_slots);
        } catch (...) {
            throw NativePortAudioExecutionDomainError(
                NativePortAudioExecutionDomainFailure::InvalidConfig);
        }
        for (std::uint32_t index = 0u;
             index < queue_config_.maximum_ack_slots; ++index)
            ack_slots_[index].store(slot_free, std::memory_order_relaxed);

        if (queue_config_.enabled &&
            queue_config_.mode == NativePortAudioCommandQueueMode::DedicatedThread) {
            try {
                worker_ = std::thread([this] { worker_main(); });
            } catch (...) {
                throw NativePortAudioExecutionDomainError(
                    NativePortAudioExecutionDomainFailure::InvalidConfig);
            }
        }
    }

    ~Impl() { shutdown(); }

    [[nodiscard]] bool configuration_matches(
        const NativePortAudioExecutionDomainConfig& requested) const noexcept {
        const auto other = effective_queue_config(requested);
        return other.mode == queue_config_.mode &&
               other.maximum_commands == queue_config_.maximum_commands &&
               other.maximum_payload_bytes == queue_config_.maximum_payload_bytes &&
               other.maximum_ack_slots == queue_config_.maximum_ack_slots &&
               other.enabled == queue_config_.enabled;
    }

    [[nodiscard]] NativePortAudioCommandQueueMode mode() const noexcept {
        return queue_config_.mode;
    }

    [[nodiscard]] bool register_target(
        const NativePortAudioExecutionDomainTarget target,
        const std::uint32_t target_slot,
        const std::uint32_t target_generation,
        void* const object,
        const NativePortAudioExecutionDomainExecutor executor,
        const NativePortAudioExecutionDomainCleanup cleanup,
        const NativePortAudioExecutionDomainConsumerService
            consumer_service) noexcept {
        if (shutdown_requested_.load(std::memory_order_acquire)) {
            record_failure(NativePortAudioExecutionDomainFailure::Shutdown, 0u);
            return false;
        }
        if (!valid_target(target) ||
            target_slot >= native_port_audio_execution_domain_max_slots_per_target ||
            target_generation == 0u || object == nullptr || executor == nullptr) {
            record_failure(
                NativePortAudioExecutionDomainFailure::InvalidTarget, 0u);
            return false;
        }
        auto& slot = slots_[target_slot_index(target, target_slot)];
        auto expected = slot_free;
        if (!slot.state.compare_exchange_strong(
                expected, slot_initializing, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            record_failure(
                NativePortAudioExecutionDomainFailure::TargetAlreadyRegistered,
                0u);
            return false;
        }
        if (shutdown_requested_.load(std::memory_order_acquire)) {
            slot.active_dispatches.fetch_or(active_dispatches_closed_bit,
                                            std::memory_order_acq_rel);
            slot.state.store(slot_free, std::memory_order_release);
            slot.state.notify_all();
            record_failure(NativePortAudioExecutionDomainFailure::Shutdown, 0u);
            return false;
        }
        const auto previous_generation =
            slot.last_generation.load(std::memory_order_acquire);
        const auto expected_generation =
            slot.next_generation.load(std::memory_order_acquire);
        // The explicit overload is retained for tests and controlled
        // lifecycle setup, but it must follow the same monotonic generation
        // contract as auto-registration. Accepting an arbitrary value here
        // could make a still-queued stale command look current after reuse.
        if (expected_generation == 0u ||
            target_generation != expected_generation ||
            previous_generation == target_generation) {
            slot.state.store(slot_free, std::memory_order_release);
            slot.state.notify_all();
            record_failure(
                NativePortAudioExecutionDomainFailure::TargetIdentityMismatch,
                0u);
            return false;
        }
        slot.object.store(object, std::memory_order_relaxed);
        slot.generation.store(target_generation, std::memory_order_relaxed);
        slot.last_generation.store(target_generation, std::memory_order_relaxed);
        slot.next_generation.store(next_target_generation(target_generation),
                                   std::memory_order_relaxed);
        slot.executor.store(executor, std::memory_order_relaxed);
        slot.cleanup.store(cleanup, std::memory_order_relaxed);
        slot.consumer_service.store(consumer_service,
                                    std::memory_order_relaxed);
        slot.executed_commands.store(0u, std::memory_order_relaxed);
        slot.failed_commands.store(0u, std::memory_order_relaxed);
        if (consumer_service != nullptr)
            consumer_service_target_mask_.fetch_or(
                std::uint64_t{1u}
                    << target_slot_index(target, target_slot),
                std::memory_order_release);
        // Publish an open acquisition gate only after every identity and
        // executor field is complete.  Do not reset this counter again until
        // retirement has closed and drained it.
        slot.active_dispatches.store(0u, std::memory_order_release);
        slot.state.store(slot_active, std::memory_order_release);
        slot.state.notify_all();
        return true;
    }

    [[nodiscard]] std::optional<
        NativePortAudioExecutionDomainTargetHandle>
    register_target(
        const NativePortAudioExecutionDomainTarget target,
        void* const object,
        const NativePortAudioExecutionDomainExecutor executor,
        const NativePortAudioExecutionDomainCleanup cleanup,
        const NativePortAudioExecutionDomainConsumerService
            consumer_service) noexcept {
        if (shutdown_requested_.load(std::memory_order_acquire)) {
            record_failure(NativePortAudioExecutionDomainFailure::Shutdown, 0u);
            return std::nullopt;
        }
        if (!valid_target(target) || object == nullptr || executor == nullptr) {
            record_failure(
                NativePortAudioExecutionDomainFailure::InvalidTarget, 0u);
            return std::nullopt;
        }
        for (std::uint32_t target_slot = 0u;
             target_slot < native_port_audio_execution_domain_max_slots_per_target;
             ++target_slot) {
            auto& slot = slots_[target_slot_index(target, target_slot)];
            auto expected = slot_free;
            if (!slot.state.compare_exchange_strong(
                    expected, slot_initializing, std::memory_order_acq_rel,
                    std::memory_order_acquire))
                continue;

            if (shutdown_requested_.load(std::memory_order_acquire)) {
                slot.active_dispatches.fetch_or(active_dispatches_closed_bit,
                                                std::memory_order_acq_rel);
                slot.state.store(slot_free, std::memory_order_release);
                slot.state.notify_all();
                record_failure(
                    NativePortAudioExecutionDomainFailure::Shutdown, 0u);
                return std::nullopt;
            }
            const auto generation =
                slot.next_generation.load(std::memory_order_relaxed);
            const auto previous_generation =
                slot.last_generation.load(std::memory_order_acquire);
            if (generation == 0u || generation == previous_generation) {
                slot.state.store(slot_free, std::memory_order_release);
                slot.state.notify_all();
                record_failure(
                    NativePortAudioExecutionDomainFailure::TargetIdentityMismatch,
                    0u);
                return std::nullopt;
            }
            slot.object.store(object, std::memory_order_relaxed);
            slot.generation.store(generation, std::memory_order_relaxed);
            slot.last_generation.store(generation, std::memory_order_relaxed);
            slot.next_generation.store(next_target_generation(generation),
                                       std::memory_order_relaxed);
            slot.executor.store(executor, std::memory_order_relaxed);
            slot.cleanup.store(cleanup, std::memory_order_relaxed);
            slot.consumer_service.store(consumer_service,
                                        std::memory_order_relaxed);
            slot.executed_commands.store(0u, std::memory_order_relaxed);
            slot.failed_commands.store(0u, std::memory_order_relaxed);
            if (consumer_service != nullptr)
                consumer_service_target_mask_.fetch_or(
                    std::uint64_t{1u}
                        << target_slot_index(target, target_slot),
                    std::memory_order_release);
            slot.active_dispatches.store(0u, std::memory_order_release);
            slot.state.store(slot_active, std::memory_order_release);
            slot.state.notify_all();
            return NativePortAudioExecutionDomainTargetHandle{
                target, target_slot, generation};
        }
        record_failure(
            NativePortAudioExecutionDomainFailure::TargetAlreadyRegistered,
            0u);
        return std::nullopt;
    }

    [[nodiscard]] bool unregister_target(
        const NativePortAudioExecutionDomainTargetHandle& handle,
        void* const object) noexcept {
        if (!handle.valid()) {
            record_failure(
                NativePortAudioExecutionDomainFailure::InvalidTarget, 0u);
            return false;
        }
        return unregister_target(handle.target, handle.slot, handle.generation,
                                 object);
    }

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult dispatch_async(
        const NativePortAudioExecutionDomainTargetHandle& handle,
        const std::uint16_t opcode,
        const std::span<const std::byte> payload,
        const std::uint64_t frame_index,
        const std::uint32_t flags,
        const NativePortAudioExecutionDomainStage stage) noexcept {
        return dispatch_async(handle.target, handle.slot, handle.generation,
                              opcode, payload, frame_index, flags, stage);
    }

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult
    dispatch_async_scatter(
        const NativePortAudioExecutionDomainTargetHandle& handle,
        const std::uint16_t opcode,
        const std::span<const NativePortAudioExecutionDomainPayloadPart> parts,
        const std::uint64_t frame_index,
        const std::uint32_t flags,
        const NativePortAudioExecutionDomainStage stage) noexcept {
        return dispatch_async_scatter(handle.target, handle.slot,
                                      handle.generation, opcode, parts,
                                      frame_index, flags, stage);
    }

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult dispatch_sync(
        const NativePortAudioExecutionDomainTargetHandle& handle,
        const std::uint16_t opcode,
        const std::span<const std::byte> payload,
        const std::uint64_t frame_index,
        const std::uint32_t flags,
        const NativePortAudioExecutionDomainStage stage) noexcept {
        return dispatch_sync(handle.target, handle.slot, handle.generation,
                             opcode, payload, frame_index, flags, stage);
    }

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult
    dispatch_sync_scatter(
        const NativePortAudioExecutionDomainTargetHandle& handle,
        const std::uint16_t opcode,
        const std::span<const NativePortAudioExecutionDomainPayloadPart> parts,
        const std::uint64_t frame_index,
        const std::uint32_t flags,
        const NativePortAudioExecutionDomainStage stage) noexcept {
        return dispatch_sync_scatter(handle.target, handle.slot,
                                     handle.generation, opcode, parts,
                                     frame_index, flags, stage);
    }

    [[nodiscard]] bool unregister_target(
        const NativePortAudioExecutionDomainTarget target,
        const std::uint32_t target_slot,
        const std::uint32_t target_generation,
        void* const object) noexcept {
        if (!valid_target(target) ||
            target_slot >= native_port_audio_execution_domain_max_slots_per_target ||
            target_generation == 0u || object == nullptr) {
            record_failure(
                NativePortAudioExecutionDomainFailure::InvalidTarget, 0u);
            return false;
        }
        auto& slot = slots_[target_slot_index(target, target_slot)];
        if (slot.state.load(std::memory_order_acquire) != slot_active ||
            slot.object.load(std::memory_order_acquire) != object ||
            slot.generation.load(std::memory_order_acquire) !=
                target_generation) {
            record_failure(
                NativePortAudioExecutionDomainFailure::TargetIdentityMismatch,
                0u);
            return false;
        }
        auto expected = slot_active;
        if (!slot.state.compare_exchange_strong(
                expected, slot_retiring, std::memory_order_acq_rel,
                std::memory_order_acquire))
            return false;

        // The preliminary identity check is only a fast rejection.  An old
        // unregister can lose the race to a full retire/re-register cycle and
        // otherwise claim the new generation with its state CAS.
        if (slot.object.load(std::memory_order_acquire) != object ||
            slot.generation.load(std::memory_order_acquire) !=
                target_generation) {
            slot.state.store(slot_active, std::memory_order_release);
            slot.state.notify_all();
            record_failure(
                NativePortAudioExecutionDomainFailure::TargetIdentityMismatch,
                0u);
            return false;
        }

        // A target must never be destroyed while an executor still owns its
        // pointer.  The only consumer is the domain worker; unregister is a
        // lifecycle operation and may wait here, but never in dispatch.
        if (on_audio_thread() &&
            (slot.active_dispatches.load(std::memory_order_acquire) &
             active_dispatches_count_mask) != 0u) {
            slot.state.store(slot_active, std::memory_order_release);
            slot.state.notify_all();
            return false;
        }
        slot.active_dispatches.fetch_or(active_dispatches_closed_bit,
                                        std::memory_order_acq_rel);
        consumer_service_target_mask_.fetch_and(
            ~(std::uint64_t{1u}
              << target_slot_index(target, target_slot)),
            std::memory_order_acq_rel);
        auto active = slot.active_dispatches.load(std::memory_order_acquire);
        while ((active & active_dispatches_count_mask) != 0u) {
            slot.active_dispatches.wait(active, std::memory_order_acquire);
            active = slot.active_dispatches.load(std::memory_order_acquire);
        }
        slot.executor.store(nullptr, std::memory_order_release);
        slot.cleanup.store(nullptr, std::memory_order_release);
        slot.consumer_service.store(nullptr, std::memory_order_release);
        slot.object.store(nullptr, std::memory_order_release);
        // Keep the last issued generation observable.  A freed slot is
        // invalidated by its state; resetting the generation would make
        // lifecycle snapshots lose the monotone identity history.
        slot.state.store(slot_free, std::memory_order_release);
        slot.state.notify_all();
        return true;
    }

    [[nodiscard]] bool unregister_target(
        const NativePortAudioExecutionDomainTarget target,
        void* const object) noexcept {
        const auto generation = registered_generation(target, 0u);
        return generation != 0u &&
               unregister_target(target, 0u, generation, object);
    }

    [[nodiscard]] std::uint32_t registered_generation(
        const NativePortAudioExecutionDomainTarget target,
        const std::uint32_t target_slot) const noexcept {
        if (!valid_target(target) ||
            target_slot >= native_port_audio_execution_domain_max_slots_per_target)
            return 0u;
        const auto& slot = slots_[target_slot_index(target, target_slot)];
        if (slot.state.load(std::memory_order_acquire) != slot_active)
            return 0u;
        return slot.generation.load(std::memory_order_acquire);
    }

    [[nodiscard]] NativePortAudioExecutionDomain::ProducerLease
    begin_async_payload(
        const NativePortAudioExecutionDomainTargetHandle& handle,
        const std::uint16_t opcode,
        const std::uint32_t payload_size,
        const std::uint64_t frame_index,
        const std::uint32_t flags,
        const NativePortAudioExecutionDomainStage stage) noexcept {
        const auto invalid_lease =
            [this](const NativePortAudioExecutionDomainFailure failure,
                   const NativePortAudioCommandQueueFailure queue_failure =
                       NativePortAudioCommandQueueFailure::None) noexcept {
                return NativePortAudioExecutionDomain::ProducerLease(
                    owner_, {}, {}, failed_result(failure, queue_failure));
            };
        if (on_audio_thread())
            return invalid_lease(
                NativePortAudioExecutionDomainFailure::ConsumerThreadViolation);
        if (!producer_thread_allowed())
            return invalid_lease(
                NativePortAudioExecutionDomainFailure::ProducerThreadViolation);
        if (const auto ready = check_dispatch_ready(); ready !=
                                                   NativePortAudioExecutionDomainFailure::None)
            return NativePortAudioExecutionDomain::ProducerLease(
                owner_, {}, {}, failed_dispatch_result(ready));
        if (!handle.valid() || !valid_target(handle.target) ||
            handle.slot >= native_port_audio_execution_domain_max_slots_per_target)
            return invalid_lease(
                NativePortAudioExecutionDomainFailure::InvalidTarget);
        std::uint32_t total_size = 0u;
        // Validate the handle/flags/registration without pretending that the
        // caller already owns source bytes.  This reservation intentionally
        // exposes the queue slab for the caller to fill in place.
        const NativePortAudioExecutionDomainPayloadPart empty_part{};
        if (!validate_request(handle.target, handle.slot, handle.generation,
                              std::span<const NativePortAudioExecutionDomainPayloadPart>(
                                  &empty_part, 1u),
                              flags, stage, total_size))
            return invalid_lease(last_request_failure_, last_queue_failure_);
        if (payload_size > queue_config_.maximum_payload_bytes)
            return invalid_lease(
                NativePortAudioExecutionDomainFailure::InvalidPayload,
                NativePortAudioCommandQueueFailure::PayloadOverflow);
        total_size = payload_size;
        NativePortAudioCommandStamp stamp;
        if (!make_stamp(frame_index, stamp))
            return invalid_lease(last_request_failure_, last_queue_failure_);

        NativePortAudioPodCommand command;
        command.stamp = stamp;
        command.target = queue_target(handle.target);
        command.opcode = opcode;
        command.payload_size = total_size;
        command.flags = encode_flags(flags, stage);
        command.target_slot = handle.slot;
        command.target_generation = handle.generation;
        std::optional<NativePortAudioCommandProducerLease> queue_lease;
        try {
            queue_lease = queue_.wait_begin_produce(command);
        } catch (const NativePortAudioCommandQueueError& error) {
            last_request_failure_ = map_queue_failure(error.failure());
            last_queue_failure_ = error.failure();
            record_failure(last_request_failure_, error.command_sequence());
            return invalid_lease(last_request_failure_, last_queue_failure_);
        } catch (...) {
            record_failure(NativePortAudioExecutionDomainFailure::WorkerFailure,
                           0u);
            return invalid_lease(
                NativePortAudioExecutionDomainFailure::WorkerFailure,
                NativePortAudioCommandQueueFailure::WorkerFailure);
        }
        if (!queue_lease.has_value())
            return invalid_lease(NativePortAudioExecutionDomainFailure::QueueFull,
                                 NativePortAudioCommandQueueFailure::None);
        NativePortAudioExecutionDomainDispatchResult result;
        result.stamp = stamp;
        return NativePortAudioExecutionDomain::ProducerLease(
            owner_, std::move(*queue_lease), stamp, result);
    }

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult dispatch_async(
        const NativePortAudioExecutionDomainTarget target,
        const std::uint32_t target_slot,
        const std::uint32_t target_generation,
        const std::uint16_t opcode,
        const std::span<const std::byte> payload,
        const std::uint64_t frame_index,
        const std::uint32_t flags,
        const NativePortAudioExecutionDomainStage stage) noexcept {
        NativePortAudioExecutionDomainPayloadPart part{payload.data(),
                                                       static_cast<std::uint32_t>(
                                                           payload.size())};
        return dispatch_async_scatter(
            target, target_slot, target_generation, opcode,
            std::span<const NativePortAudioExecutionDomainPayloadPart>(&part,
                                                                       1u),
            frame_index, flags, stage);
    }

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult dispatch_async(
        const NativePortAudioExecutionDomainTarget target,
        const std::uint16_t opcode,
        const std::span<const std::byte> payload,
        const std::uint64_t frame_index,
        const std::uint32_t flags,
        const NativePortAudioExecutionDomainStage stage) noexcept {
        return dispatch_async(
            target, 0u, registered_generation(target, 0u), opcode, payload,
            frame_index, flags, stage);
    }

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult
    dispatch_async_scatter(
        const NativePortAudioExecutionDomainTarget target,
        const std::uint32_t target_slot,
        const std::uint32_t target_generation,
        const std::uint16_t opcode,
        const std::span<const NativePortAudioExecutionDomainPayloadPart> parts,
        const std::uint64_t frame_index,
        const std::uint32_t flags,
        const NativePortAudioExecutionDomainStage stage) noexcept {
        if (on_audio_thread())
            return dispatch_inline(target, target_slot, target_generation,
                                   opcode, parts, frame_index, flags, stage,
                                   false);
        if (!producer_thread_allowed())
            return failed_result(
                NativePortAudioExecutionDomainFailure::ProducerThreadViolation);
        if (const auto ready = check_dispatch_ready(); ready !=
                                                   NativePortAudioExecutionDomainFailure::None)
            return failed_dispatch_result(ready);
        std::uint32_t total_size = 0u;
        if (!validate_request(target, target_slot, target_generation, parts,
                              flags, stage, total_size))
            return failed_result(last_request_failure_, last_queue_failure_);

        NativePortAudioCommandStamp stamp;
        if (!make_stamp(frame_index, stamp))
            return failed_result(last_request_failure_, last_queue_failure_, 0u,
                                 stamp);
        NativePortAudioPodCommand command;
        command.stamp = stamp;
        command.target = queue_target(target);
        command.opcode = opcode;
        command.payload_size = total_size;
        command.flags = encode_flags(flags, stage);
        command.target_slot = target_slot;
        command.target_generation = target_generation;

        std::optional<NativePortAudioCommandProducerLease> lease;
        try {
            lease = queue_.wait_begin_produce(command);
        } catch (const NativePortAudioCommandQueueError& error) {
            last_request_failure_ = map_queue_failure(error.failure());
            last_queue_failure_ = error.failure();
            record_failure(last_request_failure_, error.command_sequence());
            return failed_result(last_request_failure_, last_queue_failure_, 0u,
                                 stamp);
        } catch (...) {
            record_failure(NativePortAudioExecutionDomainFailure::QueueFailed,
                           0u);
            return failed_result(
                NativePortAudioExecutionDomainFailure::WorkerFailure,
                NativePortAudioCommandQueueFailure::WorkerFailure, 0u, stamp);
        }
        if (!lease.has_value())
            return failed_result(NativePortAudioExecutionDomainFailure::Disabled,
                                 NativePortAudioCommandQueueFailure::Disabled,
                                 0u, stamp);

        auto destination = lease->payload();
        std::uint32_t offset = 0u;
        for (const auto& part : parts) {
            if (part.size != 0u)
                std::memcpy(destination.data() + offset, part.data, part.size);
            offset += part.size;
        }
        if (!lease->publish()) {
            lease->abort();
            record_failure(NativePortAudioExecutionDomainFailure::QueueFailed,
                           0u);
            return failed_result(
                NativePortAudioExecutionDomainFailure::QueueFailed,
                NativePortAudioCommandQueueFailure::QueueFailed, 0u,
                stamp);
        }
        // The queue assigns command_sequence only at publish time.  Reading
        // it before publish would return the producer-lease sentinel zero
        // and hide the first command's real sequence from callers.
        const auto sequence = lease->command_sequence();
        commit_stamp(stamp);
        observe_queue_depth();
        NativePortAudioExecutionDomainDispatchResult result;
        result.command_sequence = sequence;
        result.stamp = stamp;
        return result;
    }

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult dispatch_sync(
        const NativePortAudioExecutionDomainTarget target,
        const std::uint32_t target_slot,
        const std::uint32_t target_generation,
        const std::uint16_t opcode,
        const std::span<const std::byte> payload,
        const std::uint64_t frame_index,
        const std::uint32_t flags,
        const NativePortAudioExecutionDomainStage stage) noexcept {
        NativePortAudioExecutionDomainPayloadPart part{payload.data(),
                                                       static_cast<std::uint32_t>(
                                                           payload.size())};
        return dispatch_sync_scatter(
            target, target_slot, target_generation, opcode,
            std::span<const NativePortAudioExecutionDomainPayloadPart>(&part,
                                                                       1u),
            frame_index, flags, stage);
    }

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult dispatch_sync(
        const NativePortAudioExecutionDomainTarget target,
        const std::uint16_t opcode,
        const std::span<const std::byte> payload,
        const std::uint64_t frame_index,
        const std::uint32_t flags,
        const NativePortAudioExecutionDomainStage stage) noexcept {
        return dispatch_sync(
            target, 0u, registered_generation(target, 0u), opcode, payload,
            frame_index, flags, stage);
    }

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult
    dispatch_sync_scatter(
        const NativePortAudioExecutionDomainTarget target,
        const std::uint32_t target_slot,
        const std::uint32_t target_generation,
        const std::uint16_t opcode,
        const std::span<const NativePortAudioExecutionDomainPayloadPart> parts,
        const std::uint64_t frame_index,
        const std::uint32_t flags,
        const NativePortAudioExecutionDomainStage stage) noexcept {
        if (on_audio_thread())
            return dispatch_inline(target, target_slot, target_generation,
                                   opcode, parts, frame_index, flags, stage,
                                   true);
        if (!producer_thread_allowed())
            return failed_result(
                NativePortAudioExecutionDomainFailure::ProducerThreadViolation);
        if (const auto ready = check_dispatch_ready(); ready !=
                                                   NativePortAudioExecutionDomainFailure::None)
            return failed_dispatch_result(ready);
        std::uint32_t total_size = 0u;
        if (!validate_request(target, target_slot, target_generation, parts,
                              flags, stage, total_size))
            return failed_result(last_request_failure_, last_queue_failure_);

        const auto ack_slot = reserve_ack_slot();
        if (ack_slot == native_port_audio_command_queue_invalid_ack_slot)
            return failed_result(NativePortAudioExecutionDomainFailure::AckSlotBusy);

        NativePortAudioCommandStamp stamp;
        if (!make_stamp(frame_index, stamp)) {
            release_ack_slot(ack_slot);
            return failed_result(last_request_failure_, last_queue_failure_, 0u,
                                 stamp);
        }
        NativePortAudioPodCommand command;
        command.stamp = stamp;
        command.target = queue_target(target);
        command.opcode = opcode;
        command.payload_size = total_size;
        command.ack_slot = ack_slot;
        command.flags = encode_flags(flags, stage);
        command.target_slot = target_slot;
        command.target_generation = target_generation;

        std::optional<NativePortAudioCommandProducerLease> lease;
        try {
            lease = queue_.wait_begin_produce(command);
        } catch (const NativePortAudioCommandQueueError& error) {
            release_ack_slot(ack_slot);
            last_request_failure_ = map_queue_failure(error.failure());
            last_queue_failure_ = error.failure();
            record_failure(last_request_failure_, error.command_sequence());
            return failed_result(last_request_failure_, last_queue_failure_, 0u,
                                 stamp);
        } catch (...) {
            release_ack_slot(ack_slot);
            record_failure(NativePortAudioExecutionDomainFailure::WorkerFailure,
                           0u);
            return failed_result(
                NativePortAudioExecutionDomainFailure::WorkerFailure,
                NativePortAudioCommandQueueFailure::WorkerFailure, 0u, stamp);
        }
        if (!lease.has_value()) {
            release_ack_slot(ack_slot);
            return failed_result(NativePortAudioExecutionDomainFailure::Disabled,
                                 NativePortAudioCommandQueueFailure::Disabled,
                                 0u, stamp);
        }
        auto destination = lease->payload();
        std::uint32_t offset = 0u;
        for (const auto& part : parts) {
            if (part.size != 0u)
                std::memcpy(destination.data() + offset, part.data, part.size);
            offset += part.size;
        }
        if (!lease->publish()) {
            lease->abort();
            release_ack_slot(ack_slot);
            record_failure(NativePortAudioExecutionDomainFailure::QueueFailed,
                           0u);
            return failed_result(
                NativePortAudioExecutionDomainFailure::QueueFailed,
                NativePortAudioCommandQueueFailure::QueueFailed, 0u,
                stamp);
        }
        const auto sequence = lease->command_sequence();
        commit_stamp(stamp);
        observe_queue_depth();

        std::optional<NativePortAudioCommandAck> ack;
        try {
            if (queue_.mode() == NativePortAudioCommandQueueMode::SerialReference)
                pump();
            ack = queue_.wait_read_ack(ack_slot, sequence);
        } catch (const NativePortAudioCommandQueueError& error) {
            last_request_failure_ = map_queue_failure(error.failure());
            last_queue_failure_ = error.failure();
            record_failure(last_request_failure_, sequence);
        } catch (...) {
            record_failure(NativePortAudioExecutionDomainFailure::WorkerFailure,
                           sequence);
        }
        release_ack_slot(ack_slot);
        NativePortAudioExecutionDomainDispatchResult result;
        result.command_sequence = sequence;
        result.stamp = stamp;
        if (!ack.has_value()) {
            result.failure = last_request_failure_ ==
                                     NativePortAudioExecutionDomainFailure::None
                                 ? NativePortAudioExecutionDomainFailure::AckMissing
                                 : last_request_failure_;
            result.queue_failure = last_queue_failure_;
            return result;
        }
        result.has_ack = true;
        result.ack = *ack;
        if (ack->status != NativePortAudioCommandAckStatus::Completed) {
            result.failure =
                NativePortAudioExecutionDomainFailure::TargetExecutionFailed;
            result.queue_failure = NativePortAudioCommandQueueFailure::None;
        }
        return result;
    }

    void pump() noexcept {
        if (queue_.mode() != NativePortAudioCommandQueueMode::SerialReference)
            return;
        if (!producer_thread_allowed()) {
            record_failure(
                NativePortAudioExecutionDomainFailure::ProducerThreadViolation,
                0u);
            return;
        }
        serial_consumer_token_.store(audio_domain_thread_token(),
                                     std::memory_order_release);
        ensure_worker_telemetry();
        for (;;) {
            std::optional<NativePortAudioCommandConsumerLease> lease;
            try {
                lease = queue_.try_begin_consume();
            } catch (const NativePortAudioCommandQueueError& error) {
                record_failure(map_queue_failure(error.failure()),
                               error.command_sequence());
                if (shutdown_requested_.load(std::memory_order_acquire))
                    cleanup_registered_targets();
                return;
            } catch (...) {
                record_failure(NativePortAudioExecutionDomainFailure::WorkerFailure,
                               0u);
                if (shutdown_requested_.load(std::memory_order_acquire))
                    cleanup_registered_targets();
                return;
            }
            if (!lease.has_value()) break;
            process_lease(std::move(*lease));
        }
        static_cast<void>(service_consumer_if_requested());
        if (shutdown_requested_.load(std::memory_order_acquire))
            cleanup_registered_targets();
    }

    void request_consumer_service() noexcept {
        if (shutdown_requested_.load(std::memory_order_acquire) ||
            terminal_.load(std::memory_order_acquire))
            return;
        consumer_service_requested_.store(true, std::memory_order_release);
        queue_.notify_consumer_event();
    }

    void shutdown() noexcept {
        bool expected = false;
        const bool first_request = shutdown_requested_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire);
        if (first_request) queue_.request_shutdown();
        if (queue_.mode() == NativePortAudioCommandQueueMode::SerialReference) {
            if (producer_thread_allowed()) pump();
            return;
        }
        // A nested consumer-side failure may be the first shutdown requester.
        // A later producer-side owner still has to join that same worker;
        // returning merely because the request bit was already set would
        // leave a joinable thread in Impl's destructor.
        if (worker_.joinable() && !on_audio_thread()) worker_.join();
    }

    // Called only by the serial consumer after its final drain or by the
    // dedicated worker immediately before it publishes worker death.  The
    // producer-side shutdown path only requests/drains and joins; it never
    // destroys worker-owned state itself.
    void cleanup_registered_targets() noexcept {
        const bool previous_active = active_command_;
        const auto previous_sequence = active_command_sequence_;
        const auto previous_stamp = active_command_stamp_;
        active_command_ = true;
        active_command_sequence_ = 0u;
        active_command_stamp_ = {};
        // SoundBank Core owns mixer voices in AudioEngine, so release that
        // dependent target first while the engine executor is still active.
        // Same-target outer facades occupy lower slots than the streams they
        // create; ascending slot order lets their cleanup issue the nested
        // Destroy before that inner slot is visited.
        constexpr std::array<std::size_t,
                             native_port_audio_execution_domain_target_count>
            cleanup_order{1u, 0u, 2u, 3u};
        for (const auto target : cleanup_order) {
            for (std::uint32_t target_slot = 0u;
                 target_slot < native_port_audio_execution_domain_max_slots_per_target;
                 ++target_slot) {
                auto& slot = slots_[target *
                                    native_port_audio_execution_domain_max_slots_per_target +
                                    target_slot];
                auto state = slot.state.load(std::memory_order_acquire);
                for (;;) {
                    if (state == slot_free || state == slot_active) {
                        if (slot.state.compare_exchange_weak(
                                state, slot_retiring,
                                std::memory_order_acq_rel,
                                std::memory_order_acquire))
                            break;
                        continue;
                    }
                    // Registration and producer-side retirement own the slot
                    // until they publish Active/Free.  Shutdown must never
                    // overwrite either transition while its fields are in
                    // flight.
                    slot.state.wait(state, std::memory_order_acquire);
                    state = slot.state.load(std::memory_order_acquire);
                }
                slot.active_dispatches.fetch_or(active_dispatches_closed_bit,
                                                std::memory_order_acq_rel);
                auto active =
                    slot.active_dispatches.load(std::memory_order_acquire);
                while ((active & active_dispatches_count_mask) != 0u) {
                    slot.active_dispatches.wait(active,
                                                std::memory_order_acquire);
                    active = slot.active_dispatches.load(
                        std::memory_order_acquire);
                }
                const auto object = slot.object.load(std::memory_order_acquire);
                const auto cleanup =
                    slot.cleanup.exchange(nullptr, std::memory_order_acq_rel);
                if (cleanup != nullptr && object != nullptr) cleanup(object);
                consumer_service_target_mask_.fetch_and(
                    ~(std::uint64_t{1u}
                      << (target *
                              native_port_audio_execution_domain_max_slots_per_target +
                          target_slot)),
                    std::memory_order_acq_rel);
                slot.executor.store(nullptr, std::memory_order_release);
                slot.consumer_service.store(nullptr,
                                            std::memory_order_release);
                slot.object.store(nullptr, std::memory_order_release);
                // Retain the last generation as an invalidated lifecycle
                // identity.  A free slot must never reset it to zero.
                slot.state.store(slot_free, std::memory_order_release);
                slot.state.notify_all();
            }
        }
        active_command_ = previous_active;
        active_command_sequence_ = previous_sequence;
        active_command_stamp_ = previous_stamp;
    }

    [[nodiscard]] bool on_audio_thread() const noexcept {
        const auto token = audio_domain_thread_token();
        if (queue_.mode() == NativePortAudioCommandQueueMode::DedicatedThread)
            return worker_alive_.load(std::memory_order_acquire) &&
                   worker_thread_token_.load(std::memory_order_acquire) == token;
        // SerialReference keeps the consumer token after a pump so its queue
        // ownership remains stable.  That token alone is not an active outer
        // command: only an executor currently running through process_lease
        // may issue an inline nested operation.
        return serial_consumer_token_.load(std::memory_order_acquire) == token &&
               active_command_;
    }

    [[nodiscard]] NativePortAudioExecutionDomainSnapshot
    snapshot() const noexcept {
        NativePortAudioExecutionDomainSnapshot result;
        result.queue = queue_.snapshot();
        result.first_error = first_error_.load(std::memory_order_acquire);
        result.first_error_command_sequence =
            first_error_command_sequence_.load(std::memory_order_acquire);
        result.producer_thread_identity = result.queue.producer_thread_identity;
        result.consumer_thread_identity = result.queue.consumer_thread_identity;
        result.next_guest_sequence =
            next_guest_sequence_.load(std::memory_order_acquire);
        result.last_frame_index = last_frame_index_.load(std::memory_order_acquire);
        result.has_last_frame_index =
            has_last_frame_index_.load(std::memory_order_acquire);
        result.worker_alive = worker_alive_.load(std::memory_order_acquire);
        result.shutdown_requested =
            shutdown_requested_.load(std::memory_order_acquire);
        if (async_target_failure_published_.load(std::memory_order_acquire)) {
            result.has_async_target_failure = true;
            result.async_target_failure = async_target_failure_;
        }
        for (std::size_t target = 0u;
             target < native_port_audio_execution_domain_target_count; ++target) {
            for (std::uint32_t slot = 0u;
                 slot < native_port_audio_execution_domain_max_slots_per_target;
                 ++slot) {
                const auto index = target *
                                       native_port_audio_execution_domain_max_slots_per_target +
                                   slot;
                const auto& source = slots_[index];
                auto& destination = result.targets[index];
                destination.target =
                    static_cast<NativePortAudioExecutionDomainTarget>(target);
                destination.target_slot = slot;
                destination.target_generation =
                    source.generation.load(std::memory_order_acquire);
                destination.registered =
                    source.state.load(std::memory_order_acquire) == slot_active;
                destination.active_dispatches =
                    source.active_dispatches.load(std::memory_order_acquire) &
                    active_dispatches_count_mask;
                destination.executed_commands =
                    source.executed_commands.load(std::memory_order_acquire);
                destination.failed_commands =
                    source.failed_commands.load(std::memory_order_acquire);
            }
        }
        if (result.queue.submitted_commands != 0u &&
            result.queue.mode == NativePortAudioCommandQueueMode::DedicatedThread &&
            result.producer_thread_identity == result.consumer_thread_identity)
            result.first_error =
                NativePortAudioExecutionDomainFailure::ConsumerThreadViolation;
        return result;
    }

    [[nodiscard]] std::optional<std::uint64_t>
    last_frame_index_nonblocking() const noexcept {
        if (!has_last_frame_index_.load(std::memory_order_acquire))
            return std::nullopt;
        return last_frame_index_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t queued_commands_nonblocking() const noexcept {
        return queue_.queued_commands_nonblocking();
    }

    [[nodiscard]] bool bind_telemetry(NativePortTelemetry* telemetry) noexcept {
        if (telemetry == nullptr) {
            record_failure(
                NativePortAudioExecutionDomainFailure::TargetIdentityMismatch,
                0u);
            return false;
        }
        // Binding is lifecycle-only. Serialize the reference count with a
        // small mutex so a last unbind cannot race a new first bind; the
        // command/sample path only reads the published pointer.
        std::lock_guard lock(telemetry_binding_mutex_);
        auto* const configured = telemetry_.load(std::memory_order_acquire);
        const auto count = telemetry_bind_count_.load(std::memory_order_acquire);
        if ((configured == nullptr && count != 0u) ||
            (configured != nullptr && count == 0u) ||
            (configured != nullptr && configured != telemetry)) {
            record_failure(
                NativePortAudioExecutionDomainFailure::TargetIdentityMismatch,
                0u);
            return false;
        }
        // A writer from a previous binding may still be held until the
        // worker reaches its next bounded command boundary. Never switch it
        // to a different owner while that writer is live.
        auto* const active_writer_owner =
            telemetry_writer_owner_.load(std::memory_order_acquire);
        if (configured == nullptr && active_writer_owner != nullptr &&
            active_writer_owner != telemetry) {
            record_failure(
                NativePortAudioExecutionDomainFailure::TargetIdentityMismatch,
                0u);
            return false;
        }
        if (count == std::numeric_limits<std::uint32_t>::max()) {
            record_failure(
                NativePortAudioExecutionDomainFailure::TargetIdentityMismatch,
                0u);
            return false;
        }
        if (configured == nullptr)
            telemetry_.store(telemetry, std::memory_order_release);
        telemetry_bind_count_.store(count + 1u, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool unbind_telemetry(NativePortTelemetry* telemetry) noexcept {
        if (telemetry == nullptr) {
            record_failure(
                NativePortAudioExecutionDomainFailure::TargetIdentityMismatch,
                0u);
            return false;
        }
        bool needs_worker_barrier = false;
        {
            std::lock_guard lock(telemetry_binding_mutex_);
            auto* const configured = telemetry_.load(std::memory_order_acquire);
            const auto count =
                telemetry_bind_count_.load(std::memory_order_acquire);
            if (configured != telemetry || count == 0u) {
                record_failure(
                    NativePortAudioExecutionDomainFailure::TargetIdentityMismatch,
                    0u);
                return false;
            }
            if (count == 1u) {
                // A worker-side executor may still own a stage timer backed
                // by telemetry_writer_. Returning true here would let the
                // caller destroy the NativePortTelemetry owner before that
                // timer's destructor runs. Reject without changing either
                // binding field; producer-side unbind provides the required
                // command/ACK lifetime fence.
                if (on_audio_thread() && active_command_) return false;
                // The command boundary below is the lifetime fence: it makes
                // the consumer publish and destroy its writer before the
                // caller may release the NativePortTelemetry owner.
                telemetry_.store(nullptr, std::memory_order_release);
                telemetry_bind_count_.store(0u, std::memory_order_release);
                needs_worker_barrier = true;
            } else {
                telemetry_bind_count_.store(count - 1u,
                                            std::memory_order_release);
            }
        }
        if (!needs_worker_barrier) return true;
        if (on_audio_thread()) {
            // This path is reachable only outside an active executor (for
            // example during consumer-owned teardown), so no stage timer can
            // retain the writer and immediate consumer-side retirement is a
            // complete lifetime fence.
            ensure_worker_telemetry();
            return telemetry_writer_owner_.load(std::memory_order_acquire) ==
                   nullptr;
        }
        if (dispatch_telemetry_barrier()) return true;
        // A terminal queue cannot acknowledge a barrier. Joining the sole
        // consumer is the only remaining sound lifetime fence.
        shutdown();
        return telemetry_writer_owner_.load(std::memory_order_acquire) == nullptr;
    }

  private:
    [[nodiscard]] bool producer_thread_allowed() noexcept {
        const auto token = audio_domain_thread_token();
        auto owner = producer_thread_token_.load(std::memory_order_acquire);
        if (owner == 0u && producer_thread_token_.compare_exchange_strong(
                               owner, token, std::memory_order_acq_rel,
                               std::memory_order_acquire))
            return true;
        if (owner == token) return true;
        record_failure(
            NativePortAudioExecutionDomainFailure::ProducerThreadViolation, 0u);
        return false;
    }

    [[nodiscard]] bool dispatch_telemetry_barrier() noexcept {
        if (!queue_config_.enabled || on_audio_thread() ||
            !producer_thread_allowed())
            return false;
        if (const auto ready = check_dispatch_ready();
            ready != NativePortAudioExecutionDomainFailure::None)
            return false;
        const auto ack_slot = reserve_ack_slot();
        if (ack_slot == native_port_audio_command_queue_invalid_ack_slot)
            return false;

        NativePortAudioCommandStamp stamp;
        const auto frame = last_frame_index_nonblocking().value_or(0u);
        if (!make_stamp(frame, stamp)) {
            release_ack_slot(ack_slot);
            return false;
        }
        NativePortAudioPodCommand command;
        command.stamp = stamp;
        command.target = NativePortAudioCommandTarget::Lifecycle;
        command.opcode = telemetry_barrier_opcode;
        command.ack_slot = ack_slot;

        std::optional<NativePortAudioCommandProducerLease> lease;
        try {
            lease = queue_.wait_begin_produce(command);
        } catch (...) {
            release_ack_slot(ack_slot);
            return false;
        }
        if (!lease.has_value() || !lease->publish()) {
            if (lease.has_value()) lease->abort();
            release_ack_slot(ack_slot);
            return false;
        }
        const auto sequence = lease->command_sequence();
        commit_stamp(stamp);
        if (queue_.mode() == NativePortAudioCommandQueueMode::SerialReference)
            pump();

        std::optional<NativePortAudioCommandAck> ack;
        try {
            ack = queue_.wait_read_ack(ack_slot, sequence);
        } catch (...) {
            release_ack_slot(ack_slot);
            return false;
        }
        release_ack_slot(ack_slot);
        return ack.has_value() &&
               ack->status == NativePortAudioCommandAckStatus::Completed;
    }

    [[nodiscard]] NativePortAudioExecutionDomainFailure check_dispatch_ready()
        noexcept {
        if (!queue_config_.enabled) {
            last_request_failure_ =
                NativePortAudioExecutionDomainFailure::Disabled;
            last_queue_failure_ = NativePortAudioCommandQueueFailure::Disabled;
            return last_request_failure_;
        }
        if (shutdown_requested_.load(std::memory_order_acquire)) {
            last_request_failure_ = NativePortAudioExecutionDomainFailure::Shutdown;
            last_queue_failure_ = NativePortAudioCommandQueueFailure::QueueClosed;
            return last_request_failure_;
        }
        if (terminal_.load(std::memory_order_acquire)) {
            if (async_target_failure_published_.load(
                    std::memory_order_acquire)) {
                last_request_failure_ =
                    NativePortAudioExecutionDomainFailure::TargetExecutionFailed;
                last_queue_failure_ =
                    NativePortAudioCommandQueueFailure::WorkerFailure;
            } else {
                last_request_failure_ =
                    NativePortAudioExecutionDomainFailure::QueueFailed;
                last_queue_failure_ =
                    NativePortAudioCommandQueueFailure::QueueFailed;
            }
            return last_request_failure_;
        }
        last_request_failure_ = NativePortAudioExecutionDomainFailure::None;
        last_queue_failure_ = NativePortAudioCommandQueueFailure::None;
        return NativePortAudioExecutionDomainFailure::None;
    }

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult
    failed_dispatch_result(
        const NativePortAudioExecutionDomainFailure failure) noexcept {
        auto result = failed_result(failure, last_queue_failure_);
        if (failure ==
                NativePortAudioExecutionDomainFailure::TargetExecutionFailed &&
            async_target_failure_published_.load(std::memory_order_acquire)) {
            const auto& record = async_target_failure_;
            result.has_target_failure = true;
            result.target_failure_command_sequence = record.command_sequence;
            result.target_failure_frame_index = record.frame_index;
            result.target_failure_guest_sequence = record.guest_sequence;
            result.target_failure_target = record.target;
            result.target_failure_opcode = record.opcode;
            result.target_failure_slot = record.target_slot;
            result.target_failure_generation = record.target_generation;
            result.target_failure_error_code = record.target_error_code;
            result.command_sequence = record.command_sequence;
            result.stamp.frame_index = record.frame_index;
            result.stamp.guest_sequence = record.guest_sequence;
        }
        return result;
    }

    [[nodiscard]] bool validate_request(
        const NativePortAudioExecutionDomainTarget target,
        const std::uint32_t target_slot,
        const std::uint32_t target_generation,
        const std::span<const NativePortAudioExecutionDomainPayloadPart> parts,
        const std::uint32_t flags,
        const NativePortAudioExecutionDomainStage stage,
        std::uint32_t& total_size) noexcept {
        total_size = 0u;
        if (!valid_target(target) ||
            target_slot >= native_port_audio_execution_domain_max_slots_per_target ||
            target_generation == 0u) {
            last_request_failure_ =
                NativePortAudioExecutionDomainFailure::InvalidTarget;
            last_queue_failure_ = NativePortAudioCommandQueueFailure::None;
            return false;
        }
        if ((flags & ~native_port_audio_execution_domain_user_flags_mask) != 0u ||
            static_cast<std::uint8_t>(stage) >
                static_cast<std::uint8_t>(
                    NativePortAudioExecutionDomainStage::AudioDecodeAndMix)) {
            last_request_failure_ =
                NativePortAudioExecutionDomainFailure::InvalidFlags;
            last_queue_failure_ = NativePortAudioCommandQueueFailure::None;
            return false;
        }
        const auto& slot = slots_[target_slot_index(target, target_slot)];
        if (slot.state.load(std::memory_order_acquire) != slot_active) {
            last_request_failure_ =
                NativePortAudioExecutionDomainFailure::TargetNotRegistered;
            last_queue_failure_ = NativePortAudioCommandQueueFailure::None;
            return false;
        }
        if (slot.generation.load(std::memory_order_acquire) !=
                target_generation ||
            slot.executor.load(std::memory_order_acquire) == nullptr) {
            last_request_failure_ =
                NativePortAudioExecutionDomainFailure::TargetIdentityMismatch;
            last_queue_failure_ = NativePortAudioCommandQueueFailure::None;
            return false;
        }
        for (const auto& part : parts) {
            if (part.size != 0u && part.data == nullptr) {
                last_request_failure_ =
                    NativePortAudioExecutionDomainFailure::InvalidPayload;
                last_queue_failure_ = NativePortAudioCommandQueueFailure::None;
                return false;
            }
            if (part.size > std::numeric_limits<std::uint32_t>::max() -
                                total_size) {
                last_request_failure_ =
                    NativePortAudioExecutionDomainFailure::InvalidPayload;
                last_queue_failure_ =
                    NativePortAudioCommandQueueFailure::PayloadOverflow;
                return false;
            }
            total_size += part.size;
        }
        if (total_size > queue_config_.maximum_payload_bytes) {
            last_request_failure_ =
                NativePortAudioExecutionDomainFailure::InvalidPayload;
            last_queue_failure_ =
                NativePortAudioCommandQueueFailure::PayloadOverflow;
            return false;
        }
        return true;
    }

    [[nodiscard]] std::uint32_t encode_flags(
        const std::uint32_t flags,
        const NativePortAudioExecutionDomainStage stage) const noexcept {
        return (flags & native_port_audio_execution_domain_user_flags_mask) |
               (static_cast<std::uint32_t>(stage)
                << native_port_audio_execution_domain_stage_shift);
    }

    [[nodiscard]] NativePortAudioExecutionDomainStage decode_stage(
        const std::uint32_t flags) const noexcept {
        return static_cast<NativePortAudioExecutionDomainStage>(
            (flags & native_port_audio_execution_domain_stage_mask) >>
            native_port_audio_execution_domain_stage_shift);
    }

    [[nodiscard]] bool make_stamp(
        const std::uint64_t frame_index,
        NativePortAudioCommandStamp& stamp) noexcept {
        stamp.frame_index = frame_index;
        stamp.guest_sequence =
            next_guest_sequence_.load(std::memory_order_acquire);
        if (stamp.guest_sequence == std::numeric_limits<std::uint64_t>::max()) {
            last_request_failure_ =
                NativePortAudioExecutionDomainFailure::StampOverflow;
            last_queue_failure_ = NativePortAudioCommandQueueFailure::StampOverflow;
            record_failure(last_request_failure_, 0u);
            return false;
        }
        if (has_last_frame_index_.load(std::memory_order_acquire) &&
            frame_index < last_frame_index_.load(std::memory_order_acquire)) {
            last_request_failure_ =
                NativePortAudioExecutionDomainFailure::FrameRegression;
            last_queue_failure_ = NativePortAudioCommandQueueFailure::FrameRegression;
            record_failure(last_request_failure_, 0u);
            return false;
        }
        return true;
    }

    void commit_stamp(const NativePortAudioCommandStamp stamp) noexcept {
        next_guest_sequence_.store(stamp.guest_sequence + 1u,
                                    std::memory_order_release);
        last_frame_index_.store(stamp.frame_index, std::memory_order_release);
        has_last_frame_index_.store(true, std::memory_order_release);
    }

    [[nodiscard]] std::uint32_t reserve_ack_slot() noexcept {
        if (queue_config_.maximum_ack_slots == 0u)
            return native_port_audio_command_queue_invalid_ack_slot;
        const auto start = next_ack_slot_.fetch_add(1u, std::memory_order_relaxed);
        for (std::uint32_t count = 0u;
             count < queue_config_.maximum_ack_slots; ++count) {
            const auto index = (start + count) % queue_config_.maximum_ack_slots;
            auto expected = slot_free;
            if (ack_slots_[index].compare_exchange_strong(
                    expected, 1u, std::memory_order_acq_rel,
                    std::memory_order_acquire))
                return index;
        }
        return native_port_audio_command_queue_invalid_ack_slot;
    }

    void release_ack_slot(const std::uint32_t slot) noexcept {
        if (slot < queue_config_.maximum_ack_slots)
            ack_slots_[slot].store(slot_free, std::memory_order_release);
    }

    [[nodiscard]] PinnedTarget pin_target(
        const NativePortAudioExecutionDomainTarget target,
        const std::uint32_t target_slot,
        const std::uint32_t target_generation) noexcept {
        PinnedTarget result;
        if (!valid_target(target) ||
            target_slot >= native_port_audio_execution_domain_max_slots_per_target)
            return result;
        auto& slot = slots_[target_slot_index(target, target_slot)];
        if (slot.state.load(std::memory_order_acquire) != slot_active)
            return result;

        auto active = slot.active_dispatches.load(std::memory_order_acquire);
        for (;;) {
            if ((active & active_dispatches_closed_bit) != 0u ||
                (active & active_dispatches_count_mask) ==
                    active_dispatches_count_mask)
                return result;
            if (slot.active_dispatches.compare_exchange_weak(
                    active, active + 1u, std::memory_order_acq_rel,
                    std::memory_order_acquire))
                break;
        }

        const auto object = slot.object.load(std::memory_order_acquire);
        const auto executor = slot.executor.load(std::memory_order_acquire);
        const auto consumer_service =
            slot.consumer_service.load(std::memory_order_acquire);
        const auto generation = slot.generation.load(std::memory_order_acquire);
        if (object == nullptr || executor == nullptr ||
            generation != target_generation ||
            slot.state.load(std::memory_order_acquire) != slot_active ||
            slot.object.load(std::memory_order_acquire) != object ||
            slot.generation.load(std::memory_order_acquire) != generation) {
            const auto previous = slot.active_dispatches.fetch_sub(
                1u, std::memory_order_release);
            if ((previous & active_dispatches_count_mask) == 1u)
                slot.active_dispatches.notify_all();
            return result;
        }
        result.slot = &slot;
        result.object = object;
        result.executor = executor;
        result.consumer_service = consumer_service;
        result.valid = true;
        return result;
    }

    void unpin_target(PinnedTarget& target) noexcept {
        if (target.valid && target.slot != nullptr) {
            const auto previous = target.slot->active_dispatches.fetch_sub(
                1u, std::memory_order_release);
            if ((previous & active_dispatches_count_mask) == 1u)
                target.slot->active_dispatches.notify_all();
        }
        target = {};
    }

    [[nodiscard]] NativePortAudioExecutionDomainDispatchResult dispatch_inline(
        const NativePortAudioExecutionDomainTarget target,
        const std::uint32_t target_slot,
        const std::uint32_t target_generation,
        const std::uint16_t opcode,
        const std::span<const NativePortAudioExecutionDomainPayloadPart> parts,
        const std::uint64_t /*frame_index*/,
        const std::uint32_t /*flags*/,
        const NativePortAudioExecutionDomainStage /*stage*/,
        const bool synchronous) noexcept {
        // Command sequence zero is the first valid command.  Keep a separate
        // worker-only activity bit so nested dispatch cannot mistake that
        // first command for "no active outer command".
        if (!active_command_)
            return failed_result(
                NativePortAudioExecutionDomainFailure::ConsumerThreadViolation);
        if (parts.size() != 1u)
            return failed_result(
                NativePortAudioExecutionDomainFailure::ScatterOnWorker,
                NativePortAudioCommandQueueFailure::None,
                active_command_sequence_, active_command_stamp_);
        const auto& part = parts.front();
        if (part.size != 0u && part.data == nullptr)
            return failed_result(
                NativePortAudioExecutionDomainFailure::InvalidPayload,
                NativePortAudioCommandQueueFailure::None,
                active_command_sequence_, active_command_stamp_);
        auto pinned = pin_target(target, target_slot, target_generation);
        if (!pinned.valid)
            return failed_result(
                NativePortAudioExecutionDomainFailure::TargetIdentityMismatch,
                NativePortAudioCommandQueueFailure::None,
                active_command_sequence_, active_command_stamp_);
        NativePortAudioCommandAckResult ack_result;
        execute_target(pinned, opcode,
                       std::span<const std::byte>(part.data, part.size),
                       ack_result,
                       // A nested target call is part of the same outer
                       // command region; do not create a second telemetry
                       // timer or imply another audio stage.
                       NativePortAudioExecutionDomainStage::None);
        unpin_target(pinned);
        NativePortAudioExecutionDomainDispatchResult result;
        result.command_sequence = active_command_sequence_;
        result.stamp = active_command_stamp_;
        result.inline_execution = true;
        if (ack_result.status != NativePortAudioCommandAckStatus::Completed)
            result.failure =
                NativePortAudioExecutionDomainFailure::TargetExecutionFailed;
        if (!synchronous) return result;
        result.has_ack = true;
        result.ack.command_sequence = active_command_sequence_;
        result.ack.stamp = active_command_stamp_;
        result.ack.status = ack_result.status;
        result.ack.error_code = ack_result.error_code;
        result.ack.result_size = ack_result.result_size;
        if (ack_result.result_size != 0u)
            std::memcpy(result.ack.bytes.data(), ack_result.bytes.data(),
                        ack_result.result_size);
        return result;
    }

    void execute_target(
        PinnedTarget& pinned,
        const std::uint16_t opcode,
        const std::span<const std::byte> payload,
        NativePortAudioCommandAckResult& result,
        const NativePortAudioExecutionDomainStage stage) noexcept {
        if (telemetry_writer_.has_value() &&
            stage == NativePortAudioExecutionDomainStage::AudioDecode) {
            NativePortTelemetryTimer timer(
                *telemetry_writer_, NativePortTelemetryStage::AudioDecode);
            pinned.executor(pinned.object, opcode, payload, result);
        } else if (telemetry_writer_.has_value() &&
                   stage == NativePortAudioExecutionDomainStage::AudioMix) {
            NativePortTelemetryTimer timer(
                *telemetry_writer_, NativePortTelemetryStage::AudioMix);
            pinned.executor(pinned.object, opcode, payload, result);
        } else if (telemetry_writer_.has_value() &&
                   stage == NativePortAudioExecutionDomainStage::AudioDecodeAndMix) {
            // This is one broad executor region without separable phase
            // boundaries. Keep its total distinct from both narrower stages.
            NativePortTelemetryTimer service_total_timer(
                *telemetry_writer_,
                NativePortTelemetryStage::AudioServiceTotal);
            pinned.executor(pinned.object, opcode, payload, result);
        } else {
            pinned.executor(pinned.object, opcode, payload, result);
        }
        if (result.status == NativePortAudioCommandAckStatus::Failed)
            pinned.slot->failed_commands.fetch_add(1u, std::memory_order_relaxed);
        else
            pinned.slot->executed_commands.fetch_add(1u, std::memory_order_relaxed);
    }

    [[nodiscard]] bool service_pending_targets() noexcept {
        auto mask = consumer_service_target_mask_.load(std::memory_order_acquire);
        if (mask == 0u) return true;

        const bool previous_active = active_command_;
        const auto previous_sequence = active_command_sequence_;
        const auto previous_stamp = active_command_stamp_;
        active_command_ = true;
        active_command_sequence_ = 0u;
        active_command_stamp_.frame_index =
            last_frame_index_.load(std::memory_order_acquire);
        const auto next_guest =
            next_guest_sequence_.load(std::memory_order_acquire);
        active_command_stamp_.guest_sequence =
            next_guest == 0u ? 0u : next_guest - 1u;

        bool succeeded = true;
        while (mask != 0u) {
            const auto index = static_cast<std::size_t>(std::countr_zero(mask));
            mask &= mask - 1u;
            const auto target = static_cast<NativePortAudioExecutionDomainTarget>(
                index /
                native_port_audio_execution_domain_max_slots_per_target);
            const auto target_slot = static_cast<std::uint32_t>(
                index %
                native_port_audio_execution_domain_max_slots_per_target);
            const auto generation =
                registered_generation(target, target_slot);
            if (generation == 0u) continue;
            auto pinned = pin_target(target, target_slot, generation);
            if (!pinned.valid || pinned.consumer_service == nullptr) {
                unpin_target(pinned);
                continue;
            }
            const auto error = pinned.consumer_service(pinned.object);
            if (error != 0u) {
                pinned.slot->failed_commands.fetch_add(
                    1u, std::memory_order_relaxed);
                unpin_target(pinned);
                record_failure(
                    NativePortAudioExecutionDomainFailure::TargetExecutionFailed,
                    0u);
                queue_.fail_terminal(
                    NativePortAudioCommandQueueFailure::WorkerFailure, 0u);
                succeeded = false;
                break;
            }
            unpin_target(pinned);
        }

        active_command_ = previous_active;
        active_command_sequence_ = previous_sequence;
        active_command_stamp_ = previous_stamp;
        return succeeded;
    }

    [[nodiscard]] bool service_consumer_if_requested() noexcept {
        if (!consumer_service_requested_.exchange(false,
                                                   std::memory_order_acq_rel))
            return true;
        if (terminal_.load(std::memory_order_acquire)) return true;
        return service_pending_targets();
    }

    void process_lease(NativePortAudioCommandConsumerLease lease) noexcept {
        // A facade may bind telemetry after the worker has started.  Resolve
        // that lifecycle-only handoff on the consumer before this command's
        // stage timer is created; the producer never constructs or touches a
        // writer.
        ensure_worker_telemetry();
        const auto command = lease.command();
        if (lease.cancelled()) {
            observe_queue_depth();
            publish_worker_telemetry();
            static_cast<void>(lease.cancel(1u));
            return;
        }
        NativePortAudioExecutionDomainTarget target =
            NativePortAudioExecutionDomainTarget::AudioEngine;
        switch (command.target) {
        case NativePortAudioCommandTarget::AudioEngine:
            target = NativePortAudioExecutionDomainTarget::AudioEngine;
            break;
        case NativePortAudioCommandTarget::SoundBank:
            target = NativePortAudioExecutionDomainTarget::SoundBank;
            break;
        case NativePortAudioCommandTarget::HostOutput:
            target = NativePortAudioExecutionDomainTarget::HostOutput;
            break;
        case NativePortAudioCommandTarget::Movie:
            target = NativePortAudioExecutionDomainTarget::Movie;
            break;
        case NativePortAudioCommandTarget::Lifecycle:
            observe_queue_depth();
            publish_worker_telemetry();
            if (command.opcode == telemetry_barrier_opcode &&
                command.target_slot == 0u &&
                command.target_generation == 0u && lease.payload().empty())
                static_cast<void>(lease.complete());
            else
                static_cast<void>(lease.fail(1u));
            return;
        }
        if (!queue_target_matches(target, command.target)) {
            observe_queue_depth();
            publish_worker_telemetry();
            static_cast<void>(lease.fail(1u));
            return;
        }
        const auto stage = decode_stage(command.flags);
        if (static_cast<std::uint8_t>(stage) >
            static_cast<std::uint8_t>(
                NativePortAudioExecutionDomainStage::AudioDecodeAndMix)) {
            observe_queue_depth();
            publish_worker_telemetry();
            static_cast<void>(lease.fail(1u));
            return;
        }
        auto pinned = pin_target(target, command.target_slot,
                                 command.target_generation);
        NativePortAudioCommandAckResult result;
        if (!pinned.valid) {
            result.status = NativePortAudioCommandAckStatus::Failed;
            result.error_code = 1u;
        } else {
            active_command_ = true;
            active_command_sequence_ = command.command_sequence;
            active_command_stamp_ = command.stamp;
            execute_target(pinned, command.opcode, lease.payload(), result,
                           stage);
            active_command_ = false;
            active_command_sequence_ = 0u;
            active_command_stamp_ = {};
            unpin_target(pinned);
        }
        // The ACK release is the producer-visible fence for this command.  A
        // consumer must publish completed decode/mix/queue-depth telemetry
        // before releasing that fence.
        observe_queue_depth();
        publish_worker_telemetry();
        const bool terminal_async_failure =
            command.ack_slot ==
                native_port_audio_command_queue_invalid_ack_slot &&
            result.status == NativePortAudioCommandAckStatus::Failed;
        if (terminal_async_failure) {
            publish_async_target_failure(command, result.error_code);
            record_failure(
                NativePortAudioExecutionDomainFailure::TargetExecutionFailed,
                command.command_sequence);
        }
        if (!lease.complete(result)) {
            record_failure(NativePortAudioExecutionDomainFailure::WorkerFailure,
                           command.command_sequence);
            queue_.fail_terminal(
                NativePortAudioCommandQueueFailure::WorkerFailure,
                command.command_sequence);
        } else if (terminal_async_failure) {
            // Finish this command exactly once, then cancel every later ready
            // command. The first full target-failure record was release-
            // published before the producer-visible terminal bit above.
            queue_.fail_terminal(
                NativePortAudioCommandQueueFailure::WorkerFailure,
                command.command_sequence);
        }
    }

    void worker_main() noexcept {
        worker_thread_token_.store(audio_domain_thread_token(),
                                   std::memory_order_release);
        worker_alive_.store(true, std::memory_order_release);
        ensure_worker_telemetry();
        try {
            for (;;) {
                // Commands always win over completion maintenance so a
                // Pause/Resume/Destroy behind a full output burst cannot be
                // head-of-line blocked by retry work.
                if (auto lease = queue_.try_begin_consume(); lease.has_value()) {
                    process_lease(std::move(*lease));
                    continue;
                }
                if (!service_consumer_if_requested()) continue;
                if (queue_.consumer_closed_nonblocking()) break;

                const auto observed =
                    queue_.consumer_event_epoch_nonblocking();
                // Close the observe/wait race without polling: producer
                // publication, shutdown, and host completion all advance the
                // same atomic epoch.
                if (queue_.queued_commands_nonblocking() != 0u ||
                    consumer_service_requested_.load(
                        std::memory_order_acquire) ||
                    // Draining is finalized by try_begin_consume() once the
                    // queue is empty. If shutdown advanced the event epoch
                    // before `observed` was captured, waiting here would
                    // otherwise miss the sole wake and leave join() blocked
                    // forever with the queue still in Draining.
                    shutdown_requested_.load(std::memory_order_acquire) ||
                    queue_.consumer_closed_nonblocking())
                    continue;
                queue_.wait_for_consumer_event(observed);
            }
        } catch (const NativePortAudioCommandQueueError& error) {
            record_failure(NativePortAudioExecutionDomainFailure::WorkerFailure,
                           error.command_sequence());
            queue_.fail_terminal(NativePortAudioCommandQueueFailure::WorkerFailure,
                                 error.command_sequence());
        } catch (...) {
            record_failure(NativePortAudioExecutionDomainFailure::WorkerFailure,
                           0u);
            queue_.fail_terminal(NativePortAudioCommandQueueFailure::WorkerFailure,
                                 0u);
        }
        // All terminal target destruction belongs to the consumer.  In
        // dedicated mode this is the last worker action while the facade
        // objects are still alive; shutdown() only joins afterwards.
        cleanup_registered_targets();
        publish_worker_telemetry();
        telemetry_writer_.reset();
        telemetry_writer_owner_.store(nullptr, std::memory_order_release);
        worker_alive_.store(false, std::memory_order_release);
    }

    void publish_worker_telemetry() noexcept {
        auto* const owner =
            telemetry_writer_owner_.load(std::memory_order_acquire);
        if (owner != nullptr && telemetry_writer_.has_value())
            owner->publish(*telemetry_writer_);
    }

    void ensure_worker_telemetry() noexcept {
        auto* const configured = telemetry_.load(std::memory_order_acquire);
        if (configured == nullptr) {
            // Last-lease unbind is allowed while the worker is idle or while
            // a prior writer is still retained. Reset and flush that writer
            // only here, on the owning audio worker, never on the producer.
            if (telemetry_writer_.has_value()) {
                publish_worker_telemetry();
                telemetry_writer_.reset();
            }
            telemetry_writer_owner_.store(nullptr, std::memory_order_release);
            return;
        }
        auto* const active =
            telemetry_writer_owner_.load(std::memory_order_acquire);
        if (active != nullptr && active != configured) {
            record_failure(
                NativePortAudioExecutionDomainFailure::TargetIdentityMismatch,
                active_command_sequence_);
            if (on_audio_thread())
                queue_.fail_terminal(
                    NativePortAudioCommandQueueFailure::WorkerFailure,
                    active_command_sequence_);
            return;
        }
        if (configured != nullptr && !telemetry_writer_.has_value()) {
            telemetry_writer_.emplace(configured->make_writer());
            telemetry_writer_owner_.store(configured, std::memory_order_release);
        }
    }

    void observe_queue_depth() noexcept {
        auto* const telemetry = telemetry_.load(std::memory_order_acquire);
        if (telemetry != nullptr)
            telemetry->observe_audio_queue_depth(
                queue_.queued_commands_nonblocking());
    }

    void publish_async_target_failure(
        const NativePortAudioPodCommand& command,
        const std::uint32_t target_error_code) noexcept {
        if (async_target_failure_published_.load(std::memory_order_acquire))
            return;
        async_target_failure_ = {
            command.command_sequence,
            command.stamp.frame_index,
            command.stamp.guest_sequence,
            command.target,
            command.opcode,
            command.target_slot,
            command.target_generation,
            target_error_code};
        async_target_failure_published_.store(true,
                                              std::memory_order_release);
    }

    void record_failure(
        const NativePortAudioExecutionDomainFailure failure,
        const std::uint64_t sequence) noexcept {
        if (failure == NativePortAudioExecutionDomainFailure::None) return;
        bool expected = false;
        const bool first = first_error_claimed_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire);
        if (first) {
            // Publish the sequence before the error with one release edge so
            // a snapshot that observes a non-None error cannot observe an
            // uninitialized/stale first-error sequence.
            first_error_command_sequence_.store(sequence,
                                                std::memory_order_relaxed);
            first_error_.store(failure, std::memory_order_release);
            first_error_published_.store(true, std::memory_order_release);
            first_error_published_.notify_all();
        } else {
            // A competing terminal reporter must not publish the terminal
            // bit before the CAS winner's error+sequence payload is visible.
            // Errors are exceptional, so this bounded publication handoff is
            // deliberately outside every command/sample success hotpath.
            while (!first_error_published_.load(std::memory_order_acquire))
                first_error_published_.wait(false,
                                            std::memory_order_acquire);
        }
        if (failure == NativePortAudioExecutionDomainFailure::WorkerFailure ||
            failure == NativePortAudioExecutionDomainFailure::QueueFailed ||
            failure ==
                NativePortAudioExecutionDomainFailure::TargetExecutionFailed ||
            failure == NativePortAudioExecutionDomainFailure::StampOverflow)
            terminal_.store(true, std::memory_order_release);
    }

    NativePortAudioExecutionDomain* owner_ = nullptr;
    NativePortAudioCommandQueueConfig queue_config_{};
    NativePortAudioCommandQueue queue_;
    std::unique_ptr<std::atomic<std::uint8_t>[]> ack_slots_;
    std::array<TargetSlot,
               native_port_audio_execution_domain_slot_count>
        slots_{};
    std::atomic<std::uint32_t> next_ack_slot_{0u};
    std::atomic<std::uint64_t> producer_thread_token_{0u};
    std::atomic<std::uint64_t> worker_thread_token_{0u};
    std::atomic<std::uint64_t> serial_consumer_token_{0u};
    std::atomic<std::uint64_t> next_guest_sequence_{0u};
    std::atomic<std::uint64_t> last_frame_index_{0u};
    std::atomic<bool> has_last_frame_index_{false};
    std::atomic<bool> worker_alive_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> terminal_{false};
    std::atomic<bool> consumer_service_requested_{false};
    std::atomic<std::uint64_t> consumer_service_target_mask_{0u};
    std::atomic<bool> first_error_claimed_{false};
    std::atomic<bool> first_error_published_{false};
    std::atomic<NativePortAudioExecutionDomainFailure> first_error_{
        NativePortAudioExecutionDomainFailure::None};
    std::atomic<std::uint64_t> first_error_command_sequence_{0u};
    NativePortAudioExecutionDomainTargetFailureRecord async_target_failure_{};
    std::atomic<bool> async_target_failure_published_{false};
    std::atomic<NativePortTelemetry*> telemetry_{nullptr};
    std::atomic<std::uint32_t> telemetry_bind_count_{0u};
    std::mutex telemetry_binding_mutex_;
    std::atomic<NativePortTelemetry*> telemetry_writer_owner_{nullptr};
    std::optional<NativePortTelemetryWriter> telemetry_writer_;
    std::thread worker_;
    // Accessed only by the one active consumer (dedicated worker or serial
    // pump). It is deliberately separate from active_command_sequence_,
    // whose zero value is a legitimate first sequence.
    bool active_command_ = false;
    std::uint64_t active_command_sequence_ = 0u;
    NativePortAudioCommandStamp active_command_stamp_{};
};

NativePortAudioExecutionDomain::ProducerLease::ProducerLease(
    NativePortAudioExecutionDomain* const domain,
    NativePortAudioCommandProducerLease&& queue_lease,
    const NativePortAudioCommandStamp stamp,
    const NativePortAudioExecutionDomainDispatchResult result) noexcept
    : domain_(domain), queue_lease_(std::move(queue_lease)), stamp_(stamp),
      result_(result), active_(queue_lease_.valid()) {}

NativePortAudioExecutionDomain::ProducerLease::~ProducerLease() { abort(); }

NativePortAudioExecutionDomain::ProducerLease::ProducerLease(
    ProducerLease&& other) noexcept
    : domain_(other.domain_), queue_lease_(std::move(other.queue_lease_)),
      stamp_(other.stamp_), result_(other.result_), active_(other.active_) {
    other.domain_ = nullptr;
    other.stamp_ = {};
    other.result_ = {};
    other.active_ = false;
}

NativePortAudioExecutionDomain::ProducerLease&
NativePortAudioExecutionDomain::ProducerLease::operator=(
    ProducerLease&& other) noexcept {
    if (this == &other) return *this;
    abort();
    domain_ = other.domain_;
    queue_lease_ = std::move(other.queue_lease_);
    stamp_ = other.stamp_;
    result_ = other.result_;
    active_ = other.active_;
    other.domain_ = nullptr;
    other.stamp_ = {};
    other.result_ = {};
    other.active_ = false;
    return *this;
}

bool NativePortAudioExecutionDomain::ProducerLease::valid() const noexcept {
    return active_ && queue_lease_.valid();
}

std::span<std::byte>
NativePortAudioExecutionDomain::ProducerLease::payload() noexcept {
    return valid() ? queue_lease_.payload() : std::span<std::byte>{};
}

const NativePortAudioCommandStamp&
NativePortAudioExecutionDomain::ProducerLease::stamp() const noexcept {
    return stamp_;
}

std::uint64_t NativePortAudioExecutionDomain::ProducerLease::command_sequence()
    const noexcept {
    return result_.command_sequence;
}

const NativePortAudioExecutionDomainDispatchResult&
NativePortAudioExecutionDomain::ProducerLease::result() const noexcept {
    return result_;
}

bool NativePortAudioExecutionDomain::ProducerLease::publish() noexcept {
    if (!valid()) return false;
    if (!queue_lease_.publish()) {
        queue_lease_.abort();
        result_.failure = NativePortAudioExecutionDomainFailure::QueueFailed;
        result_.queue_failure =
            NativePortAudioCommandQueueFailure::QueueFailed;
        active_ = false;
        return false;
    }
    result_.command_sequence = queue_lease_.command_sequence();
    result_.stamp = stamp_;
    if (domain_ != nullptr && domain_->impl_ != nullptr) {
        domain_->impl_->commit_stamp(stamp_);
        domain_->impl_->observe_queue_depth();
    }
    active_ = false;
    return true;
}

void NativePortAudioExecutionDomain::ProducerLease::abort() noexcept {
    if (!active_) return;
    queue_lease_.abort();
    active_ = false;
}

NativePortAudioExecutionDomain::NativePortAudioExecutionDomain(
    const NativePortAudioExecutionDomainConfig& config)
    : impl_(std::make_unique<Impl>(this, config)) {}

NativePortAudioExecutionDomain::~NativePortAudioExecutionDomain() = default;

std::shared_ptr<NativePortAudioExecutionDomain>
NativePortAudioExecutionDomain::acquire(
    const NativePortAudioExecutionDomainConfig& config) {
    return acquire_native_port_audio_execution_domain(config);
}

NativePortAudioCommandQueueMode NativePortAudioExecutionDomain::mode()
    const noexcept {
    return impl_->mode();
}

bool NativePortAudioExecutionDomain::configuration_matches(
    const NativePortAudioExecutionDomainConfig& config) const noexcept {
    return impl_->configuration_matches(config);
}

NativePortAudioExecutionDomain::ProducerLease
NativePortAudioExecutionDomain::begin_async_payload(
    const NativePortAudioExecutionDomainTargetHandle& handle,
    const std::uint16_t opcode,
    const std::uint32_t payload_size,
    const std::uint64_t frame_index,
    const std::uint32_t flags,
    const NativePortAudioExecutionDomainStage stage) noexcept {
    return impl_->begin_async_payload(handle, opcode, payload_size, frame_index,
                                      flags, stage);
}

bool NativePortAudioExecutionDomain::register_target(
    const NativePortAudioExecutionDomainTarget target,
    const std::uint32_t target_slot,
    const std::uint32_t target_generation,
    void* const object,
    const NativePortAudioExecutionDomainExecutor executor,
    const NativePortAudioExecutionDomainCleanup cleanup,
    const NativePortAudioExecutionDomainConsumerService
        consumer_service) noexcept {
    return impl_->register_target(target, target_slot, target_generation, object,
                                  executor, cleanup, consumer_service);
}

std::optional<NativePortAudioExecutionDomainTargetHandle>
NativePortAudioExecutionDomain::register_target(
    const NativePortAudioExecutionDomainTarget target,
    void* const object,
    const NativePortAudioExecutionDomainExecutor executor,
    const NativePortAudioExecutionDomainCleanup cleanup,
    const NativePortAudioExecutionDomainConsumerService
        consumer_service) noexcept {
    return impl_->register_target(target, object, executor, cleanup,
                                  consumer_service);
}

bool NativePortAudioExecutionDomain::unregister_target(
    const NativePortAudioExecutionDomainTargetHandle& handle,
    void* const object) noexcept {
    return impl_->unregister_target(handle, object);
}

bool NativePortAudioExecutionDomain::unregister_target(
    const NativePortAudioExecutionDomainTarget target,
    const std::uint32_t target_slot,
    const std::uint32_t target_generation,
    void* const object) noexcept {
    return impl_->unregister_target(target, target_slot, target_generation,
                                    object);
}

bool NativePortAudioExecutionDomain::unregister_target(
    const NativePortAudioExecutionDomainTarget target,
    void* const object) noexcept {
    return impl_->unregister_target(target, object);
}

NativePortAudioExecutionDomainDispatchResult
NativePortAudioExecutionDomain::dispatch_async(
    const NativePortAudioExecutionDomainTargetHandle& handle,
    const std::uint16_t opcode,
    const std::span<const std::byte> payload,
    const std::uint64_t frame_index,
    const std::uint32_t flags,
    const NativePortAudioExecutionDomainStage stage) noexcept {
    return impl_->dispatch_async(handle, opcode, payload, frame_index, flags,
                                 stage);
}

NativePortAudioExecutionDomainDispatchResult
NativePortAudioExecutionDomain::dispatch_async(
    const NativePortAudioExecutionDomainTarget target,
    const std::uint32_t target_slot,
    const std::uint32_t target_generation,
    const std::uint16_t opcode,
    const std::span<const std::byte> payload,
    const std::uint64_t frame_index,
    const std::uint32_t flags,
    const NativePortAudioExecutionDomainStage stage) noexcept {
    return impl_->dispatch_async(target, target_slot, target_generation, opcode,
                                 payload, frame_index, flags, stage);
}

NativePortAudioExecutionDomainDispatchResult
NativePortAudioExecutionDomain::dispatch_async(
    const NativePortAudioExecutionDomainTarget target,
    const std::uint16_t opcode,
    const std::span<const std::byte> payload,
    const std::uint64_t frame_index,
    const std::uint32_t flags,
    const NativePortAudioExecutionDomainStage stage) noexcept {
    return impl_->dispatch_async(target, opcode, payload, frame_index, flags,
                                 stage);
}

NativePortAudioExecutionDomainDispatchResult
NativePortAudioExecutionDomain::dispatch_async_scatter(
    const NativePortAudioExecutionDomainTargetHandle& handle,
    const std::uint16_t opcode,
    const std::span<const NativePortAudioExecutionDomainPayloadPart> payload,
    const std::uint64_t frame_index,
    const std::uint32_t flags,
    const NativePortAudioExecutionDomainStage stage) noexcept {
    return impl_->dispatch_async_scatter(handle, opcode, payload, frame_index,
                                         flags, stage);
}

NativePortAudioExecutionDomainDispatchResult
NativePortAudioExecutionDomain::dispatch_sync(
    const NativePortAudioExecutionDomainTargetHandle& handle,
    const std::uint16_t opcode,
    const std::span<const std::byte> payload,
    const std::uint64_t frame_index,
    const std::uint32_t flags,
    const NativePortAudioExecutionDomainStage stage) noexcept {
    return impl_->dispatch_sync(handle, opcode, payload, frame_index, flags,
                                stage);
}

NativePortAudioExecutionDomainDispatchResult
NativePortAudioExecutionDomain::dispatch_async_scatter(
    const NativePortAudioExecutionDomainTarget target,
    const std::uint32_t target_slot,
    const std::uint32_t target_generation,
    const std::uint16_t opcode,
    const std::span<const NativePortAudioExecutionDomainPayloadPart> payload,
    const std::uint64_t frame_index,
    const std::uint32_t flags,
    const NativePortAudioExecutionDomainStage stage) noexcept {
    return impl_->dispatch_async_scatter(target, target_slot, target_generation,
                                         opcode, payload, frame_index, flags,
                                         stage);
}

NativePortAudioExecutionDomainDispatchResult
NativePortAudioExecutionDomain::dispatch_sync(
    const NativePortAudioExecutionDomainTarget target,
    const std::uint32_t target_slot,
    const std::uint32_t target_generation,
    const std::uint16_t opcode,
    const std::span<const std::byte> payload,
    const std::uint64_t frame_index,
    const std::uint32_t flags,
    const NativePortAudioExecutionDomainStage stage) noexcept {
    return impl_->dispatch_sync(target, target_slot, target_generation, opcode,
                                payload, frame_index, flags, stage);
}

NativePortAudioExecutionDomainDispatchResult
NativePortAudioExecutionDomain::dispatch_sync(
    const NativePortAudioExecutionDomainTarget target,
    const std::uint16_t opcode,
    const std::span<const std::byte> payload,
    const std::uint64_t frame_index,
    const std::uint32_t flags,
    const NativePortAudioExecutionDomainStage stage) noexcept {
    return impl_->dispatch_sync(target, opcode, payload, frame_index, flags,
                                stage);
}

NativePortAudioExecutionDomainDispatchResult
NativePortAudioExecutionDomain::dispatch_sync_scatter(
    const NativePortAudioExecutionDomainTargetHandle& handle,
    const std::uint16_t opcode,
    const std::span<const NativePortAudioExecutionDomainPayloadPart> payload,
    const std::uint64_t frame_index,
    const std::uint32_t flags,
    const NativePortAudioExecutionDomainStage stage) noexcept {
    return impl_->dispatch_sync_scatter(handle, opcode, payload, frame_index,
                                        flags, stage);
}

NativePortAudioExecutionDomainDispatchResult
NativePortAudioExecutionDomain::dispatch_sync_scatter(
    const NativePortAudioExecutionDomainTarget target,
    const std::uint32_t target_slot,
    const std::uint32_t target_generation,
    const std::uint16_t opcode,
    const std::span<const NativePortAudioExecutionDomainPayloadPart> payload,
    const std::uint64_t frame_index,
    const std::uint32_t flags,
    const NativePortAudioExecutionDomainStage stage) noexcept {
    return impl_->dispatch_sync_scatter(target, target_slot, target_generation,
                                        opcode, payload, frame_index, flags,
                                        stage);
}

void NativePortAudioExecutionDomain::pump() noexcept { impl_->pump(); }

void NativePortAudioExecutionDomain::request_consumer_service() noexcept {
    impl_->request_consumer_service();
}

void NativePortAudioExecutionDomain::shutdown() noexcept { impl_->shutdown(); }

bool NativePortAudioExecutionDomain::on_audio_thread() const noexcept {
    return impl_->on_audio_thread();
}

std::optional<std::uint64_t>
NativePortAudioExecutionDomain::last_frame_index_nonblocking() const noexcept {
    return impl_->last_frame_index_nonblocking();
}

std::uint64_t
NativePortAudioExecutionDomain::queued_commands_nonblocking() const noexcept {
    return impl_->queued_commands_nonblocking();
}

NativePortAudioExecutionDomainSnapshot
NativePortAudioExecutionDomain::snapshot() const noexcept {
    return impl_->snapshot();
}

bool NativePortAudioExecutionDomain::bind_telemetry(
    NativePortTelemetry* const telemetry) noexcept {
    return impl_->bind_telemetry(telemetry);
}

bool NativePortAudioExecutionDomain::unbind_telemetry(
    NativePortTelemetry* const telemetry) noexcept {
    return impl_->unbind_telemetry(telemetry);
}

std::shared_ptr<NativePortAudioExecutionDomain>
acquire_native_port_audio_execution_domain(
    const NativePortAudioExecutionDomainConfig& config) {
    static std::mutex acquire_mutex;
    static std::weak_ptr<NativePortAudioExecutionDomain> process_domain;
    std::lock_guard lock(acquire_mutex);
    auto domain = process_domain.lock();
    if (domain == nullptr) {
        domain = std::shared_ptr<NativePortAudioExecutionDomain>(
            new NativePortAudioExecutionDomain(config));
        process_domain = domain;
    } else if (!domain->configuration_matches(config)) {
        throw NativePortAudioExecutionDomainError(
            NativePortAudioExecutionDomainFailure::ConfigMismatch);
    }
    return domain;
}

} // namespace katana::runtime
