#include "katana/codegen/port_export.hpp"
#include "katana/ir/lower.hpp"
#include "katana/platform/dreamcast_disc.hpp"
#include "katana/runtime/cache_control.hpp"
#include "katana/runtime/disc_install.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/executable_modules.hpp"
#include "katana/runtime/indirect_dispatch.hpp"
#include "katana/runtime/packed_disc.hpp"
#include "katana/runtime/platform_services.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t raw_sector_size = 2352u;
constexpr std::size_t payload_size = 2048u;
constexpr std::uint32_t data_lba = 45'000u;

std::vector<std::string> observed_progress;

void observe_progress(const std::string_view phase) {
    observed_progress.emplace_back(phase);
}

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception = std::exception, typename Function>
void require_failure(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    require(false, message);
}

struct Fixture final {
    std::filesystem::path root = std::filesystem::current_path() / "katana-port-export-fixture";

    Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "disc");
    }

    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

void both32(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0u; index < 4u; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
        bytes[offset + 4u + index] = static_cast<std::uint8_t>(value >> ((3u - index) * 8u));
    }
}

std::size_t record(std::vector<std::uint8_t>& bytes,
                   const std::size_t offset,
                   const std::uint32_t lba,
                   const std::uint32_t size,
                   const std::string& name,
                   const bool directory) {
    const auto length =
        static_cast<std::uint8_t>(33u + name.size() + (name.size() % 2u == 0u ? 1u : 0u));
    bytes[offset] = length;
    both32(bytes, offset + 2u, lba);
    both32(bytes, offset + 10u, size);
    bytes[offset + 25u] = directory ? 2u : 0u;
    bytes[offset + 28u] = 1u;
    bytes[offset + 31u] = 1u;
    bytes[offset + 32u] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 33u));
    return length;
}

std::size_t payload_offset(const std::size_t sector, const std::size_t byte = 0u) {
    return sector * raw_sector_size + 16u + byte;
}

enum class FixtureProgram : std::uint8_t {
    Normal,
    ImmediateTrap,
    UnknownDynamicTarget,
};

std::vector<std::uint8_t> boot_track(
    const FixtureProgram fixture_program = FixtureProgram::Normal) {
    std::vector<std::uint8_t> bytes(22u * raw_sector_size);
    for (std::size_t sector = 0u; sector < 22u; ++sector) {
        bytes[sector * raw_sector_size + 15u] = 1u;
    }
    const std::string hardware = "SEGA SEGAKATANA ";
    const std::string boot_file = "BOOT.BIN        ";
    std::copy(hardware.begin(),
              hardware.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(0u)));
    std::copy(boot_file.begin(),
              boot_file.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(0u, 0x60u)));
    constexpr std::array<std::uint8_t, 12u> system_bootstrap = {
        0x01u, 0xD0u, // mov.l @(1,pc),r0 -> P2 literal at 0xAC008308
        0x2Bu, 0x40u, // jmp @r0
        0x09u, 0x00u, // delay-slot nop
        0x09u, 0x00u, // aligned padding
        0x00u, 0x00u, 0x01u, 0x8Cu // 0x8C010000
    };
    std::copy(system_bootstrap.begin(),
              system_bootstrap.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(0u, 0x300u)));

    const auto pvd = payload_offset(16u);
    bytes[pvd] = 1u;
    std::copy_n("CD001", 5u, bytes.begin() + static_cast<std::ptrdiff_t>(pvd + 1u));
    bytes[pvd + 6u] = 1u;
    record(bytes, pvd + 156u, data_lba + 20u, payload_size, std::string(1u, '\0'), true);
    auto directory = payload_offset(20u);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\0'), true);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\1'), true);
    record(bytes, directory, data_lba + 21u, 24u, "BOOT.BIN;1", false);
    constexpr std::array<std::uint8_t, 24u> normal_program = {
        0x00u, 0xA0u, // bra 0x8C010004: force a locally chainable static entry block
        0x22u, 0x4Fu, // delay slot: sts.l pr,@-r15, preserve the root sentinel
        0x08u, 0xE0u, // mov #8,r0
        0x03u, 0x00u, // bsrf r0 -> 0x8C010012 from a second, dynamic call block
        0x07u, 0xE2u, // delay slot: mov #7,r2
        0x26u, 0x4Fu, // lds.l @r15+,pr: restore the program-root sentinel
        0x0Bu, 0x00u, // caller rts
        0x09u, 0x00u, // delay-slot nop
        0x09u, 0x00u, // padding nop
        0x05u, 0xE1u, // callee: mov #5,r1
        0x0Bu, 0x00u, // callee rts
        0x09u, 0x00u  // delay-slot nop
    };
    constexpr std::array<std::uint8_t, 24u> trap_program = {
        0x00u, 0xC3u, // trapa #0
        0x0Bu, 0x00u, // unreachable rts
        0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u,
        0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u};
    constexpr std::array<std::uint8_t, 24u> unknown_dynamic_target_program = {
        0x01u, 0xD1u, // mov.l @(1,pc),r1 -> live target literal at 0x8C010008
        0x2Bu, 0x41u, // jmp @r1
        0x09u, 0x00u, // delay-slot nop
        0x09u, 0x00u, // aligned padding
        0x00u, 0x00u, 0x10u, 0x8Cu, // mapped main RAM at 0x8C100000, no code provenance
        0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u,
        0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u};
    const auto& program = fixture_program == FixtureProgram::ImmediateTrap
                              ? trap_program
                          : fixture_program == FixtureProgram::UnknownDynamicTarget
                              ? unknown_dynamic_target_program
                              : normal_program;
    std::copy(program.begin(),
              program.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(21u)));
    return bytes;
}

std::vector<std::uint8_t> stored_unknown_candidate_boot_track() {
    auto bytes = boot_track();
    constexpr std::uint32_t boot_size = 0x34u;
    auto directory = payload_offset(20u);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\0'), true);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\1'), true);
    static_cast<void>(
        record(bytes, directory, data_lba + 21u, boot_size, "BOOT.BIN;1", false));

    std::vector<std::uint8_t> program(boot_size);
    for (std::size_t offset = 0u; offset < program.size(); offset += 2u)
        program[offset] = 0x09u; // nop
    program[0x00u] = 0x03u;
    program[0x01u] = 0xD4u; // mov.l @(0x10,pc),r4 -> candidate 0x8C010030
    program[0x02u] = 0x0Du;
    program[0x03u] = 0xB0u; // bsr 0x8C010020
    program[0x04u] = 0x09u;
    program[0x05u] = 0x00u; // nop (delay)
    program[0x06u] = 0x0Bu;
    program[0x07u] = 0x00u; // rts
    program[0x08u] = 0x09u;
    program[0x09u] = 0x00u; // nop (delay)
    constexpr auto candidate_address =
        katana::platform::dreamcast_disc_boot_address + 0x30u;
    program[0x10u] = static_cast<std::uint8_t>(candidate_address);
    program[0x11u] = static_cast<std::uint8_t>(candidate_address >> 8u);
    program[0x12u] = static_cast<std::uint8_t>(candidate_address >> 16u);
    program[0x13u] = static_cast<std::uint8_t>(candidate_address >> 24u);
    program[0x20u] = 0x28u;
    program[0x21u] = 0xE2u; // mov #0x28,r2 (proven non-stack destination)
    program[0x22u] = 0x42u;
    program[0x23u] = 0x22u; // mov.l r4,@r2
    program[0x24u] = 0x0Bu;
    program[0x25u] = 0x00u; // rts
    program[0x26u] = 0x09u;
    program[0x27u] = 0x00u; // nop (delay)
    program[0x30u] = 0xFFu;
    program[0x31u] = 0xFFu; // speculative unknown opcode
    std::copy(program.begin(),
              program.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(21u)));
    return bytes;
}

std::vector<std::uint8_t> poll_loop_boot_track() {
    auto bytes = boot_track();
    constexpr std::array<std::uint8_t, 24u> program = {
        0x03u, 0xD3u, // loop: mov.l @(3,pc),r3 (conservative loop-local read)
        0x04u, 0xD0u, // mov.l @(4,pc),r0 (proven loop guard read)
        0x08u, 0x20u, // tst r0,r0
        0xFBu, 0x89u, // bt 0x8C010000
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u, // delay-slot nop
        0x09u, 0x00u, // aligned padding
        0x09u, 0x00u, // aligned padding
        0x11u, 0x11u, 0x11u, 0x11u, // conservative source at 0x8C010010
        0x00u, 0x00u, 0x00u, 0x00u  // guard source at 0x8C010014
    };
    std::copy(program.begin(),
              program.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(21u)));
    return bytes;
}

std::vector<std::uint8_t> mmio_wait_loop_boot_track(
    const std::uint32_t mmio_address = 0xA05F6900u) {
    auto bytes = boot_track();
    std::array<std::uint8_t, 24u> program = {
        0x03u, 0xD3u, // loop: mov.l @(3,pc),r3 -> pointer literal at 0x8C010010
        0x32u, 0x60u, // mov.l @r3,r0
        0x08u, 0xC8u, // tst #8,r0
        0xFBu, 0x89u, // bt 0x8C010000
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u, // delay-slot nop
        0x09u, 0x00u, // aligned padding
        0x09u, 0x00u, // aligned padding
        0x00u, 0x00u, 0x00u, 0x00u, // MMIO pointer literal
        0x09u, 0x00u, 0x09u, 0x00u};
    for (std::size_t index = 0u; index < 4u; ++index)
        program[0x10u + index] =
            static_cast<std::uint8_t>(mmio_address >> (index * 8u));
    std::copy(program.begin(),
              program.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(21u)));
    return bytes;
}

std::vector<std::uint8_t> counted_loop_boot_track(const bool valid_step = true,
                                                  const bool aliased_counter = false,
                                                  const bool on_chip_counter = false) {
    auto bytes = boot_track();
    const std::uint32_t boot_size = on_chip_counter ? 0x30u : 0x28u;
    auto directory = payload_offset(20u);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\0'), true);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\1'), true);
    static_cast<void>(record(bytes, directory, data_lba + 21u, boot_size, "BOOT.BIN;1", false));
    std::vector<std::uint8_t> program(boot_size, 0x09u);
    for (std::size_t offset = 1u; offset < program.size(); offset += 2u)
        program[offset] = 0x00u;
    if (on_chip_counter) {
        constexpr auto synthetic_on_chip_stack =
            katana::runtime::sh4_on_chip_ram_address + 0x01001230u;
        program[0x00u] = 0x09u;
        program[0x01u] = 0xDFu; // mov.l @(36,pc),r15 -> synthetic OCRAM alias
        program[0x02u] = 0x0Au;
        program[0x03u] = 0xD1u; // mov.l @(40,pc),r1 -> CCR 0xFF00001C
        program[0x04u] = 0x20u;
        program[0x05u] = 0xE0u; // mov #0x20,r0 (operand RAM enable)
        program[0x06u] = 0x02u;
        program[0x07u] = 0x21u; // mov.l r0,@r1
        program[0x08u] = 0x05u;
        program[0x09u] = 0xA0u; // bra 0x8C010016
        program[0x0Au] = 0x09u;
        program[0x0Bu] = 0x00u; // delay-slot nop
        for (std::size_t byte = 0u; byte < sizeof(synthetic_on_chip_stack); ++byte)
            program[0x28u + byte] =
                static_cast<std::uint8_t>(synthetic_on_chip_stack >> (byte * 8u));
        program[0x2Cu] = 0x1Cu;
        program[0x2Du] = 0x00u;
        program[0x2Eu] = 0x00u;
        program[0x2Fu] = 0xFFu;
    } else if (aliased_counter) {
        program[0x00u] = 0x01u;
        program[0x01u] = 0xDFu; // mov.l @(4,pc),r15 -> 0x8D01001C
        program[0x02u] = 0x08u;
        program[0x03u] = 0xA0u; // bra 0x8C010016
        program[0x04u] = 0x09u;
        program[0x05u] = 0x00u; // delay-slot nop
        program[0x08u] = 0x1Cu;
        program[0x09u] = 0x00u;
        program[0x0Au] = 0x01u;
        program[0x0Bu] = 0x8Du;
    } else {
        program[0x00u] = 0x09u;
        program[0x01u] = 0xA0u; // bra 0x8C010016: explicit guard block boundary
        program[0x02u] = 0x09u;
        program[0x03u] = 0x00u; // delay-slot nop
    }
    program[0x10u] = 0xF2u;
    program[0x11u] = 0x51u; // increment: mov.l @(8,r15),r1
    program[0x12u] = static_cast<std::uint8_t>(valid_step ? 0x01u : 0x00u);
    program[0x13u] = 0x71u; // add #1,r1 (add #0 is the negative proof fixture)
    program[0x14u] = 0x12u;
    program[0x15u] = 0x1Fu; // mov.l r1,@(8,r15)
    program[0x16u] = 0x05u;
    program[0x17u] = 0x93u; // guard: mov.w @(10,pc),r3 -> 0x8C010024
    program[0x18u] = 0xF2u;
    program[0x19u] = 0x52u; // mov.l @(8,r15),r2
    program[0x1Au] = 0x33u;
    program[0x1Bu] = 0x32u; // cmp/ge r3,r2
    program[0x1Cu] = 0xF8u;
    program[0x1Du] = 0x8Bu; // bf 0x8C010010
    program[0x1Eu] = 0x0Bu;
    program[0x1Fu] = 0x00u; // exit: rts
    program[0x20u] = 0x09u;
    program[0x21u] = 0x00u; // delay-slot nop
    program[0x24u] = 0x05u;
    program[0x25u] = 0x00u; // signed loop limit
    std::copy(program.begin(),
              program.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(21u)));
    return bytes;
}

std::vector<std::uint8_t> composite_callback_boot_track() {
    auto bytes = boot_track();
    constexpr std::uint32_t boot_size = 0xA4u;
    auto directory = payload_offset(20u);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\0'), true);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\1'), true);
    static_cast<void>(record(bytes, directory, data_lba + 21u, boot_size, "BOOT.BIN;1", false));

    std::vector<std::uint8_t> program(boot_size, 0x09u);
    for (std::size_t offset = 1u; offset < program.size(); offset += 2u)
        program[offset] = 0x00u;
    const auto put_u16 = [&program](const std::size_t offset, const std::uint16_t value) {
        program[offset] = static_cast<std::uint8_t>(value);
        program[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&program](const std::size_t offset, const std::uint32_t value) {
        program[offset] = static_cast<std::uint8_t>(value);
        program[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        program[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        program[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };

    put_u16(0x00u, 0x0D2Au); // sts pr,r13: preserve the generated root sentinel
    put_u16(0x02u, 0xD10Bu); // mov.l @(0x2c,pc),r1 -> accessor 0x8C010050
    put_u16(0x04u, 0x410Bu); // jsr @r1
    put_u16(0x06u, 0x0009u); // nop (delay)
    put_u16(0x08u, 0x6C03u); // mov r0,r12
    put_u16(0x0Au, 0x67C2u); // mov.l @r12,r7 -> live callback candidate
    put_u16(0x0Cu, 0xDF09u); // mov.l @(0x24,pc),r15 -> repeated source
    put_u16(0x0Eu, 0xD80Au); // mov.l @(0x28,pc),r8 -> destination
    put_u16(0x10u, 0xE900u); // mov #0,r9
    put_u16(0x12u, 0xEA40u); // mov #64,r10
    put_u16(0x14u, 0xA000u); // bra 0x8C010018: exact call-block boundary
    put_u16(0x16u, 0x0009u); // nop (delay)

    put_u16(0x18u, 0x65F3u); // mov r15,r5
    put_u16(0x1Au, 0xE604u); // mov #4,r6
    put_u16(0x1Cu, 0x470Bu); // jsr @r7
    put_u16(0x1Eu, 0x6483u); // mov r8,r4 (delay)
    put_u16(0x20u, 0x7901u); // add #1,r9
    put_u16(0x22u, 0x39A3u); // cmp/ge r10,r9
    put_u16(0x24u, 0x8FF8u); // bf/s 0x8C010018
    put_u16(0x26u, 0x7804u); // add #4,r8 (delay)
    put_u16(0x28u, 0x4D2Au); // lds r13,pr
    put_u16(0x2Au, 0x000Bu); // rts
    put_u16(0x2Cu, 0x0009u); // nop (delay)

    put_u32(0x30u, katana::platform::dreamcast_disc_boot_address + 0x50u);
    put_u32(0x34u, katana::platform::dreamcast_disc_boot_address + 0xA0u);
    put_u32(0x38u, 0x8C020000u);
    put_u16(0x50u, 0xD002u); // mov.l @(8,pc),r0 -> table 0x8C010080
    put_u16(0x52u, 0x000Bu); // rts
    put_u16(0x54u, 0x0009u); // nop (delay)
    put_u32(0x5Cu, katana::platform::dreamcast_disc_boot_address + 0x80u);
    put_u32(0x80u, katana::platform::dreamcast_disc_boot_address + 0x90u);
    put_u32(0x84u, 1u); // bounded returned-table sentinel

    put_u16(0x90u, 0x4609u); // shlr2 r6
    put_u16(0x92u, 0x6056u); // mov.l @r5+,r0
    put_u16(0x94u, 0x4610u); // dt r6
    put_u16(0x96u, 0x2402u); // mov.l r0,@r4
    put_u16(0x98u, 0x8FFAu); // bf/s 0x8C010090
    put_u16(0x9Au, 0x7404u); // add #4,r4 (delay)
    put_u16(0x9Cu, 0x000Bu); // rts
    put_u16(0x9Eu, 0x0009u); // nop (delay)
    put_u32(0xA0u, 0x44332211u);

    std::copy(program.begin(),
              program.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(21u)));
    return bytes;
}

enum class MemoryFillFixtureVariant : std::uint8_t {
    Valid,
    SignedCompare,
    StoreWord,
    StepTwo,
    BranchTrue,
    CompareOperandsSwapped,
    LimitPointerAliasesCursor,
    ExtraBodyPredecessor,
};

std::vector<std::uint8_t>
memory_fill_loop_boot_track(const MemoryFillFixtureVariant variant =
                                MemoryFillFixtureVariant::Valid) {
    auto bytes = boot_track();
    constexpr std::uint32_t boot_size = 0x2Cu;
    auto directory = payload_offset(20u);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\0'), true);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\1'), true);
    static_cast<void>(record(bytes, directory, data_lba + 21u, boot_size, "BOOT.BIN;1", false));

    std::vector<std::uint8_t> program(boot_size, 0x09u);
    for (std::size_t offset = 1u; offset < program.size(); offset += 2u)
        program[offset] = 0x00u;
    program[0x00u] = 0x00u;
    program[0x01u] = 0xE4u; // mov #0,r4
    program[0x02u] = 0x07u;
    program[0x03u] = 0xD5u; // mov.l @(7,pc),r5 -> cursor at 0x8C010020
    program[0x04u] = 0x07u;
    program[0x05u] = 0xD6u; // mov.l @(7,pc),r6 -> limit pointer at 0x8C010024
    program[0x06u] =
        variant == MemoryFillFixtureVariant::ExtraBodyPredecessor ? 0x00u : 0x02u;
    program[0x07u] = 0xA0u; // bra guard; negative CFG fixture branches directly to body
    program[0x08u] = 0x09u;
    program[0x09u] = 0x00u; // delay-slot nop
    program[0x0Au] =
        variant == MemoryFillFixtureVariant::StoreWord ? 0x41u : 0x40u;
    program[0x0Bu] = 0x25u; // mov.b/mov.w r4,@r5
    program[0x0Cu] =
        variant == MemoryFillFixtureVariant::StepTwo ? 0x02u : 0x01u;
    program[0x0Du] = 0x75u; // add #1/#2,r5
    program[0x0Eu] =
        variant == MemoryFillFixtureVariant::LimitPointerAliasesCursor ? 0x52u : 0x62u;
    program[0x0Fu] = 0x63u; // mov.l @r6,r3; alias fixture uses @r5
    program[0x10u] = variant == MemoryFillFixtureVariant::CompareOperandsSwapped
                         ? 0x52u
                         : (variant == MemoryFillFixtureVariant::SignedCompare ? 0x33u : 0x32u);
    program[0x11u] =
        variant == MemoryFillFixtureVariant::CompareOperandsSwapped ? 0x33u : 0x35u;
    // cmp/hs r3,r5; negative fixtures use cmp/ge or cmp/hs r5,r3
    program[0x12u] = 0xFAu;
    program[0x13u] =
        variant == MemoryFillFixtureVariant::BranchTrue ? 0x89u : 0x8Bu;
    // bf/bt 0x8C01000A
    program[0x14u] = 0x0Bu;
    program[0x15u] = 0x00u; // rts
    program[0x16u] = 0x09u;
    program[0x17u] = 0x00u; // delay-slot nop
    constexpr std::array<std::uint32_t, 3u> literals{
        0x8C020000u, 0x8C010028u, 0x8C020010u};
    for (std::size_t literal = 0u; literal < literals.size(); ++literal) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            program[0x20u + literal * 4u + byte] =
                static_cast<std::uint8_t>(literals[literal] >> (byte * 8u));
    }
    std::copy(program.begin(),
              program.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(21u)));
    return bytes;
}

std::vector<std::uint8_t> latent_module_boot_track() {
    auto bytes = boot_track();
    bytes.resize(23u * raw_sector_size);
    bytes[22u * raw_sector_size + 15u] = 1u;
    auto directory = payload_offset(20u);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\0'), true);
    directory +=
        record(bytes, directory, data_lba + 20u, payload_size, std::string(1u, '\1'), true);
    directory += record(bytes, directory, data_lba + 21u, 24u, "BOOT.BIN;1", false);
    static_cast<void>(
        record(bytes, directory, data_lba + 22u, 4u, "ENGINE.BIN;1", false));
    constexpr std::array<std::uint8_t, 4u> module{
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u  // delay-slot nop
    };
    std::copy(module.begin(),
              module.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset(22u)));
    return bytes;
}

void write_binary(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void write_fixture(const std::filesystem::path& directory,
                   const FixtureProgram fixture_program = FixtureProgram::Normal) {
    std::vector<std::uint8_t> low_track(24u * raw_sector_size);
    for (std::size_t sector = 0u; sector < 24u; ++sector)
        low_track[sector * raw_sector_size + 15u] = 1u;
    write_binary(directory / "low.bin", low_track);
    write_binary(directory / "audio.raw", std::vector<std::uint8_t>(raw_sector_size));
    write_binary(directory / "high.bin", boot_track(fixture_program));
    std::ofstream descriptor(directory / "disc.gdi", std::ios::trunc);
    descriptor << "3\n"
               << "1 0 4 2352 low.bin 0\n"
               << "2 30 0 2352 audio.raw 0\n"
               << "3 " << data_lba << " 4 2352 high.bin 0\n";
}

void write_stored_unknown_candidate_fixture(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    std::vector<std::uint8_t> low_track(24u * raw_sector_size);
    for (std::size_t sector = 0u; sector < 24u; ++sector)
        low_track[sector * raw_sector_size + 15u] = 1u;
    write_binary(directory / "low.bin", low_track);
    write_binary(directory / "audio.raw", std::vector<std::uint8_t>(raw_sector_size));
    write_binary(directory / "high.bin", stored_unknown_candidate_boot_track());
    std::ofstream descriptor(directory / "disc.gdi", std::ios::trunc);
    descriptor << "3\n"
               << "1 0 4 2352 low.bin 0\n"
               << "2 30 0 2352 audio.raw 0\n"
               << "3 " << data_lba << " 4 2352 high.bin 0\n";
}

std::map<std::string, std::string> snapshot(const std::filesystem::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream input(entry.path(), std::ios::binary);
        std::ostringstream content;
        content << input.rdbuf();
        result.emplace(entry.path().lexically_relative(root).generic_string(), content.str());
    }
    return result;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string hex_symbol(const std::uint32_t address) {
    std::ostringstream output;
    output << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << address;
    return output.str();
}

std::size_t occurrences(const std::string_view text, const std::string_view needle) {
    std::size_t count = 0u;
    for (auto offset = text.find(needle); offset != std::string_view::npos;
         offset = text.find(needle, offset + needle.size()))
        ++count;
    return count;
}

void disabled_product_materializer_regression() {
    using namespace katana::runtime;
    CpuState cpu;
    ExecutableModuleCatalog modules;
    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    BlockMaterializationPolicy policy;
    policy.enabled = false;
    DemandBlockMaterializer materializer(modules, blocks, &tracker, policy, {});
    IndirectDispatchMetrics metrics;
    IndirectDispatchRequest request;
    request.kind = IndirectDispatchKind::TailJump;
    request.callsite = 0x1000u;
    request.target = 0x2000u;
    request.source = {request.callsite, canonical_physical_address(request.callsite)};
    request.resolution_origin = DispatchResolutionOrigin::RuntimeOnly;
    request.dispatch_class = RuntimeDispatchClass::RuntimeOnly;
    DispatchDiagnosticRecorder diagnostics;
    request.diagnostics = &diagnostics;
    request.metrics = &metrics;
    request.materializer = &materializer;
    bool typed_abort = false;
    try {
        static_cast<void>(dispatch_indirect(cpu, blocks, request));
    } catch (const IndirectDispatchError& error) {
        typed_abort = error.metrics_json().find("\"error\":\"unknown-target\"") !=
                      std::string::npos;
    }
    const auto profile = metrics.runtime_only_sites().find(request.callsite);
    require(typed_abort && request.callsite != request.target &&
                materializer.last_failure() == MaterializationFailure::Disabled &&
                materializer.metrics().requests == 1u && materializer.metrics().misses == 1u &&
                materializer.metrics().first_failure == MaterializationFailure::Disabled &&
                materializer.metrics().first_failure_target == request.target &&
                metrics.misses() == 1u && metrics.runtime_only_misses() == 1u &&
                metrics.runtime_only_site_count() == 1u &&
                profile != metrics.runtime_only_sites().end() && profile->second.calls == 1u &&
                profile->second.misses == 1u && profile->second.targets.size() == 1u &&
                profile->second.targets.front() == request.target &&
                !metrics.runtime_only_sites().contains(request.target) &&
                diagnostics.events().size() == 1u &&
                diagnostics.events().front().callsite == request.callsite &&
                diagnostics.events().front().source_virtual == request.callsite &&
                diagnostics.events().front().virtual_target == request.target &&
                diagnostics.events().front().origin == DispatchResolutionOrigin::RuntimeOnly &&
                blocks.size() == 0u,
            "Produktmaterializer setzt ungebundenen Code nicht typisiert und ohne Ausfuehrung ab.");
}

} // namespace

