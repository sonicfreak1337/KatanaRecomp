#include "katana/codegen/cpp_emitter.hpp"
#include "katana/runtime/platform_services.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class MockServices final : public katana::runtime::PlatformServices {
  public:
    std::uint32_t version = katana::runtime::platform_services_abi_version;
    std::uint32_t timing_contract = katana::runtime::guest_cycle_contract_version;
    katana::runtime::PlatformCapabilities available =
        katana::runtime::core_platform_capabilities |
        katana::runtime::platform_capability(
            katana::runtime::PlatformCapability::ControlledFallback) |
        katana::runtime::platform_capability(katana::runtime::PlatformCapability::Mmu) |
        katana::runtime::platform_capability(katana::runtime::PlatformCapability::Watchpoints) |
        katana::runtime::platform_capability(katana::runtime::PlatformCapability::ExecutableRam) |
        katana::runtime::platform_capability(katana::runtime::PlatformCapability::FirmwareMode);
    std::array<std::uint8_t, 64u> memory{};
    std::uint64_t cycle = 0u;
    std::array<std::string_view, 8u> completion_order{};
    std::size_t completion_order_size = 0u;
    std::uint32_t observed_checkpoint = 0u;
    std::uint64_t observed_retired = 0u;
    bool observed_new_exception = false;
    bool observed_exception_exit = false;
    bool throw_during_cycle_commit = false;

    [[nodiscard]] std::string_view name() const noexcept override {
        return "mock";
    }
    [[nodiscard]] std::uint32_t abi_version() const noexcept override {
        return version;
    }
    [[nodiscard]] std::uint32_t guest_cycle_contract() const noexcept override {
        return timing_contract;
    }
    [[nodiscard]] katana::runtime::PlatformCapabilities capabilities() const noexcept override {
        return available;
    }
    void read_memory(const std::uint32_t address,
                     const std::span<std::uint8_t> destination) override {
        std::copy_n(memory.begin() + address, destination.size(), destination.begin());
    }
    void write_memory(const std::uint32_t address,
                      const std::span<const std::uint8_t> source) override {
        std::copy(source.begin(), source.end(), memory.begin() + address);
    }
    [[nodiscard]] std::uint64_t scheduler_cycle() const noexcept override {
        return cycle;
    }
    [[nodiscard]] std::optional<std::uint64_t>
    next_scheduler_event_cycle() const noexcept override {
        return cycle + 10u;
    }
    [[nodiscard]] katana::runtime::PlatformSchedulerResult
    consume_guest_cycles(const std::uint64_t guest_cycles,
                         const std::size_t event_budget) override {
        completion_order[completion_order_size++] = "cycles";
        if (throw_during_cycle_commit) {
            cycle += std::min<std::uint64_t>(guest_cycles, 2u);
            throw std::runtime_error("expected-cycle-commit-throw");
        }
        cycle += guest_cycles;
        return {cycle, std::min<std::size_t>(event_budget, 2u), event_budget < 2u, false};
    }
    [[nodiscard]] std::optional<katana::runtime::PlatformInterruptRequest>
    poll_interrupt() override {
        completion_order[completion_order_size++] = "interrupt";
        return katana::runtime::PlatformInterruptRequest{7u, 11u, 0x320u};
    }
    [[nodiscard]] katana::runtime::PlatformDmaResult
    start_dma(const katana::runtime::PlatformDmaRequest& request) override {
        std::copy_n(
            memory.begin() + request.source, request.length, memory.begin() + request.destination);
        return {request.length, true};
    }
    [[nodiscard]] katana::runtime::PlatformFallbackResult
    controlled_fallback(katana::runtime::CpuState& cpu,
                        const katana::runtime::PlatformFallbackRequest& request) override {
        cpu.pc = request.guest_pc + 2u;
        return {true, cpu.pc};
    }
    [[nodiscard]] bool prefetch(katana::runtime::CpuState& cpu,
                                const katana::runtime::GuestInstructionOrigin,
                                const std::uint32_t address) override {
        katana::runtime::prefetch(cpu, address);
        return cpu.last_prefetch_was_store_queue;
    }
    void observe_guest_block_completion(const std::uint32_t checkpoint,
                                        const std::uint64_t retired,
                                        const bool new_exception,
                                        const bool exception_exit) noexcept override {
        completion_order[completion_order_size++] = "checkpoint";
        observed_checkpoint = checkpoint;
        observed_retired = retired;
        observed_new_exception = new_exception;
        observed_exception_exit = exception_exit;
    }
};

template <typename Function> std::string failure(Function&& function) {
    try {
        function();
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return {};
}

} // namespace

