#include "generated_single_block_fallthrough_program.cpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace single_block_fallthrough_fixture {

std::array<std::uint32_t, 4u> observed_blocks{};
std::size_t observed_block_count = 0u;

void note_block_entry(const std::uint32_t address) noexcept {
    if (observed_block_count < observed_blocks.size()) {
        observed_blocks[observed_block_count] = address;
    }
    ++observed_block_count;
}

} // namespace single_block_fallthrough_fixture

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class ChainServices final : public katana::runtime::PlatformServices {
  public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "single-block-fallthrough";
    }
    [[nodiscard]] std::uint32_t abi_version() const noexcept override {
        return katana::runtime::platform_services_abi_version;
    }
    [[nodiscard]] std::uint32_t guest_cycle_contract() const noexcept override {
        return katana::runtime::guest_cycle_contract_version;
    }
    [[nodiscard]] katana::runtime::PlatformCapabilities capabilities() const noexcept override {
        return katana::runtime::core_platform_capabilities;
    }
    void read_memory(std::uint32_t, std::span<std::uint8_t>) override {}
    void write_memory(std::uint32_t, std::span<const std::uint8_t>) override {}
    [[nodiscard]] std::uint64_t scheduler_cycle() const noexcept override { return 0u; }
    [[nodiscard]] std::optional<std::uint64_t>
    next_scheduler_event_cycle() const noexcept override {
        return std::nullopt;
    }
    [[nodiscard]] katana::runtime::PlatformSchedulerResult
    consume_guest_cycles(std::uint64_t, std::size_t) override {
        return {};
    }
    [[nodiscard]] std::optional<katana::runtime::PlatformInterruptRequest>
    poll_interrupt() override {
        return std::nullopt;
    }
    [[nodiscard]] katana::runtime::PlatformDmaResult
    start_dma(const katana::runtime::PlatformDmaRequest&) override {
        return {};
    }
    [[nodiscard]] katana::runtime::PlatformFallbackResult
    controlled_fallback(katana::runtime::CpuState&,
                        const katana::runtime::PlatformFallbackRequest&) override {
        return {};
    }
    [[nodiscard]] bool prefetch(katana::runtime::CpuState&,
                                katana::runtime::GuestInstructionOrigin,
                                std::uint32_t) override {
        return false;
    }
    [[nodiscard]] bool
    can_chain_executable_block(const std::uint32_t address) const noexcept override {
        return address == 0x2002u;
    }
};

} // namespace

int main() {
    using namespace single_block_fallthrough_fixture;

    CpuState external_cpu;
    run(external_cpu);
    require(external_cpu.r[3] == 7u,
            "Der externe Eininstruktionsblock wurde nicht ausgefuehrt.");
    require(external_cpu.pc == 0x1002u,
            "Der externe Eininstruktionsblock gibt nicht den exakten CFG-Fallthrough-PC zurueck.");
    require(external_cpu.retired_guest_instructions == 1u && observed_block_count == 1u &&
                observed_blocks[0] == 0x1000u,
            "Der externe Eininstruktionsblock wurde nicht exakt einmal retired.");

    observed_block_count = 0u;
    CpuState chained_cpu;
    ChainServices services;
    chained_cpu.pc = 0x2000u;
    fn_00002000_with_services(chained_cpu, &services);
    require(chained_cpu.r[0] == 2u && chained_cpu.pc == 0x2004u &&
                chained_cpu.retired_guest_instructions == 2u,
            "Lokales Mehrblock-Chaining wurde durch den externen Fallthrough-Fix beschaedigt.");
    require(observed_block_count == 2u && observed_blocks[0] == 0x2000u &&
                observed_blocks[1] == 0x2002u,
            "Lokales Mehrblock-Chaining betritt nicht beide Bloecke genau einmal.");

    normal_fallthrough_fixture::CpuState normal_cpu;
    normal_fallthrough_fixture::run(normal_cpu);
    require(normal_cpu.r[3] == 7u && normal_cpu.pc == 0x1002u &&
                normal_cpu.retired_guest_instructions == 1u,
            "Normaler Backendmodus gibt nicht den exakten funktionsgrenzenueberschreitenden "
            "Fallthrough-PC zurueck.");

    std::cout << "Fallthrough in Single-Block- und normalem Backendmodus erfolgreich.\n";
    return EXIT_SUCCESS;
}
