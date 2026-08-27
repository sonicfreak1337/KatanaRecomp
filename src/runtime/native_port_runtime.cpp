#include "katana/runtime/native_port_aot_runtime.hpp"
#include "katana/runtime/native_port_content.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace katana::runtime {

namespace {

[[nodiscard]] std::uint32_t native_port_backing_address(
    const std::uint32_t address) noexcept {
    const auto physical = canonical_physical_address(address);
    if (physical < native_port_main_memory_physical_base ||
        physical >= native_port_main_memory_physical_base +
                        native_port_main_memory_physical_span)
        return physical;
    return native_port_main_memory_physical_base +
           ((physical - native_port_main_memory_physical_base) &
            (native_port_main_memory_backing_size - 1u));
}

} // namespace

NativePortImmutableWriteGuard::NativePortImmutableWriteGuard(
    const std::span<const NativePortImmutableRange> ranges)
    : fixed_ranges_(ranges.begin(), ranges.end()),
      ranges_(ranges.begin(), ranges.end()) {
    if (ranges_.empty())
        throw NativePortContractError(
            NativePortContractFailure::InvalidDefinition,
            "native-immutable-ranges-empty");
    std::uint64_t previous_end = 0u;
    bool first = true;
    for (const auto& range : ranges_) {
        const auto range_end =
            static_cast<std::uint64_t>(range.physical_address) +
            range.byte_size;
        constexpr std::uint8_t valid_kind_mask =
            native_port_immutable_range_mask(
                NativePortImmutableRangeKind::Executable) |
            native_port_immutable_range_mask(
                NativePortImmutableRangeKind::ReadOnlyImage);
        if (range.byte_size == 0u || range.kind_mask == 0u ||
            (range.kind_mask & ~valid_kind_mask) != 0u ||
            canonical_physical_address(range.physical_address) !=
                range.physical_address ||
            range_end >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::uint32_t>::max()) +
                    1u ||
            (!first && range.physical_address < previous_end))
            throw NativePortContractError(
                NativePortContractFailure::InvalidDefinition,
                "native-immutable-ranges-invalid");
        all_kind_mask_ |= range.kind_mask;
        previous_end = range_end;
        first = false;
    }
}

void NativePortImmutableWriteGuard::add_runtime_executable_range(
    const std::uint32_t address,
    const std::size_t size) {
    if (size == 0u || size > std::numeric_limits<std::uint32_t>::max())
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "native-runtime-executable-range-size");
    const auto physical = native_port_backing_address(address);
    const auto final_address =
        address + static_cast<std::uint32_t>(size - 1u);
    const auto end = static_cast<std::uint64_t>(physical) + size;
    if (final_address < address ||
        native_port_backing_address(final_address) != end - 1u ||
        end > static_cast<std::uint64_t>(
                  std::numeric_limits<std::uint32_t>::max()) +
                  1u)
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "native-runtime-executable-range-address");

    const NativePortImmutableRange inserted{
        physical, static_cast<std::uint32_t>(size),
        native_port_immutable_range_mask(
            NativePortImmutableRangeKind::Executable)};
    if (std::find(runtime_ranges_.begin(), runtime_ranges_.end(), inserted) !=
        runtime_ranges_.end())
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "native-runtime-executable-range-duplicate");
    if (runtime_ranges_.size() >= maximum_runtime_ranges_)
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "native-runtime-executable-range-capacity");
    runtime_ranges_.push_back(inserted);
    rebuild_ranges();
}

void NativePortImmutableWriteGuard::reserve_additional_runtime_executable_ranges(
    const std::size_t maximum_additional_ranges) {
    if (maximum_additional_ranges >
            std::numeric_limits<std::size_t>::max() -
                maximum_runtime_ranges_ ||
        fixed_ranges_.size() >
            std::numeric_limits<std::size_t>::max() -
                (maximum_runtime_ranges_ + maximum_additional_ranges))
        throw NativePortContractError(
            NativePortContractFailure::InvalidDefinition,
            "native-runtime-executable-range-capacity");
    const auto maximum_runtime_ranges =
        maximum_runtime_ranges_ + maximum_additional_ranges;
    runtime_ranges_.reserve(maximum_runtime_ranges);
    ranges_.reserve(fixed_ranges_.size() + maximum_runtime_ranges);
    maximum_runtime_ranges_ = maximum_runtime_ranges;
}

