#pragma once

#include "katana/runtime/native_aot_state.hpp"
#include "katana/runtime/native_port.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace katana::runtime {

using NativePortStaticEntryQuery = bool (*)(std::uint32_t address) noexcept;

// Exact immutable instruction bytes and non-writable content ranges emitted
// with a native product. This compact product view deliberately replaces the
// historical runtime's mutable-code/device catalogs: native products cannot
// decode, recompile or silently make a read-only image writable.
class NativePortImmutableWriteGuard final {
  public:
    explicit NativePortImmutableWriteGuard(
        std::span<const NativePortImmutableRange> ranges);

    [[nodiscard]] bool tracks_address(std::uint32_t address,
                                      std::size_t size) const noexcept;
    // Runtime-loaded modules are hashed before their pre-generated AOT is
    // activated.  Once active, their exact executable block ranges join the
    // same immutable-write contract as bootstrap code; data bytes in the file
    // remain writable.
    void reserve_additional_runtime_executable_ranges(
        std::size_t maximum_additional_ranges);
    void add_runtime_executable_range(std::uint32_t address,
                                      std::size_t size);
    void validate_runtime_executable_range_present(
        std::uint32_t address,
        std::size_t size) const;
    // Removes one exact range previously registered by a runtime-image
    // binding. Bootstrap/static immutable ranges can never be removed.
    void remove_runtime_executable_range(std::uint32_t address,
                                         std::size_t size);
    // Commit half of a prepare/commit transaction. The exact range must have
    // passed validate_runtime_executable_range_present after the last
    // mutation. Capacity is reserved before any mapping becomes active, so
    // this path cannot allocate or throw while another lifecycle owner is
    // already being retired.
    void remove_runtime_executable_range_committed(
        std::uint32_t address,
        std::size_t size) noexcept;
    void observe_write(const GuestWriteEvent& event) noexcept;

    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] bool write_detected() const noexcept;
    [[nodiscard]] std::uint32_t first_write_address() const noexcept;
    [[nodiscard]] std::size_t first_write_size() const noexcept;
    [[nodiscard]] std::uint8_t first_write_kind_mask() const noexcept;

  private:
    void rebuild_ranges() const noexcept;
    void rebuild_range_page_index() const noexcept;
    [[nodiscard]] std::uint8_t range_kind_mask(
        std::uint32_t address,
        std::size_t size) const noexcept;
    [[nodiscard]] bool fixed_tracks_address(
        std::uint32_t address,
        std::size_t size) const noexcept;
    [[nodiscard]] bool
    acknowledge_reconciled_runtime_executable_write() noexcept;

    friend bool reconcile_native_port_runtime_executable_write(
        NativePortContext& context,
        NativePortImmutableWriteGuard& immutable_guard) noexcept;

    std::vector<NativePortImmutableRange> fixed_ranges_;
    std::vector<NativePortImmutableRange> runtime_ranges_;
    mutable std::vector<NativePortImmutableRange> ranges_;
    mutable std::vector<std::uint8_t> range_page_kind_masks_;
    std::size_t maximum_runtime_ranges_ = 0u;
    std::uint64_t generation_ = 0u;
    std::uint32_t first_write_address_ = 0u;
    std::size_t first_write_size_ = 0u;
    std::uint8_t first_write_kind_mask_ = 0u;
    mutable std::uint8_t all_kind_mask_ = 0u;
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
    // Converts one completed guest write into a dynamic executable-lifecycle
    // retirement only when its exact owner can be proven safely retired.
    [[nodiscard]] bool reconcile_runtime_executable_write() noexcept;
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
    std::uint64_t host_boundary_cycles_remaining_ = 0u;
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