int main() {
    using namespace katana::runtime;
    MockServices services;
    PlatformServiceRequirements all;
    all.capabilities = services.available;
    validate_platform_services(services, all);

    const std::array<std::uint8_t, 4u> input = {0x11u, 0x22u, 0x33u, 0x44u};
    services.write_memory(4u, input);
    std::array<std::uint8_t, 4u> output{};
    services.read_memory(4u, output);
    const auto scheduler = services.consume_guest_cycles(100u, 3u);
    const auto interrupt = services.poll_interrupt();
    const auto dma = services.start_dma({4u, 16u, 4u});
    CpuState cpu;
    const auto fallback = services.controlled_fallback(cpu, {0x8C010000u, 0xFFFFu});
    const bool store_queue =
        services.prefetch(cpu, GuestInstructionOrigin{0x100u, 0x8C000100u, true}, 0xE0000000u);
    require(output == input && scheduler.guest_cycle == 100u && scheduler.processed_events == 2u &&
                !scheduler.budget_exhausted && interrupt && interrupt->level == 11u &&
                interrupt->event_code == 0x320u && dma.completed && dma.transferred == 4u &&
                services.memory[16u] == 0x11u && fallback.handled && cpu.pc == 0x8C010002u &&
                store_queue,
            "Mock-Plattform erreicht nicht alle Speicher-, Scheduler-, Interrupt-, DMA- und "
            "Fallbackgrenzen.");

    services.completion_order_size = 0u;
    cpu.pending_guest_cycles = 7u;
    const auto completed =
        finalize_guest_block(cpu, services, 3u, 0x8C010000u, 2u, false, false);
    require(completed.scheduler.guest_cycle == 107u && completed.interrupt.has_value() &&
                cpu.pending_guest_cycles == 0u && cpu.total_guest_cycles == 7u &&
                services.observed_checkpoint == 0x8C010000u &&
                services.observed_retired == 2u && !services.observed_new_exception &&
                !services.observed_exception_exit &&
                services.completion_order_size == 3u &&
                services.completion_order[0] == "cycles" &&
                services.completion_order[1] == "checkpoint" &&
                services.completion_order[2] == "interrupt",
            "Blockabschluss committed Zeit, Checkpoint und Interrupt nicht in derselben Reihenfolge.");

    services.completion_order_size = 0u;
    cpu.pending_guest_cycles = 2u;
    const auto faulted =
        finalize_guest_block(cpu, services, 3u, 0x8C010002u, 0u, true, true);
    require(!faulted.interrupt.has_value() &&
                services.completion_order_size == 2u &&
                services.completion_order[0] == "cycles" &&
                services.completion_order[1] == "checkpoint" &&
                services.observed_new_exception && services.observed_exception_exit,
            "Exception-Blockabschluss pollt einen Interrupt oder verliert seine Exceptionkante.");

    services.completion_order_size = 0u;
    services.throw_during_cycle_commit = true;
    cpu.pending_guest_cycles = 5u;
    bool commit_threw = false;
    try {
        static_cast<void>(commit_pending_guest_cycles(cpu, services, 3u));
    } catch (const std::runtime_error& error) {
        commit_threw =
            std::string(error.what()).find("expected-cycle-commit-throw") != std::string::npos;
    }
    services.throw_during_cycle_commit = false;
    require(commit_threw && services.cycle == 111u &&
                cpu.total_guest_cycles == 11u && cpu.pending_guest_cycles == 3u,
            "Scheduler-Throw verliert bereits gelieferte Gastzeit oder verbucht Restzeit "
            "vorzeitig.");

    ++services.version;
    const auto abi_error = failure([&] { validate_platform_services(services, all); });
    services.version = platform_services_abi_version;
    ++services.timing_contract;
    const auto timing_error = failure([&] { validate_platform_services(services, all); });
    services.timing_contract = guest_cycle_contract_version;
    services.available &= ~platform_capability(PlatformCapability::Watchpoints);
    const auto capability_error = failure([&] { validate_platform_services(services, all); });
    require(abi_error.find("mock") != std::string::npos &&
                abi_error.find("ABI " + std::to_string(platform_services_abi_version + 1u)) !=
                    std::string::npos &&
                abi_error.find("erforderlich ist ABI " +
                               std::to_string(platform_services_abi_version)) !=
                    std::string::npos &&
                timing_error.find("mock") != std::string::npos &&
                timing_error.find(
                    "Gastzyklusvertrag " +
                    std::to_string(guest_cycle_contract_version + 1u)) != std::string::npos &&
                timing_error.find(
                    "erforderlich ist Vertrag " +
                    std::to_string(guest_cycle_contract_version)) != std::string::npos &&
                capability_error.find("mock") != std::string::npos &&
                capability_error.find("ABI " + std::to_string(platform_services_abi_version)) !=
                    std::string::npos &&
                capability_error.find(
                    "Maske " +
                    std::to_string(platform_capability(PlatformCapability::Watchpoints))) !=
                    std::string::npos,
            "Fehlende Plattformdienste nennen Name, ABI oder Ursache nicht.");

    const katana::codegen::CppBackend backend;
    require((backend.capabilities() &
             katana::codegen::capability(katana::codegen::BackendCapability::PlatformServices)) !=
                0u,
            "C++-Backend meldet den Plattformdienstvertrag nicht.");

    std::cout << "KR-3205 Plattformdienst-Schnittstelle erfolgreich.\n";
    return 0;
}