void NativePortImmutableWriteGuard::
validate_runtime_executable_range_present(
    const std::uint32_t address,
    const std::size_t size) const {
    if (size == 0u || size > std::numeric_limits<std::uint32_t>::max())
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "native-runtime-executable-remove-size");
    const NativePortImmutableRange removed{
        native_port_backing_address(address),
        static_cast<std::uint32_t>(size),
        native_port_immutable_range_mask(
            NativePortImmutableRangeKind::Executable)};
    if (std::find(runtime_ranges_.begin(), runtime_ranges_.end(), removed) ==
        runtime_ranges_.end())
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "native-runtime-executable-remove-missing");
}

void NativePortImmutableWriteGuard::remove_runtime_executable_range(
    const std::uint32_t address,
    const std::size_t size) {
    validate_runtime_executable_range_present(address, size);
    remove_runtime_executable_range_committed(address, size);
}

void NativePortImmutableWriteGuard::
remove_runtime_executable_range_committed(
    const std::uint32_t address,
    const std::size_t size) noexcept {
    const auto physical = native_port_backing_address(address);
    const NativePortImmutableRange removed{
        physical, static_cast<std::uint32_t>(size),
        native_port_immutable_range_mask(
            NativePortImmutableRangeKind::Executable)};
    const auto found =
        std::find(runtime_ranges_.begin(), runtime_ranges_.end(), removed);
    if (found == runtime_ranges_.end()) std::terminate();
    runtime_ranges_.erase(found);
    rebuild_ranges();
}

void NativePortImmutableWriteGuard::rebuild_ranges() noexcept {
    ranges_.clear();
    ranges_.insert(
        ranges_.end(), fixed_ranges_.begin(), fixed_ranges_.end());
    ranges_.insert(
        ranges_.end(), runtime_ranges_.begin(), runtime_ranges_.end());
    std::sort(ranges_.begin(), ranges_.end(), [](const auto& left,
                                                 const auto& right) {
        return std::tie(left.physical_address, left.byte_size,
                        left.kind_mask) <
               std::tie(right.physical_address, right.byte_size,
                        right.kind_mask);
    });
    std::size_t merged_size = 0u;
    for (std::size_t index = 0u; index < ranges_.size(); ++index) {
        const auto range = ranges_[index];
        if (merged_size == 0u) {
            ranges_[merged_size++] = range;
            continue;
        }
        auto& previous = ranges_[merged_size - 1u];
        const auto previous_end =
            static_cast<std::uint64_t>(previous.physical_address) +
            previous.byte_size;
        const auto range_end =
            static_cast<std::uint64_t>(range.physical_address) +
            range.byte_size;
        if (range.physical_address > previous_end) {
            ranges_[merged_size++] = range;
            continue;
        }
        const auto merged_end = std::max(previous_end, range_end);
        previous.byte_size = static_cast<std::uint32_t>(
            merged_end - previous.physical_address);
        previous.kind_mask |= range.kind_mask;
    }
    ranges_.resize(merged_size);
    all_kind_mask_ = 0u;
    for (const auto& range : ranges_) all_kind_mask_ |= range.kind_mask;
}

