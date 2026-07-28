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

namespace native_owner_exit_fixture::runtime_dispatch_detail {

thread_local PlatformServices* active_services = nullptr;
thread_local katana::runtime::BlockAddress active_exit_source;
thread_local katana::runtime::BlockEndKind active_exit_kind =
    katana::runtime::BlockEndKind::Fallthrough;
thread_local katana::runtime::DynamicDispatchSiteClass active_exit_site_class =
    katana::runtime::DynamicDispatchSiteClass::NotDynamic;
thread_local bool tail_dispatch_completed = false;

} // namespace native_owner_exit_fixture::runtime_dispatch_detail

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
        if (observed_chain_count < observed_chain_addresses.size())
            observed_chain_addresses[observed_chain_count] = address;
        ++observed_chain_count;
        return address == 0x2002u || address == 12u;
    }

    mutable std::array<std::uint32_t, 4u> observed_chain_addresses{};
    mutable std::size_t observed_chain_count = 0u;
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

    services.observed_chain_count = 0u;
    native_owner_exit_fixture::CpuState owner_exit_cpu;
    katana::runtime::BlockExecutionContext owner_exit_context;
    owner_exit_cpu.pc = 0u;
    constexpr katana::runtime::BlockAddress saved_exit_source{
        0x11111111u, 0x01234567u};
    native_owner_exit_fixture::runtime_dispatch_detail::active_services =
        &services;
    native_owner_exit_fixture::runtime_dispatch_detail::active_exit_source =
        saved_exit_source;
    native_owner_exit_fixture::runtime_dispatch_detail::active_exit_kind =
        katana::runtime::BlockEndKind::Sleep;
    native_owner_exit_fixture::runtime_dispatch_detail::active_exit_site_class =
        katana::runtime::DynamicDispatchSiteClass::RuntimeOnly;
    native_owner_exit_fixture::runtime_dispatch_detail::
        tail_dispatch_completed = true;
    const auto owner_exit =
        native_owner_exit_fixture::fn_00000000_runtime_entry(
            owner_exit_cpu, owner_exit_context);
    native_owner_exit_fixture::runtime_dispatch_detail::active_services =
        nullptr;
    require(
        services.observed_chain_count == 2u &&
            services.observed_chain_addresses[0] == 12u &&
            services.observed_chain_addresses[1] == 6u &&
            owner_exit_cpu.pc == 6u &&
            owner_exit_cpu.retired_guest_instructions == 5u,
        "Aufgeloester indirekter Native-Call kehrt nicht normal zur "
        "abgelehnten Caller-Fortsetzung zurueck.");
    require(
        owner_exit.kind == katana::runtime::BlockEndKind::Call &&
            owner_exit.source ==
                katana::runtime::BlockAddress{
                    2u, katana::runtime::canonical_physical_address(2u)} &&
            owner_exit.target.has_value() &&
            *owner_exit.target ==
                katana::runtime::BlockAddress{
                    6u, katana::runtime::canonical_physical_address(6u)},
        "Verschachtelter Owner-Entry ersetzt den aeusseren Caller-BlockExit.");
    require(
        native_owner_exit_fixture::runtime_dispatch_detail::
                active_exit_source ==
                katana::runtime::BlockAddress{
                    2u, katana::runtime::canonical_physical_address(2u)} &&
            native_owner_exit_fixture::runtime_dispatch_detail::
                active_exit_kind == katana::runtime::BlockEndKind::Call &&
            native_owner_exit_fixture::runtime_dispatch_detail::
                    active_exit_site_class ==
                katana::runtime::DynamicDispatchSiteClass::Unresolved &&
            !native_owner_exit_fixture::runtime_dispatch_detail::
                tail_dispatch_completed,
        "Aeusserer Owner-Entry veroeffentlicht nicht seinen eigenen "
        "Exit-Zustand an den zentralen Dispatcher.");

    normal_fallthrough_fixture::CpuState normal_cpu;
    normal_fallthrough_fixture::run(normal_cpu);
    require(normal_cpu.r[3] == 7u && normal_cpu.pc == 0x1002u &&
                normal_cpu.retired_guest_instructions == 1u,
            "Normaler Backendmodus gibt nicht den exakten funktionsgrenzenueberschreitenden "
            "Fallthrough-PC zurueck.");

    std::cout << "Fallthrough in Single-Block- und normalem Backendmodus erfolgreich.\n";
    return EXIT_SUCCESS;
}
