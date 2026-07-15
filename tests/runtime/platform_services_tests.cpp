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
    katana::runtime::PlatformCapabilities available =
        katana::runtime::core_platform_capabilities |
        katana::runtime::platform_capability(
            katana::runtime::PlatformCapability::ControlledFallback
        ) |
        katana::runtime::platform_capability(katana::runtime::PlatformCapability::Mmu) |
        katana::runtime::platform_capability(katana::runtime::PlatformCapability::Watchpoints) |
        katana::runtime::platform_capability(
            katana::runtime::PlatformCapability::ExecutableRam
        ) |
        katana::runtime::platform_capability(
            katana::runtime::PlatformCapability::FirmwareMode
        );
    std::array<std::uint8_t, 64u> memory{};
    std::uint64_t cycle = 0u;

    [[nodiscard]] std::string_view name() const noexcept override { return "mock"; }
    [[nodiscard]] std::uint32_t abi_version() const noexcept override { return version; }
    [[nodiscard]] katana::runtime::PlatformCapabilities capabilities() const noexcept override {
        return available;
    }
    void read_memory(
        const std::uint32_t address,
        const std::span<std::uint8_t> destination
    ) override {
        std::copy_n(memory.begin() + address, destination.size(), destination.begin());
    }
    void write_memory(
        const std::uint32_t address,
        const std::span<const std::uint8_t> source
    ) override {
        std::copy(source.begin(), source.end(), memory.begin() + address);
    }
    [[nodiscard]] std::uint64_t scheduler_cycle() const noexcept override { return cycle; }
    [[nodiscard]] katana::runtime::PlatformSchedulerResult advance_scheduler(
        const std::uint64_t guest_cycle,
        const std::size_t event_budget
    ) override {
        cycle = guest_cycle;
        return {cycle, std::min<std::size_t>(event_budget, 2u), event_budget < 2u};
    }
    [[nodiscard]] std::optional<katana::runtime::PlatformInterruptRequest>
    poll_interrupt() override {
        return katana::runtime::PlatformInterruptRequest{7u, 11u, 0x320u};
    }
    [[nodiscard]] katana::runtime::PlatformDmaResult start_dma(
        const katana::runtime::PlatformDmaRequest& request
    ) override {
        std::copy_n(memory.begin() + request.source, request.length,
            memory.begin() + request.destination);
        return {request.length, true};
    }
    [[nodiscard]] katana::runtime::PlatformFallbackResult controlled_fallback(
        katana::runtime::CpuState& cpu,
        const katana::runtime::PlatformFallbackRequest& request
    ) override {
        cpu.pc = request.guest_pc + 2u;
        return {true, cpu.pc};
    }
};

template <typename Function>
std::string failure(Function&& function) {
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
    const auto scheduler = services.advance_scheduler(100u, 3u);
    const auto interrupt = services.poll_interrupt();
    const auto dma = services.start_dma({4u, 16u, 4u});
    CpuState cpu;
    const auto fallback = services.controlled_fallback(cpu, {0x8C010000u, 0xFFFFu});
    require(
        output == input && scheduler.guest_cycle == 100u &&
            scheduler.processed_events == 2u && !scheduler.budget_exhausted &&
            interrupt && interrupt->level == 11u && interrupt->event_code == 0x320u &&
            dma.completed && dma.transferred == 4u && services.memory[16u] == 0x11u &&
            fallback.handled && cpu.pc == 0x8C010002u,
        "Mock-Plattform erreicht nicht alle Speicher-, Scheduler-, Interrupt-, DMA- und Fallbackgrenzen."
    );

    ++services.version;
    const auto abi_error = failure([&] { validate_platform_services(services, all); });
    services.version = platform_services_abi_version;
    services.available &= ~platform_capability(PlatformCapability::Watchpoints);
    const auto capability_error = failure([&] { validate_platform_services(services, all); });
    require(
        abi_error.find("mock") != std::string::npos &&
            abi_error.find("ABI 2") != std::string::npos &&
            abi_error.find("erforderlich ist ABI 1") != std::string::npos &&
            capability_error.find("mock") != std::string::npos &&
            capability_error.find("ABI 1") != std::string::npos &&
            capability_error.find("Maske 64") != std::string::npos,
        "Fehlende Plattformdienste nennen Name, ABI oder Ursache nicht."
    );

    const katana::codegen::CppBackend backend;
    require(
        (backend.capabilities() & katana::codegen::capability(
            katana::codegen::BackendCapability::PlatformServices
        )) != 0u,
        "C++-Backend meldet den Plattformdienstvertrag nicht."
    );

    std::cout << "KR-3205 Plattformdienst-Schnittstelle erfolgreich.\n";
    return 0;
}