std::uint8_t NativePortImmutableWriteGuard::range_kind_mask(
    const std::uint32_t address,
    const std::size_t size) const noexcept {
    if (size == 0u) return 0u;
    if (size > std::numeric_limits<std::uint32_t>::max())
        return all_kind_mask_;
    const auto physical = native_port_backing_address(address);
    const auto access_end =
        static_cast<std::uint64_t>(physical) + size;
    if (access_end >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::uint32_t>::max()) +
            1u)
        return all_kind_mask_;
    const auto final_virtual =
        address + static_cast<std::uint32_t>(size - 1u);
    if (native_port_backing_address(final_virtual) != access_end - 1u)
        // A single contiguous guest write which crosses a physical alias,
        // native-memory backing mirror or address-space boundary cannot be
        // represented by one canonical interval. It must never become a
        // proof of non-aliasing for the static product.
        return all_kind_mask_;
    const auto found = std::lower_bound(
        ranges_.begin(), ranges_.end(), physical,
        [](const NativePortImmutableRange& range,
           const std::uint32_t candidate) {
            return static_cast<std::uint64_t>(range.physical_address) +
                       range.byte_size <=
                   candidate;
        });
    std::uint8_t result = 0u;
    for (auto current = found;
         current != ranges_.end() && current->physical_address < access_end;
         ++current)
        result |= current->kind_mask;
    return result;
}

bool NativePortImmutableWriteGuard::tracks_address(
    const std::uint32_t address,
    const std::size_t size) const noexcept {
    return range_kind_mask(address, size) != 0u;
}

void NativePortImmutableWriteGuard::observe_write(
    const GuestWriteEvent& event) noexcept {
    const auto kind_mask = range_kind_mask(event.address, event.size);
    if (kind_mask == 0u) return;
    const bool read_only_image =
        (kind_mask & native_port_immutable_range_mask(
                         NativePortImmutableRangeKind::ReadOnlyImage)) != 0u;
    if (!event.bytes_changed && !read_only_image) return;
    if (!write_detected_) {
        first_write_address_ = native_port_backing_address(event.address);
        first_write_size_ = event.size;
        first_write_kind_mask_ = kind_mask;
        write_detected_ = true;
    }
    if (generation_ != std::numeric_limits<std::uint64_t>::max())
        ++generation_;
}

std::uint64_t NativePortImmutableWriteGuard::generation() const noexcept {
    return generation_;
}

bool NativePortImmutableWriteGuard::write_detected() const noexcept {
    return write_detected_;
}

std::uint32_t
NativePortImmutableWriteGuard::first_write_address() const noexcept {
    return first_write_address_;
}

std::size_t
NativePortImmutableWriteGuard::first_write_size() const noexcept {
    return first_write_size_;
}

std::uint8_t
NativePortImmutableWriteGuard::first_write_kind_mask() const noexcept {
    return first_write_kind_mask_;
}

std::uint64_t native_aot_code_tracker_generation(
    const NativePortImmutableWriteGuard* const guard) noexcept {
    return guard != nullptr ? guard->generation() : 0u;
}

bool native_aot_code_tracker_tracks_address(
    const NativePortImmutableWriteGuard* const guard,
    const std::uint32_t physical_address,
    const std::size_t size) noexcept {
    return guard != nullptr &&
           guard->tracks_address(physical_address, size);
}

// This symbol is the positive link anchor for the native product archive.
// Generated bootstrap code must consume it, so merely naming the archive as a
// transitive dependency cannot masquerade as a successfully linked native
// runtime in the post-link map audit.
const NativePortLinkContract& native_port_link_contract() noexcept {
    static constexpr NativePortLinkContract contract;
    return contract;
}

NativePortAotServices::NativePortAotServices(
    NativePortContext& context,
    const NativePortStaticEntryQuery static_entry_query,
    NativePortImmutableWriteGuard& immutable_guard)
    : context_(&context), static_entry_query_(static_entry_query),
      immutable_guard_(&immutable_guard) {
    if (context.cpu == nullptr || context.host == nullptr ||
        static_entry_query == nullptr ||
        context.cpu->memory.has_guest_write_observer() ||
        context.cpu->memory.has_guest_write_batch_observer())
        throw NativePortContractError(
            NativePortContractFailure::AotContractViolation,
            "native-aot-services");
    // Host callbacks may throw.  Sample the initial lifecycle boundary before
    // installing a callback which captures the guard, so construction cannot
    // leave a dangling observer behind on failure.
    refresh_host_boundary();
    context.cpu->memory.set_guest_write_observer(
        [guard = immutable_guard_](const GuestWriteEvent& event) noexcept {
            guard->observe_write(event);
        },
        GuestWriteObserverContract::StableForPrevalidatedLinearWrites);
    write_observer_bound_ = true;
    context.cpu->memory.set_guest_write_batch_observer(
        {immutable_guard_,
         [](void*, std::span<const GuestWriteEvent>) noexcept {
             return true;
         },
         [](void* const raw_guard,
            const std::span<const GuestWriteEvent> events) noexcept {
             auto& guard = *static_cast<NativePortImmutableWriteGuard*>(
                 raw_guard);
             for (const auto& event : events) guard.observe_write(event);
         }});
    write_observer_generation_ =
        context.cpu->memory.guest_write_observer_generation();
}

