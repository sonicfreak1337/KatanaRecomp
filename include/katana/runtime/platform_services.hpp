#pragma once

#include "katana/build_contract.hpp"
#include "katana/runtime/runtime.hpp"
#include "katana/runtime/scheduler.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace katana::runtime {

class ExecutableCodeTracker;
class ExecutableModuleCatalog;
class DemandBlockMaterializer;
class RuntimeBlockTable;
struct BlockVariantKey;

inline constexpr std::uint32_t platform_services_abi_version =
    build_contract::platform_services_abi_version;
inline constexpr std::uint64_t base_guest_cycles_per_instruction = 1u;
inline constexpr std::size_t guest_cycle_flush_event_budget = 1024u;

class GuestCycleBudgetReached final : public std::runtime_error {
  public:
    explicit GuestCycleBudgetReached(const std::uint64_t final_guest_cycle)
        : std::runtime_error("guest-cycle-budget-reached"),
          final_guest_cycle_(final_guest_cycle) {}

    [[nodiscard]] std::uint64_t final_guest_cycle() const noexcept {
        return final_guest_cycle_;
    }

  private:
    std::uint64_t final_guest_cycle_ = 0u;
};

enum class ExecutableBlockTimingClass : std::uint8_t {
    PureCpu,
    LinearRamOnly,
    RequiresCycleFlush,
    NeverChain,
};

// POD reason left by the last native-AOT chaining decision. Product dispatch
// accounting consumes it only when native code returns to the central
// dispatcher; no strings, containers or allocations are involved.
enum class ExecutableChainRejectionReason : std::uint8_t {
    None,
    DiagnosticMode,
    ChainingDisabled,
    MissingRuntimeContract,
    NoPendingGuestCycles,
    GameEntryBarrier,
    TargetNotRegistered,
    TargetNotNativeEntrySafe,
    TimingNotDeferrable,
    CycleQuantum,
    GuestCycleBudget,
    SchedulerDue,
    InterruptAcceptable,
    AddressTranslation,
    VariantOrGeneration,
    CodeGeneration,
};

enum class PlatformCapability : std::uint64_t {
    Memory = 1ull << 0u,
    Scheduler = 1ull << 1u,
    Interrupts = 1ull << 2u,
    Dma = 1ull << 3u,
    ControlledFallback = 1ull << 4u,
    Mmu = 1ull << 5u,
    Watchpoints = 1ull << 6u,
    ExecutableRam = 1ull << 7u,
    FirmwareMode = 1ull << 8u,
    StoreQueues = 1ull << 9u
};

using PlatformCapabilities = std::uint64_t;

[[nodiscard]] constexpr PlatformCapabilities
platform_capability(const PlatformCapability capability) noexcept {
    return static_cast<PlatformCapabilities>(capability);
}

inline constexpr PlatformCapabilities core_platform_capabilities =
    platform_capability(PlatformCapability::Memory) |
    platform_capability(PlatformCapability::Scheduler) |
    platform_capability(PlatformCapability::Interrupts) |
    platform_capability(PlatformCapability::Dma) |
    platform_capability(PlatformCapability::StoreQueues);

struct PlatformServiceRequirements {
    std::uint32_t abi_version = platform_services_abi_version;
    std::uint32_t guest_cycle_contract = guest_cycle_contract_version;
    PlatformCapabilities capabilities = core_platform_capabilities;
};

struct PlatformSchedulerResult {
    std::uint64_t guest_cycle = 0u;
    std::size_t processed_events = 0u;
    bool budget_exhausted = false;
    bool guest_cycle_budget_exhausted = false;
};

struct PlatformInterruptRequest {
    std::uint32_t source_id = 0u;
    std::uint8_t level = 0u;
    std::uint32_t event_code = 0u;
};

struct PlatformDmaRequest {
    std::uint32_t source = 0u;
    std::uint32_t destination = 0u;
    std::uint32_t length = 0u;
};

struct PlatformDmaResult {
    std::uint32_t transferred = 0u;
    bool completed = false;
};

struct PlatformFallbackRequest {
    std::uint32_t guest_pc = 0u;
    std::uint16_t opcode = 0u;
};

struct PlatformFallbackResult {
    bool handled = false;
    std::uint32_t next_guest_pc = 0u;
};

enum class PlatformLifecycleState : std::uint8_t { Running, Paused, Shutdown };

enum class PlatformLifecycleExitReason : std::uint8_t { Reset, BiosMenu, CdMenu };

struct PlatformLifecycleExitEvidence {
    std::uint64_t guest_cycle = 0u;
    std::uint32_t callsite = 0u;
    std::uint32_t return_address = 0u;
    std::array<std::uint32_t, general_register_count> registers{};
    std::uint32_t last_gdrom_request = 0u;
    std::uint32_t last_gdrom_command = 0u;
    std::uint32_t last_gdrom_state = 0u;
    std::array<std::uint32_t, 4u> last_gdrom_status{};
};

