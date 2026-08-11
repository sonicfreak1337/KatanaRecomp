#pragma once

#include "katana/runtime/native_aot_state.hpp"
#include "katana/runtime/native_port.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace katana::runtime {

using NativePortStaticEntryQuery = bool (*)(std::uint32_t address) noexcept;

enum class NativePortImmutableRangeKind : std::uint8_t {
    Executable = 1u << 0u,
    ReadOnlyImage = 1u << 1u,
};

[[nodiscard]] constexpr std::uint8_t native_port_immutable_range_mask(
    const NativePortImmutableRangeKind kind) noexcept {
    return static_cast<std::uint8_t>(kind);
}

// Exact immutable instruction bytes and non-writable content ranges emitted
// with a native product. This compact product view deliberately replaces the
// historical runtime's mutable-code/device catalogs: native products cannot
// decode, recompile or silently make a read-only image writable.
struct NativePortImmutableRange final {
    std::uint32_t physical_address = 0u;
    std::uint32_t byte_size = 0u;
    std::uint8_t kind_mask = 0u;

    [[nodiscard]] bool operator==(
        const NativePortImmutableRange&) const = default;
};

class NativePortImmutableWriteGuard final {
  public:
    explicit NativePortImmutableWriteGuard(
        std::span<const NativePortImmutableRange> ranges);

    [[nodiscard]] bool tracks_address(std::uint32_t address,
                                      std::size_t size) const noexcept;
    void observe_write(const GuestWriteEvent& event) noexcept;

    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] bool write_detected() const noexcept;
    [[nodiscard]] std::uint32_t first_write_address() const noexcept;
    [[nodiscard]] std::size_t first_write_size() const noexcept;
    [[nodiscard]] std::uint8_t first_write_kind_mask() const noexcept;

  private:
    [[nodiscard]] std::uint8_t range_kind_mask(
        std::uint32_t address,
        std::size_t size) const noexcept;

    std::span<const NativePortImmutableRange> ranges_;
    std::uint64_t generation_ = 0u;
    std::uint32_t first_write_address_ = 0u;
    std::size_t first_write_size_ = 0u;
    std::uint8_t first_write_kind_mask_ = 0u;
    std::uint8_t all_kind_mask_ = 0u;
    bool write_detected_ = false;
};

// Generated direct-RAM helpers use the same narrow query names for the
// historical mutable-code tracker and the native immutable-code guard. The
// pointer type selected by the runtime binding keeps either implementation
// out of the other product's dependency graph.
[[nodiscard]] std::uint64_t native_aot_code_tracker_generation(
    const NativePortImmutableWriteGuard* guard) noexcept;
[[nodiscard]] bool native_aot_code_tracker_tracks_address(
    const NativePortImmutableWriteGuard* guard,
    std::uint32_t physical_address,
    std::size_t size) noexcept;

struct NativePortCycleCommit final {
    std::uint64_t guest_sequence = 0u;
    std::size_t processed_boundaries = 0u;
    bool budget_exhausted = false;
    bool guest_cycle_budget_exhausted = false;
};

struct NativePortBlockCompletion final {
    NativePortCycleCommit scheduler;
    // Generated AOT already treats a present interrupt as a request to leave
    // the current owner.  The native product has no guest interrupt model;
    // this optional carries only a host lifecycle/deadline boundary.
    std::optional<NativePortStopReason> interrupt;
};

// Minimal product-side AOT binding. Guest ticks preserve ordering inside the
// recompiled CPU compatibility state, but they do not emulate a Dreamcast
// scheduler or claim a host MHz rate.
class NativePortAotServices final {
  public:
    NativePortAotServices(NativePortContext& context,
                          NativePortStaticEntryQuery static_entry_query,
                          NativePortImmutableWriteGuard& immutable_guard);
    ~NativePortAotServices() noexcept;

    NativePortAotServices(const NativePortAotServices&) = delete;
    NativePortAotServices& operator=(const NativePortAotServices&) = delete;
    NativePortAotServices(NativePortAotServices&&) = delete;
    NativePortAotServices& operator=(NativePortAotServices&&) = delete;

    [[nodiscard]] std::uint64_t scheduler_cycle() const noexcept;
    [[nodiscard]] NativePortCycleCommit
    consume_guest_cycles(std::uint64_t guest_cycles,
                         std::size_t boundary_budget);
    [[nodiscard]] std::optional<NativePortStopReason>
    poll_interrupt() const noexcept;
    [[nodiscard]] bool prefetch(CpuState& cpu,
                                GuestInstructionOrigin instruction,
                                std::uint32_t address);
    [[nodiscard]] bool
    can_chain_executable_block(std::uint32_t address) const noexcept;
    [[nodiscard]] NativePortImmutableWriteGuard*
    immutable_write_guard() noexcept;
    [[nodiscard]] bool immutable_write_detected() const noexcept;
    [[nodiscard]] bool aot_contract_valid() const noexcept;
    [[nodiscard]] std::uint32_t
    first_immutable_write_address() const noexcept;
    [[nodiscard]] std::size_t first_immutable_write_size() const noexcept;
    [[nodiscard]] std::uint8_t
    first_immutable_write_kind_mask() const noexcept;
    void observe_guest_block_completion(std::uint32_t checkpoint,
                                        std::uint64_t retired_instructions,
                                        bool new_exception,
                                        bool exception_exit) noexcept;

    [[nodiscard]] NativePortContext& context() const noexcept;

  private:
    void refresh_host_boundary();

    NativePortContext* context_ = nullptr;
    NativePortStaticEntryQuery static_entry_query_ = nullptr;
    NativePortImmutableWriteGuard* immutable_guard_ = nullptr;
    std::uint64_t guest_sequence_ = 0u;
    std::uint64_t write_observer_generation_ = 0u;
    std::optional<NativePortStopReason> boundary_;
    bool write_observer_bound_ = false;
};

[[nodiscard]] NativePortCycleCommit
commit_pending_guest_cycles(CpuState& cpu,
                            NativePortAotServices& services,
                            std::size_t boundary_budget);

void flush_pending_guest_cycles(CpuState& cpu,
                                NativePortAotServices& services,
                                std::size_t boundary_budget = 1024u);

[[nodiscard]] NativePortBlockCompletion
finalize_guest_block(CpuState& cpu,
                     NativePortAotServices& services,
                     std::size_t boundary_budget,
                     std::uint32_t checkpoint,
                     std::uint64_t retired_guest_instructions = 0u,
                     bool new_exception = false,
                     bool exception_exit = false,
                     bool observe_checkpoint = true);

} // namespace katana::runtime