NativePortAotServices::~NativePortAotServices() noexcept {
    if (write_observer_bound_ && context_ != nullptr &&
        context_->cpu != nullptr &&
        context_->cpu->memory.guest_write_observer_generation() ==
            write_observer_generation_)
        context_->cpu->memory.clear_guest_write_observer();
}

std::uint64_t NativePortAotServices::scheduler_cycle() const noexcept {
    return guest_sequence_;
}

NativePortCycleCommit NativePortAotServices::consume_guest_cycles(
    const std::uint64_t guest_cycles,
    const std::size_t boundary_budget) {
    if (boundary_budget == 0u)
        throw std::invalid_argument("native-port-boundary-budget-zero");
    if (guest_cycles > std::numeric_limits<std::uint64_t>::max() -
                           guest_sequence_)
        throw std::overflow_error("native-port-guest-sequence-overflow");
    guest_sequence_ += guest_cycles;
    // Host lifecycle/deadline polling is a completed-safepoint sampling budget,
    // not part of guest-cycle accounting. Keep the exact guest sequence and
    // poll no less often than every boundary_budget cycles measured between
    // completed safepoints. A single large block can cross the budget before
    // its first legal safepoint; it still polls once at completion.
    if (host_boundary_cycles_remaining_ == 0u ||
        host_boundary_cycles_remaining_ > boundary_budget ||
        guest_cycles >= host_boundary_cycles_remaining_) {
        refresh_host_boundary();
        host_boundary_cycles_remaining_ = boundary_budget;
    } else {
        host_boundary_cycles_remaining_ -= guest_cycles;
    }
    return {guest_sequence_, boundary_.has_value() ? 1u : 0u, false, false};
}

std::optional<NativePortStopReason>
NativePortAotServices::poll_interrupt() const noexcept {
    return boundary_;
}

bool NativePortAotServices::prefetch(
    CpuState& cpu,
    const GuestInstructionOrigin instruction,
    const std::uint32_t address) {
    if (address >= 0xE0000000u && address <= 0xE3FFFFFFu) {
        if (context_ != nullptr)
            context_->stop_reason =
                NativePortStopReason::ForbiddenHardwareOperation;
        throw NativePortContractError(
            NativePortContractFailure::ForbiddenHardwareOperation,
            "store-queue-prefetch");
    }
    const auto physical = canonical_physical_address(address);
    constexpr auto main_memory_end =
        native_port_main_memory_physical_base +
        native_port_main_memory_physical_span;
    if (physical < native_port_main_memory_physical_base ||
        physical >= main_memory_end) {
        if (context_ != nullptr)
            context_->stop_reason =
                NativePortStopReason::UnresolvedHardwareAccess;
        std::ostringstream detail;
        detail << "native-prefetch-unresolved:address=0x" << std::hex
               << address << ";site=0x" << instruction.source_pc
               << ";runtime-pc=0x" << instruction.runtime_pc << std::dec
               << ";instruction-valid=" << (instruction.valid ? 1 : 0);
        throw NativePortContractError(
            NativePortContractFailure::UnresolvedHardwareAccess,
            detail.str());
    }
    katana::runtime::prefetch(cpu, address);
    return true;
}

bool NativePortAotServices::can_chain_executable_block(
    const std::uint32_t address) const noexcept {
    return !boundary_.has_value() && aot_contract_valid() &&
           !immutable_write_detected() &&
           static_entry_query_ != nullptr &&
           static_entry_query_(address);
}