class PlatformLifecycleExit final : public std::exception {
  public:
    PlatformLifecycleExit(const PlatformLifecycleExitReason reason,
                          PlatformLifecycleExitEvidence evidence) noexcept
        : reason_(reason), evidence_(std::move(evidence)) {}
    [[nodiscard]] PlatformLifecycleExitReason reason() const noexcept { return reason_; }
    [[nodiscard]] const PlatformLifecycleExitEvidence& evidence() const noexcept {
        return evidence_;
    }
    [[nodiscard]] const char* what() const noexcept override {
        switch (reason_) {
        case PlatformLifecycleExitReason::Reset:
            return "Guest requested platform reset";
        case PlatformLifecycleExitReason::BiosMenu:
            return "Guest requested BIOS menu";
        case PlatformLifecycleExitReason::CdMenu:
            return "Guest requested CD menu";
        }
        return "Guest requested platform lifecycle exit";
    }

  private:
    PlatformLifecycleExitReason reason_;
    PlatformLifecycleExitEvidence evidence_;
};

class PlatformShutdownRequested final : public std::exception {
  public:
    [[nodiscard]] const char* what() const noexcept override {
        return "Host requested native guest shutdown";
    }
};

class PlatformServices {
  public:
    virtual ~PlatformServices() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t abi_version() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t guest_cycle_contract() const noexcept = 0;
    [[nodiscard]] virtual PlatformCapabilities capabilities() const noexcept = 0;

    virtual void read_memory(std::uint32_t address, std::span<std::uint8_t> destination) = 0;
    virtual void write_memory(std::uint32_t address, std::span<const std::uint8_t> source) = 0;
    [[nodiscard]] virtual std::uint64_t scheduler_cycle() const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t>
    next_scheduler_event_cycle() const noexcept = 0;
    [[nodiscard]] virtual PlatformSchedulerResult
    consume_guest_cycles(std::uint64_t guest_cycles, std::size_t event_budget) = 0;
    [[nodiscard]] virtual std::optional<PlatformInterruptRequest> poll_interrupt() = 0;
    [[nodiscard]] virtual PlatformDmaResult start_dma(const PlatformDmaRequest& request) = 0;
    [[nodiscard]] virtual PlatformFallbackResult
    controlled_fallback(CpuState& cpu, const PlatformFallbackRequest& request) = 0;
    [[nodiscard]] virtual bool prefetch(CpuState& cpu,
                                        GuestInstructionOrigin instruction,
                                        std::uint32_t address) = 0;
    [[nodiscard]] virtual PlatformLifecycleState poll_host_lifecycle() {
        return PlatformLifecycleState::Running;
    }
    virtual void observe_guest_checkpoint(std::uint32_t) noexcept {}
    virtual void observe_guest_block_completion(std::uint32_t checkpoint,
                                                std::uint64_t,
                                                bool,
                                                bool) noexcept {
        observe_guest_checkpoint(checkpoint);
    }
    virtual void register_executable_block(std::uint32_t,
                                           std::uint32_t,
                                           std::uint32_t,
                                           std::string_view,
                                           ExecutableBlockTimingClass,
                                           std::uint64_t) {}
    virtual void allow_executable_block_chaining(std::uint32_t) {}
    // Generated products register every AOT-template source entry here, including
    // entries which are intentionally absent from the static runtime table.  The
    // platform may use this bounded metadata only after the runtime dispatcher has
    // bound the table/materializer which proved the concrete destination identity.
    virtual void reserve_runtime_aot_chain_contracts(std::size_t) {}
    virtual void register_runtime_aot_chain_contract(std::uint32_t,
                                                     std::uint32_t,
                                                     ExecutableBlockTimingClass,
                                                     std::uint64_t,
                                                     bool) {}
    virtual void bind_runtime_dispatch_context(RuntimeBlockTable*,
                                               DemandBlockMaterializer*) noexcept {}
    virtual void begin_executable_block(const BlockVariantKey&) noexcept {}
    virtual void begin_executable_block(std::uint32_t,
                                        std::uint32_t,
                                        std::uint32_t,
                                        const BlockVariantKey& variant) noexcept {
        begin_executable_block(variant);
    }
    virtual void begin_executable_block(std::uint32_t virtual_start,
                                        std::uint32_t physical_start,
                                        std::uint32_t size,
                                        const BlockVariantKey& variant,
                                        bool) noexcept {
        begin_executable_block(virtual_start, physical_start, size, variant);
    }
    [[nodiscard]] virtual bool can_chain_executable_block(std::uint32_t) const noexcept {
        return false;
    }
    [[nodiscard]] virtual ExecutableChainRejectionReason
    last_executable_chain_rejection() const noexcept {
        return ExecutableChainRejectionReason::MissingRuntimeContract;
    }
    // Product runtimes may retain pending guest cycles across a central dispatch
    // only while the completed block is a proven pure/static region and no
    // scheduler, interrupt, lifecycle, replay, or architectural boundary is due.
    // The conservative default preserves the per-block commit contract.
    [[nodiscard]] virtual bool can_defer_guest_block_completion() const noexcept {
        return false;
    }
    [[nodiscard]] virtual ExecutableCodeTracker* executable_code_tracker() noexcept {
        return nullptr;
    }
    [[nodiscard]] virtual ExecutableModuleCatalog* executable_module_catalog() noexcept {
        return nullptr;
    }
};