int run_test(const int argc, char* argv[]) {
    if (argc == 3 && (std::string(argv[1]) == "--write-fixture" ||
                      std::string(argv[1]) == "--write-trap-fixture" ||
                      std::string(argv[1]) == "--write-unknown-target-fixture" ||
                      std::string(argv[1]) == "--write-counted-loop-fixture" ||
                      std::string(argv[1]) ==
                          "--write-counted-loop-on-chip-fixture" ||
                      std::string(argv[1]) == "--write-memory-fill-fixture" ||
                      std::string(argv[1]) == "--write-composite-callback-fixture")) {
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        const auto program = std::string(argv[1]) == "--write-trap-fixture"
                                 ? FixtureProgram::ImmediateTrap
                             : std::string(argv[1]) == "--write-unknown-target-fixture"
                                 ? FixtureProgram::UnknownDynamicTarget
                                 : FixtureProgram::Normal;
        write_fixture(directory, program);
        if (std::string(argv[1]) == "--write-counted-loop-fixture" ||
            std::string(argv[1]) == "--write-counted-loop-on-chip-fixture") {
            const bool on_chip_counter =
                std::string(argv[1]) == "--write-counted-loop-on-chip-fixture";
            const auto track = counted_loop_boot_track(true, false, on_chip_counter);
            write_binary(directory / "high.bin", track);
            const auto program_begin =
                track.begin() + static_cast<std::ptrdiff_t>(payload_offset(21u));
            write_binary(directory / "program.bin",
                         std::vector<std::uint8_t>(
                             program_begin,
                             program_begin + (on_chip_counter ? 0x30u : 0x28u)));
        }
        if (std::string(argv[1]) == "--write-memory-fill-fixture") {
            const auto track = memory_fill_loop_boot_track();
            write_binary(directory / "high.bin", track);
            const auto program_begin =
                track.begin() + static_cast<std::ptrdiff_t>(payload_offset(21u));
            write_binary(directory / "program.bin",
                         std::vector<std::uint8_t>(program_begin, program_begin + 0x2C));
        }
        if (std::string(argv[1]) == "--write-composite-callback-fixture") {
            const auto track = composite_callback_boot_track();
            write_binary(directory / "high.bin", track);
            const auto program_begin =
                track.begin() + static_cast<std::ptrdiff_t>(payload_offset(21u));
            write_binary(directory / "program.bin",
                         std::vector<std::uint8_t>(program_begin, program_begin + 0xA4));
        }
        return EXIT_SUCCESS;
    }
    require(argc == 1, "Unerwartete Argumente fuer den Portexporttest.");
    disabled_product_materializer_regression();
    using namespace katana::codegen;
    Fixture fixture;
    const auto previous_port = fixture.root / "previous-port";
    const auto published_port = fixture.root / "published-port";
    std::filesystem::create_directories(previous_port / "user-data" / "content");
    std::filesystem::create_directories(published_port / "user-data" / "content");
    {
        std::ofstream local_pack(previous_port / "user-data" / "content" /
                                 "game.katana-disc",
                                 std::ios::binary);
        local_pack << "local-retail-cache";
    }
    preserve_local_port_user_data(previous_port, published_port);
    require(!std::filesystem::exists(previous_port / "user-data") &&
                read_text(published_port / "user-data" / "content" /
                          "game.katana-disc") == "local-retail-cache",
            "Atomarer Portaustausch verliert oder kopiert lokale Disc-/Speicherdaten.");
    write_fixture(fixture.root / "disc");
    const auto gdi = fixture.root / "disc" / "disc.gdi";
    const auto runtime_boot = katana::runtime::load_dreamcast_runtime_boot(gdi);
    katana::runtime::CpuState runtime_cpu;
    const auto runtime_state =
        katana::runtime::initialize_dreamcast_runtime(runtime_cpu, runtime_boot);
    require(runtime_state.loaded_boot_bytes == 24u && runtime_cpu.pc == 0x8C010000u &&
                runtime_cpu.r[15] == 0x8D000000u &&
                runtime_cpu.vbr == katana::runtime::dreamcast_direct_boot_vector_base &&
                runtime_cpu.read_sr() == katana::runtime::dreamcast_disc_boot_status &&
                runtime_cpu.read_fpscr() == katana::runtime::dreamcast_disc_boot_fpscr &&
                runtime_cpu.gbr == katana::runtime::dreamcast_bios_handoff_gbr &&
                runtime_cpu.ssr == katana::runtime::dreamcast_bios_handoff_ssr &&
                runtime_cpu.spc == katana::runtime::dreamcast_bios_handoff_spc &&
                runtime_cpu.sgr == katana::runtime::dreamcast_direct_boot_stack &&
                runtime_cpu.dbr == katana::runtime::dreamcast_bios_handoff_dbr &&
                runtime_cpu.pr == katana::runtime::dreamcast_bios_handoff_pr && runtime_cpu.t &&
                runtime_cpu.privileged_mode() && runtime_cpu.interrupt_mask() == 15u &&
                runtime_cpu.memory.read_u16(0x8C010000u) == 0xA000u &&
                runtime_state.runtime_blocks && runtime_state.runtime_blocks->size() == 0u &&
                runtime_state.system_asic && runtime_state.interrupt_router &&
                runtime_state.cache_control && runtime_state.io_ports &&
                runtime_state.dmac->operation() ==
                    katana::runtime::dreamcast_bios_handoff_dmaor &&
                runtime_state.holly_dma.g1->read(0x04u) == 0u &&
                runtime_state.holly_dma.g1->read(0x08u) == 0u &&
                runtime_state.holly_dma.g1->read(0xF4u) == 0x0C010800u &&
                runtime_state.holly_dma.g1->read(0xF8u) == 0u &&
                runtime_state.aica_registers->read(0x289Cu,
                                                   katana::runtime::MemoryAccessWidth::Halfword) ==
                    0x48u &&
                runtime_state.aica_registers->read(0x28A8u,
                                                   katana::runtime::MemoryAccessWidth::Byte) ==
                    0x18u &&
                runtime_state.io_ports->control_a() ==
                    katana::runtime::dreamcast_bios_handoff_pctra &&
                runtime_cpu.memory.read_u16(katana::runtime::sh4_port_data_a_address) ==
                    katana::runtime::dreamcast_composite_port_a_input &&
                runtime_state.pvr_registers->read(katana::runtime::pvr_register::SpgLoad) ==
                    0x020C0359u &&
                runtime_cpu.memory.read_u32(katana::runtime::sh4_cache_control_address) == 0u,
            "Eigenstaendiger GDI-Boot initialisiert Bootimage, privilegierten CPU-Handoff oder "
            "Speicher nicht.");
    auto pal_runtime_boot = runtime_boot;
    pal_runtime_boot.area_symbols = "E";
    auto pal_runtime_cpu_storage = std::make_unique<katana::runtime::CpuState>();
    auto& pal_runtime_cpu = *pal_runtime_cpu_storage;
    const auto pal_runtime_state =
        katana::runtime::initialize_dreamcast_runtime(
            pal_runtime_cpu,
            pal_runtime_boot,
            katana::runtime::DreamcastRuntimeFirmwareMode::Direct,
            {},
            katana::runtime::DreamcastConsoleProfile::EuropePal);
    require(pal_runtime_state.io_ports->data_a() ==
                katana::runtime::dreamcast_composite_port_a_input,
            "PAL-BIOS-Handoff reicht das Latch im alternativen Pinmodus als GPIO-Ausgang durch.");
    pal_runtime_state.io_ports->write_control_a(0x00000010u);
    require((pal_runtime_state.io_ports->data_a() &
             katana::runtime::dreamcast_bios_handoff_pal_pdtra) != 0u,
            "PAL-BIOS-Handoff setzt den Broadcast-Portzustand nicht.");
    runtime_cpu.memory.write_u32(katana::runtime::sh4_cache_control_address,
                                 katana::runtime::Sh4CacheControl::instruction_invalidate);
    require(runtime_state.cache_control->instruction_invalidation_count() == 1u &&
                runtime_cpu.memory.read_u32(katana::runtime::sh4_cache_control_address) == 0u,
            "Produktiver GDI-Boot bindet das SH-4-Cache-Control-Register nicht ein.");
    static_cast<void>(runtime_state.code_tracker->register_block(
        {"sq-code",
         0x0C000000u,
         32u,
         "synthetic",
         {},
         katana::runtime::ExecutableBlockOrigin::ImageSegment}));
    runtime_cpu.memory.write_u32(0xFF000038u, 0x0Cu);
    for (std::uint32_t offset = 0u; offset < 32u; offset += 4u)
        runtime_cpu.memory.write_u32(0xE0000000u + offset, 0x03020100u + offset * 0x01010101u);
    require(runtime_state.store_queues->prefetch(0xE0000000u) &&
                runtime_cpu.memory.read_u32(0x0C000000u) == 0x03020100u &&
                runtime_state.code_tracker->invalidation_count() == 1u,
            "Produktive Store Queue uebertraegt keine 32 Byte nach RAM oder invalidiert Code.");
    runtime_cpu.memory.write_u32(0xFF00003Cu, 0x10u);
    runtime_cpu.memory.write_u32(0xE2000020u, 0x80000000u);
    const auto ta_position_before_store_queue = runtime_state.pvr_registers->read(
        katana::runtime::pvr_register::TaIspCurrent);
    require(runtime_state.store_queues->prefetch(0xE2000020u) &&
                runtime_state.store_queue_transfers->back().target ==
                    katana::runtime::StoreQueueTarget::TileAccelerator &&
                runtime_state.store_queue_transfers->back().bytes[0] == 0u &&
                runtime_state.pvr_registers->read(
                    katana::runtime::pvr_register::TaIspCurrent) ==
                    ta_position_before_store_queue + 32u,
            "Produktive Store Queue verliert QACR-basierten TA-Transfer oder TA-Zeiger.");

    const auto gdrom_events_before_packet = std::count_if(
        runtime_state.system_asic->events().begin(),
        runtime_state.system_asic->events().end(),
        [](const auto& event) {
            return event.event == katana::runtime::SystemAsicEvent::GdromCommand;
        });
    runtime_state.gdrom->write(0x9Cu, 0xA0u, katana::runtime::MemoryAccessWidth::Byte);
    for (std::size_t word = 0u; word < 6u; ++word)
        runtime_state.gdrom->write(0x80u, 0u, katana::runtime::MemoryAccessWidth::Halfword);
    const auto packet_completion = runtime_state.scheduler->advance_by(1'000u, 1u);
    const auto gdrom_events_after_completion = std::count_if(
        runtime_state.system_asic->events().begin(),
        runtime_state.system_asic->events().end(),
        [](const auto& event) {
            return event.event == katana::runtime::SystemAsicEvent::GdromCommand;
        });
    require(packet_completion.processed_events == 1u &&
                gdrom_events_after_completion == gdrom_events_before_packet + 1u,
            "GD-ROM-Completion setzt den Command-IRQ nicht atomar im Completion-Ereignis.");
    static_cast<void>(runtime_state.gdrom->read(
        0x9Cu, katana::runtime::MemoryAccessWidth::Byte));
    const auto post_ack = runtime_state.scheduler->advance_to(
        runtime_state.scheduler->current_cycle(), 1u);
    const auto gdrom_events_after_ack = std::count_if(
        runtime_state.system_asic->events().begin(),
        runtime_state.system_asic->events().end(),
        [](const auto& event) {
            return event.event == katana::runtime::SystemAsicEvent::GdromCommand;
        });
    require(post_ack.processed_events == 0u &&
                gdrom_events_after_ack == gdrom_events_after_completion,
            "STATUS-Quittierung laesst einen verspaeteten Same-Cycle-GD-ROM-IRQ zurueck.");
    const auto ta_packets_before_channel2 = runtime_state.pvr_ta_fifo->metrics().packets;
    const auto pvr_dma_events_before_channel2 = std::count_if(
        runtime_state.system_asic->events().begin(),
        runtime_state.system_asic->events().end(),
        [](const auto& event) {
            return event.event == katana::runtime::SystemAsicEvent::Channel2Dma;
        });
    for (std::uint32_t offset = 0u; offset < 32u; offset += 4u)
        runtime_cpu.memory.write_u32(0x8C000800u + offset, 0u);
    runtime_state.dmac->write_source(2u, 0x8C000800u);
    runtime_state.dmac->write_count(2u, 1u);
    runtime_state.dmac->write_control(2u, 0x000012C1u);
    runtime_state.dmac->write_operation(0x00008201u);
    runtime_state.system_bus_control->write(
        katana::runtime::system_bus_register::Channel2Destination, 0x10000000u);
    runtime_state.system_bus_control->write(
        katana::runtime::system_bus_register::Channel2Length, 32u);
    runtime_state.system_bus_control->write(
        katana::runtime::system_bus_register::Channel2Start, 1u);
    static_cast<void>(runtime_state.scheduler->advance_by(32u, 1u));
    const auto pvr_dma_events_after_channel2 = std::count_if(
        runtime_state.system_asic->events().begin(),
        runtime_state.system_asic->events().end(),
        [](const auto& event) {
            return event.event == katana::runtime::SystemAsicEvent::Channel2Dma;
        });
    require(runtime_state.pvr_ta_fifo->metrics().packets ==
                    ta_packets_before_channel2 + 1u &&
                runtime_state.dmac->completed_transfer_units(2u) == 1u &&
                (runtime_state.dmac->control(2u) & katana::runtime::Sh4Dmac::transfer_end) != 0u &&
                runtime_state.system_bus_control->read(
                    katana::runtime::system_bus_register::Channel2Start) == 0u &&
                runtime_state.system_bus_control->read(
                    katana::runtime::system_bus_register::Channel2Length) == 0u &&
                pvr_dma_events_after_channel2 == pvr_dma_events_before_channel2 + 1u,
            "Produktiver Channel-2-DMAC erreicht TA-FIFO, Abschlussstatus oder System-ASIC nicht.");
    katana::runtime::CpuState hle_runtime_cpu;
    const auto hle_runtime_state = katana::runtime::initialize_dreamcast_runtime(
        hle_runtime_cpu, runtime_boot, katana::runtime::DreamcastRuntimeFirmwareMode::HleBiosAbi);
    require(hle_runtime_cpu.memory.read_u32(0x8C0000B0u) ==
                    katana::runtime::hle_bios_abi_vectors()[0].handler_address &&
                hle_runtime_cpu.pc ==
                    katana::runtime::dreamcast_system_bootstrap_entry_address &&
                hle_runtime_state.loaded_system_bootstrap_bytes ==
                    katana::runtime::dreamcast_system_bootstrap_size &&
                hle_runtime_cpu.memory.read_u16(
                    katana::runtime::dreamcast_system_bootstrap_entry_address) == 0xD001u &&
                hle_runtime_state.runtime_blocks->size() == 7u &&
                hle_runtime_cpu.memory.read_u32(0x8C002400u) == 0xFFFFFFFFu &&
                hle_runtime_state.runtime_blocks
                    ->lookup(katana::runtime::hle_bios_gdrom2_direct_alias_address, {})
                    .has_value(),
            "Produktiver GDI-HLE-Runtimepfad installiert BIOS-ABI oder Disc-Bootstrap nicht.");
    hle_runtime_cpu.memory.write_u32(0x8C002400u, 0xC001D00Du);
    hle_runtime_cpu.memory.write_u32(
        katana::runtime::dreamcast_system_bootstrap_entry_address, 0xDEADBEEFu);
    hle_runtime_cpu.memory.write_u32(
        katana::runtime::dreamcast_disc_boot_address, 0xDEADBEEFu);
    hle_runtime_state.dmac->write_operation(katana::runtime::Sh4Dmac::master_enable);
    hle_runtime_state.aica_registers->write(
        0x289Cu, 0u, katana::runtime::MemoryAccessWidth::Halfword);
    hle_runtime_state.cache_control->write(
        katana::runtime::Sh4CacheControl::operand_ram_enable);
    hle_runtime_cpu.memory.write_u32(katana::runtime::sh4_on_chip_ram_address, 0x12345678u);
    require(hle_runtime_state.code_tracker->page_generation(
                katana::runtime::sh4_on_chip_ram_address) == 0u,
            "Reine OCRAM-Stackdaten erzeugen unnoetige Codeinvalidierungsprovenienz.");
    hle_runtime_state.io_ports->write_control_a(0x10u);
    hle_runtime_state.io_ports->write_data_a(0u);
    const auto system_vector = katana::runtime::hle_bios_abi_vectors()[5];
    const auto system_handle = hle_runtime_state.runtime_blocks->lookup(
        system_vector.handler_address, {});
    const auto system_block = system_handle
                                  ? hle_runtime_state.runtime_blocks->resolve(*system_handle)
                                  : std::nullopt;
    require(system_block.has_value(), "SYSTEM-1-Runtimeblock fehlt im produktiven HLE-Pfad.");
    hle_runtime_cpu.pc = system_vector.handler_address;
    hle_runtime_cpu.r[4] = 1u;
    katana::runtime::BlockExecutionContext lifecycle_context;
    lifecycle_context.scheduler_cycle = 77u;
    try {
        static_cast<void>(system_block->get().function(hle_runtime_cpu, lifecycle_context));
        require(false, "SYSTEM 1 kehrt im produktiven HLE-Pfad zurueck.");
    } catch (const katana::runtime::PlatformLifecycleExit& exit) {
        require(exit.reason() == katana::runtime::PlatformLifecycleExitReason::BiosMenu &&
                    exit.evidence().guest_cycle == 77u &&
                    hle_runtime_cpu.pc == system_vector.handler_address &&
                    hle_runtime_cpu.memory.read_u32(
                        katana::runtime::dreamcast_system_bootstrap_entry_address) ==
                        0xDEADBEEFu &&
                    hle_runtime_cpu.memory.read_u32(
                        katana::runtime::dreamcast_disc_boot_address) == 0xDEADBEEFu &&
                    hle_runtime_cpu.memory.read_u32(0x8C002400u) == 0xC001D00Du &&
                    hle_runtime_state.dmac->operation() ==
                        katana::runtime::Sh4Dmac::master_enable &&
                    hle_runtime_state.aica_registers->read(
                        0x289Cu, katana::runtime::MemoryAccessWidth::Halfword) == 0u &&
                    hle_runtime_cpu.memory.read_u32(
                        katana::runtime::sh4_cache_control_address) ==
                        katana::runtime::Sh4CacheControl::operand_ram_enable &&
                    hle_runtime_state.io_ports->control_a() == 0x10u,
                "SYSTEM 1 mutiert Bootbytes oder Geraetezustand statt als BIOS-Menue zu enden.");
    }
    const auto render_done_count = [&] {
        return std::count_if(hle_runtime_state.system_asic->events().begin(),
                             hle_runtime_state.system_asic->events().end(),
                             [](const auto& event) {
                                 return event.event ==
                                        katana::runtime::SystemAsicEvent::PvrRenderDone;
                             });
    };
    hle_runtime_state.pvr_registers->write(
        katana::runtime::pvr_register::FramebufferWriteControl, 7u);
    const auto render_done_before_failure = render_done_count();
    const auto render_completions_before_failure =
        hle_runtime_state.pvr_registers->render_completion_count();
    const auto render_failures_before_failure =
        hle_runtime_state.pvr_registers->render_failure_count();
    hle_runtime_state.pvr_registers->write(katana::runtime::pvr_register::StartRender, 1u);
    std::optional<katana::runtime::PvrRenderFailure> typed_render_failure;
    try {
        static_cast<void>(hle_runtime_state.scheduler->advance_by(2'000u, 64u));
    } catch (const katana::runtime::PvrRenderFailed& error) {
        typed_render_failure = error.failure();
    }
    require(hle_runtime_state.pvr_registers->render_completion_count() ==
                    render_completions_before_failure &&
                hle_runtime_state.pvr_registers->render_failure_count() ==
                    render_failures_before_failure + 1u &&
                render_done_count() == render_done_before_failure &&
                hle_runtime_state.pvr_renderer->first_error().has_value() &&
                typed_render_failure &&
                typed_render_failure->request != 0u &&
                typed_render_failure->generation != 0u &&
                typed_render_failure->ta_packet_class == "none" &&
                typed_render_failure->register_digest != 0u &&
                typed_render_failure->guest_cycle != 0u &&
                hle_runtime_state.pvr_registers->last_render_failure() ==
                    typed_render_failure,
            "Fehlgeschlagener PVR-Renderpfad signalisiert RenderDone oder bleibt untypisiert.");
    hle_runtime_state.pvr_registers->write(
        katana::runtime::pvr_register::FramebufferWriteControl, 0u);
    constexpr std::uint32_t render_background = 0x00100000u;
    hle_runtime_state.pvr_registers->write(
        katana::runtime::pvr_register::ParameterBase, render_background);
    hle_runtime_state.pvr_registers->write(
        katana::runtime::pvr_register::BackgroundPlaneConfig, 1u << 24u);
    hle_runtime_state.vram->write_u32(render_background, 0u);
    hle_runtime_state.vram->write_u32(
        render_background + 4u, (1u << 29u) | (2u << 22u) | (1u << 20u));
    hle_runtime_state.vram->write_u32(render_background + 8u, 0u);
    for (const auto vertex : {render_background + 12u,
                              render_background + 28u,
                              render_background + 44u}) {
        hle_runtime_state.vram->write_u32(vertex, 0u);
        hle_runtime_state.vram->write_u32(vertex + 4u, 0u);
        hle_runtime_state.vram->write_u32(vertex + 8u, 0u);
        hle_runtime_state.vram->write_u32(vertex + 12u, 0xFF204060u);
    }
    const auto render_completions_before_success =
        hle_runtime_state.pvr_registers->render_completion_count();
    hle_runtime_state.pvr_registers->write(katana::runtime::pvr_register::StartRender, 1u);
    static_cast<void>(hle_runtime_state.scheduler->advance_by(2'000u, 64u));
    require(hle_runtime_state.pvr_registers->render_completion_count() ==
                    render_completions_before_success + 1u &&
                render_done_count() == render_done_before_failure + 1u,
            "Gueltiger PVR-Renderabschluss erreicht RenderDone nicht deterministisch.");
    auto input = std::make_shared<katana::runtime::ReplayInputBackend>(
        std::vector<katana::runtime::ControllerState>{{}});
    hle_runtime_state.maple->attach(
        0u, 0u, std::make_shared<katana::runtime::MapleControllerDevice>(input));
    static_cast<void>(hle_runtime_state.maple->exchange(
        0u, 0u, {katana::runtime::MapleCommand::GetCondition, {}}));
    hle_runtime_cpu.memory.write_u32(0x8C000400u, 150u);
    hle_runtime_cpu.memory.write_u32(0x8C000404u, 1u);
    hle_runtime_cpu.memory.write_u32(0x8C000408u, 0x8C001000u);
    hle_runtime_cpu.r[4] = 16u;
    hle_runtime_cpu.r[5] = 0x8C000400u;
    static_cast<void>(hle_runtime_state.gdrom->bios_call(hle_runtime_cpu, 0u, 0u));
    static_cast<void>(hle_runtime_state.gdrom->bios_call(hle_runtime_cpu, 2u, 0u));
    static_cast<void>(hle_runtime_state.scheduler->advance_by(2'000u, 8u));
    hle_runtime_state.aica->interrupts().set_enabled(1u);
    hle_runtime_state.aica->interrupts().request(1u);
    const auto has_asic_event = [&](const katana::runtime::SystemAsicEvent expected) {
        return std::any_of(hle_runtime_state.system_asic->events().begin(),
                           hle_runtime_state.system_asic->events().end(),
                           [&](const auto& event) { return event.event == expected; });
    };
    require(has_asic_event(katana::runtime::SystemAsicEvent::PvrRenderDone),
            "Produktives PVR-RenderDone erreicht das System-ASIC nicht.");
    require(has_asic_event(katana::runtime::SystemAsicEvent::MapleDma),
            "Produktives Maple-DMA erreicht das System-ASIC nicht.");
    require(has_asic_event(katana::runtime::SystemAsicEvent::GdromCommand),
            "Produktive GD-ROM-Completion erreicht das System-ASIC nicht.");
    require(has_asic_event(katana::runtime::SystemAsicEvent::AicaInterrupt),
            "Produktiver AICA-Interrupt erreicht das System-ASIC nicht.");
    const auto output = fixture.root / "port";
    const PortExportOptions options{"synthetic_game",
                                    "0.37.0-dev",
                                    {1u, 4096u},
                                    {},
                                    false,
                                    "japan-ntsc",
                                    observe_progress};

    const auto stored_unknown_disc_root = fixture.root / "stored-unknown-disc";
    write_stored_unknown_candidate_fixture(stored_unknown_disc_root);
    const auto stored_unknown_gdi = stored_unknown_disc_root / "disc.gdi";
    const auto stored_unknown_disc =
        katana::platform::load_dreamcast_gdi_boot(stored_unknown_gdi);
    auto stored_unknown_image = katana::platform::make_dreamcast_disc_executable(
        stored_unknown_disc,
        katana::platform::DreamcastDiscExecutionPath::NativeSystemBootstrap);
    const auto stored_unknown_analysis =
        katana::analysis::analyze_control_flow(stored_unknown_image);
    constexpr auto stored_unknown_candidate =
        katana::platform::dreamcast_disc_boot_address + 0x30u;
    const auto stored_unknown_function =
        std::find_if(stored_unknown_analysis.recursive.functions.begin(),
                     stored_unknown_analysis.recursive.functions.end(),
                     [](const auto& function) {
                         return function.address == stored_unknown_candidate;
                     });
    const auto stored_unknown_diagnostic =
        std::find_if(stored_unknown_analysis.recursive.diagnostics.begin(),
                     stored_unknown_analysis.recursive.diagnostics.end(),
                     [](const auto& diagnostic) {
                         return diagnostic.address == stored_unknown_candidate &&
                                diagnostic.reason == "unknown-opcode";
                     });
    require(
        stored_unknown_function != stored_unknown_analysis.recursive.functions.end() &&
            std::find(stored_unknown_function->origins.begin(),
                      stored_unknown_function->origins.end(),
                      katana::analysis::FunctionOrigin::StoredCodeAddress) !=
                stored_unknown_function->origins.end() &&
            stored_unknown_function->evidence ==
                katana::analysis::ControlFlowEvidence::GuardedPartial &&
            std::binary_search(
                stored_unknown_analysis.recursive.guarded_candidate_instruction_addresses.begin(),
                stored_unknown_analysis.recursive.guarded_candidate_instruction_addresses.end(),
                stored_unknown_candidate) &&
            stored_unknown_diagnostic != stored_unknown_analysis.recursive.diagnostics.end() &&
            stored_unknown_diagnostic->evidence ==
                katana::analysis::ControlFlowEvidence::GuardedPartial &&
            !katana::analysis::analysis_diagnostic_blocks_codegen(*stored_unknown_diagnostic),
        "Gespeicherter BOOT.BIN-Codekandidat verlor Guarded-Partial-Diagnoseevidenz.");
    const auto stored_unknown_program = katana::ir::lower_program(stored_unknown_analysis);
    require(std::none_of(
                stored_unknown_program.begin(),
                stored_unknown_program.end(),
                [](const auto& function) {
                    return function.entry_address == stored_unknown_candidate ||
                           std::any_of(
                               function.blocks.begin(),
                               function.blocks.end(),
                               [](const auto& block) {
                                   return std::any_of(
                                       block.instructions.begin(),
                                       block.instructions.end(),
                                       [](const auto& instruction) {
                                           return instruction.source_address ==
                                                  stored_unknown_candidate;
                                       });
                               });
                }),
            "Guarded-Partial-Unknown wurde als dispatchbares AOT-Inventar abgesenkt.");

    const auto stored_unknown_output = fixture.root / "stored-unknown-port";
    const auto stored_unknown_export =
        export_dreamcast_port_project(stored_unknown_gdi, stored_unknown_output, options);
    const auto stored_unknown_sources = snapshot(stored_unknown_output / "generated");
    std::string stored_unknown_units;
    for (const auto& [path, content] : stored_unknown_sources)
        if (path.starts_with("code/unit-")) stored_unknown_units += content;
    require(stored_unknown_export.functions != 0u,
            "Guarded-Partial-Unknown entfernte das gesamte Portprogramm.");
    require(stored_unknown_units.find("katana-guest 0x8C010030") == std::string::npos,
            "Guarded-Partial-Unknown blieb als dispatchbares AOT-Inventar erhalten.");
    require(stored_unknown_sources.at("metadata/port-project.json")
                    .find("\"diagnostic_partial\":false") != std::string::npos,
            "Guarded-Partial-Unknown markierte den erfolgreichen Port als partiell.");
    require(stored_unknown_sources.at("code/runtime-dispatch.cpp")
                    .find("dynamic_interpreter.hpp") == std::string::npos,
            "Guarded-Partial-Unknown aktivierte einen Interpreterpfad.");

    auto proven_unknown_image = stored_unknown_image;
    proven_unknown_image.add_entry_point(stored_unknown_candidate);
    const auto proven_unknown_analysis =
        katana::analysis::analyze_control_flow(proven_unknown_image);
    const auto proven_unknown_diagnostic =
        std::find_if(proven_unknown_analysis.recursive.diagnostics.begin(),
                     proven_unknown_analysis.recursive.diagnostics.end(),
                     [](const auto& diagnostic) {
                         return diagnostic.address == stored_unknown_candidate &&
                                diagnostic.reason == "unknown-opcode";
                     });
    require(proven_unknown_diagnostic != proven_unknown_analysis.recursive.diagnostics.end() &&
                proven_unknown_diagnostic->evidence ==
                    katana::analysis::ControlFlowEvidence::ProvenComplete &&
                katana::analysis::analysis_diagnostic_blocks_codegen(
                    *proven_unknown_diagnostic),
            "Bewiesener BOOT.BIN-Unknown verlor seinen Codegen-Blocker.");
    const auto proven_unknown_program = katana::ir::lower_program(proven_unknown_analysis);
    const std::vector<katana::io::InputProvenance> proven_unknown_inputs;
    bool proven_unknown_rejected = false;
    try {
        static_cast<void>(export_dreamcast_port_project(
            {proven_unknown_image,
             proven_unknown_analysis,
             proven_unknown_program,
             proven_unknown_inputs,
             katana::platform::dreamcast_system_bootstrap_entry_address,
             katana::platform::dreamcast_disc_boot_address,
             stored_unknown_disc.boot_file.size(),
             "proven-unknown-fixture"},
            fixture.root / "proven-unknown-port",
            options));
    } catch (const std::runtime_error& error) {
        proven_unknown_rejected =
            std::string_view(error.what()).find("unbekannte Instruktionen") !=
            std::string_view::npos;
    }
    require(proven_unknown_rejected,
            "Bewiesener unbekannter Opcode wurde vom Produktport akzeptiert.");

    observed_progress.clear();
    const auto first = export_dreamcast_port_project(gdi, output, options);
    const auto generated_before = snapshot(output / "generated");
    const auto generated_main = read_text(output / "src" / "main.cpp");
    const auto& runtime_dispatch = generated_before.at("code/runtime-dispatch.cpp");
    std::string runtime_dispatch_shards;
    std::size_t runtime_dispatch_shard_count = 0u;
    for (const auto& [path, content] : generated_before) {
        if (!path.starts_with("code/runtime-dispatch-shard-") || !path.ends_with(".cpp"))
            continue;
        runtime_dispatch_shards += content;
        ++runtime_dispatch_shard_count;
    }
    const auto p2_registration_begin = runtime_dispatch_shards.find(
        "register_executable_block(table, services, 0xAC008300u");
    const auto p2_registration_end =
        runtime_dispatch_shards.find('\n', p2_registration_begin);
    const auto p2_registration =
        p2_registration_begin == std::string::npos
            ? std::string_view{}
            : std::string_view{runtime_dispatch_shards}.substr(
                  p2_registration_begin,
                  p2_registration_end == std::string::npos
                      ? std::string_view::npos
                      : p2_registration_end - p2_registration_begin);
    const auto unit =
        std::find_if(generated_before.begin(), generated_before.end(), [](const auto& entry) {
            return entry.first.starts_with("code/unit-") && entry.first.ends_with(".cpp");
        });
    require(first.functions == 3u && first.partitions == 3u && first.checkpoints.size() == 8u &&
                first.checkpoints.back() == "port-project-written",
            "Synthetische GDI durchlaeuft den Portexport nicht vollstaendig.");
    const std::vector<std::string> expected_progress = {"disc-load",
                                                        "boot-image",
                                                        "control-flow-analysis",
                                                        "ir-lowering",
                                                        "ir-optimization",
                                                        "input-provenance",
                                                        "program-validation",
                                                        "partition-codegen",
                                                        "metadata",
                                                        "disc-recipe",
                                                        "artifact-write"};
    auto progress_cursor = observed_progress.cbegin();
    for (const auto& expected : expected_progress) {
        progress_cursor = std::find(progress_cursor, observed_progress.cend(), expected);
        require(progress_cursor != observed_progress.cend(),
                "Portexport verliert die Subphase " + expected + ".");
        ++progress_cursor;
    }
    require(std::any_of(observed_progress.begin(), observed_progress.end(), [](const auto& phase) {
                return phase.starts_with("control-flow-iteration-start-i1-");
            }) &&
                std::any_of(observed_progress.begin(),
                            observed_progress.end(),
                            [](const auto& phase) {
                                return phase.starts_with("control-flow-complete-");
                            }),
            "Portexport verliert budgetierte Kontrollfluss-Fixpunktzaehler.");
    require(std::filesystem::exists(output / "content" / "game.katana-install") &&
                !std::filesystem::exists(output / "content" / "game.katana-disc") &&
                read_text(output / ".gitignore").find("*.katana-disc") != std::string::npos &&
                read_text(output / "INSTALL_ORIGINAL_DISC.txt").find("ORIGINAL DISC REQUIRED") !=
                    std::string::npos &&
                read_text(output / "INSTALL_ORIGINAL_DISC.txt").find("never modified or deleted") !=
                    std::string::npos &&
                first.disc_tracks == 3u,
            "Portexport trennt distributionsfaehige Recipe und lokalen Retailcache nicht.");
    const auto recipe = katana::runtime::parse_disc_install_recipe(first.disc_install_recipe);
    require(recipe.tracks.size() == 3u && recipe.content_identity == first.content_identity &&
                recipe.job_generation == first.job_generation &&
                read_text(first.disc_install_recipe).find("low.bin") == std::string::npos &&
                read_text(first.disc_install_recipe).find(fixture.root.string()) ==
                    std::string::npos,
            "Generische Disc-Recipe verliert Bindung oder enthaelt private Quellpfade.");

    const auto private_boot_root = fixture.root / "private-boot-executable";
    const auto private_boot =
        katana::platform::extract_dreamcast_boot_executable_artifact(
            gdi, private_boot_root);
    const auto reloaded_private_boot =
        katana::platform::load_dreamcast_boot_executable_artifact(
            private_boot.manifest_path);
    const auto direct_boot_output = fixture.root / "direct-boot-port";
    const auto direct_boot_result =
        export_dreamcast_port_project_from_boot_artifact(
            private_boot.manifest_path, direct_boot_output, options);
    const auto direct_boot_main =
        read_text(direct_boot_output / "src" / "main.cpp");
    const auto direct_boot_metadata =
        read_text(
            direct_boot_output / "generated" / "metadata" /
            "port-project.json");
    const auto direct_boot_provenance =
        read_text(
            direct_boot_output / "generated" / "metadata" /
            "provenance.json");
    require(private_boot.boot_file == reloaded_private_boot.boot_file &&
                private_boot.boot_sha256 ==
                    reloaded_private_boot.boot_sha256 &&
                private_boot.install_recipe.content_identity ==
                    reloaded_private_boot.install_recipe.content_identity &&
                direct_boot_result.checkpoints.front() ==
                    "boot-executable-artifact-validated" &&
                direct_boot_main.find(
                    "DreamcastRuntimeBootPath::DirectBootExecutable") !=
                    std::string::npos &&
                direct_boot_main.find(
                    "DreamcastRuntimeFirmwareMode::HleBiosAbi") !=
                    std::string::npos &&
                direct_boot_main.find(
                    "runtime_boot_config.executable_identity") !=
                    std::string::npos &&
                direct_boot_main.find(
                    private_boot.metadata.boot_file_name) !=
                    std::string::npos &&
                direct_boot_metadata.find(
                    "\"boot_path\":\"direct-boot-executable\"") !=
                    std::string::npos &&
                direct_boot_provenance.find(
                    "\"role\":\"boot-executable-private\"") !=
                    std::string::npos &&
                direct_boot_provenance.find(private_boot_root.string()) ==
                    std::string::npos &&
                read_text(direct_boot_result.disc_install_recipe) ==
                    katana::runtime::format_disc_install_recipe(
                        private_boot.install_recipe) &&
                !std::filesystem::exists(
                    direct_boot_output / "boot.bin") &&
                !std::filesystem::exists(
                    direct_boot_output / "content" / "boot.bin"),
            "Privates Boot-Executable wird nicht hashgebunden und retailfrei "
            "als DirectBoot-Port exportiert.");

    const auto latent_disc_directory = fixture.root / "latent-aot-disc";
    std::filesystem::create_directories(latent_disc_directory);
    write_fixture(latent_disc_directory);
    write_binary(latent_disc_directory / "high.bin", latent_module_boot_track());
    const auto latent_gdi_path = latent_disc_directory / "disc.gdi";
    const auto latent_output = fixture.root / "latent-aot-port";
    const auto latent_result =
        export_dreamcast_port_project(latent_gdi_path, latent_output, options);
    const auto latent_generated = snapshot(latent_output / "generated");
    const auto& latent_dispatch = latent_generated.at("code/runtime-dispatch.cpp");
    const auto latent_metadata = latent_generated.at("metadata/port-project.json");
    const auto latent_main = read_text(latent_output / "src" / "main.cpp");
    const auto latent_recipe =
        katana::runtime::parse_disc_install_recipe(latent_result.disc_install_recipe);
    const auto latent_gdi = katana::runtime::GdiDiscSource::open(latent_gdi_path);
    const auto representation_specific_identity =
        katana::runtime::gdi_content_identity(latent_gdi->descriptor());
    require(latent_result.functions == first.functions + 1u &&
                std::find(latent_result.checkpoints.begin(),
                          latent_result.checkpoints.end(),
                          "latent-aot-registry-written") != latent_result.checkpoints.end() &&
                latent_metadata.find("\"latent_aot_modules\":1") != std::string::npos &&
                latent_dispatch.find("register_latent_aot_modules") != std::string::npos &&
                latent_dispatch.find(
                    "NativeAotTemplateDestination::LoadedModule") != std::string::npos &&
                latent_dispatch.find("set_aot_module_descriptors") != std::string::npos &&
                latent_dispatch.find("source->read(") == std::string::npos &&
                latent_dispatch.find("sha256_bytes(") == std::string::npos &&
                latent_dispatch.find("ExecutableModule source_module") == std::string::npos &&
                latent_dispatch.find(
                    "const std::shared_ptr<const katana::runtime::DiscSource>& source") ==
                    std::string::npos &&
                latent_dispatch.find(latent_recipe.content_identity) != std::string::npos &&
                latent_main.find(
                    "packed->info().tracks.size(), packed->info().content_identity)") !=
                    std::string::npos &&
                latent_main.find("auto gdi = katana::runtime::GdiDiscSource::open(source)") !=
                    std::string::npos &&
                latent_main.find(
                    "gdi->descriptor().tracks.size(), "
                    "std::string(expected_content_identity))") != std::string::npos &&
                latent_dispatch.find("ENGINE.BIN") == std::string::npos &&
                latent_dispatch.find(latent_disc_directory.string()) == std::string::npos,
            "Latentes natives Disc-AOT wurde nicht generisch, lokal oder metadatengebunden "
            "exportiert.");
    require(latent_main.find("start_byte_transfer(") != std::string::npos &&
                latent_main.find("0x00005400u") == std::string::npos,
            "Exportierter Produkt-DMA verwendet nicht den expliziten Bytevertrag.");
    require(representation_specific_identity != latent_recipe.content_identity &&
                katana::runtime::packed_disc_content_identity(*latent_gdi) ==
                    latent_recipe.content_identity &&
                latent_dispatch.find(representation_specific_identity) == std::string::npos,
            "Latentes AOT bindet faelschlich die repraesentationsspezifische GDI-Identitaet "
            "statt der GDI-zu-Pack-Contentidentitaet.");
    const auto trace_enable = generated_main.find(
        "if (!enabled || runtime_wait_loop_descriptors.empty()) return;");
    const auto trace_allocate =
        generated_main.find("recorder_.emplace(runtime_wait_loop_descriptors)");
    const auto trace_set_sink =
        generated_main.find("set_guest_memory_access_sink(recorder_->sink())");
    const auto trace_clear_sink =
        generated_main.find("clear_guest_memory_access_sink()");
    const auto trace_serialize = generated_main.find("recorder_->serialize_json()");
    require(
        generated_main.find("#include \"katana/runtime/wait_loop_trace.hpp\"") !=
                std::string::npos &&
            generated_main.find(
                "std::array<katana::runtime::RuntimeWaitLoopDescriptor, 0u> "
                "runtime_wait_loop_descriptors") != std::string::npos &&
            generated_main.find(
                "std::optional<katana::runtime::RuntimeWaitLoopTraceRecorder> recorder_") !=
                std::string::npos &&
            generated_main.find(
                "std::getenv(\"KATANA_PORT_WAIT_LOOP_TRACE\")") != std::string::npos &&
            generated_main.find(
                "std::string_view(wait_loop_trace_value) == \"1\"") !=
                std::string::npos &&
            generated_main.find(
                "KATANA_WAIT_LOOP_TRACE_NOTICE local-only; contains raw guest-memory "
                "values; do not share without review") != std::string::npos &&
            generated_main.find("\"contains_raw_guest_values\\\":true") !=
                std::string::npos &&
            generated_main.find(
                "~RuntimeWaitLoopTraceSession() noexcept { finish(); }") !=
                std::string::npos &&
            generated_main.find("KATANA_WAIT_LOOP_TRACE ") != std::string::npos &&
            generated_main.find("katana.runtime-wait-loop-trace") !=
                std::string::npos &&
            trace_enable != std::string::npos && trace_allocate != std::string::npos &&
            trace_set_sink != std::string::npos && trace_clear_sink != std::string::npos &&
            trace_serialize != std::string::npos && trace_enable < trace_allocate &&
            trace_allocate < trace_set_sink && trace_clear_sink < trace_serialize &&
            generated_before.at("metadata/port-project.json")
                    .find("\"contract_version\":" +
                          std::to_string(port_project_contract_version)) !=
                std::string::npos &&
            generated_before.at("metadata/provenance.json")
                    .find("\"manifest_version\":" +
                          std::to_string(port_project_contract_version)) !=
                std::string::npos,
        "Portprodukt bindet den versionierten Wait-Loop-Trace nicht strikt opt-in, "
        "allokationsfrei im Normalpfad und RAII-bereinigt ein.");
    const auto poll_disc_directory = fixture.root / "poll-loop-disc";
    std::filesystem::create_directories(poll_disc_directory);
    write_fixture(poll_disc_directory);
    write_binary(poll_disc_directory / "high.bin", poll_loop_boot_track());
    const auto poll_output = fixture.root / "poll-loop-port";
    static_cast<void>(
        export_dreamcast_port_project(poll_disc_directory / "disc.gdi", poll_output, options));
    const auto poll_main = read_text(poll_output / "src" / "main.cpp");
    const auto conservative_descriptor = poll_main.find(
        "{0x8C010000u, 0x8C010000u, 0x8C010000u, "
        "katana::runtime::RuntimeWaitLoopEvidence::ConservativeCandidate}");
    const auto proven_descriptor = poll_main.find(
        "{0x8C010000u, 0x8C010000u, 0x8C010002u, "
        "katana::runtime::RuntimeWaitLoopEvidence::ProvenGuard}");
    static_cast<void>(
        export_dreamcast_port_project(poll_disc_directory / "disc.gdi", poll_output, options));
    require(
        poll_main.find(
            "std::array<katana::runtime::RuntimeWaitLoopDescriptor, 2u> "
            "runtime_wait_loop_descriptors") != std::string::npos &&
            conservative_descriptor != std::string::npos &&
            proven_descriptor != std::string::npos &&
            conservative_descriptor < proven_descriptor &&
            occurrences(poll_main, "RuntimeWaitLoopEvidence::ConservativeCandidate") == 1u &&
            occurrences(poll_main, "RuntimeWaitLoopEvidence::ProvenGuard") == 1u &&
            read_text(poll_output / "src" / "main.cpp") == poll_main &&
            poll_main.find(fixture.root.string()) == std::string::npos &&
            poll_main.find("poll-loop-disc") == std::string::npos &&
            poll_main.find("high.bin") == std::string::npos,
        "Generische Poll-Loops werden nicht deterministisch, dedupliziert oder frei von "
        "privaten Quelldaten in den Produkttrace exportiert.");
    const auto mmio_wait_disc_directory = fixture.root / "mmio-wait-loop-disc";
    std::filesystem::create_directories(mmio_wait_disc_directory);
    write_fixture(mmio_wait_disc_directory);
    write_binary(mmio_wait_disc_directory / "high.bin", mmio_wait_loop_boot_track());
    const auto mmio_wait_output = fixture.root / "mmio-wait-loop-port";
    static_cast<void>(export_dreamcast_port_project(
        mmio_wait_disc_directory / "disc.gdi", mmio_wait_output, options));
    const auto mmio_wait_main = read_text(mmio_wait_output / "src" / "main.cpp");
    const auto mmio_wait_dispatch =
        read_text(mmio_wait_output / "generated" / "code" / "runtime-dispatch.cpp");
    const auto mmio_wait_header =
        read_text(mmio_wait_output / "generated" / "include" / "katana_port.hpp");
    constexpr std::string_view mmio_wait_descriptor =
        "{0x8C010000u, 0x8C010002u, 0x8C010010u, 0xA05F6900u, "
        "0x005F6900u, 0x8C010006u, 3u, 0u, 255u, 4u, 8u, false, true, 7u, 2u, "
        "2u, 1u, 2u, \"generated-block-8C010000\"}";
    const auto mmio_wait_call =
        mmio_wait_dispatch.find("try_product_mmio_wait_loop_batch(");
    const auto mmio_wait_execute =
        mmio_wait_dispatch.find("execute_runtime_block(", mmio_wait_call);
    const auto mmio_wait_method_begin =
        mmio_wait_main.find("bool try_mmio_wait_loop_batch(");
    const auto mmio_wait_method_end =
        mmio_wait_main.find("bool try_counted_loop_batch(", mmio_wait_method_begin);
    const auto mmio_wait_method =
        mmio_wait_method_begin == std::string::npos ||
                mmio_wait_method_end == std::string::npos
            ? std::string_view{}
            : std::string_view{mmio_wait_main}.substr(
                  mmio_wait_method_begin,
                  mmio_wait_method_end - mmio_wait_method_begin);
    const auto mmio_pointer_attempt =
        mmio_wait_method.find("GuestInstructionAttempt pointer_attempt");
    const auto mmio_prefix_flush =
        mmio_wait_method.find("flush_pending_guest_cycles(cpu_, *this)", mmio_pointer_attempt);
    const auto mmio_read_attempt =
        mmio_wait_method.find("GuestInstructionAttempt read_attempt", mmio_prefix_flush);
    const auto mmio_scalar_completion =
        mmio_wait_method.find("const auto finish_scalar_round", mmio_read_attempt);
    const auto mmio_scalar_flush =
        mmio_wait_method.find("flush_pending_guest_cycles(cpu_, *this)",
                              mmio_scalar_completion);
    const auto mmio_synthetic_accounting =
        mmio_wait_method.find("account_prevalidated_unobserved_accesses(",
                              mmio_scalar_completion);
    const auto fastpath_finalizer =
        mmio_wait_dispatch.find("const auto finalize_product_fastpath");
    const auto fastpath_completion =
        mmio_wait_dispatch.find("katana::runtime::finalize_guest_block(",
                                fastpath_finalizer);
    const auto mmio_fastpath_snapshot =
        mmio_wait_dispatch.rfind("const auto fastpath_retired_before",
                                 mmio_wait_call);
    const auto mmio_fastpath_exception_snapshot =
        mmio_wait_dispatch.find("const auto fastpath_exception_generation_before",
                                mmio_fastpath_snapshot);
    const auto mmio_fastpath_finalize =
        mmio_wait_dispatch.find("finalize_product_fastpath(", mmio_wait_call);
    const auto mmio_fastpath_target =
        mmio_wait_dispatch.find("target = cpu.pc", mmio_fastpath_finalize);
    require(
        mmio_wait_dispatch.find(
            "std::array<MmioWaitLoopBatchDescriptor, 1u> "
            "mmio_wait_loop_batch_descriptors") != std::string::npos &&
            mmio_wait_dispatch.find(mmio_wait_descriptor) != std::string::npos &&
            mmio_wait_header.find("struct MmioWaitLoopBatchDescriptor") !=
                std::string::npos &&
            mmio_wait_header.find("bool try_product_mmio_wait_loop_batch(") !=
                std::string::npos &&
            mmio_wait_call != std::string::npos &&
            mmio_wait_execute != std::string::npos &&
            mmio_wait_call < mmio_wait_execute &&
            mmio_wait_main.find("mmio_wait_loop_batching_enabled_ = "
                                "!diagnostic_partial_port") != std::string::npos &&
            mmio_wait_main.find("\"KATANA_PORT_PROGRESS_INTERVAL\"") !=
                std::string::npos &&
            mmio_wait_main.find("\"KATANA_PORT_WAIT_LOOP_TRACE\"") !=
                std::string::npos &&
            mmio_wait_main.find("\"KATANA_PORT_COUNTED_LOOP_TRACE\"") !=
                std::string::npos &&
            mmio_wait_method.find("cpu_.memory.watchpoint_count() == 0u") !=
                std::string::npos &&
            mmio_wait_method.find("cpu_.memory.has_mmio_trace_handler()") !=
                std::string::npos &&
            mmio_wait_method.find("cpu_.memory.has_guest_memory_access_sink()") !=
                std::string::npos &&
            mmio_wait_method.find("(cpu_.interrupts_blocked() || "
                                  "cpu_.interrupt_mask() == 15u)") !=
                std::string::npos &&
            mmio_wait_method.find("MemoryLookupMode::Indexed") != std::string::npos &&
            mmio_wait_method.find("state_.system_asic_device") != std::string::npos &&
            mmio_wait_method.find("descriptor.mmio_physical_address !=\n"
                                  "                katana::runtime::"
                                  "system_asic_physical_base") !=
                std::string::npos &&
            mmio_wait_method.find("prove_main_ram_translation(") !=
                std::string::npos &&
            mmio_wait_method.find("literal->utlb_slot != 0xFFu") !=
                std::string::npos &&
            mmio_wait_method.find("prove_contiguous_translation(") !=
                std::string::npos &&
            mmio_wait_method.find("mmio->utlb_slot != 0xFFu") !=
                std::string::npos &&
            occurrences(mmio_wait_method,
                        "state_.system_asic_device.get(), false") == 2u &&
            mmio_wait_method.find("instruction_translation_path(") !=
                std::string::npos &&
            mmio_wait_method.find("InstructionTranslationPath::Direct") !=
                std::string::npos &&
            mmio_wait_method.find("state_.system_asic->read(0u)") ==
                std::string::npos &&
            occurrences(mmio_wait_method, "guest_read_u32_at(") == 2u &&
            mmio_pointer_attempt != std::string::npos &&
            mmio_prefix_flush != std::string::npos &&
            mmio_read_attempt != std::string::npos &&
            mmio_scalar_completion != std::string::npos &&
            mmio_scalar_flush != std::string::npos &&
            mmio_synthetic_accounting != std::string::npos &&
            mmio_pointer_attempt < mmio_prefix_flush &&
            mmio_prefix_flush < mmio_read_attempt &&
            mmio_read_attempt < mmio_scalar_completion &&
            mmio_scalar_completion < mmio_scalar_flush &&
            mmio_scalar_flush < mmio_synthetic_accounting &&
            fastpath_finalizer != std::string::npos &&
            fastpath_completion != std::string::npos &&
            fastpath_finalizer < fastpath_completion &&
            occurrences(mmio_wait_dispatch,
                        "const auto fastpath_retired_before") == 4u &&
            occurrences(mmio_wait_dispatch,
                        "const auto fastpath_exception_generation_before") == 4u &&
            mmio_fastpath_snapshot != std::string::npos &&
            mmio_fastpath_exception_snapshot != std::string::npos &&
            mmio_fastpath_finalize != std::string::npos &&
            mmio_fastpath_target != std::string::npos &&
            mmio_fastpath_snapshot < mmio_fastpath_exception_snapshot &&
            mmio_fastpath_exception_snapshot < mmio_wait_call &&
            mmio_wait_call < mmio_fastpath_finalize &&
            mmio_fastpath_finalize < mmio_fastpath_target &&
            mmio_wait_dispatch.find(
                "selected.diagnostic_target, fastpath_retired") !=
                std::string::npos &&
            mmio_wait_dispatch.find(
                "fastpath_new_exception, fastpath_new_exception") !=
                std::string::npos &&
            mmio_wait_dispatch.find(
                "fastpath_completion.scheduler.guest_cycle") !=
                std::string::npos &&
            mmio_wait_dispatch.find(
                "const katana::runtime::BlockAddress nominal_source") !=
                std::string::npos &&
            mmio_wait_dispatch.find(
                "cpu.last_exception_instruction_pc") != std::string::npos &&
            mmio_wait_dispatch.find(
                "cpu.last_exception_instruction_physical_pc") !=
                std::string::npos &&
            mmio_wait_dispatch.find(
                "active_exit_source = fastpath_source") != std::string::npos &&
            mmio_wait_dispatch.find(
                "katana::runtime::BlockEndKind::Exception") !=
                std::string::npos &&
            mmio_wait_dispatch.find(
                "fastpath_completion.interrupt.has_value()") !=
                std::string::npos &&
            mmio_wait_dispatch.find(
                "katana::runtime::BlockEndKind::InterruptSafepoint") !=
                std::string::npos &&
            mmio_wait_dispatch.find("return fastpath_source") !=
                std::string::npos &&
            occurrences(mmio_wait_dispatch,
                        "dispatch_callsite = fastpath_source.virtual_address") ==
                4u &&
            occurrences(mmio_wait_dispatch,
                        "dispatch_source = fastpath_source") == 4u &&
            mmio_wait_dispatch.find(
                "active_observations->observe_guest_exception(") !=
                std::string::npos &&
            mmio_wait_method.find(
                "descriptor.loop_header, selected_physical_origin, true") !=
                std::string::npos &&
            mmio_wait_method.find("post_flush_batch_contract") !=
                std::string::npos &&
            mmio_wait_method.find(
                "(mmio_value & test_mask) == 0u") !=
                std::string::npos &&
            mmio_wait_method.find(
                "test_result != descriptor.branch_on_true") != std::string::npos &&
            mmio_wait_method.find(
                "const auto available = *event - scheduler_cycle - 1u") !=
                std::string::npos &&
            mmio_wait_method.find(
                "auto admitted = quantum / descriptor.round_guest_cycles") !=
                std::string::npos &&
            mmio_wait_method.find(
                "*remaining - first_round_tail_guest_cycles - 1u") !=
                std::string::npos &&
            mmio_wait_method.find("carried_guest_cycles") == std::string::npos &&
            mmio_wait_method.find(
                "cpu_.r[descriptor.pointer_register] = literal_value") !=
                std::string::npos &&
            mmio_wait_method.find(
                "cpu_.r[descriptor.value_register] = mmio_value") !=
                std::string::npos &&
            mmio_wait_method.find("cpu_.t = test_result") != std::string::npos &&
            mmio_wait_method.find(
                "cpu_.attempted_guest_instructions += remaining_batch_instructions") !=
                std::string::npos &&
            mmio_wait_method.find(
                "cpu_.retired_guest_instructions += remaining_batch_instructions") !=
                std::string::npos &&
            mmio_wait_method.find(
                "cpu_.active_instruction_pc =\n"
                "            descriptor.backedge_instruction_address") !=
                std::string::npos &&
            mmio_wait_method.find(
                "remaining_batch_cycles - descriptor.read_guest_cycles") !=
                std::string::npos &&
            mmio_wait_method.find(
                "flush_pending_guest_cycles(cpu_, *this)") != std::string::npos &&
            mmio_wait_method.find("cpu_.pc = descriptor.loop_header") !=
                std::string::npos,
        "Bewiesene read-only System-ASIC-Wait-Loop verliert Produktisolation, "
        "No-TLB-Proof, exakte CPU-Zaehler oder die strikte Event-/Budgetgrenze.");
    const auto unsafe_mmio_wait_disc_directory =
        fixture.root / "unsafe-mmio-wait-loop-disc";
    std::filesystem::create_directories(unsafe_mmio_wait_disc_directory);
    write_fixture(unsafe_mmio_wait_disc_directory);
    write_binary(unsafe_mmio_wait_disc_directory / "high.bin",
                 mmio_wait_loop_boot_track(0xA05F6904u));
    const auto unsafe_mmio_wait_output =
        fixture.root / "unsafe-mmio-wait-loop-port";
    static_cast<void>(export_dreamcast_port_project(
        unsafe_mmio_wait_disc_directory / "disc.gdi",
        unsafe_mmio_wait_output,
        options));
    const auto unsafe_mmio_wait_dispatch = read_text(
        unsafe_mmio_wait_output / "generated" / "code" / "runtime-dispatch.cpp");
    require(
        unsafe_mmio_wait_dispatch.find(
            "std::array<MmioWaitLoopBatchDescriptor, 0u> "
            "mmio_wait_loop_batch_descriptors") != std::string::npos &&
            unsafe_mmio_wait_dispatch.find("0xA05F6904u") == std::string::npos,
        "Nicht freigegebener System-ASIC-Registerpoll erhaelt einen Produkt-Fastpath.");
    const auto non_ram_pointer_disc = katana::platform::load_dreamcast_gdi_boot(
        mmio_wait_disc_directory / "disc.gdi");
    const auto non_ram_pointer_image =
        katana::platform::make_dreamcast_disc_executable(non_ram_pointer_disc);
    const auto non_ram_pointer_analysis =
        katana::analysis::analyze_control_flow(non_ram_pointer_image);
    auto non_ram_pointer_program =
        katana::ir::lower_program(non_ram_pointer_analysis);
    bool marked_non_ram_pointer = false;
    for (auto& function : non_ram_pointer_program) {
        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.source_address != 0x8C010000u) continue;
                instruction.memory_effects.region =
                    katana::ir::MemoryRegionKind::Volatile;
                marked_non_ram_pointer = true;
            }
        }
    }
    require(marked_non_ram_pointer,
            "MMIO-Negativfixture besitzt keinen PC-relativen Pointer-Load.");
    std::vector<katana::io::InputProvenance> non_ram_pointer_inputs;
    const auto& non_ram_pointer_descriptor =
        non_ram_pointer_disc.source->descriptor();
    non_ram_pointer_inputs.push_back(katana::io::capture_input_provenance(
        "gdi-descriptor", non_ram_pointer_descriptor.resolved_path));
    for (const auto& track : non_ram_pointer_descriptor.tracks)
        non_ram_pointer_inputs.push_back(katana::io::capture_input_provenance(
            "gdi-track-" + std::to_string(track.number), track.resolved_path));
    const auto non_ram_pointer_output =
        fixture.root / "non-ram-pointer-mmio-wait-loop-port";
    static_cast<void>(export_dreamcast_port_project(
        {non_ram_pointer_image,
         non_ram_pointer_analysis,
         non_ram_pointer_program,
         non_ram_pointer_inputs,
         katana::platform::dreamcast_disc_boot_address,
         katana::platform::dreamcast_disc_boot_address,
         non_ram_pointer_disc.boot_file.size(),
         "non-ram-pointer-mmio-wait-loop"},
        non_ram_pointer_output,
        options));
    const auto non_ram_pointer_dispatch = read_text(
        non_ram_pointer_output / "generated" / "code" / "runtime-dispatch.cpp");
    require(
        non_ram_pointer_dispatch.find(
            "std::array<MmioWaitLoopBatchDescriptor, 0u> "
            "mmio_wait_loop_batch_descriptors") != std::string::npos,
        "PC-relativer Pointer-Load ohne bewiesene Normal-RAM-Region erhaelt einen "
        "MMIO-Wait-Loop-Fastpath.");
    auto normal_ram_mmio_program =
        katana::ir::lower_program(non_ram_pointer_analysis);
    bool marked_normal_ram_mmio_read = false;
    for (auto& function : normal_ram_mmio_program) {
        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.source_address != 0x8C010002u) continue;
                require(instruction.memory_effects.region !=
                            katana::ir::MemoryRegionKind::NormalRam,
                        "Positive MMIO-Fixture klassifiziert den dynamischen Read als RAM.");
                instruction.memory_effects.region =
                    katana::ir::MemoryRegionKind::NormalRam;
                marked_normal_ram_mmio_read = true;
            }
        }
    }
    require(marked_normal_ram_mmio_read,
            "MMIO-Negativfixture besitzt keinen dynamischen Register-Read.");
    const auto normal_ram_mmio_output =
        fixture.root / "normal-ram-read-mmio-wait-loop-port";
    static_cast<void>(export_dreamcast_port_project(
        {non_ram_pointer_image,
         non_ram_pointer_analysis,
         normal_ram_mmio_program,
         non_ram_pointer_inputs,
         katana::platform::dreamcast_disc_boot_address,
         katana::platform::dreamcast_disc_boot_address,
         non_ram_pointer_disc.boot_file.size(),
         "normal-ram-read-mmio-wait-loop"},
        normal_ram_mmio_output,
        options));
    const auto normal_ram_mmio_dispatch = read_text(
        normal_ram_mmio_output / "generated" / "code" / "runtime-dispatch.cpp");
    require(
        normal_ram_mmio_dispatch.find(
            "std::array<MmioWaitLoopBatchDescriptor, 0u> "
            "mmio_wait_loop_batch_descriptors") != std::string::npos,
        "Dynamischer Read mit Normal-RAM-Region erhaelt einen "
        "MMIO-Wait-Loop-Fastpath.");
    const auto memory_fill_disc_directory = fixture.root / "memory-fill-loop-disc";
    std::filesystem::create_directories(memory_fill_disc_directory);
    write_fixture(memory_fill_disc_directory);
    write_binary(memory_fill_disc_directory / "high.bin", memory_fill_loop_boot_track());
    const auto memory_fill_output = fixture.root / "memory-fill-loop-port";
    static_cast<void>(export_dreamcast_port_project(
        memory_fill_disc_directory / "disc.gdi", memory_fill_output, options));
    const auto memory_fill_dispatch =
        read_text(memory_fill_output / "generated" / "code" / "runtime-dispatch.cpp");
    const auto memory_fill_main =
        read_text(memory_fill_output / "src" / "main.cpp");
    const auto memory_fill_header =
        read_text(memory_fill_output / "generated" / "include" / "katana_port.hpp");
    const auto memory_fill_port_cmake =
        read_text(memory_fill_output / "generated" / "katana-port.cmake");
    constexpr std::string_view memory_fill_descriptor =
        "{0x8C01000Eu, 0x8C01000Au, 0x8C010014u, 0x8C01000Au, "
        "0x8C01000Cu, 0x8C01000Eu, 0x8C010010u, 0x8C010012u, 6u, 4u, "
        "5u, 4u, 3u, 6u, 1u, 1u, 5u, 3u, 2u, 5u, 8u, "
        "\"generated-block-8C01000E\", \"generated-block-8C01000A\"}";
    const auto memory_fill_method_begin =
        memory_fill_main.find("bool try_memory_fill_loop_batch(");
    const auto memory_fill_method_end =
        memory_fill_main.find("bool try_mmio_wait_loop_batch(", memory_fill_method_begin);
    const auto memory_fill_method =
        memory_fill_method_begin == std::string::npos ||
                memory_fill_method_end == std::string::npos
            ? std::string_view{}
            : std::string_view{memory_fill_main}.substr(
                  memory_fill_method_begin,
                  memory_fill_method_end - memory_fill_method_begin);
    const auto memory_fill_environment_begin =
        memory_fill_main.find("constexpr std::array memory_fill_loop_debug_environment");
    const auto memory_fill_environment_end = memory_fill_main.find(
        "for (const auto* name : memory_fill_loop_debug_environment",
        memory_fill_environment_begin);
    const auto memory_fill_environment =
        memory_fill_environment_begin == std::string::npos ||
                memory_fill_environment_end == std::string::npos
            ? std::string_view{}
            : std::string_view{memory_fill_main}.substr(
                  memory_fill_environment_begin,
                  memory_fill_environment_end - memory_fill_environment_begin);
    const auto memory_fill_preflush = memory_fill_method.find(
        "if (cpu_.pending_guest_cycles != 0u)\n"
        "            katana::runtime::flush_pending_guest_cycles(cpu_, *this)");
    const auto memory_fill_selected_shape =
        memory_fill_method.find("const bool entry_is_guard");
    const auto memory_fill_selected_proof =
        memory_fill_method.find("if (entry_is_guard) {", memory_fill_selected_shape);
    const auto memory_fill_registration =
        memory_fill_method.find("const auto guard = executable_blocks_.find(");
    const auto memory_fill_instruction_proof =
        memory_fill_method.find("const auto proves_instruction_block");
    const auto memory_fill_limit_read =
        memory_fill_method.find("const auto limit_pointer");
    const auto memory_fill_prepare = memory_fill_method.find(
        "cpu_.memory.prepare_prevalidated_linear_fill(",
        memory_fill_limit_read);
    const auto memory_fill_time_accept = memory_fill_method.find(
        "accept_batch_guest_cycles_before_commit(",
        memory_fill_prepare);
    const auto memory_fill_commit =
        memory_fill_method.find("cpu_.memory.commit_prepared_linear_fill(",
                                memory_fill_time_accept);
    const auto memory_fill_cpu_commit = memory_fill_method.find(
        "cpu_.r[descriptor.cursor_register] = final_cursor",
        memory_fill_commit);
    const auto memory_fill_call =
        memory_fill_dispatch.find("try_product_memory_fill_loop_batch(");
    const auto memory_fill_fastpath_snapshot =
        memory_fill_dispatch.rfind("const auto fastpath_retired_before",
                                   memory_fill_call);
    const auto memory_fill_fastpath_finalize =
        memory_fill_dispatch.find("finalize_product_fastpath(",
                                  memory_fill_call);
    const auto memory_fill_fastpath_target =
        memory_fill_dispatch.find("target = cpu.pc",
                                  memory_fill_fastpath_finalize);
    const auto memory_fill_execute =
        memory_fill_dispatch.find("execute_runtime_block(", memory_fill_call);
    require(
        memory_fill_dispatch.find(
            "std::array<MemoryFillLoopBatchDescriptor, 1u> "
            "memory_fill_loop_batch_descriptors") != std::string::npos &&
            memory_fill_dispatch.find(memory_fill_descriptor) != std::string::npos &&
            memory_fill_dispatch.find(
                "descriptor.guard_address == address ||\n"
                "            descriptor.body_address == address") != std::string::npos &&
            memory_fill_header.find("struct MemoryFillLoopBatchDescriptor") !=
                std::string::npos &&
            memory_fill_header.find("bool try_product_memory_fill_loop_batch(") !=
                std::string::npos &&
            memory_fill_call != std::string::npos &&
            memory_fill_execute != std::string::npos &&
            memory_fill_call < memory_fill_execute &&
            memory_fill_main.find(
                "memory_fill_loop_batching_enabled_ = !diagnostic_partial_port") !=
                std::string::npos &&
            memory_fill_main.find("KATANA_PORT_TEST_DISABLE_MEMORY_FILL") !=
                std::string::npos &&
            memory_fill_main.find("KATANA_PORT_MEMORY_FILL_DIFFERENTIAL_TEST") !=
                std::string::npos &&
            memory_fill_main.find("KATANA_PORT_TEST_ACTIVE_MMU") != std::string::npos &&
            memory_fill_main.find("KATANA_PORT_MEMORY_FILL_TRACE") !=
                std::string::npos &&
            memory_fill_main.find("KATANA_MEMORY_FILL_ADMIT iterations=") !=
                std::string::npos &&
            memory_fill_main.find("KATANA_MEMORY_FILL_STATE cpu=") !=
                std::string::npos &&
            memory_fill_port_cmake.find(
                "option(KATANA_INTERNAL_MEMORY_FILL_DIFFERENTIAL_TEST") !=
                std::string::npos &&
            memory_fill_port_cmake.find(
                "PRIVATE KATANA_INTERNAL_MEMORY_FILL_DIFFERENTIAL_TEST=1") !=
                std::string::npos &&
            memory_fill_environment.find("\"KATANA_PORT_PROGRESS_INTERVAL\"") !=
                std::string::npos &&
            memory_fill_method.find("runtime_state_allows_batch") !=
                std::string::npos &&
            memory_fill_method.find("cpu_.memory.has_guest_memory_access_sink()") !=
                std::string::npos &&
            memory_fill_method.find(
                "guest_write_observer_allows_prevalidated_linear_writes()") !=
                std::string::npos &&
            memory_fill_method.find("MemoryLookupMode::Indexed") !=
                std::string::npos &&
            memory_fill_method.find("state_.disc_load_transactions->transaction_active()") !=
                std::string::npos &&
            memory_fill_method.find("ScopedCpuActiveBlockProvenance") !=
                std::string::npos &&
            memory_fill_method.find(
                "cpu_.active_block_virtual_start = descriptor.guard_address") ==
                std::string::npos &&
            memory_fill_method.find("proves_instruction_block(") != std::string::npos &&
            memory_fill_method.find("instruction_translation_path(") !=
                std::string::npos &&
            memory_fill_method.find("InstructionTranslationPath::Direct") !=
                std::string::npos &&
            memory_fill_method.find("limit_read->no_mmu_fastpath") !=
                std::string::npos &&
            memory_fill_method.find("limit_read->utlb_slot != 0xFFu") !=
                std::string::npos &&
            memory_fill_method.find("target_write->no_mmu_fastpath") !=
                std::string::npos &&
            memory_fill_method.find("target_write->utlb_slot != 0xFFu") !=
                std::string::npos &&
            memory_fill_main.find(
                "translated->physical_address, size, state.main_ram.get(), false") !=
                std::string::npos &&
            memory_fill_main.find(
                "translated->physical_address, size, false") != std::string::npos &&
            memory_fill_method.find(
                "dreamcast_main_ram_backing_is_executable(") != std::string::npos &&
            memory_fill_method.find(
                "state_.runtime_blocks->may_overlap_active_physical(") !=
                std::string::npos &&
            memory_fill_preflush != std::string::npos &&
            memory_fill_selected_shape != std::string::npos &&
            memory_fill_selected_proof != std::string::npos &&
            memory_fill_registration != std::string::npos &&
            memory_fill_instruction_proof != std::string::npos &&
            memory_fill_limit_read != std::string::npos &&
            memory_fill_prepare != std::string::npos &&
            memory_fill_time_accept != std::string::npos &&
            memory_fill_commit != std::string::npos &&
            memory_fill_cpu_commit != std::string::npos &&
            memory_fill_selected_shape < memory_fill_selected_proof &&
            memory_fill_selected_proof < memory_fill_preflush &&
            memory_fill_preflush < memory_fill_registration &&
            memory_fill_registration < memory_fill_instruction_proof &&
            memory_fill_instruction_proof < memory_fill_limit_read &&
            memory_fill_limit_read < memory_fill_prepare &&
            memory_fill_prepare < memory_fill_time_accept &&
            memory_fill_time_accept < memory_fill_commit &&
            memory_fill_commit < memory_fill_cpu_commit &&
            memory_fill_method.find("if (!prepared_fill)") !=
                std::string::npos &&
            memory_fill_method.find(
                "if (!cpu_.memory.commit_prepared_linear_fill(") ==
                std::string::npos &&
            memory_fill_method.find(
                "Memory-Fill-Commit scheiterte nach Gastzeitannahme") ==
                std::string::npos &&
            memory_fill_fastpath_snapshot != std::string::npos &&
            memory_fill_fastpath_finalize != std::string::npos &&
            memory_fill_fastpath_target != std::string::npos &&
            memory_fill_fastpath_snapshot < memory_fill_call &&
            memory_fill_call < memory_fill_fastpath_finalize &&
            memory_fill_fastpath_finalize < memory_fill_fastpath_target &&
            memory_fill_method.find("carried_guest_cycles") == std::string::npos &&
            memory_fill_method.find(
                "if (cpu_.pending_guest_cycles != 0u ||\n"
                "            !runtime_state_allows_batch())") != std::string::npos &&
            memory_fill_method.find(
                "target_write->physical_address, fill_size, fill_value") !=
                std::string::npos &&
            memory_fill_method.find("synthesized_limit_reads") !=
                std::string::npos &&
            memory_fill_method.find("synthesized_indexed_region_hits") !=
                std::string::npos &&
            memory_fill_method.find(
                "synthesized_limit_reads,\n"
                "            synthesized_indexed_region_hits") !=
                std::string::npos &&
            memory_fill_method.find(
                "const auto available = *event - scheduler_cycle - 1u") !=
                std::string::npos &&
            memory_fill_method.find(
                "*remaining - prefix_guest_cycles - 1u") !=
                std::string::npos &&
            memory_fill_method.find(
                "cpu_.attempted_guest_instructions += batch_instructions") !=
                std::string::npos &&
            memory_fill_method.find(
                "cpu_.retired_guest_instructions += batch_instructions") !=
                std::string::npos &&
            memory_fill_method.find(
                "cpu_.active_instruction_pc = descriptor.branch_instruction_address") !=
                std::string::npos &&
            memory_fill_method.find(
                "flush_pending_guest_cycles(cpu_, *this)",
                memory_fill_commit) == std::string::npos,
        "Bewiesener linearer RAM-Fill verliert Shape, Guard/Body-Einstieg, "
        "No-TLB-/Code-Proof, exakte Zaehler oder Event-/Budgetgrenzen.");
    for (const auto [name, variant] :
         std::array{std::pair{"signed", MemoryFillFixtureVariant::SignedCompare},
                     std::pair{"word", MemoryFillFixtureVariant::StoreWord},
                     std::pair{"step", MemoryFillFixtureVariant::StepTwo},
                     std::pair{"bt", MemoryFillFixtureVariant::BranchTrue},
                     std::pair{"compare-order",
                               MemoryFillFixtureVariant::CompareOperandsSwapped},
                     std::pair{"register-alias",
                               MemoryFillFixtureVariant::LimitPointerAliasesCursor},
                     std::pair{"extra-predecessor",
                               MemoryFillFixtureVariant::ExtraBodyPredecessor}}) {
        const auto negative_disc = fixture.root / ("memory-fill-" + std::string(name) + "-disc");
        std::filesystem::create_directories(negative_disc);
        write_fixture(negative_disc);
        write_binary(negative_disc / "high.bin", memory_fill_loop_boot_track(variant));
        const auto negative_output =
            fixture.root / ("memory-fill-" + std::string(name) + "-port");
        static_cast<void>(export_dreamcast_port_project(
            negative_disc / "disc.gdi", negative_output, options));
        const auto negative_dispatch =
            read_text(negative_output / "generated" / "code" / "runtime-dispatch.cpp");
        require(
            negative_dispatch.find(
                "std::array<MemoryFillLoopBatchDescriptor, 0u> "
                "memory_fill_loop_batch_descriptors") != std::string::npos,
            "Unsicherer RAM-Fill-Near-Miss erzeugt einen Produkt-Fastpath: " +
                std::string(name));
    }

    constexpr auto composite_call_block =
        katana::platform::dreamcast_disc_boot_address + 0x18u;
    constexpr auto composite_callsite =
        katana::platform::dreamcast_disc_boot_address + 0x1Cu;
    constexpr auto composite_kernel =
        katana::platform::dreamcast_disc_boot_address + 0x90u;
    constexpr auto composite_kernel_return =
        katana::platform::dreamcast_disc_boot_address + 0x9Cu;
    const auto composite_disc_directory = fixture.root / "composite-callback-disc";
    std::filesystem::create_directories(composite_disc_directory);
    write_fixture(composite_disc_directory);
    write_binary(composite_disc_directory / "high.bin", composite_callback_boot_track());
    const auto composite_gdi = composite_disc_directory / "disc.gdi";
    const auto composite_disc =
        katana::platform::load_dreamcast_gdi_boot(composite_gdi);
    const auto composite_image =
        katana::platform::make_dreamcast_disc_executable(composite_disc);
    const auto composite_analysis =
        katana::analysis::analyze_control_flow(composite_image);
    const auto composite_program = katana::ir::lower_program(composite_analysis);
    const auto composite_resolution = std::find_if(
        composite_analysis.indirect_control_flow.begin(),
        composite_analysis.indirect_control_flow.end(),
        [](const auto& resolution) {
            return resolution.instruction_address == composite_callsite;
        });
    const auto composite_candidate = std::find_if(
        composite_analysis.recursive.functions.begin(),
        composite_analysis.recursive.functions.end(),
        [](const auto& candidate) { return candidate.address == composite_kernel; });
    require(
        composite_resolution != composite_analysis.indirect_control_flow.end() &&
            composite_resolution->kind ==
                katana::analysis::IndirectControlFlowKind::Call &&
            composite_resolution->evidence ==
                katana::analysis::ControlFlowEvidence::RuntimeOnly &&
            !composite_resolution->target.has_value() &&
            composite_resolution->targets.empty() &&
            composite_resolution->analysis_candidates ==
                std::vector<std::uint32_t>{composite_kernel} &&
            composite_candidate != composite_analysis.recursive.functions.end() &&
            composite_candidate->evidence ==
                katana::analysis::ControlFlowEvidence::GuardedPartial &&
            std::find(composite_candidate->origins.begin(),
                      composite_candidate->origins.end(),
                      katana::analysis::FunctionOrigin::GuardedSnapshot) !=
                composite_candidate->origins.end() &&
            std::none_of(
                composite_analysis.resolved_edges.begin(),
                composite_analysis.resolved_edges.end(),
                [](const auto& edge) {
                    return edge.instruction_address == composite_callsite &&
                           edge.target_address == composite_kernel;
                }),
        "Composite-Callback-Fixture verliert Returned-Table-Provenienz oder erfindet eine "
        "feste CFG-Kante.");
    std::vector<katana::io::InputProvenance> composite_inputs;
    const auto& composite_source_descriptor = composite_disc.source->descriptor();
    composite_inputs.push_back(katana::io::capture_input_provenance(
        "gdi-descriptor", composite_source_descriptor.resolved_path));
    for (const auto& track : composite_source_descriptor.tracks)
        composite_inputs.push_back(katana::io::capture_input_provenance(
            "gdi-track-" + std::to_string(track.number), track.resolved_path));
    const auto export_composite_program =
        [&](const std::string& identity,
            const std::vector<katana::ir::Function>& program) {
            const auto output_directory = fixture.root / (identity + "-port");
            static_cast<void>(export_dreamcast_port_project(
                {composite_image,
                 composite_analysis,
                 program,
                 composite_inputs,
                 katana::platform::dreamcast_disc_boot_address,
                 katana::platform::dreamcast_disc_boot_address,
                 composite_disc.boot_file.size(),
                 identity},
                output_directory,
                options));
            return output_directory;
        };
    const auto composite_output =
        export_composite_program("composite-callback", composite_program);
    const auto composite_dispatch =
        read_text(composite_output / "generated" / "code" / "runtime-dispatch.cpp");
    const auto composite_main = read_text(composite_output / "src" / "main.cpp");
    const auto composite_header =
        read_text(composite_output / "generated" / "include" / "katana_port.hpp");
    const auto composite_port_cmake =
        read_text(composite_output / "generated" / "katana-port.cmake");
    const auto composite_batch_call =
        composite_dispatch.find("try_product_composite_callback_batch(");
    const auto composite_ordinary_execute =
        composite_dispatch.find("execute_runtime_block(", composite_batch_call);
    const auto composite_method_begin =
        composite_main.find("bool try_composite_callback_batch(");
    const auto composite_method_end =
        composite_main.find("bool try_memory_fill_loop_batch(", composite_method_begin);
    const auto composite_method =
        composite_method_begin == std::string::npos ||
                composite_method_end == std::string::npos ||
                composite_method_end < composite_method_begin
            ? std::string_view{}
            : std::string_view{composite_main}.substr(
                  composite_method_begin,
                  composite_method_end - composite_method_begin);
    const auto flag_poll_method_begin =
        composite_main.find("bool try_composite_callback_flag_poll_batch(");
    const auto flag_poll_method_end =
        composite_main.find("bool try_composite_callback_batch(", flag_poll_method_begin);
    const auto flag_poll_method =
        flag_poll_method_begin == std::string::npos ||
                flag_poll_method_end == std::string::npos ||
                flag_poll_method_end < flag_poll_method_begin
            ? std::string_view{}
            : std::string_view{composite_main}.substr(
                  flag_poll_method_begin,
                  flag_poll_method_end - flag_poll_method_begin);
    const auto composite_prepare =
        composite_method.find("prepare_prevalidated_linear_u32_pattern(");
    const auto composite_time_accept =
        composite_method.find("accept_batch_guest_cycles_before_commit(",
                              composite_prepare);
    const auto composite_commit =
        composite_method.find("commit_prepared_linear_u32_pattern(",
                              composite_time_accept);
    const auto composite_cpu_commit =
        composite_method.find("cpu_.r[0u] = pattern", composite_commit);
    require(
        composite_dispatch.find(
            "std::array<CompositeCallbackBatchDescriptor, 1u> "
            "composite_callback_batch_descriptors") != std::string::npos &&
            composite_dispatch.find("0x8C010018u, 0x8C01001Cu, 0x8C010020u, "
                                    "0x8C010028u, 0x8C010090u, 0x8C01009Cu") !=
                std::string::npos &&
            composite_header.find("struct CompositeCallbackBatchDescriptor") !=
                std::string::npos &&
            composite_header.find("try_product_composite_callback_batch(") !=
                std::string::npos &&
            composite_batch_call != std::string::npos &&
            composite_ordinary_execute != std::string::npos &&
            composite_batch_call < composite_ordinary_execute &&
            composite_method.find(
                "katana::runtime::AddressTranslationMode::NoMmu") !=
                std::string::npos &&
            composite_method.find(
                "const auto live_target = "
                "cpu_.r[descriptor.callback_register]") !=
                std::string::npos &&
            composite_method.find("if (cpu_.pc != live_target") !=
                std::string::npos &&
            composite_method.find("selected_block.aot_template") !=
                std::string::npos &&
            composite_method.find("*target_backing < *source_backing + 4u") !=
                std::string::npos &&
            composite_method.find(
                "dreamcast_main_ram_backing_is_executable(") !=
                std::string::npos &&
            composite_method.find("may_overlap_active_physical(") !=
                std::string::npos &&
            composite_method.find(
                "guest_write_observer_allows_prevalidated_linear_writes()") !=
                std::string::npos &&
            composite_method.find("next_event_cycle()") != std::string::npos &&
            composite_prepare != std::string::npos &&
            composite_time_accept != std::string::npos &&
            composite_commit != std::string::npos &&
            composite_cpu_commit != std::string::npos &&
            composite_prepare < composite_time_accept &&
            composite_time_accept < composite_commit &&
            composite_commit < composite_cpu_commit &&
            composite_method.find("return reject(\"memory-prepare\")") !=
                std::string::npos &&
            composite_method.find(
                "if (!cpu_.memory.commit_prepared_linear_u32_pattern(") ==
                std::string::npos &&
            composite_method.find(
                "Composite-Callback-Commit scheiterte nach Gastzeitannahme") ==
                std::string::npos &&
            composite_method.find(
                "flush_pending_guest_cycles(cpu_, *this)",
                composite_commit) == std::string::npos &&
            composite_method.find(
                "const ScopedCpuActiveBlockProvenance active_block_provenance(") !=
                std::string::npos &&
            composite_method.find(
                "cpu_.active_block_virtual_start = descriptor.continuation_address") ==
                std::string::npos &&
            composite_main.find(
                "KATANA_PORT_TEST_DISABLE_COMPOSITE_CALLBACK") !=
                std::string::npos &&
            composite_main.find("\" code_provenance=\"") !=
                std::string::npos &&
            composite_main.find("RuntimeProbeDeviceKind::CodeTracker") !=
                std::string::npos &&
            composite_main.find("KATANA_COMPOSITE_CALLBACK_STATE cpu=") !=
                std::string::npos &&
            composite_main.find("KATANA_COMPOSITE_CALLBACK_ADMIT iterations=") !=
                std::string::npos &&
            composite_main.find("KATANA_COMPOSITE_CALLBACK_REJECT stage=") !=
                std::string::npos &&
            composite_main.find("enum class FlagPollBatchRejectionStage") !=
                std::string::npos &&
            flag_poll_method.find(
                "guest_write_observer_allows_prevalidated_linear_writes()") ==
                std::string::npos &&
            flag_poll_method.find(
                "katana::runtime::AddressTranslationMode::NoMmu") ==
                std::string::npos &&
            flag_poll_method.find("const auto proves_direct_instruction_range") !=
                std::string::npos &&
            occurrences(flag_poll_method,
                        "InstructionTranslationPath::Direct") == 2u &&
            occurrences(flag_poll_method,
                        "proves_direct_instruction_range(") == 3u &&
            flag_poll_method.find(
                "descriptor.call_block_address,\n"
                "                call_block->second.physical_origin") !=
                std::string::npos &&
            flag_poll_method.find(
                "live_target, selected_block.physical_origin") !=
                std::string::npos &&
            flag_poll_method.find(
                "descriptor.continuation_address,\n"
                "                continuation->second.physical_origin") !=
                std::string::npos &&
            flag_poll_method.find("!first.no_mmu_fastpath") != std::string::npos &&
            flag_poll_method.find("!last.no_mmu_fastpath") != std::string::npos &&
            flag_poll_method.find(
                "flag_read->mmu_generation != active_block_variant_->mmu_generation") !=
                std::string::npos &&
            flag_poll_method.find("++flag_poll_batch_counters_.attempts;") !=
                std::string::npos &&
            occurrences(flag_poll_method,
                        "return reject(FlagPollBatchRejectionStage::") == 18u &&
            flag_poll_method.find("composite_callback_batch_rejected(") ==
                std::string::npos &&
            flag_poll_method.find("trace_composite_callback_batch_admission(") ==
                std::string::npos &&
            flag_poll_method.find("++flag_poll_batch_counters_.admissions;") !=
                std::string::npos &&
            flag_poll_method.find(
                "flag_poll_batch_counters_.batched_rounds += admitted;") !=
                std::string::npos &&
            flag_poll_method.find(
                "flag_poll_batch_counters_.batched_guest_cycles += batch_guest_cycles;") !=
                std::string::npos &&
            composite_method.find("trace_composite_callback_batch_admission(admitted);") !=
                std::string::npos &&
            composite_main.find("KATANA_FLAG_POLL_BATCH_STATS attempts=") !=
                std::string::npos &&
            occurrences(composite_main, "\" reject_poll_") == 18u &&
            composite_main.find(
                "services.report_flag_poll_batch_statistics(std::cerr);") !=
                std::string::npos &&
            composite_port_cmake.find(
                "option(KATANA_INTERNAL_COMPOSITE_CALLBACK_DIFFERENTIAL_TEST") !=
                std::string::npos &&
            composite_port_cmake.find(
                "PRIVATE KATANA_INTERNAL_COMPOSITE_CALLBACK_DIFFERENTIAL_TEST=1") !=
                std::string::npos &&
            composite_port_cmake.find(
                "option(KATANA_INTERNAL_BATCH_COMMIT_LIFECYCLE_TEST") !=
                std::string::npos &&
            composite_port_cmake.find(
                "PRIVATE KATANA_INTERNAL_BATCH_COMMIT_LIFECYCLE_TEST=1") !=
                std::string::npos,
        "Composite-Callback-Descriptor oder Produkt-/No-MMU-/Differentialvertrag ist "
        "unvollstaendig.");
    require(
        composite_resolution->analysis_candidates ==
                std::vector<std::uint32_t>{composite_kernel} &&
            std::none_of(
                composite_program.begin(), composite_program.end(), [](const auto& function) {
                    return std::any_of(
                        function.blocks.begin(),
                        function.blocks.end(),
                        [](const auto& block) {
                            return std::any_of(
                                block.instructions.begin(),
                                block.instructions.end(),
                                [](const auto& instruction) {
                                    return instruction.source_address == composite_callsite &&
                                           (instruction.target_address.has_value() ||
                                            !instruction.resolved_targets.empty());
                                });
                        });
                }),
        "Composite-Callback-Kandidat wurde als feste IR-/CFG-Kante materialisiert.");
    const auto composite_dispatch_end = composite_dispatch.find(
        "if (const auto* memory_fill_loop =", composite_batch_call);
    const auto composite_dispatch_fastpath =
        composite_batch_call == std::string::npos ||
                composite_dispatch_end == std::string::npos ||
                composite_dispatch_end < composite_batch_call
            ? std::string_view{}
            : std::string_view{composite_dispatch}.substr(
                  composite_batch_call,
                  composite_dispatch_end - composite_batch_call);
    require(
        composite_dispatch_fastpath.find("record_hit(") == std::string::npos &&
            composite_dispatch_fastpath.find(
                "observe_block_dispatch_hit(") == std::string::npos &&
            composite_dispatch_fastpath.find("active_table->lookup(") ==
                std::string::npos &&
            composite_dispatch_fastpath.find("synthesized_target_fetches") ==
                std::string::npos,
        "Composite-Fastpath synthetisiert Hostdispatch-/Diagnosearbeit und vernichtet den "
        "belegten Dispatch-Speedup.");

    const auto kernel_function = std::find_if(
        composite_program.begin(), composite_program.end(), [](const auto& function) {
            return std::any_of(
                function.blocks.begin(), function.blocks.end(), [](const auto& block) {
                    return block.start_address == composite_kernel;
                });
        });
    const auto outer_function = std::find_if(
        composite_program.begin(), composite_program.end(), [](const auto& function) {
            return std::any_of(
                function.blocks.begin(), function.blocks.end(), [](const auto& block) {
                    return block.start_address == composite_call_block;
                });
        });
    require(kernel_function != composite_program.end() &&
                outer_function != composite_program.end() &&
                kernel_function != outer_function,
            "Composite-Overlap-Fixture besitzt keine getrennten Kernel-/Outer-Funktionen.");
    const auto append_kernel_overlap = [&](auto& program) {
        const auto kernel_index =
            static_cast<std::size_t>(kernel_function - composite_program.begin());
        const auto outer_index =
            static_cast<std::size_t>(outer_function - composite_program.begin());
        for (const auto& block : program[kernel_index].blocks) {
            if (block.start_address == composite_kernel ||
                block.start_address == composite_kernel_return)
                program[outer_index].blocks.push_back(block);
        }
    };
    auto identical_overlap_program = composite_program;
    append_kernel_overlap(identical_overlap_program);
    const auto identical_overlap_output = export_composite_program(
        "composite-callback-identical-overlap", identical_overlap_program);
    const auto identical_overlap_dispatch =
        read_text(identical_overlap_output / "generated" / "code" /
                  "runtime-dispatch.cpp");
    require(
        identical_overlap_dispatch.find(
            "std::array<CompositeCallbackBatchDescriptor, 1u> "
            "composite_callback_batch_descriptors") != std::string::npos,
        "Byte- und CFG-identischer Kernel-Overlap wurde pauschal verworfen.");

    auto divergent_overlap_program = composite_program;
    append_kernel_overlap(divergent_overlap_program);
    const auto divergent_outer_index =
        static_cast<std::size_t>(outer_function - composite_program.begin());
    auto divergent_overlap_block = std::find_if(
        divergent_overlap_program[divergent_outer_index].blocks.begin(),
        divergent_overlap_program[divergent_outer_index].blocks.end(),
        [](const auto& block) { return block.start_address == composite_kernel; });
    require(divergent_overlap_block !=
                divergent_overlap_program[divergent_outer_index].blocks.end() &&
                !divergent_overlap_block->instructions.empty(),
            "Composite-Negativfixture findet ihren ueberlappenden Kernel nicht.");
    divergent_overlap_block->instructions.front().original_opcode = 0x0009u;
    bool divergent_overlap_rejected = false;
    try {
        static_cast<void>(export_composite_program(
            "composite-callback-divergent-overlap", divergent_overlap_program));
    } catch (const std::runtime_error& error) {
        divergent_overlap_rejected =
            std::string_view(error.what()) ==
            "IR-Basic-Block besitzt abweichende Funktionsbesitzer.";
    }
    require(divergent_overlap_rejected,
            "Semantisch abweichender Kernel-Overlap wurde nicht typisiert verworfen.");

    const auto counted_loop_disc_directory = fixture.root / "counted-loop-disc";
    std::filesystem::create_directories(counted_loop_disc_directory);
    write_fixture(counted_loop_disc_directory);
    write_binary(counted_loop_disc_directory / "high.bin", counted_loop_boot_track());
    const auto counted_loop_gdi = counted_loop_disc_directory / "disc.gdi";
    const auto counted_loop_output = fixture.root / "counted-loop-port";
    static_cast<void>(
        export_dreamcast_port_project(counted_loop_gdi, counted_loop_output, options));
    const auto counted_loop_dispatch =
        read_text(counted_loop_output / "generated" / "code" / "runtime-dispatch.cpp");
    const auto counted_loop_main = read_text(counted_loop_output / "src" / "main.cpp");
    const auto counted_loop_header =
        read_text(counted_loop_output / "generated" / "include" / "katana_port.hpp");
    const auto counted_loop_port_cmake =
        read_text(counted_loop_output / "generated" / "katana-port.cmake");
    constexpr std::string_view counted_loop_descriptor =
        "{0x8C010016u, 0x8C010010u, 6u, 0x8C010024u, 0x8C010018u, "
        "0x8C010014u, 0x8C010012u, 8, 15u, 3u, 2u, 1u, 1u, 2u, 4u, "
        "7u, 2u, 2u, 2u, true, 7u, 12u, \"generated-block-8C010016\", "
        "\"generated-block-8C010010\"}";
    const auto counted_loop_call = counted_loop_dispatch.find("try_product_counted_loop_batch(");
    const auto ordinary_block_execute =
        counted_loop_dispatch.find("execute_runtime_block(", counted_loop_call);
    const auto counted_loop_method_begin = counted_loop_main.find("bool try_counted_loop_batch(");
    const auto counted_loop_method_end = counted_loop_main.find(
        "ExecutableCodeTracker* executable_code_tracker()", counted_loop_method_begin);
    const auto counted_loop_method =
        counted_loop_method_begin == std::string::npos ||
                counted_loop_method_end == std::string::npos
            ? std::string_view{}
            : std::string_view{counted_loop_main}.substr(
                  counted_loop_method_begin, counted_loop_method_end - counted_loop_method_begin);
    const auto counted_limit_attempt =
        counted_loop_method.find("GuestInstructionAttempt limit_attempt");
    const auto counted_limit_read = std::min(
        {counted_loop_method.find("guest_read_u16_at(", counted_limit_attempt),
         counted_loop_method.find("guest_read_s16_at(", counted_limit_attempt),
         counted_loop_method.find("guest_read_u32_at(", counted_limit_attempt)});
    const auto counted_first_flush =
        counted_loop_method.find("flush_pending_guest_cycles(cpu_, *this)");
    const auto counted_prefix_flush = counted_loop_method.find(
        "flush_pending_guest_cycles(cpu_, *this)",
        counted_first_flush == std::string::npos
            ? std::string::npos
            : counted_first_flush + 1u);
    const auto counted_post_flush_contract =
        counted_loop_method.find("post_flush_batch_contract", counted_prefix_flush);
    const auto counted_post_flush_limit_snapshot =
        counted_loop_method.find("post_flush_limit_unchanged", counted_post_flush_contract);
    const auto counted_first_read_attempt = counted_loop_method.find(
        "GuestInstructionAttempt first_counter_read_attempt",
        counted_post_flush_contract);
    const auto counted_first_read =
        counted_loop_method.find("guest_read_u32_at(", counted_first_read_attempt);
    const auto counted_scalar_guard =
        counted_loop_method.find("const auto finish_scalar_guard", counted_first_read);
    const auto counted_first_round_tail =
        counted_loop_method.find("first_round_tail_guest_cycles", counted_scalar_guard);
    const auto counted_synthetic_reads =
        counted_loop_method.find("admitted * 3u - 2u", counted_first_round_tail);
    const auto counted_sequence_prepare = counted_loop_method.find(
        "prepare_prevalidated_repeated_u32_sequence(",
        counted_synthetic_reads);
    const auto counted_time_accept = counted_loop_method.find(
        "accept_batch_guest_cycles_before_commit(",
        counted_sequence_prepare);
    const auto counted_atomic_sequence = counted_loop_method.find(
        "commit_prepared_repeated_u32_sequence(",
        counted_time_accept);
    const auto counted_cpu_commit = counted_loop_method.find(
        "cpu_.r[descriptor.limit_register] = limit_bits",
        counted_atomic_sequence);
    const auto counted_dynamic_kind =
        counted_loop_dispatch.find("const auto counted_fastpath_kind", counted_loop_call);
    const auto counted_dynamic_kind_store = counted_loop_dispatch.find(
        "counted_loop->store_instruction_address", counted_dynamic_kind);
    const auto counted_dynamic_kind_fallthrough = counted_loop_dispatch.find(
        "BlockEndKind::Fallthrough", counted_dynamic_kind_store);
    const auto counted_fastpath_finalize =
        counted_loop_dispatch.find("finalize_product_fastpath(", counted_loop_call);
    const auto counted_dynamic_source =
        counted_loop_dispatch.find("cpu.active_instruction_pc", counted_fastpath_finalize);
    const auto counted_dynamic_physical_source = counted_loop_dispatch.find(
        "cpu.active_instruction_physical_pc", counted_dynamic_source);
    const auto counted_post_flush_scope =
        counted_post_flush_contract == std::string::npos ||
                counted_first_read_attempt == std::string::npos ||
                counted_first_read_attempt < counted_post_flush_contract
            ? std::string_view{}
            : counted_loop_method.substr(
                  counted_post_flush_contract,
                  counted_first_read_attempt - counted_post_flush_contract);
    const auto counted_contract_proof_begin = counted_loop_main.find(
        "std::optional<CountedLoopContract> prove_counted_loop_contract(");
    const auto counted_contract_proof_end = counted_loop_main.find(
        "static constexpr std::uint64_t local_block_chain_guest_cycle_budget",
        counted_contract_proof_begin);
    const auto counted_contract_proof =
        counted_contract_proof_begin == std::string::npos ||
                counted_contract_proof_end == std::string::npos ||
                counted_contract_proof_end < counted_contract_proof_begin
            ? std::string_view{}
            : std::string_view{counted_loop_main}.substr(
                  counted_contract_proof_begin,
                  counted_contract_proof_end - counted_contract_proof_begin);
    const auto counted_alias_proof_begin = counted_loop_main.find(
        "bool counted_loop_counter_aliases_are_inert(");
    const auto counted_alias_proof_end =
        counted_loop_main.find("std::optional<CountedLoopContract>",
                               counted_alias_proof_begin);
    const auto counted_alias_proof =
        counted_alias_proof_begin == std::string::npos ||
                counted_alias_proof_end == std::string::npos ||
                counted_alias_proof_end < counted_alias_proof_begin
            ? std::string_view{}
            : std::string_view{counted_loop_main}.substr(
                  counted_alias_proof_begin,
                  counted_alias_proof_end - counted_alias_proof_begin);
    const auto counted_runtime_gate_begin = counted_loop_main.find(
        "bool runtime_state_allows_counted_loop_batch()");
    const auto counted_runtime_gate_end = counted_loop_main.find(
        "bool proves_counted_instruction_block(", counted_runtime_gate_begin);
    const auto counted_runtime_gate =
        counted_runtime_gate_begin == std::string::npos ||
                counted_runtime_gate_end == std::string::npos ||
                counted_runtime_gate_end < counted_runtime_gate_begin
            ? std::string_view{}
            : std::string_view{counted_loop_main}.substr(
                  counted_runtime_gate_begin,
                  counted_runtime_gate_end - counted_runtime_gate_begin);
    const auto counted_gate_begin = counted_loop_main.find(
        "counted_loop_batching_enabled_ = !diagnostic_partial_port &&");
    const auto counted_gate_end =
        counted_loop_main.find("mmio_wait_loop_batching_enabled_", counted_gate_begin);
    const auto counted_gate =
        counted_gate_begin == std::string::npos ||
                counted_gate_end == std::string::npos ||
                counted_gate_end < counted_gate_begin
            ? std::string_view{}
            : std::string_view{counted_loop_main}.substr(
                  counted_gate_begin, counted_gate_end - counted_gate_begin);
    require(counted_loop_dispatch.find("std::array<CountedLoopBatchDescriptor, 1u> "
                                       "counted_loop_batch_descriptors") != std::string::npos,
            "Exakte positive Counted-Loop-Fixture erzeugt nicht genau einen Descriptor.");
    const auto counted_loop_array = counted_loop_dispatch.find("counted_loop_batch_descriptors{{");
    require(counted_loop_dispatch.find(counted_loop_descriptor) != std::string::npos,
            "Counted-Loop-Descriptor verliert Adressen, Register, Timing oder Schrittweite: " +
                counted_loop_dispatch.substr(counted_loop_array, 320u));
    require(
        counted_loop_header.find("struct CountedLoopBatchDescriptor") != std::string::npos &&
            counted_loop_header.find(
                "std::uint32_t first_counter_read_instruction_address") !=
                std::string::npos &&
            counted_loop_header.find("std::uint8_t limit_width") != std::string::npos &&
            counted_loop_header.find("std::uint8_t guard_instruction_count") !=
                std::string::npos &&
            counted_loop_header.find("std::uint8_t prefix_guest_cycles") !=
                std::string::npos &&
            counted_loop_header.find(
                "std::uint8_t first_counter_read_guest_cycles") !=
                std::string::npos &&
            counted_loop_header.find("std::uint64_t guard_guest_cycles") !=
                std::string::npos &&
            counted_loop_header.find("std::string_view increment_provenance") !=
                std::string::npos &&
            counted_loop_port_cmake.find(
                "option(KATANA_INTERNAL_COUNTED_LOOP_DIFFERENTIAL_TEST") !=
                std::string::npos &&
            counted_loop_port_cmake.find(
                "PRIVATE KATANA_INTERNAL_COUNTED_LOOP_DIFFERENTIAL_TEST=1") !=
                std::string::npos &&
            counted_loop_port_cmake.find(
                "option(KATANA_INTERNAL_BATCH_COMMIT_LIFECYCLE_TEST") !=
                std::string::npos &&
            counted_loop_port_cmake.find(
                "PRIVATE KATANA_INTERNAL_BATCH_COMMIT_LIFECYCLE_TEST=1") !=
                std::string::npos &&
            counted_loop_main.find("KATANA_PORT_TEST_BATCH_COMMIT_ABORT") !=
                std::string::npos &&
            counted_loop_main.find("KATANA_BATCH_COMMIT_ABORT_CLEAN kind=") !=
                std::string::npos &&
            counted_loop_main.find("restore_unaccepted_time") !=
                std::string::npos &&
            counted_loop_main.find(
                "#if defined(KATANA_INTERNAL_COUNTED_LOOP_DIFFERENTIAL_TEST)") !=
                std::string::npos &&
            counted_loop_main.find("KATANA_PORT_TEST_ACTIVE_MMU") != std::string::npos &&
            counted_loop_main.find("KATANA_PORT_TEST_MAPPED_COUNTER_SEGMENT") !=
                std::string::npos &&
            counted_loop_main.find("KATANA_PORT_TEST_DISABLE_COUNTED_LOOP") !=
                std::string::npos &&
            counted_loop_main.find("KATANA_COUNTED_LOOP_STATE cpu=") !=
                std::string::npos &&
            counted_loop_main.find("module_provenance=") != std::string::npos &&
            counted_loop_main.find("class ScopedCpuActiveBlockProvenance final") !=
                std::string::npos &&
            counted_loop_method.find(
                "const ScopedCpuActiveBlockProvenance active_block_provenance(") !=
                std::string::npos &&
            counted_loop_dispatch.find("selected_block, *counted_loop") !=
                std::string::npos &&
            counted_loop_dispatch.find("active_context->scheduler_cycle =") != std::string::npos &&
            counted_loop_call != std::string::npos && ordinary_block_execute != std::string::npos &&
            counted_loop_call < ordinary_block_execute &&
            counted_gate_begin != std::string::npos &&
            counted_gate_end != std::string::npos &&
            counted_gate.find("!runtime_probe_mode_ && replay_log_ == nullptr") !=
                std::string::npos &&
            counted_gate.find("\"KATANA_PORT_PROGRESS_INTERVAL\"") !=
                std::string::npos &&
            counted_gate.find("\"KATANA_PORT_COUNTED_LOOP_TRACE\"") ==
                std::string::npos &&
            counted_gate.find("counted_loop_batching_enabled_ = false") !=
                std::string::npos &&
            counted_loop_main.find("selected_block.runtime_registered") != std::string::npos &&
            counted_loop_main.find("selected_block.aot_template") != std::string::npos &&
            counted_loop_method.find(
                "selected_block.provenance, descriptor.guard_provenance") !=
                std::string::npos &&
            counted_loop_main.find("actual.starts_with(expected)") !=
                std::string::npos &&
            counted_loop_main.find("actual.ends_with(\"-mmu-variant\")") !=
                std::string::npos &&
            counted_contract_proof.find(
                "increment->second.identity, descriptor.increment_provenance") !=
                std::string::npos &&
            counted_loop_main.find("(counter_address & 3u) != 0u") != std::string::npos &&
            counted_loop_main.find(
                "descriptor.limit_address % descriptor.limit_width != 0u") !=
                std::string::npos &&
            counted_alias_proof.find("counter_address + offset, 1u") !=
                std::string::npos &&
            counted_runtime_gate.find("state_.main_ram->size() ==\n"
                                      "                katana::runtime::dreamcast_main_ram_size") !=
                std::string::npos &&
            counted_runtime_gate.find("AddressTranslationMode::NoMmu ||") !=
                std::string::npos &&
            counted_runtime_gate.find("AddressTranslationMode::Mmu) &&") !=
                std::string::npos &&
            counted_loop_main.find(
                "static bool counted_loop_direct_p1_p2_range(") !=
                std::string::npos &&
            counted_loop_main.find("first_segment != 4u && first_segment != 5u") !=
                std::string::npos &&
            counted_loop_main.find(
                "return (last_address >> 29u) == first_segment;") !=
                std::string::npos &&
            occurrences(counted_loop_main,
                        "counted_loop_range_has_no_mmu_side_effects(") == 4u &&
            counted_loop_main.find("active-mmu-nondirect-range") !=
                std::string::npos &&
            counted_contract_proof.find("counter_address_sum < 0") !=
                std::string::npos &&
            counted_contract_proof.find("active-mmu-address-overflow") !=
                std::string::npos &&
            counted_contract_proof.find(
                "counter_address, 4u") != std::string::npos &&
            counted_contract_proof.find(
                "descriptor.limit_address, limit_size") != std::string::npos &&
            counted_loop_main.find("constexpr std::uint32_t test_tlb_flags") !=
                std::string::npos &&
            occurrences(counted_loop_main, "katana::runtime::load_tlb(cpu);") == 2u &&
            occurrences(counted_loop_main,
                        "katana::runtime::translate_guest_address(") >= 3u &&
            counted_loop_main.find("katana::runtime::dreamcast_main_ram_area_bases") !=
                std::string::npos &&
            counted_loop_main.find(
                "mirror < katana::runtime::dreamcast_main_ram_mirrors_per_area") !=
                std::string::npos &&
            counted_loop_method.find("carried_guest_cycles") == std::string::npos &&
            counted_loop_method.find(
                "quantum / descriptor.round_guest_cycles") != std::string::npos &&
            counted_loop_method.find(
                "descriptor.round_guest_cycles - descriptor.prefix_guest_cycles") !=
                std::string::npos &&
            counted_loop_method.find(
                "available - first_round_tail_guest_cycles") != std::string::npos &&
            counted_loop_method.find(
                "*remaining - first_round_tail_guest_cycles - 1u") !=
                std::string::npos &&
            counted_loop_method.find(
                "batch_cycles - descriptor.prefix_guest_cycles") !=
                std::string::npos &&
            counted_loop_main.find("struct ProvenMemoryTranslation") != std::string::npos &&
            counted_loop_main.find("prove_contiguous_translation(") != std::string::npos &&
            counted_loop_main.find("prove_main_ram_translation(") != std::string::npos &&
            counted_loop_main.find("prove_on_chip_ram_translation(") != std::string::npos &&
            counted_loop_main.find("first.mmu_generation != last.mmu_generation") !=
                std::string::npos &&
            counted_loop_main.find("first.utlb_slot != last.utlb_slot") != std::string::npos &&
            counted_loop_main.find("last.physical_address) != physical + size - 1u") !=
                std::string::npos &&
            counted_loop_main.find("physical != "
                                   "katana::runtime::canonical_physical_address(address)") ==
                std::string::npos &&
            counted_loop_main.find("counter_read->physical_address !=\n"
                                   "                counter_write->physical_address") !=
                std::string::npos &&
            counted_contract_proof.find("counter_read->no_mmu_fastpath") !=
                std::string::npos &&
            counted_contract_proof.find("counter_write->no_mmu_fastpath") !=
                std::string::npos &&
            counted_contract_proof.find("limit_read->no_mmu_fastpath") !=
                std::string::npos &&
            occurrences(counted_contract_proof, "utlb_slot != 0xFFu") == 3u &&
            counted_loop_main.find("state.cache_control->on_chip_ram_device()") !=
                std::string::npos &&
            counted_loop_main.find("cpu_.address_space.get() == state_.address_space.get()") !=
                std::string::npos &&
            counted_loop_main.find("Sh4CacheControl::operand_ram_enable") !=
                std::string::npos &&
            counted_loop_main.find("translated->raw_physical_address != address") !=
                std::string::npos &&
            counted_loop_main.find("translated->physical_address != address") !=
                std::string::npos &&
            counted_loop_main.find(
                "translated->physical_address, size, device.get(), false") !=
                std::string::npos &&
            counted_loop_main.find("const bool counter_is_on_chip_ram") != std::string::npos &&
            occurrences(counted_loop_main,
                        "state_.cache_control->read_on_chip_ram(") == 0u &&
            counted_loop_main.find("utlb_accesses_per_round") ==
                std::string::npos &&
            counted_loop_main.find("advance_utlb_access_counter(") ==
                std::string::npos &&
            counted_loop_main.find("*counter_backing < *limit_backing + limit_size") !=
                std::string::npos &&
            counted_loop_main.find("\"KATANA_PORT_LIFECYCLE_TEST\"") != std::string::npos &&
            counted_loop_method.find("remaining_batch_cycles >\n"
                                     "                std::numeric_limits<std::uint64_t>::max() - "
                                     "scheduler_cycle") !=
                std::string::npos &&
            counted_loop_main.find("constexpr std::uint64_t quantum = 131'072u") !=
                std::string::npos &&
            counted_limit_attempt != std::string::npos &&
            counted_limit_read != std::string::npos &&
            counted_first_flush != std::string::npos &&
            counted_prefix_flush != std::string::npos &&
            counted_post_flush_contract != std::string::npos &&
            counted_first_read_attempt != std::string::npos &&
            counted_first_read != std::string::npos &&
            counted_scalar_guard != std::string::npos &&
            counted_first_round_tail != std::string::npos &&
            counted_synthetic_reads != std::string::npos &&
            counted_sequence_prepare != std::string::npos &&
            counted_time_accept != std::string::npos &&
            counted_atomic_sequence != std::string::npos &&
            counted_cpu_commit != std::string::npos &&
            counted_synthetic_reads < counted_sequence_prepare &&
            counted_sequence_prepare < counted_time_accept &&
            counted_time_accept < counted_atomic_sequence &&
            counted_atomic_sequence < counted_cpu_commit &&
            counted_loop_method.find(
                "counted_loop_batch_rejected(\"memory-prepare\")") !=
                std::string::npos &&
            counted_loop_method.find(
                "if (!cpu_.memory.commit_prepared_repeated_u32_sequence(") ==
                std::string::npos &&
            counted_loop_method.find(
                "Counted-Loop-Commit scheiterte nach Gastzeitannahme") ==
                std::string::npos &&
            counted_loop_method.find(
                "flush_pending_guest_cycles(cpu_, *this)",
                counted_atomic_sequence) == std::string::npos &&
            counted_limit_attempt < counted_limit_read &&
            counted_limit_read < counted_first_flush &&
            counted_first_flush < counted_prefix_flush &&
            counted_prefix_flush < counted_post_flush_contract &&
            counted_post_flush_contract < counted_post_flush_limit_snapshot &&
            counted_post_flush_limit_snapshot < counted_first_read_attempt &&
            counted_post_flush_contract < counted_first_read_attempt &&
            counted_contract_proof_begin != std::string::npos &&
            counted_contract_proof_end != std::string::npos &&
            counted_alias_proof_begin != std::string::npos &&
            counted_alias_proof_end != std::string::npos &&
            counted_runtime_gate_begin != std::string::npos &&
            counted_runtime_gate_end != std::string::npos &&
            counted_loop_main.find("struct CountedLoopContract") !=
                std::string::npos &&
            counted_loop_method.find("struct CountedLoopContract") ==
                std::string::npos &&
            counted_loop_method.find("const auto prove_counted_loop_contract") ==
                std::string::npos &&
            counted_runtime_gate.find(
                "guest_write_observer_allows_prevalidated_linear_writes()") !=
                std::string::npos &&
            counted_alias_proof.find(
                "katana::runtime::dreamcast_main_ram_area_bases") !=
                std::string::npos &&
            counted_alias_proof.find(
                "katana::runtime::dreamcast_main_ram_mirrors_per_area") !=
                std::string::npos &&
            counted_alias_proof.find(
                "state_.runtime_blocks->may_overlap_active_physical(") !=
                std::string::npos &&
            counted_alias_proof.find(
                "state_.module_catalog->may_overlap_active_extent(") !=
                std::string::npos &&
            counted_contract_proof.find("selected_physical_origin") !=
                std::string::npos &&
            counted_contract_proof.find("selected_block") ==
                std::string::npos &&
            counted_post_flush_scope.find("selected_block") ==
                std::string::npos &&
            counted_post_flush_scope.find(
                "post_flush_contract->limit_backing_offset") !=
                std::string::npos &&
            counted_post_flush_scope.find(
                "static_cast<std::uint16_t>(limit_bits)") !=
                std::string::npos &&
            counted_loop_method.find(
                "!post_flush_batch_contract || !post_flush_limit_unchanged") !=
                std::string::npos &&
            counted_first_read_attempt < counted_first_read &&
            counted_first_read < counted_scalar_guard &&
            counted_scalar_guard < counted_first_round_tail &&
            counted_first_round_tail < counted_synthetic_reads &&
            counted_synthetic_reads < counted_sequence_prepare &&
            counted_sequence_prepare < counted_time_accept &&
            counted_time_accept < counted_atomic_sequence &&
            counted_atomic_sequence < counted_cpu_commit &&
            counted_loop_method.find("return false;", counted_prefix_flush) ==
                std::string::npos &&
            counted_loop_method.find(
                "return finish_scalar_guard();", counted_scalar_guard) !=
                std::string::npos &&
            counted_loop_method.find("account_prevalidated_unobserved_accesses(") ==
                std::string::npos &&
            counted_loop_method.find(
                "cpu_.attempted_guest_instructions += instructions_through_final_store") !=
                std::string::npos &&
            counted_loop_method.find(
                "cpu_.retired_guest_instructions += instructions_through_final_store") !=
                std::string::npos &&
            counted_loop_method.find(
                "commit_prepared_repeated_u32_sequence(") !=
                std::string::npos &&
            counted_loop_method.find(
                "synthetic_memory_accesses + admitted") !=
                std::string::npos &&
            counted_loop_main.find(
                "cpu_.active_instruction_physical_pc = "
                "store_contract->store_physical") !=
                std::string::npos &&
            counted_dynamic_kind != std::string::npos &&
            counted_dynamic_kind_store != std::string::npos &&
            counted_dynamic_kind_fallthrough != std::string::npos &&
            counted_fastpath_finalize != std::string::npos &&
            counted_dynamic_source != std::string::npos &&
            counted_dynamic_physical_source != std::string::npos &&
            counted_loop_call < counted_dynamic_kind &&
            counted_dynamic_kind < counted_dynamic_kind_store &&
            counted_dynamic_kind_store < counted_dynamic_kind_fallthrough &&
            counted_dynamic_kind_fallthrough < counted_fastpath_finalize &&
            counted_fastpath_finalize < counted_dynamic_source &&
            counted_dynamic_source < counted_dynamic_physical_source &&
            counted_dynamic_physical_source < ordinary_block_execute &&
            occurrences(counted_loop_method, "cpu_.memory.write_u32(") == 0u &&
            occurrences(counted_loop_method, "guest_write_u32_at(") == 0u &&
            occurrences(counted_loop_method, "state_.main_ram->write_u32(") == 0u &&
            occurrences(counted_loop_method,
                        "state_.cache_control->write_on_chip_ram(") == 0u &&
            counted_loop_method.find(
                "cpu_.active_instruction_pc = descriptor.store_instruction_address") !=
                std::string::npos &&
            counted_loop_method.find(
                "cpu_.active_instruction_physical_pc = "
                "store_contract->store_physical") !=
                std::string::npos,
        "Der produktive Counted-Loop-Pfad verliert Staged-Prefix, Post-Flush-Proof, "
        "exakte Event-/Budget-/Zaehlerformeln, atomaren Zeit-vor-Store-Commit oder "
        "dynamische Exit-Provenienz.");

    auto counted_loop_diagnostic_options = options;
    counted_loop_diagnostic_options.diagnostic_partial = true;
    const auto counted_loop_diagnostic_output = fixture.root / "counted-loop-diagnostic-port";
    static_cast<void>(export_dreamcast_port_project(
        counted_loop_gdi, counted_loop_diagnostic_output, counted_loop_diagnostic_options));
    const auto counted_loop_diagnostic_dispatch =
        read_text(counted_loop_diagnostic_output / "generated" / "code" / "runtime-dispatch.cpp");
    require(counted_loop_diagnostic_dispatch.find("std::array<CountedLoopBatchDescriptor, 0u> "
                                                  "counted_loop_batch_descriptors") !=
                    std::string::npos &&
                counted_loop_diagnostic_dispatch.find(counted_loop_descriptor) == std::string::npos,
            "Diagnose-/Interpreterports enthalten einen Produkt-Counted-Loop-Fastpath.");

    const auto counted_loop_disc =
        katana::platform::load_dreamcast_gdi_boot(counted_loop_gdi);
    const auto counted_loop_image =
        katana::platform::make_dreamcast_disc_executable(counted_loop_disc);
    const auto counted_loop_analysis =
        katana::analysis::analyze_control_flow(counted_loop_image);
    const auto counted_loop_program =
        katana::ir::lower_program(counted_loop_analysis);
    std::vector<katana::io::InputProvenance> counted_loop_inputs;
    const auto& counted_loop_source_descriptor =
        counted_loop_disc.source->descriptor();
    counted_loop_inputs.push_back(katana::io::capture_input_provenance(
        "gdi-descriptor", counted_loop_source_descriptor.resolved_path));
    for (const auto& track : counted_loop_source_descriptor.tracks)
        counted_loop_inputs.push_back(katana::io::capture_input_provenance(
            "gdi-track-" + std::to_string(track.number), track.resolved_path));
    const auto require_rejected_counted_loop_ir =
        [&](const std::string_view fixture_name,
            const std::uint32_t instruction_address,
            auto&& mutate) {
            auto negative_program = counted_loop_program;
            bool mutated = false;
            for (auto& function : negative_program) {
                for (auto& block : function.blocks) {
                    for (auto& instruction : block.instructions) {
                        if (instruction.source_address != instruction_address) continue;
                        mutate(instruction);
                        mutated = true;
                    }
                }
            }
            require(mutated,
                    "Counted-Loop-Negativfixture findet ihre Zielinstruktion nicht: " +
                        std::string(fixture_name));
            const auto fixture_identity =
                "counted-loop-" + std::string(fixture_name);
            const auto negative_output =
                fixture.root / (fixture_identity + "-port");
            static_cast<void>(export_dreamcast_port_project(
                {counted_loop_image,
                 counted_loop_analysis,
                 negative_program,
                 counted_loop_inputs,
                 katana::platform::dreamcast_disc_boot_address,
                 katana::platform::dreamcast_disc_boot_address,
                 counted_loop_disc.boot_file.size(),
                 fixture_identity},
                negative_output,
                options));
            const auto negative_dispatch = read_text(
                negative_output / "generated" / "code" / "runtime-dispatch.cpp");
            require(
                negative_dispatch.find(
                    "std::array<CountedLoopBatchDescriptor, 0u> "
                    "counted_loop_batch_descriptors") != std::string::npos &&
                    negative_dispatch.find(counted_loop_descriptor) ==
                        std::string::npos,
                "Counted-Loop-Negativfixture erzeugt einen Produkt-Fastpath: " +
                    std::string(fixture_name));
        };
    require_rejected_counted_loop_ir(
        "limit-width",
        0x8C010016u,
        [](auto& instruction) {
            require(instruction.memory_effects.width ==
                        katana::ir::OperandWidth::Bits16,
                    "Counted-Loop-Limitfixture beginnt nicht als 16-Bit-Read.");
            const auto region = instruction.memory_effects.region;
            instruction.operation =
                katana::ir::Operation::LoadByteSigned;
            instruction.original_operation = instruction.operation;
            instruction.widths =
                katana::ir::operation_operand_widths(instruction.operation);
            instruction.memory_effects = katana::ir::instruction_memory_effects(
                instruction.operation,
                instruction.destination_register,
                instruction.source_register);
            instruction.memory_effects.region = region;
        });
    for (const auto [name, address] :
         std::array{std::pair{"guard-counter-normal-ram", 0x8C010018u},
                    std::pair{"increment-counter-normal-ram", 0x8C010010u},
                    std::pair{"store-counter-normal-ram", 0x8C010014u}}) {
        require_rejected_counted_loop_ir(
            name,
            address,
            [](auto& instruction) {
                require(instruction.memory_effects.region !=
                            katana::ir::MemoryRegionKind::NormalRam,
                        "Dynamische Counted-Loop-Adresse ist unerwartet statisches RAM.");
                instruction.memory_effects.region =
                    katana::ir::MemoryRegionKind::NormalRam;
            });
    }
    for (const auto [name, address] :
         std::array{std::pair{"limit-no-cycle-flush", 0x8C010016u},
                    std::pair{"guard-counter-no-cycle-flush", 0x8C010018u},
                    std::pair{"increment-counter-no-cycle-flush", 0x8C010010u},
                    std::pair{"store-no-cycle-flush", 0x8C010014u}}) {
        require_rejected_counted_loop_ir(
            name,
            address,
            [](auto& instruction) {
                require(instruction.original_opcode != 0x0009u,
                        "Counted-Loop-Timingfixture ist bereits ein NOP.");
                instruction.original_opcode = 0x0009u;
            });
    }

    write_binary(counted_loop_disc_directory / "high.bin", counted_loop_boot_track(false));
    const auto rejected_counted_loop_output = fixture.root / "rejected-counted-loop-port";
    static_cast<void>(
        export_dreamcast_port_project(counted_loop_gdi, rejected_counted_loop_output, options));
    const auto rejected_counted_loop_dispatch =
        read_text(rejected_counted_loop_output / "generated" / "code" / "runtime-dispatch.cpp");
    require(rejected_counted_loop_dispatch.find("std::array<CountedLoopBatchDescriptor, 0u> "
                                                "counted_loop_batch_descriptors") !=
                    std::string::npos &&
                rejected_counted_loop_dispatch.find(counted_loop_descriptor) == std::string::npos,
            "Nicht-positive oder semantisch abweichende Counted-Loops wurden gebatcht.");

    const auto aliased_counted_loop_track = counted_loop_boot_track(true, true);
    const auto aliased_program_offset = payload_offset(21u);
    require(aliased_counted_loop_track[aliased_program_offset + 0x08u] == 0x1Cu &&
                aliased_counted_loop_track[aliased_program_offset + 0x09u] == 0x00u &&
                aliased_counted_loop_track[aliased_program_offset + 0x0Au] == 0x01u &&
                aliased_counted_loop_track[aliased_program_offset + 0x0Bu] == 0x8Du,
            "Mirror-Alias-Negativfixture enthaelt nicht die R15-Basis 0x8D01001C.");
    write_binary(counted_loop_disc_directory / "high.bin", aliased_counted_loop_track);
    const auto aliased_counted_loop_output = fixture.root / "aliased-counted-loop-port";
    static_cast<void>(
        export_dreamcast_port_project(counted_loop_gdi, aliased_counted_loop_output, options));
    const auto aliased_counted_loop_dispatch =
        read_text(aliased_counted_loop_output / "generated" / "code" / "runtime-dispatch.cpp");
    const auto aliased_counted_loop_main =
        read_text(aliased_counted_loop_output / "src" / "main.cpp");
    require(aliased_counted_loop_dispatch.find("std::array<CountedLoopBatchDescriptor, 1u> "
                                               "counted_loop_batch_descriptors") !=
                    std::string::npos &&
                aliased_counted_loop_dispatch.find(counted_loop_descriptor) != std::string::npos,
            "Mirror-Alias-Negativfixture verliert ihren statischen Counted-Loop-Descriptor.");
    require(
        aliased_counted_loop_main.find(
            ": dreamcast_main_ram_backing_offset(counter_physical, 4u)") !=
                std::string::npos &&
            aliased_counted_loop_main.find(
                "const auto limit_backing = dreamcast_main_ram_backing_offset(") !=
                std::string::npos &&
            aliased_counted_loop_main.find("*counter_backing < *limit_backing + limit_size &&") !=
                std::string::npos &&
            aliased_counted_loop_main.find("*limit_backing < *counter_backing + 4u;") !=
                std::string::npos,
        "Counter und Limit in verschiedenen Dreamcast-Mirrors desselben Main-RAM-Backings "
        "werden nicht vor dem Batch auf Backing-Overlap geprueft.");
    require(unit != generated_before.end(),
            "Portexport besitzt keine deterministische Translation Unit.");
    std::size_t entry_metadata_count = 0u;
    bool p2_pc_relative_literal = false;
    bool p2_pc_relative_literal_avoids_cycle_flush = false;
    for (const auto& [path, content] : generated_before) {
        if (path.starts_with("code/unit-") && path.ends_with(".cpp")) {
            require(content.find("generated_entry_address = 0xAC008300u") != std::string::npos,
                    "Portpartition besitzt einen abweichenden globalen Programmeinstieg.");
            p2_pc_relative_literal =
                p2_pc_relative_literal || content.find("0xAC008308u") != std::string::npos;
            const auto literal_instruction =
                content.find("// katana-guest 0xAC008300u");
            if (literal_instruction != std::string::npos) {
                const auto next_instruction =
                    content.find("// katana-guest ", literal_instruction + 1u);
                const auto emitted_literal = content.substr(
                    literal_instruction,
                    next_instruction == std::string::npos
                        ? std::string::npos
                        : next_instruction - literal_instruction);
                p2_pc_relative_literal_avoids_cycle_flush =
                    emitted_literal.find("guest_read_u32_at(cpu, guest_origin") !=
                        std::string::npos &&
                    emitted_literal.find(
                        "katana::runtime::flush_pending_guest_cycles(cpu, *services)") ==
                        std::string::npos;
            }
            constexpr std::string_view service_declaration =
                "_with_services(CpuState& cpu, PlatformServices* services);";
            std::size_t declaration_count = 0u;
            for (auto offset = content.find(service_declaration); offset != std::string::npos;
                 offset = content.find(service_declaration, offset + service_declaration.size()))
                ++declaration_count;
            require(declaration_count >= 1u && declaration_count <= 2u,
                    "Portpartition deklariert mehr als ihren lokalen Einstieg und den "
                    "bewiesenen direkten Callee.");
            ++entry_metadata_count;
        }
    }
    require(entry_metadata_count == 3u && p2_pc_relative_literal &&
                p2_pc_relative_literal_avoids_cycle_flush,
            "Mehrteiliger Portexport verliert P2-Einstieg, PC-relativen P2-Literalzugriff oder "
            "dessen linearen RAM-Beweis.");
    for (const auto& path : {"include/katana_port.hpp",
                              "include/runtime-dispatch-internal.hpp",
                              "code/runtime-dispatch.cpp",
                              "code/runtime-dispatch-shard-00000.cpp",
                              "metadata/port-project.json",
                             "metadata/provenance.json",
                             "metadata/source-map.json",
                             "metadata/cfg.json",
                             "metadata/cfg.dot",
                             "metadata/callgraph.json",
                             "metadata/callgraph.dot",
                             "katana-port.cmake"}) {
        require(generated_before.contains(path),
                "Portexport verliert Artefakt: " + std::string(path));
    }
    require(runtime_dispatch_shard_count == 1u &&
                generated_before.at("CMakeLists.txt")
                        .find("code/runtime-dispatch-shard-00000.cpp") != std::string::npos &&
                generated_before.at(".katana-generated-artifacts")
                        .find("code/runtime-dispatch-shard-00000.cpp") != std::string::npos &&
                generated_before.at("code/runtime-dispatch.cpp")
                        .find("runtime_dispatch_detail::append_static_blocks_shard_00000") !=
                    std::string::npos &&
                generated_before.at("code/runtime-dispatch.cpp").find("generated-block-") ==
                    std::string::npos &&
                runtime_dispatch_shards.find(
                    "const auto exception_generation_on_entry = "
                    "cpu.exception_generation;") != std::string::npos &&
                runtime_dispatch_shards.find(
                    "if (cpu.exception_generation != exception_generation_on_entry)") !=
                    std::string::npos &&
                runtime_dispatch_shards.find(
                    "cpu.exception_generation != exception_generation_on_entry &&") ==
                    std::string::npos &&
                runtime_dispatch_shards.find("cpu.trap_pending") == std::string::npos &&
                runtime_dispatch_shards.find("exception_active_on_entry") ==
                    std::string::npos &&
                occurrences(runtime_dispatch_shards,
                            "BlockExit dispatch_owner_8C010000(") == 1u &&
                occurrences(runtime_dispatch_shards, "&dispatch_owner_8C010000") == 3u,
            "Runtime-Dispatch ist nicht deterministisch geshardet, dupliziert Wrapper pro Block "
            "oder koppelt neue Exceptionkanten an einen persistenten Trapzustand.");
    const auto runtime_probe_function =
        generated_main.find("int run_deterministic_runtime_probe(");
    const auto runtime_probe_function_end =
        generated_main.find("\nstd::string redact_source(", runtime_probe_function);
    const auto runtime_probe_replay_attach =
        generated_main.find("state.scheduler->attach_replay_log(replay)", runtime_probe_function);
    const auto runtime_probe_scheduler_coverage =
        generated_main.find("SystemReplayCoverage::SchedulerCallback",
                            runtime_probe_replay_attach);
    const auto runtime_probe_audio =
        generated_main.find("katana::runtime::RecordingHostAudioOutput audio",
                            runtime_probe_scheduler_coverage);
    const auto runtime_probe_clock =
        generated_main.find("katana::runtime::DreamcastMediaClock media_clock",
                             runtime_probe_audio);
    const auto runtime_probe_media_coverage =
        generated_main.find("SystemReplayCoverage::Video",
                            runtime_probe_clock);
    const auto runtime_probe_mmio_trace =
        generated_main.find("RuntimeProbeMmioTraceSession mmio_trace",
                            runtime_probe_media_coverage);
    const auto runtime_probe_input_coverage =
        generated_main.find("SystemReplayCoverage::Input",
                            runtime_probe_mmio_trace);
    const auto runtime_probe_input_source =
        generated_main.find("input->inject(1u, state.scheduler->current_cycle(), {})",
                            runtime_probe_input_coverage);
    const auto runtime_probe_input_event =
        generated_main.find("replay.inject({", runtime_probe_input_source);
    const auto runtime_probe_clock_start =
        generated_main.find("media_clock.start()", runtime_probe_input_event);
    const auto runtime_probe_dispatch = generated_main.find(
        "katana_port_generated::run_runtime(cpu, services, *state.runtime_blocks,",
        runtime_probe_clock_start);
    const auto runtime_probe_budget_catch = generated_main.find(
        "catch (const katana::runtime::RuntimeProbeBudgetReached& reached)",
        runtime_probe_dispatch);
    const auto runtime_probe_dispatch_miss_catch = generated_main.find(
        "catch (const katana::runtime::IndirectDispatchError&)",
        runtime_probe_budget_catch);
    const auto runtime_probe_std_exception_catch = generated_main.find(
        "catch (const std::exception&)", runtime_probe_dispatch_miss_catch);
    const auto runtime_probe_catch_all =
        generated_main.find("catch (...)", runtime_probe_std_exception_catch);
    const auto runtime_probe_quiesce =
        generated_main.find("media_clock.stop();\n    mmio_trace.finish();",
                            runtime_probe_budget_catch);
    const auto runtime_probe_capture =
        generated_main.find("capture_runtime_probe_dreamcast(",
                            runtime_probe_quiesce);
    const auto runtime_probe_replay_seal =
        generated_main.find("replay.seal(provisional.hashes.guest_state)",
                            runtime_probe_capture);
    const auto runtime_probe_output =
        generated_main.find("std::cout << \"KATANA_RUNTIME_PROBE \"",
                            runtime_probe_replay_seal);
    const auto runtime_probe_serialize = generated_main.find(
        "serialize_runtime_probe_report_json(report)", runtime_probe_output);
    const auto runtime_probe_newline =
        generated_main.find("<< '\\n';", runtime_probe_serialize);
    const auto runtime_probe_return =
        generated_main.find("return 0;", runtime_probe_newline);
    const auto runtime_probe_branch =
        generated_main.find("if (deterministic_runtime_probe)");
    const auto runtime_probe_branch_call =
        generated_main.find("return run_deterministic_runtime_probe(",
                            runtime_probe_branch);
    const auto normal_controller_timeline =
        generated_main.find("katana::runtime::ControllerInputTimeline",
                            runtime_probe_branch_call);
    const auto normal_controller_attach =
        generated_main.find("katana::runtime::MapleControllerDevice>(\n"
                            "                controller_input)",
                            normal_controller_timeline);
    const auto normal_native_video =
        generated_main.find("katana::runtime::native_video_available()",
                            normal_controller_attach);
    const auto normal_native_gamepad =
        generated_main.find("katana::runtime::create_native_gamepad_source()",
                            normal_native_video);
    const auto normal_native_audio =
        generated_main.find("katana::runtime::native_audio_available()",
                            normal_native_gamepad);
    const auto normal_host_runtime =
        generated_main.find("katana::runtime::HostRuntimeSession host",
                            normal_native_audio);
    const auto normal_frame_pump =
        generated_main.find("const auto pump_guest_frame = [&] {",
                            normal_host_runtime);
    const auto normal_frame_proof =
        generated_main.find("pump_guest_frame_proof(", normal_frame_pump);
    const auto normal_host_event_pump =
        generated_main.find("const auto pump_host_events = [&] {",
                            normal_frame_proof);
    const auto normal_focus_reset =
        generated_main.find("controller_input->set_focus(false, guest_cycle)",
                            normal_host_event_pump);
    const auto normal_keyboard_input =
        generated_main.find("controller_input->keyboard_event(",
                            normal_focus_reset);
    const auto normal_gamepad_poll =
        generated_main.find("controller_input->poll(",
                            normal_keyboard_input);
    const auto normal_runtime_dispatch = generated_main.find(
        "katana_port_generated::run_runtime(cpu, services, *state.runtime_blocks,",
        normal_gamepad_poll);
    const auto dispatch_probe_profile =
        runtime_dispatch.find("std::getenv(\"KATANA_RUNTIME_PROBE\")");
    const auto dispatch_probe_determinism =
        runtime_dispatch.find("materialization_policy.deterministic_no_host_time = true",
                              dispatch_probe_profile);
    const auto dispatch_probe_budget_catch = runtime_dispatch.find(
        "catch (const katana::runtime::RuntimeProbeBudgetReached&)");
    const auto dispatch_generic_catch =
        runtime_dispatch.find("catch (...)", dispatch_probe_budget_catch);
    require(
        generated_main.find("#include \"katana/runtime/runtime_probe.hpp\"") !=
                std::string::npos &&
            occurrences(generated_main,
                        "std::getenv(\"KATANA_RUNTIME_PROBE\")") == 1u &&
            generated_main.find(
                "std::string_view(value) == \"deterministic-v1\"") !=
                std::string::npos &&
            generated_main.find("runtime-probe-profile-invalid") !=
                std::string::npos &&
            generated_main.find(
                "std::string_view(diagnostics) != \"0\"") !=
                std::string::npos &&
            generated_main.find(
                "std::string_view(diagnostics) != \"1\"") !=
                std::string::npos &&
            generated_main.find(
                "guest_cycle_budget_from_environment().has_value()") !=
                std::string::npos &&
            generated_main.find("runtime-probe-budget-required") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_WAIT_LOOP_TRACE\"") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_DIAGNOSTICS_FULL\"") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_PROGRESS_INTERVAL\"") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_LIFECYCLE_TEST\"") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_BLOCK_LIMIT\"") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_IGNORE_FOCUS\"") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_CONTROLLER_TEST\"") !=
                std::string::npos &&
            generated_main.find("\"KATANA_PORT_MEMORY_PROBES\"") !=
                std::string::npos &&
            generated_main.find("runtime-probe-environment-conflict") !=
                std::string::npos,
        "Deterministische Runtime-Probe besitzt kein exaktes Profil oder keine "
        "geschlossene Umgebungsvalidierung.");
    require(
        runtime_probe_function != std::string::npos &&
            runtime_probe_function_end != std::string::npos &&
            runtime_probe_replay_attach != std::string::npos &&
            runtime_probe_scheduler_coverage != std::string::npos &&
            runtime_probe_audio != std::string::npos &&
            runtime_probe_clock != std::string::npos &&
            runtime_probe_media_coverage != std::string::npos &&
            runtime_probe_mmio_trace != std::string::npos &&
            runtime_probe_input_coverage != std::string::npos &&
            runtime_probe_input_source != std::string::npos &&
            runtime_probe_input_event != std::string::npos &&
            runtime_probe_clock_start != std::string::npos &&
            runtime_probe_dispatch != std::string::npos &&
            runtime_probe_budget_catch != std::string::npos &&
            runtime_probe_dispatch_miss_catch != std::string::npos &&
            runtime_probe_std_exception_catch != std::string::npos &&
            runtime_probe_catch_all != std::string::npos &&
            runtime_probe_quiesce != std::string::npos &&
            runtime_probe_capture != std::string::npos &&
            runtime_probe_replay_seal != std::string::npos &&
            runtime_probe_output != std::string::npos &&
            runtime_probe_serialize != std::string::npos &&
            runtime_probe_newline != std::string::npos &&
            runtime_probe_return != std::string::npos &&
            runtime_probe_function < runtime_probe_replay_attach &&
            runtime_probe_replay_attach < runtime_probe_scheduler_coverage &&
            runtime_probe_scheduler_coverage < runtime_probe_audio &&
            runtime_probe_replay_attach < runtime_probe_audio &&
            runtime_probe_audio < runtime_probe_clock &&
            runtime_probe_clock < runtime_probe_media_coverage &&
            runtime_probe_media_coverage < runtime_probe_mmio_trace &&
            runtime_probe_clock < runtime_probe_mmio_trace &&
            runtime_probe_mmio_trace < runtime_probe_input_coverage &&
            runtime_probe_input_coverage < runtime_probe_input_source &&
            runtime_probe_input_source < runtime_probe_input_event &&
            runtime_probe_input_event < runtime_probe_clock_start &&
            runtime_probe_clock < runtime_probe_clock_start &&
            runtime_probe_clock_start < runtime_probe_dispatch &&
            runtime_probe_dispatch < runtime_probe_budget_catch &&
            runtime_probe_budget_catch < runtime_probe_dispatch_miss_catch &&
            runtime_probe_dispatch_miss_catch < runtime_probe_std_exception_catch &&
            runtime_probe_std_exception_catch < runtime_probe_catch_all &&
            runtime_probe_budget_catch < runtime_probe_quiesce &&
            runtime_probe_quiesce < runtime_probe_capture &&
            runtime_probe_capture < runtime_probe_replay_seal &&
            runtime_probe_replay_seal < runtime_probe_output &&
            runtime_probe_output < runtime_probe_serialize &&
            runtime_probe_serialize < runtime_probe_newline &&
            runtime_probe_newline < runtime_probe_return &&
            runtime_probe_return < runtime_probe_function_end &&
            generated_main.find("create_native_video_output",
                                runtime_probe_function) >
                runtime_probe_function_end &&
            generated_main.find("create_native_audio_output",
                                runtime_probe_function) >
                runtime_probe_function_end &&
            generated_main.find("create_native_gamepad_source",
                                runtime_probe_function) >
                runtime_probe_function_end &&
            occurrences(generated_main,
                        "std::cout << \"KATANA_RUNTIME_PROBE \"") == 1u &&
            generated_main.find(
                "std::cerr << \"KATANA_RUNTIME_PROBE \"") ==
                std::string::npos &&
            generated_main.find(
                "throw katana::runtime::RuntimeProbeBudgetReached("
                "result.guest_cycle)") != std::string::npos &&
            generated_main.find(
                "termination = "
                "katana::runtime::RuntimeProbeTermination::BudgetReached",
                runtime_probe_budget_catch) != std::string::npos &&
             generated_main.find(
                 "report.guest_cycle != *report.guest_cycle_budget",
                 runtime_probe_budget_catch) != std::string::npos &&
            generated_main.find(
                "SystemReplayProfile::DeterministicV1",
                runtime_probe_function) != std::string::npos &&
            generated_main.find(
                "SystemReplayStorageMode::DigestStream",
                runtime_probe_function) != std::string::npos &&
            generated_main.find(
                "SystemReplayConfig::default_capacity",
                runtime_probe_function) != std::string::npos &&
            generated_main.find(
                "system_replay_mmio_observer(") != std::string::npos &&
            generated_main.find(
                "SystemReplayEventKind::Dma") != std::string::npos &&
            generated_main.find(
                "\"neutral-controller-input\"",
                runtime_probe_function) != std::string::npos &&
            occurrences(generated_main, "enable_coverage(") == 5u &&
            generated_main.find(
                "enable_coverage(replay.required_coverage())") == std::string::npos &&
            generated_main.find("SystemReplayCoverage::CpuSafepoint") !=
                std::string::npos &&
            generated_main.find("SystemReplayCoverage::AcceptedInterrupt") !=
                std::string::npos &&
            generated_main.find("SystemReplayCoverage::Dma") != std::string::npos &&
            generated_main.find("SystemReplayCoverage::Audio") != std::string::npos &&
            generated_main.find("SystemReplayCoverage::Mmio") != std::string::npos,
        "Runtime-Probe bindet Replay, typed Budget-Exit oder genau eine Ergebniszeile "
        "nicht deterministisch beziehungsweise quiesziert vor dem Seal nicht.");
    require(
        generated_before.at("include/katana_port.hpp")
                    .find("SystemReplayObservationSession& observations") !=
                std::string::npos &&
            generated_main.find(
                "SystemReplayObservationSession replay_observations_") !=
                std::string::npos &&
            generated_main.find("RuntimeProbeObservationState probe_observations_") !=
                std::string::npos &&
            occurrences(generated_main, "services.observe_runtime_started();") == 2u &&
            generated_main.find("runtime_probe_checkpoint_line_prefix") !=
                std::string::npos &&
            generated_main.find("serialize_checkpoint_json()") != std::string::npos &&
            generated_main.find("runtime_probe_fault_line_prefix") !=
                std::string::npos &&
            generated_main.find("serialize_runtime_probe_fault_envelope_json(") !=
                std::string::npos &&
            generated_main.find("RuntimeProbeTermination::DispatchMiss") !=
                std::string::npos &&
            generated_main.find("RuntimeProbeTermination::Failed") != std::string::npos &&
            generated_main
                    .substr(runtime_probe_std_exception_catch,
                            runtime_probe_catch_all -
                                runtime_probe_std_exception_catch)
                    .find("cpu.last_exception_cause") == std::string::npos &&
            generated_main
                    .substr(runtime_probe_std_exception_catch,
                            runtime_probe_catch_all -
                                runtime_probe_std_exception_catch)
                    .find("RuntimeProbeTermination::Failed") !=
                std::string::npos &&
            generated_main.find("replay_observations_.observe_controlled_fallback()") !=
                std::string::npos &&
            generated_main.find(
                "SystemReplayCheckpointKind::GuestProgramEntered") !=
                std::string::npos &&
            generated_main.find(
                "constexpr katana::runtime::GuestProgramRange "
                "expected_guest_program_range") !=
                std::string::npos &&
            generated_main.find(
                "guest_program_range_matcher_.contains_instruction(") !=
                std::string::npos &&
            generated_main.find(
                "GuestProgramRangeMatcher "
                "guest_program_range_matcher_{expected_guest_program_range}") !=
                std::string::npos &&
            generated_main.find(
                "if (address != " +
                std::to_string(
                    katana::platform::dreamcast_system_bootstrap_entry_address) +
                "u)") == std::string::npos &&
            runtime_dispatch.find(
                "SystemReplayObservationSession* active_observations") !=
                std::string::npos &&
            runtime_dispatch.find(
                "dispatch_class, result.materialized)") !=
                std::string::npos &&
            runtime_dispatch.find(
                "materializer.reconcile_inactive_origins(&dispatch_metrics)") !=
                std::string::npos &&
            runtime_dispatch.find(
                "active_observations->observe_block_dispatch_miss(") !=
                std::string::npos &&
            runtime_dispatch.find("*active_dispatch_metrics") != std::string::npos &&
            runtime_dispatch.find(
                "active_observations->observe_guest_exception("
                "cpu.last_exception_cause)") != std::string::npos &&
            runtime_dispatch.find(
                "ServiceScope scope(\n"
                "        services, table, context, diagnostics, dispatch_metrics,\n"
                "        observations, materializer, registered_game_project)") !=
                std::string::npos,
        "Generierter Produktpfad verdrahtet Dispatch, Fallback, Guest-Exception, "
        "Checkpoint oder redigiertes Fehlerpaket nicht ueber eine zentrale Observation-Session.");
    require(
        runtime_probe_branch != std::string::npos &&
            runtime_probe_branch_call != std::string::npos &&
            normal_controller_timeline != std::string::npos &&
            normal_controller_attach != std::string::npos &&
            normal_native_video != std::string::npos &&
            normal_native_gamepad != std::string::npos &&
            normal_native_audio != std::string::npos &&
            normal_host_runtime != std::string::npos &&
            normal_frame_pump != std::string::npos &&
            normal_frame_proof != std::string::npos &&
            normal_host_event_pump != std::string::npos &&
            normal_focus_reset != std::string::npos &&
            normal_keyboard_input != std::string::npos &&
            normal_gamepad_poll != std::string::npos &&
            normal_runtime_dispatch != std::string::npos &&
            generated_main.find("#include \"katana/runtime/host_input.hpp\"") !=
                std::string::npos &&
            generated_main.find("else if (lifecycle_test.empty() &&\n"
                                "            katana::runtime::"
                                "native_gamepad_input_available())") !=
                std::string::npos &&
            generated_main.find("class ControllerContractGamepadSource final") !=
                std::string::npos &&
            generated_main.find("controller-contract-change-dedup-mismatch") !=
                std::string::npos &&
            generated_main.find("controller-contract-maple-mismatch") !=
                std::string::npos &&
            generated_main.find("controller_contract=", normal_host_event_pump) !=
                std::string::npos &&
            generated_main.find("if (gamepad_source && controller_input->focused() &&") !=
                std::string::npos &&
            generated_main.find("controller_changes=", normal_host_event_pump) !=
                std::string::npos &&
            generated_main.find("const auto bit = event.key") == std::string::npos &&
            runtime_probe_branch < runtime_probe_branch_call &&
            runtime_probe_branch_call < normal_controller_timeline &&
            normal_controller_timeline < normal_controller_attach &&
            normal_controller_attach < normal_native_video &&
            normal_native_video < normal_native_gamepad &&
            normal_native_gamepad < normal_native_audio &&
            normal_native_audio < normal_host_runtime &&
            normal_host_runtime < normal_frame_pump &&
            normal_frame_pump < normal_frame_proof &&
            normal_frame_proof < normal_host_event_pump &&
            normal_host_event_pump < normal_focus_reset &&
            normal_focus_reset < normal_keyboard_input &&
            normal_keyboard_input < normal_gamepad_poll &&
            normal_frame_proof < normal_runtime_dispatch &&
            dispatch_probe_profile != std::string::npos &&
            dispatch_probe_determinism != std::string::npos &&
            dispatch_probe_budget_catch != std::string::npos &&
            dispatch_generic_catch != std::string::npos &&
            dispatch_probe_profile < dispatch_probe_determinism &&
            dispatch_probe_budget_catch < dispatch_generic_catch,
        "Runtime-Probe liegt nicht vor nativen Hostbackends oder der normale Produktpfad "
        "verliert Timeline-, Gamepad-, Keyboard-, Fokus-, Frame- oder Dispatchintegration.");
    const auto memory_probe_begin = generated_main.find(
        "if (const auto* probes = std::getenv(\"KATANA_PORT_MEMORY_PROBES\");");
    const auto memory_probe_end =
        generated_main.find("std::cerr << '\\n';", memory_probe_begin);
    const auto memory_probe_source =
        memory_probe_begin == std::string::npos || memory_probe_end == std::string::npos
            ? std::string_view{}
            : std::string_view{generated_main}.substr(memory_probe_begin,
                                                      memory_probe_end - memory_probe_begin);
    require(
        generated_before.at("katana-port.cmake").find("add_executable(synthetic_game") !=
                std::string::npos &&
            generated_before.at("katana-port.cmake")
                    .find("${KATANA_PORT_RUNTIME_TARGET}") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("dispatch_indirect") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("execute_dynamic_sh4_block(cpu, *active_services, 1u)") ==
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("dynamic_interpreter.hpp") == std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("dispatch_dynamic_interpreter") == std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("runtime-sh4-interpreter") == std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("materialization_policy.enabled = false") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("native_aot_template.hpp") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("NativeAotTemplateBinder native_aot_binder") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("target, physical_origin, bytes, variant)") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("materialization_policy, {}") == std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("count < 64u") ==
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("candidate.instructions = 1u") == std::string::npos &&
            runtime_dispatch_shards
                    .find("generated-block-AC008300") != std::string::npos &&
            p2_registration.find(
                "katana::runtime::ExecutableBlockTimingClass::LinearRamOnly") !=
                std::string::npos &&
            generated_before.at("metadata/port-project.json")
                    .find("\"execution_coverage_contract\":"
                          "\"native-aot-or-typed-abort-v1\"") !=
                std::string::npos &&
            generated_before.at("metadata/port-project.json")
                    .find("\"dispatch_paths_without_validation\":0") != std::string::npos &&
            generated_before.at("metadata/port-project.json")
                    .find("\"execution_profile\":\"native-aot-product\"") !=
                std::string::npos &&
            generated_before.at("metadata/port-project.json")
                    .find("\"runtime_interpreter_enabled\":false") != std::string::npos &&
            generated_before.at("metadata/port-project.json")
                    .find("\"unbound_code_policy\":\"typed-materialization-error\"") !=
                std::string::npos &&
            runtime_dispatch_shards.find("generated-block-8C010000") !=
                std::string::npos &&
            runtime_dispatch_shards
                    .find("register_executable_block(table, services, 0x8C010000u") !=
                std::string::npos &&
            runtime_dispatch_shards
                    .find("services.allow_executable_block_chaining(") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("executed_dispatch_blocks >= block_budget") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("KATANA_PORT_BLOCK_PROGRESS") !=
                std::string::npos &&
            runtime_dispatch_shards
                    .find("append_static_block(blocks, 0x8C010000u") != std::string::npos &&
            runtime_dispatch_shards.find("static_blocks.push_back({") ==
                std::string::npos &&
            runtime_dispatch_shards
                    .find("if (const auto registered_handle = table.lookup(0x8C010000u") ==
                std::string::npos &&
            runtime_dispatch_shards
                    .find("6u, katana::runtime::BlockEndKind::Call") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("SLEEP besitzt kein Wakeup-Ereignis") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("Schedulerbudget erschoepft") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("throw katana::runtime::GuestCycleBudgetReached(") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("Runtime-Blockbudget erschoepft") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("KATANA_PORT_BLOCK_LIMIT") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("DispatchChainBoundary::ProgramRoot") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("cpu.pc == program_return_sentinel") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("target = cpu.pc;\n"
                          "            dispatch_callsite = exit.source.virtual_address;\n"
                          "            dispatch_source = exit.source;\n"
                          "            kind = "
                          "katana::runtime::IndirectDispatchKind::TailJump") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("DispatchChainBoundary::NestedCall") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("poll_host_lifecycle") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("PlatformShutdownRequested") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("blocks < 1000000u") ==
                std::string::npos &&
            generated_before.at("include/katana_port.hpp").find("runtime_only_profile_json") !=
                std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("runtime_only_dispatch_share_ppm") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("KATANA_RUNTIME_DISPATCH_DIAGNOSTICS") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("KATANA_PORT_DIAGNOSTICS_FULL") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp")
                    .find("KATANA_RUNTIME_DISPATCH_EVENTS") != std::string::npos &&
            generated_before.at("code/runtime-dispatch.cpp").find("serialize_json(true)") !=
                std::string::npos &&
            std::filesystem::exists(output / "CMakeLists.txt") &&
            read_text(output / "CMakeLists.txt").find("katana_core") == std::string::npos &&
            std::filesystem::exists(output / "src" / "main.cpp") &&
            read_text(output / "src" / "main.cpp")
                    .find("DreamcastRuntimeFirmwareMode::HleBiosAbi") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("DreamcastConsoleProfile::JapanNtsc") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("KATANA_PLATFORM_LIFECYCLE_EXIT") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("KATANA_GDROM_BIOS_EVENTS") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("KATANA_PORT_MEMORY_PROBES") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("memory_probe_value=") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("peek_guest_u32") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("pvr_registers->snapshot()") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("system_bus_control->snapshot()") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("pvr_registers->read(") ==
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("system_bus_control->read(") ==
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("std::array<const katana::runtime::MemoryDevice*, 3u>") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("state.flash.get()};") ==
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("cpu.memory.read_u32(address)") == std::string::npos &&
            memory_probe_begin != std::string::npos &&
            memory_probe_end != std::string::npos &&
            memory_probe_source.find("translate_guest_address") == std::string_view::npos &&
            read_text(output / "src" / "main.cpp").find("load_dreamcast_runtime_boot") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("source.info().content_identity != expected_content_identity") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("verify_boot_identity(boot)") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("create_native_video_output") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("pump_host_events") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("next_lifecycle_poll_") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("KATANA_PORT_BLOCK_LIMIT\") == nullptr") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("KATANA_PORT_BLOCK_LIMIT\") == nullptr &&") == std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("std::getenv(\"KATANA_PORT_PROGRESS_INTERVAL\") == nullptr") ==
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("can_chain_executable_block(std::uint32_t address)") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("registration == nullptr || !registration->chainable") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("active_block_variant_->runtime_generation") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("cpu_.active_block_virtual_start = virtual_start") ==
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("const auto pending_guest_cycles = cpu_.pending_guest_cycles") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("inspected.physical_address) != registration->physical_origin") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("prove_instruction_mapping(") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("katana::runtime::translate_guest_address(") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("cpu.attempted_guest_instructions") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("ExecutableBlockTimingClass::PureCpu") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("local_block_chain_guest_cycle_budget = 4'096u") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("pending_guest_cycles + registration->maximum_guest_cycles") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("prospective_guest_cycles > *remaining") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("*event <= current_cycle + prospective_guest_cycles") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("result.processed_events != 0u") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("!lifecycle_test.empty()") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KR_HOST_SHUTDOWN") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("pump_guest_frame_proof") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("guest_frame_evidence.observe(") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KATANA_PORT_PROGRESS") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("set_mmio_access_tracking") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("highest_pending") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("runtime_materialization_status") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("retained_validation_bytes=") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("peak_retained_validation_bytes=") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("reclaimed_validation_bytes=") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("exception_cause=") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("cpu.expevt") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("cpu.spc") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("retired_guest_instructions=") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("register_index < cpu.r.size()") != std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("cpu.r[register_index]") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("cpu.read_sr()") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("cpu.read_fpscr()") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("framebuffer.configure(640u") ==
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("pump_guest_frame") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("result.frame_presented") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KR_FIRST_GUEST_FRAME") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KR_FIRST_GUEST_SCANOUT") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KR_FIRST_TA_FRAME") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("KR_FIRST_POST_BOOTSTRAP_TA_FRAME") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KR_FIRST_PRESENTED_FRAME") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("GuestFrameEvidenceTracker") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("runtime_observer->guest_program_progressed()") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("retired_delta == 0u || new_exception ||") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("!services.guest_program_progressed()") != std::string::npos &&
            normal_frame_proof < normal_runtime_dispatch &&
            read_text(output / "src" / "main.cpp").find("HostRuntimeSession") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("DreamcastMutableStorage::open") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("HostPacer") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("mutable_storage->save") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("KATANA_HOST_PACING_ERROR") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("KATANA_STORE_QUEUE_PREFETCH_REJECTED") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp")
                    .find("KATANA_PVR_RENDER_FAILED") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("audio_hash") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("source-identity-mismatch") !=
                std::string::npos &&
            read_text(output / "src" / "main.cpp").find("weakly_canonical") != std::string::npos &&
            read_text(output / "src" / "main.cpp").find("source.parent_path().string()") ==
                std::string::npos,
        "Portprojekt besitzt keinen ausfuehrbaren GDI-/Runtimevertrag.");
    auto diagnostic_options = options;
    diagnostic_options.diagnostic_partial = true;
    const auto diagnostic_output = fixture.root / "diagnostic-port";
    static_cast<void>(export_dreamcast_port_project(gdi, diagnostic_output, diagnostic_options));
    const auto diagnostic_dispatch =
        read_text(diagnostic_output / "generated" / "code" / "runtime-dispatch.cpp");
    require(diagnostic_dispatch.find("dynamic_interpreter.hpp") != std::string::npos &&
                diagnostic_dispatch.find(
                    "execute_dynamic_sh4_block(cpu, *active_services, 1u)") !=
                    std::string::npos &&
                diagnostic_dispatch.find("runtime-sh4-interpreter") != std::string::npos &&
                diagnostic_dispatch.find("candidate.interpreter_backed = true") !=
                    std::string::npos &&
                diagnostic_dispatch.find("materialization_policy.enabled = true") !=
                    std::string::npos &&
                read_text(diagnostic_output / "generated" / "metadata" /
                          "port-project.json")
                        .find("\"diagnostic_partial\":true") != std::string::npos &&
                read_text(diagnostic_output / "generated" / "metadata" /
                          "port-project.json")
                        .find("\"execution_profile\":\"diagnostic-interpreter\"") !=
                    std::string::npos &&
                read_text(diagnostic_output / "generated" / "metadata" /
                          "port-project.json")
                        .find("\"runtime_interpreter_enabled\":true") !=
                    std::string::npos &&
                read_text(diagnostic_output / "generated" / "metadata" /
                          "port-project.json")
                        .find("\"execution_coverage_contract\":"
                              "\"diagnostic-validated-demand-v1\"") !=
                    std::string::npos,
            "Explizites Diagnoseprofil besitzt keinen klar isolierten SH-4-Interpreter.");
    const auto retire_marker = runtime_dispatch.find(
        "const auto retired_before = cpu.retired_guest_instructions");
    const auto execute_marker = runtime_dispatch.find(
        "katana::runtime::execute_runtime_block(", retire_marker);
    const auto scheduler_marker =
        runtime_dispatch.find("katana::runtime::finalize_guest_block(", execute_marker);
    const auto interrupt_marker =
        runtime_dispatch.find("completion.interrupt.has_value()", scheduler_marker);
    require(retire_marker != std::string::npos && execute_marker != std::string::npos &&
                scheduler_marker != std::string::npos && interrupt_marker != std::string::npos &&
                retire_marker < execute_marker && execute_marker < scheduler_marker &&
                scheduler_marker < interrupt_marker,
            "Gastzyklen oder Interruptannahme liegen nicht hinter der ausgefuehrten Blocksemantik.");
    const auto fallthrough_stall_marker = runtime_dispatch.find(
        "exit.kind == katana::runtime::BlockEndKind::Fallthrough", execute_marker);
    const auto source_relative_marker = runtime_dispatch.find(
        "exit.source.virtual_address + 2u", execute_marker);
    const auto exact_target_marker = runtime_dispatch.find(
        "exit.target->virtual_address != expected_fallthrough", fallthrough_stall_marker);
    const auto stale_entry_comparison = runtime_dispatch.find("cpu.pc == block_entry_pc");
    require(source_relative_marker != std::string::npos &&
                fallthrough_stall_marker != std::string::npos &&
                exact_target_marker != std::string::npos &&
                stale_entry_comparison == std::string::npos &&
                execute_marker < source_relative_marker &&
                source_relative_marker < fallthrough_stall_marker &&
                fallthrough_stall_marker < exact_target_marker &&
                exact_target_marker < scheduler_marker,
            "Portdispatch prueft den exakten Fallthrough nicht relativ zur zuletzt ausgefuehrten "
            "Quellinstruktion oder verwechselt ihn mit dem Root-Blockeintritt.");
    const auto progress_marker =
        runtime_dispatch.find("executed_dispatch_blocks % progress_interval");
    const auto root_dispatch_marker = runtime_dispatch.find(
        "auto result = katana::runtime::dispatch_indirect", progress_marker);
    const auto root_begin_marker =
        runtime_dispatch.find("active_services->begin_executable_block", root_dispatch_marker);
    require(progress_marker != std::string::npos && root_dispatch_marker != std::string::npos &&
                root_begin_marker != std::string::npos && progress_marker < root_dispatch_marker &&
                root_dispatch_marker < root_begin_marker,
            "Portfortschritt liegt nicht am kontrollierten Root-Dispatch-Safepoint.");
    const auto callsite_state_marker =
        runtime_dispatch.find("std::uint32_t dispatch_callsite = cpu.pc");
    const auto attributed_request_marker = runtime_dispatch.find(
        "{kind, dispatch_callsite, target, cpu.pr, dispatch_source", callsite_state_marker);
    const auto site_reset_marker = runtime_dispatch.find(
        "active_exit_site_class = katana::runtime::DynamicDispatchSiteClass::NotDynamic",
        attributed_request_marker);
    const auto continuation_marker = runtime_dispatch.find(
        "make_indirect_dispatch_continuation", site_reset_marker);
    const auto continuation_callsite_marker = runtime_dispatch.find(
        "dispatch_callsite = continuation.callsite", continuation_marker);
    require(callsite_state_marker != std::string::npos &&
                attributed_request_marker != std::string::npos &&
                site_reset_marker != std::string::npos &&
                continuation_marker != std::string::npos &&
                continuation_callsite_marker != std::string::npos &&
                callsite_state_marker < attributed_request_marker &&
                attributed_request_marker < site_reset_marker &&
                site_reset_marker < continuation_marker &&
                continuation_marker < continuation_callsite_marker,
            "Dynamische Portkette verliert Terminator-Callsite oder RuntimeOnly-Klasse.");
    const auto require_chain_registration = [&](const std::string_view end_kind,
                                                const bool expected) {
        const auto marker = ", katana::runtime::BlockEndKind::" + std::string(end_kind);
        const auto marker_position = runtime_dispatch_shards.find(marker);
        require(marker_position != std::string::npos,
                "Portfixture besitzt keine Endklasse " + std::string(end_kind) + ".");
        const auto address_begin = runtime_dispatch_shards.rfind("0x", marker_position);
        const auto address_end = runtime_dispatch_shards.find('u', address_begin);
        require(address_begin != std::string::npos && address_end != std::string::npos,
                "Portfixture verliert die Blockadresse vor " + std::string(end_kind) + ".");
        const auto address =
            runtime_dispatch_shards.substr(address_begin, address_end - address_begin);
        const auto registration =
            "services.allow_executable_block_chaining(" + address + "u)";
        require((runtime_dispatch_shards.find(registration) != std::string::npos) == expected,
                "Lokales Chaining behandelt Endklasse " + std::string(end_kind) +
                    " nicht als Hostgrenze.");
    };
    require_chain_registration("Call", true);
    require_chain_registration("Return", false);
    std::string portable_content;
    for (const auto& [path, content] : generated_before) {
        static_cast<void>(path);
        portable_content += content;
    }
    require(portable_content.find(fixture.root.string()) == std::string::npos &&
                portable_content.find("disc.gdi") == std::string::npos &&
                portable_content.find("high.bin") == std::string::npos,
            "Portartefakte enthalten absolute oder private Disc-/Trackpfade.");

    const auto guarded_disc = katana::platform::load_dreamcast_gdi_boot(gdi);
    const auto native_boot_image = katana::platform::make_dreamcast_disc_executable(
        guarded_disc,
        katana::platform::DreamcastDiscExecutionPath::NativeSystemBootstrap);
    require(native_boot_image.entry_points().size() == 2u &&
                native_boot_image.initial_snapshot_entry() ==
                    katana::platform::dreamcast_system_bootstrap_entry_address,
            "Native Disc-AOT-Wurzeln verlieren den ausgezeichneten Bootstrap-Snapshotentry.");
    auto guarded_image = katana::platform::make_dreamcast_disc_executable(guarded_disc);
    const auto guarded_boot_segment = std::find_if(
        guarded_image.segments().begin(), guarded_image.segments().end(), [](const auto& segment) {
            return segment.virtual_address == katana::platform::dreamcast_disc_boot_address;
        });
    require(guarded_image.segments().size() == 2u &&
                guarded_boot_segment != guarded_image.segments().end() &&
                guarded_boot_segment->kind == katana::io::SegmentKind::Mixed &&
                guarded_boot_segment->source_kind ==
                    katana::io::ImageSourceKind::DiscBootFile &&
                guarded_boot_segment->bytes.size() == guarded_boot_segment->memory_size,
            "GDI-Loader markiert die Bootdatei pauschal als Code oder erfindet Zero-Fill.");
    auto guarded_analysis = katana::analysis::analyze_control_flow(guarded_image);
    require(!guarded_analysis.indirect_control_flow.empty() &&
                !guarded_analysis.resolved_edges.empty(),
            "Guarded-Portfixture besitzt keine indirekte Kandidatenkante.");
    guarded_analysis.indirect_control_flow.front().status =
        katana::analysis::ResolutionStatus::Guarded;
    guarded_analysis.indirect_control_flow.front().evidence =
        katana::analysis::ControlFlowEvidence::GuardedComplete;
    guarded_analysis.resolved_edges.front().guarded = true;
    auto guarded_program = katana::ir::lower_program(guarded_analysis);
    std::vector<katana::io::InputProvenance> guarded_inputs;
    guarded_inputs.push_back(katana::io::capture_input_provenance("gdi-descriptor", gdi));
    for (const auto& track : guarded_disc.source->descriptor().tracks)
        guarded_inputs.push_back(katana::io::capture_input_provenance(
            "gdi-track-" + std::to_string(track.number), track.resolved_path));
    const auto guarded_output = fixture.root / "guarded-port";
    const auto guarded_export =
        export_dreamcast_port_project({guarded_image,
                                       guarded_analysis,
                                       guarded_program,
                                       guarded_inputs,
                                       katana::platform::dreamcast_disc_boot_address,
                                       katana::platform::dreamcast_disc_boot_address,
                                       guarded_disc.boot_file.size(),
                                       "guarded-fixture"},
                                      guarded_output,
                                      options);
    const auto guarded_sources = snapshot(guarded_output / "generated");
    std::string guarded_text;
    for (const auto& [path, content] : guarded_sources) {
        if (path.starts_with("code/unit-")) guarded_text += content;
    }
    const auto& guarded_metadata = guarded_sources.at("metadata/port-project.json");
    require(guarded_export.functions != 0u && guarded_text.find("default:") != std::string::npos &&
                guarded_text.find("unresolved_call") != std::string::npos &&
                guarded_metadata.find("\"guarded_control_flow\":1") != std::string::npos &&
                guarded_metadata.find("\"guarded_complete_control_flow\":1") != std::string::npos &&
                guarded_metadata.find("\"guarded_partial_control_flow\":0") != std::string::npos &&
                guarded_metadata.find("\"unresolved_control_flow\":0") != std::string::npos,
            "Guarded-Kandidaten erreichen Portcodegen oder dynamischen Default nicht.");

    auto runtime_only_analysis = guarded_analysis;
    auto& runtime_only_resolution = runtime_only_analysis.indirect_control_flow.front();
    runtime_only_resolution.status = katana::analysis::ResolutionStatus::Unresolved;
    runtime_only_resolution.evidence = katana::analysis::ControlFlowEvidence::RuntimeOnly;
    runtime_only_resolution.target.reset();
    runtime_only_resolution.targets.clear();
    runtime_only_resolution.analysis_candidates.clear();
    runtime_only_resolution.reason = "synthetic-runtime-contract";
    runtime_only_analysis.resolved_edges.clear();
    const auto runtime_only_program = katana::ir::lower_program(runtime_only_analysis);
    const katana::ir::BasicBlock* runtime_only_block = nullptr;
    const katana::ir::BasicBlock* runtime_only_predecessor = nullptr;
    for (const auto& function : runtime_only_program) {
        for (const auto& block : function.blocks) {
            const auto site = std::find_if(
                block.instructions.begin(), block.instructions.end(), [&](const auto& instruction) {
                    return instruction.source_address ==
                           runtime_only_resolution.instruction_address;
                });
            if (site != block.instructions.end()) runtime_only_block = &block;
        }
    }
    require(runtime_only_block != nullptr,
            "Synthetische Runtime-only-Stelle besitzt keinen IR-Block.");
    for (const auto& function : runtime_only_program) {
        for (const auto& block : function.blocks) {
            if (std::find(block.successors.begin(),
                          block.successors.end(),
                          runtime_only_block->start_address) != block.successors.end())
                runtime_only_predecessor = &block;
        }
    }
    require(runtime_only_predecessor != nullptr,
            "Runtime-only-Fixture besitzt keinen statisch chainbaren Vorgaengerblock.");
    const auto runtime_only_output = fixture.root / "runtime-only-port";
    static_cast<void>(export_dreamcast_port_project({guarded_image,
                                                     runtime_only_analysis,
                                                     runtime_only_program,
                                                     guarded_inputs,
                                                     katana::platform::dreamcast_disc_boot_address,
                                                     katana::platform::dreamcast_disc_boot_address,
                                                     guarded_disc.boot_file.size(),
                                                     "runtime-only-fixture"},
                                                    runtime_only_output,
                                                    options));
    const auto runtime_only_sources = snapshot(runtime_only_output / "generated");
    std::string runtime_only_text;
    std::string runtime_only_dispatch_shards;
    for (const auto& [path, content] : runtime_only_sources)
        if (path.starts_with("code/unit-"))
            runtime_only_text += content;
        else if (path.starts_with("code/runtime-dispatch-shard-"))
            runtime_only_dispatch_shards += content;
    const auto block_symbol = hex_symbol(runtime_only_block->start_address);
    const auto site_symbol = hex_symbol(runtime_only_resolution.instruction_address);
    const auto predecessor_symbol = hex_symbol(runtime_only_predecessor->start_address);
    const auto predecessor_label =
        runtime_only_text.find("katana_block_" + predecessor_symbol + ":");
    const auto local_chain = runtime_only_text.find(
        "services->can_chain_executable_block(cpu.pc)) goto katana_block_" +
            block_symbol,
        predecessor_label);
    const auto runtime_only_label =
        runtime_only_text.find("katana_block_" + block_symbol + ":", local_chain);
    const auto runtime_only_source =
        runtime_only_text.find(
            "katana::runtime::relocate_code_address(0x" + site_symbol + "u)",
            runtime_only_label);
    const auto runtime_only_class = runtime_only_text.find(
        "DynamicDispatchSiteClass::RuntimeOnly", runtime_only_source);
    require(runtime_only_text.find("runtime_only_jump") != std::string::npos &&
                predecessor_label != std::string::npos &&
                local_chain != std::string::npos &&
                runtime_only_label != std::string::npos &&
                runtime_only_source != std::string::npos &&
                runtime_only_class != std::string::npos &&
                predecessor_label < local_chain &&
                local_chain < runtime_only_label &&
                runtime_only_label < runtime_only_source &&
                runtime_only_source < runtime_only_class &&
                runtime_only_dispatch_shards.find(
                    "katana::runtime::BlockEndKind::Call") !=
                    std::string::npos &&
                runtime_only_sources.at("code/runtime-dispatch.cpp")
                        .find("make_indirect_dispatch_continuation(\n"
                              "                exit, active_exit_site_class)") !=
                    std::string::npos &&
                runtime_only_sources.at("metadata/port-project.json")
                        .find("\"runtime_only_control_flow\":1") != std::string::npos &&
                runtime_only_sources.at("metadata/port-project.json")
                        .find("\"unresolved_control_flow\":0") != std::string::npos,
            "Portexport verliert den validierenden Runtime-only-Vertrag.");

    const auto make_shard_program = [](const std::size_t count) {
        constexpr std::uint32_t base = katana::platform::dreamcast_disc_boot_address;
        katana::ir::Function function;
        function.entry_address = base;
        function.blocks.reserve(count);
        for (std::size_t index = 0u; index < count; ++index) {
            const auto address = base + static_cast<std::uint32_t>(index * 2u);
            katana::ir::Instruction instruction;
            instruction.source_address = address;
            instruction.original_opcode = 0x0009u;
            instruction.original_operation = katana::ir::Operation::Nop;
            instruction.operation = katana::ir::Operation::Nop;
            instruction.widths = katana::ir::operation_operand_widths(instruction.operation);
            instruction.status_effects =
                katana::ir::instruction_status_effects(instruction.operation);
            instruction.memory_effects =
                katana::ir::instruction_memory_effects(instruction.operation);
            instruction.accumulator_effects =
                katana::ir::operation_accumulator_effects(instruction.operation);
            katana::ir::BasicBlock block;
            block.start_address = address;
            block.instructions.push_back(instruction);
            if (index + 1u != count) block.successors.push_back(address + 2u);
            function.blocks.push_back(std::move(block));
        }
        return std::vector<katana::ir::Function>{std::move(function)};
    };
    katana::io::ExecutableImage shard_image(gdi);
    katana::io::ImageSegment shard_segment;
    shard_segment.name = "dispatch-shard-fixture";
    shard_segment.virtual_address = katana::platform::dreamcast_disc_boot_address;
    shard_segment.memory_size = 2048u;
    shard_segment.kind = katana::io::SegmentKind::Code;
    shard_segment.permissions = {true, false, true};
    shard_segment.bytes.resize(static_cast<std::size_t>(shard_segment.memory_size));
    for (std::size_t offset = 0u; offset < shard_segment.bytes.size(); offset += 2u)
        shard_segment.bytes[offset] = 0x09u;
    shard_segment.source_kind = katana::io::ImageSourceKind::DiscBootFile;
    shard_segment.load_phase = katana::io::ImageLoadPhase::Initial;
    shard_image.add_segment(std::move(shard_segment));
    shard_image.add_entry_point(katana::platform::dreamcast_disc_boot_address);
    katana::analysis::ControlFlowAnalysisResult shard_analysis;
    auto native_template_program = make_shard_program(33u);
    auto& native_template_blocks = native_template_program.front().blocks;
    native_template_blocks.erase(native_template_blocks.begin() + 1u,
                                  native_template_blocks.end() - 1u);
    native_template_blocks.front().successors.clear();
    for (std::uint32_t offset = 2u; offset < 8u; offset += 2u) {
        auto instruction = native_template_blocks.front().instructions.front();
        instruction.source_address =
            katana::platform::dreamcast_disc_boot_address + offset;
        native_template_blocks.front().instructions.push_back(std::move(instruction));
    }
    auto native_template_analysis = shard_analysis;
    constexpr auto native_template_source = katana::platform::dreamcast_disc_boot_address;
    constexpr auto native_template_patch_slot = native_template_source + 12u;
    constexpr auto native_template_handler = native_template_source + 0x40u;
    constexpr auto native_template_live_handler = native_template_handler + 0x20000000u;
    native_template_analysis.runtime_code_copies.copies.push_back(
        {native_template_source,
         native_template_source,
         native_template_source,
         native_template_source + 12u,
         16u,
         0x600,
         {{native_template_source,
           native_template_patch_slot,
           native_template_live_handler,
           native_template_handler}},
         {{native_template_source + 4u,
           native_template_source + 6u,
           native_template_source + 8u,
           4u}},
         true,
         katana::analysis::ControlFlowEvidence::GuardedPartial,
         true,
         "synthetic bounded runtime copy"});
    const auto native_template_output = fixture.root / "native-template-port";
    static_cast<void>(export_dreamcast_port_project(
        {shard_image,
         native_template_analysis,
         native_template_program,
         guarded_inputs,
         native_template_source,
         native_template_source,
         2048u,
         "native-template-fixture"},
        native_template_output,
        options));
    const auto native_template_sources = snapshot(native_template_output / "generated");
    const auto& native_template_dispatch =
        native_template_sources.at("code/runtime-dispatch.cpp");
    require(native_template_dispatch.find("materialization_policy.enabled = true") !=
                    std::string::npos &&
                native_template_dispatch.find(
                    "dreamcast_initial_boot_executable_module_id), \"sha256:") !=
                    std::string::npos &&
                native_template_dispatch.find("{0xAC010040u,0x8C010040u}") !=
                    std::string::npos &&
                native_template_dispatch.find(
                    "NativeAotTemplateDestination::VbrRelative, {}, {}, {{8u,4u},") !=
                    std::string::npos &&
                native_template_dispatch.find("{0x8C010040u,0x00000000u}") ==
                    std::string::npos,
            "Portexport verliert Rohzeiger/native Blockadresse oder aktiviert den nativen "
            "Templatebinder nicht.");
    auto unproven_native_template_analysis = native_template_analysis;
    unproven_native_template_analysis.runtime_code_copies.copies.front()
        .mutable_range_analysis_complete = false;
    require_failure<std::runtime_error>(
        [&] {
            static_cast<void>(export_dreamcast_port_project(
                {shard_image,
                 unproven_native_template_analysis,
                 native_template_program,
                 guarded_inputs,
                 native_template_source,
                 native_template_source,
                 2048u,
                 "unproven-native-template-fixture"},
                fixture.root / "unproven-native-template-port",
                options));
        },
        "Portexport akzeptierte einen selbstmodifizierenden Slot ohne Dominanzbeweis.");
    const auto shard_output = fixture.root / "dispatch-shard-port";
    auto shard_program = make_shard_program(513u);
    static_cast<void>(export_dreamcast_port_project(
        {shard_image,
         shard_analysis,
         shard_program,
         guarded_inputs,
         katana::platform::dreamcast_disc_boot_address,
         katana::platform::dreamcast_disc_boot_address,
         24u,
         "dispatch-shard-fixture"},
        shard_output,
        options));
    const auto shard_sources = snapshot(shard_output / "generated");
    const auto& shard_core = shard_sources.at("code/runtime-dispatch.cpp");
    const auto& shard_zero = shard_sources.at("code/runtime-dispatch-shard-00000.cpp");
    const auto& shard_one = shard_sources.at("code/runtime-dispatch-shard-00001.cpp");
    require(!shard_sources.contains("code/runtime-dispatch-shard-00002.cpp") &&
                shard_core.find(
                    "append_static_blocks_shard_00001(static_blocks)") !=
                    std::string::npos &&
                shard_zero.find("append_static_block(blocks, 0x8C0103FEu") !=
                    std::string::npos &&
                shard_zero.find("append_static_block(blocks, 0x8C010400u") ==
                    std::string::npos &&
                shard_one.find("append_static_block(blocks, 0x8C010400u") !=
                    std::string::npos &&
                occurrences(shard_zero, "append_static_block(blocks,") == 512u &&
                occurrences(shard_one, "append_static_block(blocks,") == 1u &&
                occurrences(shard_zero, "BlockExit dispatch_owner_8C010000(") == 1u &&
                occurrences(shard_one, "BlockExit dispatch_owner_8C010000(") == 1u &&
                shard_sources.at("CMakeLists.txt")
                        .find("code/runtime-dispatch-shard-00001.cpp") != std::string::npos &&
                shard_sources.at(".katana-generated-artifacts")
                        .find("code/runtime-dispatch-shard-00001.cpp") != std::string::npos,
            "513 Bloecke werden nicht an der deterministischen 512er-Shardgrenze getrennt.");
    shard_program = make_shard_program(512u);
    const auto shrunk_shard_export = export_dreamcast_port_project(
        {shard_image,
         shard_analysis,
         shard_program,
         guarded_inputs,
         katana::platform::dreamcast_disc_boot_address,
         katana::platform::dreamcast_disc_boot_address,
         24u,
         "dispatch-shard-fixture"},
        shard_output,
        options);
    const auto shrunk_shard_sources = snapshot(shard_output / "generated");
    require(shrunk_shard_export.removed_files >= 1u &&
                !shrunk_shard_sources.contains("code/runtime-dispatch-shard-00001.cpp") &&
                shrunk_shard_sources.at("CMakeLists.txt")
                        .find("code/runtime-dispatch-shard-00001.cpp") == std::string::npos &&
                shrunk_shard_sources.at(".katana-generated-artifacts")
                        .find("code/runtime-dispatch-shard-00001.cpp") == std::string::npos,
            "Geschrumpfter Portexport entfernt veraltete Runtime-Dispatch-Shards nicht.");

    {
        std::ofstream user(output / "src" / "notes.txt", std::ios::trunc);
        user << "keep-user-file\n";
    }
    const auto second = export_dreamcast_port_project(gdi, output, options);
    require(generated_before == snapshot(output / "generated") &&
                read_text(output / "src" / "main.cpp") == generated_main &&
                second.removed_files == 0u,
            "Identische Portregenerierung ist nicht bytegleich.");
    std::ifstream user(output / "src" / "notes.txt");
    std::ostringstream user_content;
    user_content << user.rdbuf();
    require(user_content.str() == "keep-user-file\n",
            "Portregenerierung hat eine handgeschriebene Nutzerdatei veraendert.");

    auto low = std::vector<std::uint8_t>(24u * raw_sector_size);
    low.front() = 0xA5u;
    write_binary(fixture.root / "disc" / "low.bin", low);
    const auto provenance_before = generated_before.at("metadata/provenance.json");
    static_cast<void>(export_dreamcast_port_project(gdi, output, options));
    const auto changed = snapshot(output / "generated");
    require(changed.at("metadata/provenance.json") != provenance_before &&
                changed.at(unit->first) == unit->second &&
                std::filesystem::exists(output / "src" / "notes.txt"),
            "Geaenderte Eingabe invalidiert Provenienz nicht gezielt oder loescht Nutzerdateien.");

    auto invalid_options = options;
    invalid_options.target_name = "../invalid";
    require_failure<std::invalid_argument>(
        [&] { static_cast<void>(export_dreamcast_port_project(gdi, output, invalid_options)); },
        "Unportabler Port-Zielname wurde akzeptiert.");

    auto protected_options = options;
    protected_options.forbidden_source_root = fixture.root;
    require_failure<std::invalid_argument>(
        [&] {
            static_cast<void>(export_dreamcast_port_project(
                gdi, fixture.root / "generated-commercial-port", protected_options));
        },
        "Portausgabe innerhalb des geschuetzten Quellbaums wurde akzeptiert.");
    const auto link = std::filesystem::temp_directory_path() / "katana-port-parent-link";
    std::error_code link_error;
    std::filesystem::remove(link, link_error);
    link_error.clear();
    std::filesystem::create_directory_symlink(fixture.root, link, link_error);
    if (!link_error) {
        require_failure<std::invalid_argument>(
            [&] {
                static_cast<void>(export_dreamcast_port_project(
                    gdi, link / "through-parent-link", protected_options));
            },
            "Symlink-Elternpfad umgeht den geschuetzten Quellbaum.");
        std::filesystem::remove(link, link_error);
    }

    auto incomplete_track = boot_track();
    incomplete_track[payload_offset(21u, 4u)] = 0x09u;
    incomplete_track[payload_offset(21u, 5u)] = 0x00u;
    write_binary(fixture.root / "disc" / "high.bin", incomplete_track);
    const auto incomplete_output = fixture.root / "incomplete-port";
    static_cast<void>(export_dreamcast_port_project(gdi, incomplete_output, options));
    const auto inferred_runtime_sources = snapshot(incomplete_output / "generated");
    std::string inferred_runtime_text;
    for (const auto& [path, content] : inferred_runtime_sources)
        if (path.starts_with("code/unit-")) inferred_runtime_text += content;
    require(inferred_runtime_text.find("runtime_only_jump") != std::string::npos &&
                inferred_runtime_sources.at("metadata/port-project.json")
                        .find("\"runtime_only_control_flow\":1") != std::string::npos &&
                inferred_runtime_sources.at("metadata/port-project.json")
                        .find("\"unresolved_control_flow\":0") != std::string::npos,
            "Allgemeiner Runtimezeiger erreicht den validierenden Portvertrag nicht; der "
            "ausgezeichnete Bootstrap-Snapshot muss separat statisch bleiben.");

    std::cout << "KR-3507/KR-4502/KR-4507 reproduzierbarer Port-Projektexport erfolgreich.\n";
    return EXIT_SUCCESS;
}

int main(const int argc, char* argv[]) {
    try {
        return run_test(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