NativePortImmutableWriteGuard*
NativePortAotServices::immutable_write_guard() noexcept {
    return immutable_guard_;
}

bool NativePortAotServices::immutable_write_detected() const noexcept {
    return immutable_guard_ != nullptr &&
           immutable_guard_->write_detected();
}

bool NativePortAotServices::aot_contract_valid() const noexcept {
    return write_observer_bound_ && context_ != nullptr &&
           context_->cpu != nullptr &&
           context_->cpu->memory.guest_write_observer_pair_current(
               write_observer_generation_);
}

std::uint32_t
NativePortAotServices::first_immutable_write_address() const noexcept {
    return immutable_guard_ != nullptr
               ? immutable_guard_->first_write_address()
               : 0u;
}

std::size_t
NativePortAotServices::first_immutable_write_size() const noexcept {
    return immutable_guard_ != nullptr
               ? immutable_guard_->first_write_size()
               : 0u;
}

std::uint8_t
NativePortAotServices::first_immutable_write_kind_mask() const noexcept {
    return immutable_guard_ != nullptr
               ? immutable_guard_->first_write_kind_mask()
               : 0u;
}

void NativePortAotServices::observe_guest_block_completion(
    const std::uint32_t checkpoint,
    const std::uint64_t retired_instructions,
    const bool new_exception,
    const bool exception_exit) noexcept {
    static_cast<void>(checkpoint);
    static_cast<void>(retired_instructions);
    static_cast<void>(new_exception);
    static_cast<void>(exception_exit);
}

NativePortContext& NativePortAotServices::context() const noexcept {
    return *context_;
}

void NativePortAotServices::refresh_host_boundary() {
    if (context_ == nullptr || context_->host == nullptr) return;
    const auto now = context_->host->monotonic_time_nanoseconds();
    if (context_->host_deadline_nanoseconds != 0u &&
        now >= context_->host_deadline_nanoseconds) {
        context_->stop_reason = NativePortStopReason::HostDeadline;
        boundary_ = NativePortStopReason::HostDeadline;
        return;
    }
    switch (context_->host->poll_lifecycle()) {
    case NativePortLifecycleState::Running:
        boundary_.reset();
        return;
    case NativePortLifecycleState::Paused:
        // Pause is a temporary dispatch boundary, not a terminal stop reason.
        boundary_ = NativePortStopReason::None;
        return;
    case NativePortLifecycleState::Shutdown:
        context_->stop_reason = NativePortStopReason::HostRequested;
        boundary_ = NativePortStopReason::HostRequested;
        return;
    }
}

NativePortCycleCommit commit_pending_guest_cycles(
    CpuState& cpu,
    NativePortAotServices& services,
    const std::size_t boundary_budget) {
    if (boundary_budget == 0u)
        throw std::invalid_argument("native-port-boundary-budget-zero");
    const auto pending = cpu.pending_guest_cycles;
    if (pending == 0u)
        return {services.scheduler_cycle(), 0u, false, false};
    const auto result =
        services.consume_guest_cycles(pending, boundary_budget);
    cpu.pending_guest_cycles = 0u;
    cpu.total_guest_cycles += pending;
    return result;
}

void flush_pending_guest_cycles(CpuState& cpu,
                                NativePortAotServices& services,
                                const std::size_t boundary_budget) {
    static_cast<void>(
        commit_pending_guest_cycles(cpu, services, boundary_budget));
}

NativePortBlockCompletion finalize_guest_block(
    CpuState& cpu,
    NativePortAotServices& services,
    const std::size_t boundary_budget,
    const std::uint32_t checkpoint,
    const std::uint64_t retired_guest_instructions,
    const bool new_exception,
    const bool exception_exit,
    const bool observe_checkpoint) {
    auto scheduler =
        commit_pending_guest_cycles(cpu, services, boundary_budget);
    if (observe_checkpoint)
        services.observe_guest_block_completion(
            checkpoint,
            retired_guest_instructions,
            new_exception,
            exception_exit);
    return {scheduler, new_exception ? std::optional<NativePortStopReason>{}
                                     : services.poll_interrupt()};
}

} // namespace katana::runtime