[[nodiscard]] inline PlatformSchedulerResult
commit_pending_guest_cycles(CpuState& cpu,
                            PlatformServices& services,
                            const std::size_t event_budget) {
    if (event_budget == 0u)
        throw std::invalid_argument("platform-services-event-budget-zero");
    const auto pending = cpu.pending_guest_cycles;
    if (pending == 0u) {
        return {services.scheduler_cycle(), 0u, false, false};
    }
    const auto cycle_before = services.scheduler_cycle();
    const auto commit_delivered_cycles = [&](const std::uint64_t cycle_after,
                                             const bool complete) noexcept {
        const auto delivered =
            complete ? pending
                     : cycle_after >= cycle_before
                           ? std::min(pending, cycle_after - cycle_before)
                           : 0u;
        const auto accountable = std::min(delivered, cpu.pending_guest_cycles);
        cpu.pending_guest_cycles -= accountable;
        cpu.total_guest_cycles += accountable;
    };
    PlatformSchedulerResult result;
    try {
        result = services.consume_guest_cycles(pending, event_budget);
    } catch (...) {
        commit_delivered_cycles(services.scheduler_cycle(), false);
        throw;
    }
    commit_delivered_cycles(
        result.guest_cycle,
        !result.budget_exhausted && !result.guest_cycle_budget_exhausted);
    return result;
}

inline void flush_pending_guest_cycles(CpuState& cpu,
                                       PlatformServices& services,
                                       const std::size_t event_budget =
                                           guest_cycle_flush_event_budget) {
    const auto result = commit_pending_guest_cycles(cpu, services, event_budget);
    if (result.budget_exhausted)
        throw std::runtime_error("Schedulerbudget beim Gastzeit-Flush erschoepft.");
    if (result.guest_cycle_budget_exhausted)
        throw GuestCycleBudgetReached(result.guest_cycle);
}

struct PlatformBlockCompletion {
    PlatformSchedulerResult scheduler;
    std::optional<PlatformInterruptRequest> interrupt;
};

[[nodiscard]] inline PlatformBlockCompletion
finalize_guest_block(CpuState& cpu,
                     PlatformServices& services,
                     const std::size_t event_budget,
                     const std::uint32_t checkpoint,
                     const std::uint64_t retired_guest_instructions = 0u,
                     const bool new_exception = false,
                     const bool exception_exit = false,
                     const bool observe_checkpoint = true) {
    auto scheduler = commit_pending_guest_cycles(cpu, services, event_budget);
    if (scheduler.budget_exhausted)
        throw std::runtime_error("Schedulerbudget beim Blockabschluss erschoepft.");
    if (scheduler.guest_cycle_budget_exhausted)
        throw GuestCycleBudgetReached(scheduler.guest_cycle);
    if (observe_checkpoint)
        services.observe_guest_block_completion(checkpoint,
                                                retired_guest_instructions,
                                                new_exception,
                                                exception_exit);
    return {scheduler, new_exception ? std::nullopt : services.poll_interrupt()};
}

inline void validate_platform_services(const PlatformServices& services,
                                       const PlatformServiceRequirements& requirements = {}) {
    const std::string service_name(services.name());
    if (service_name.empty()) {
        throw std::invalid_argument("Plattformdienst besitzt keinen Namen.");
    }
    if (services.abi_version() != requirements.abi_version) {
        throw std::invalid_argument("Plattformdienst '" + service_name + "' meldet ABI " +
                                    std::to_string(services.abi_version()) +
                                    ", erforderlich ist ABI " +
                                    std::to_string(requirements.abi_version) + ".");
    }
    if (services.guest_cycle_contract() != requirements.guest_cycle_contract) {
        throw std::invalid_argument(
            "Plattformdienst '" + service_name + "' meldet Gastzyklusvertrag " +
            std::to_string(services.guest_cycle_contract()) + ", erforderlich ist Vertrag " +
            std::to_string(requirements.guest_cycle_contract) + ".");
    }
    const auto missing = requirements.capabilities & ~services.capabilities();
    if (missing != 0u) {
        throw std::invalid_argument(
            "Plattformdienst '" + service_name + "' mit ABI " +
            std::to_string(services.abi_version()) +
            " besitzt nicht die erforderliche Faehigkeitsmaske; fehlende Ursache/Maske " +
            std::to_string(missing) + ".");
    }
}

} // namespace katana::runtime
