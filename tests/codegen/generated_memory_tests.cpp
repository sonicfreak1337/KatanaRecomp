#include "generated_memory_program.cpp"
#include "katana/runtime/code_invalidation.hpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void print_hex(const char* name, const std::uint32_t value) {
    std::cout << name << " = 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
              << value << '\n';
}

class GeneratedMemoryServices final : public katana::runtime::PlatformServices {
  public:
    explicit GeneratedMemoryServices(katana::runtime::CpuState& cpu) : cpu_(cpu) {
        const auto registration = tracker_.register_block(
            {fixture_identity,
             fixture_physical_start(),
             fixture_size,
             "generated-memory-test",
             {}});
        if (registration != katana::runtime::BlockRegistrationResult::Inserted) {
            throw std::runtime_error("Generated-Memory-Fixture konnte nicht registriert werden.");
        }
        cpu_.memory.set_guest_write_observer([this](const auto& event) {
            static_cast<void>(tracker_.observe_write(
                event.address, event.size, event.source, event.bytes_changed));
        });
    }

    ~GeneratedMemoryServices() override {
        cpu_.memory.clear_guest_write_observer();
    }

    GeneratedMemoryServices(const GeneratedMemoryServices&) = delete;
    GeneratedMemoryServices& operator=(const GeneratedMemoryServices&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override {
        return "generated-memory-test";
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
    void read_memory(const std::uint32_t address,
                     const std::span<std::uint8_t> destination) override {
        for (std::size_t index = 0u; index < destination.size(); ++index) {
            destination[index] =
                cpu_.memory.read_u8(address + static_cast<std::uint32_t>(index));
        }
    }
    void write_memory(const std::uint32_t address,
                      const std::span<const std::uint8_t> source) override {
        for (std::size_t index = 0u; index < source.size(); ++index) {
            cpu_.memory.write_u8(address + static_cast<std::uint32_t>(index), source[index]);
        }
    }
    [[nodiscard]] std::uint64_t scheduler_cycle() const noexcept override {
        return scheduler_cycle_;
    }
    [[nodiscard]] std::optional<std::uint64_t>
    next_scheduler_event_cycle() const noexcept override {
        return std::nullopt;
    }
    [[nodiscard]] katana::runtime::PlatformSchedulerResult
    consume_guest_cycles(const std::uint64_t guest_cycles, const std::size_t) override {
        scheduler_cycle_ += guest_cycles;
        return {scheduler_cycle_, 0u, false, false};
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
    [[nodiscard]] katana::runtime::ExecutableCodeTracker*
    executable_code_tracker() noexcept override {
        return &tracker_;
    }
    [[nodiscard]] bool fixture_code_valid() const {
        return tracker_.valid(fixture_identity);
    }
    [[nodiscard]] bool fixture_code_range_exact() const noexcept {
        return tracker_.tracks_address(fixture_physical_start(), fixture_size) &&
               !tracker_.tracks_address(fixture_physical_start() - 1u) &&
               !tracker_.tracks_address(fixture_physical_start() + fixture_size);
    }
    [[nodiscard]] std::uint64_t invalidation_count() const noexcept {
        return tracker_.invalidation_count();
    }
    [[nodiscard]] std::uint64_t data_page_generation() const noexcept {
        return tracker_.page_generation(0x40u);
    }

  private:
    static constexpr const char* fixture_identity = "generated-memory-fixture";
    static constexpr std::uint32_t fixture_size = 24u;
    [[nodiscard]] static std::uint32_t fixture_physical_start() noexcept {
        return katana::runtime::canonical_physical_address(
            katana_generated::generated_entry_address);
    }
    katana::runtime::CpuState& cpu_;
    katana::runtime::ExecutableCodeTracker tracker_;
    std::uint64_t scheduler_cycle_ = 0u;
};

} // namespace

int main() {
    katana_generated::CpuState cpu;
    GeneratedMemoryServices services(cpu);

    katana_generated::run(cpu, services);

    require(cpu.r[1] == 0x40u, "r1 muss die Testadresse 0x40 enthalten.");

    require(cpu.r[3] == 0xFFFFFF80u, "MOV.B-Load muss das Byte mit Vorzeichen erweitern.");

    require(cpu.r[5] == 0xFFFFFFFFu, "MOV.W-Load muss das Word mit Vorzeichen erweitern.");

    require(cpu.r[6] == 127u, "MOV.L-Load muss den gespeicherten 32-Bit-Wert laden.");

    require(cpu.memory.read_u8(0x40u) == 0x7Fu,
            "Das niederwertige Byte des Long-Stores ist falsch.");

    require(cpu.memory.read_u8(0x41u) == 0x00u && cpu.memory.read_u8(0x42u) == 0x00u &&
                cpu.memory.read_u8(0x43u) == 0x00u,
            "Der Long-Store ist nicht korrekt Little Endian.");

    require(cpu.pc == 0u, "Der finale PC muss dem initialen PR-Wert entsprechen.");

    require(services.fixture_code_valid() && services.fixture_code_range_exact() &&
                services.invalidation_count() == 0u &&
                services.data_page_generation() == 3u,
            "Datenwrites werden nicht exakt beobachtet oder invalidieren den Fixture-Code.");

    std::cout << "Generierte Speicherzugriffe wurden erfolgreich ausgefuehrt.\n";

    print_hex("r3", cpu.r[3]);
    print_hex("r5", cpu.r[5]);

    std::cout << "r6 = " << std::dec << cpu.r[6] << '\n';

    std::cout << "memory[0x40..0x43] = " << std::hex << std::uppercase << std::setw(2)
              << std::setfill('0') << static_cast<unsigned>(cpu.memory.read_u8(0x40u)) << " "
              << std::setw(2) << static_cast<unsigned>(cpu.memory.read_u8(0x41u)) << " "
              << std::setw(2) << static_cast<unsigned>(cpu.memory.read_u8(0x42u)) << " "
              << std::setw(2) << static_cast<unsigned>(cpu.memory.read_u8(0x43u)) << '\n';

    return EXIT_SUCCESS;
}
