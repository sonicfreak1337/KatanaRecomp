#pragma once

#include "katana/build_contract.hpp"
#include "katana/runtime/runtime.hpp"
#include "katana/runtime/scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace katana::runtime {

class ExecutableCodeTracker;

inline constexpr std::uint32_t platform_services_abi_version =
    build_contract::platform_services_abi_version;
inline constexpr std::uint64_t base_guest_cycles_per_instruction = 1u;

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
    [[nodiscard]] virtual bool prefetch(CpuState& cpu, std::uint32_t address) = 0;
    virtual void observe_guest_checkpoint(std::uint32_t) noexcept {}
    virtual void register_executable_block(std::uint32_t, std::uint32_t, std::string_view) {}
    [[nodiscard]] virtual ExecutableCodeTracker* executable_code_tracker() noexcept {
        return nullptr;
    }
};

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
