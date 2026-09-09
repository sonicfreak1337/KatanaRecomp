#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t base_address = 0x100u;
constexpr std::uint32_t fmov_cache_fixture_address = 0x200u;
constexpr std::uint32_t fmov_delay_fixture_address = 0x222u;
constexpr std::uint32_t fpu_cache_plan_fixture_address = 0x300u;
constexpr std::uint32_t fpu_cache_plan_slot_size = 0x20u;
constexpr std::size_t fpu_cache_plan_slot_count = 7u;
constexpr std::array<std::uint8_t, 54> fixture = {
    0x9Du, 0xF0u, 0x9Du, 0xF1u, 0x00u, 0xF1u, 0x02u, 0xF1u, 0x0Du, 0xF2u, 0x1Du,
    0xF3u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0xFDu, 0xFBu, 0xFDu, 0xF3u, 0x0Bu, 0x00u,
    0x09u, 0x00u, 0x05u, 0xF1u, 0x2Du, 0xF2u, 0x3Du, 0xF2u, 0x0Bu, 0x00u, 0x09u,
    0x00u, 0x00u, 0xF2u, 0xBDu, 0xF2u, 0xADu, 0xF4u, 0x0Bu, 0x00u, 0x09u, 0x00u,
    0x48u, 0xF5u, 0x5Au, 0xF6u, 0x49u, 0xF7u, 0x7Bu, 0xF6u, 0x5Cu, 0xF8u};

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<katana::ir::Function> build_program() {
    auto bytes = std::vector<std::uint8_t>(fixture.begin(), fixture.end());
    const std::array<std::uint8_t, 64> tail = {
        0x46u, 0xF9u, 0x97u, 0xF6u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0x00u, 0xA0u, 0x00u, 0xF1u, 0x0Bu,
        0x00u, 0x09u, 0x00u, 0x10u, 0xF3u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0x49u, 0xF4u, 0x0Bu, 0x00u,
        0x09u, 0x00u, 0x4Bu, 0xF4u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0xFDu, 0xF2u, 0x0Bu, 0x00u, 0x09u,
        0x00u, 0x7Du, 0xF4u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0xEDu, 0xF1u, 0x0Bu, 0x00u, 0x09u, 0x00u,
        0xFDu, 0xF9u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0x83u, 0x03u, 0x0Bu, 0x00u, 0x09u, 0x00u};
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    const std::array<std::uint8_t, 26> precise_arithmetic = {
        0x03u, 0xF2u, 0x01u, 0xE0u, 0x0Bu, 0x00u, 0x09u, 0x00u,
        0x00u, 0xA0u, 0x03u, 0xF2u, 0x01u, 0xE0u, 0x0Bu, 0x00u, 0x09u, 0x00u,
        0x6Du, 0xF2u, 0x01u, 0xE0u, 0x0Bu, 0x00u, 0x09u, 0x00u};
    bytes.insert(bytes.end(), precise_arithmetic.begin(), precise_arithmetic.end());
    // Register-only arithmetic runs occur in Sonic's profiled matrix routines.
    // Ending without RTS exposes the final arithmetic instruction's diagnostic
    // PC as well as its architectural fallthrough at the generated boundary.
    const std::array<std::uint8_t, 12> arithmetic_epoch = {
        0x00u, 0xF1u, // FADD FR0,FR1
        0x22u, 0xF1u, // FMUL FR2,FR1
        0x33u, 0xF1u, // FDIV FR3,FR1
        0x41u, 0xF1u, // FSUB FR4,FR1
        0x52u, 0xF1u, // FMUL FR5,FR1
        0x60u, 0xF1u  // FADD FR6,FR1
    };
    bytes.insert(bytes.end(), arithmetic_epoch.begin(), arithmetic_epoch.end());
    const std::array<std::uint8_t, 20> mixed_arithmetic_epoch = {
        0x0Cu, 0xF2u, // FMOV FR0,FR2: no rounding-mode guard of its own
        0x20u, 0xF4u, // FADD FR2,FR4: even registers permit PR=1
        0x4Du, 0xF4u, // FNEG FR4
        0x02u, 0xF5u, // FMUL FR0,FR5: odd destination requires PR=0
        0x5Du, 0xF5u, // FABS FR5
        0x9Du, 0xF6u, // FLDI1 FR6
        0x61u, 0xF5u, // FSUB FR6,FR5
        0x5Cu, 0xF7u, // FMOV FR5,FR7
        0x33u, 0xF7u, // FDIV FR3,FR7
        0x20u, 0xF7u  // FADD FR2,FR7
    };
    bytes.insert(bytes.end(), mixed_arithmetic_epoch.begin(), mixed_arithmetic_epoch.end());
    const auto lines = katana::sh4::disassemble(bytes, base_address);
    constexpr std::array<std::uint32_t, 19> seeds = {0x100u,
                                                     0x110u,
                                                     0x118u,
                                                     0x122u,
                                                     0x12Cu,
                                                     0x13Eu,
                                                     0x146u,
                                                     0x14Cu,
                                                     0x152u,
                                                     0x158u,
                                                     0x15Eu,
                                                     0x164u,
                                                     0x16Au,
                                                     0x170u,
                                                     0x176u, 0x17Eu, 0x188u, 0x190u, 0x19Cu};
    const auto functions = katana::analysis::discover_functions(lines, seeds);
    return katana::ir::lower_program(lines, functions);
}

std::vector<katana::ir::Function> build_fmov_cache_program() {
    // This is the small Sonic-like native-register-cache witness.  Integer
    // address mutations precede all six FMOV memory forms; the final FMOV is
    // in the RTS delay slot so the same fixture also exercises delayed FPU
    // guards.  All addresses stay in the low linear-RAM test window.
    constexpr std::array<std::uint8_t, 34> fmov_cache_fixture = {
        0x20u, 0xE2u, // MOV #0x20,R2
        0x04u, 0xE0u, // MOV #4,R0
        0x30u, 0xE4u, // MOV #0x30,R4
        0x40u, 0xE6u, // MOV #0x40,R6
        0x50u, 0xE8u, // MOV #0x50,R8
        0x60u, 0xEAu, // MOV #0x60,R10
        0x70u, 0xECu, // MOV #0x70,R12
        0x04u, 0x70u, // ADD #4,R0
        0x04u, 0x74u, // ADD #4,R4
        0x28u, 0xF0u, // FMOV.S @R2,FR0
        0x49u, 0xF2u, // FMOV.S @R4+,FR2
        0x66u, 0xF4u, // FMOV.S @(R0,R6),FR4
        0x6Au, 0xF8u, // FMOV.S FR6,@R8
        0x8Bu, 0xFAu, // FMOV.S FR8,@-R10
        0xA7u, 0xFCu, // FMOV.S FR10,@(R0,R12)
        0x0Bu, 0x00u, // RTS
        0x28u, 0xFCu  // delay: FMOV.S @R2,FR12
    };
    constexpr std::array<std::uint8_t, 8> fmov_delay_fixture = {
        0x1Cu, 0xE2u, // MOV #0x1C,R2
        0x04u, 0x72u, // ADD #4,R2
        0x0Bu, 0x00u, // RTS
        0x28u, 0xF6u  // delay: FMOV.S @R2,FR6
    };
    std::vector<std::uint8_t> bytes(fmov_cache_fixture.begin(), fmov_cache_fixture.end());
    bytes.insert(bytes.end(), fmov_delay_fixture.begin(), fmov_delay_fixture.end());
    const auto lines = katana::sh4::disassemble(bytes, fmov_cache_fixture_address);
    constexpr std::array<std::uint32_t, 2> seeds = {
        fmov_cache_fixture_address, fmov_delay_fixture_address};
    const auto functions = katana::analysis::discover_functions(lines, seeds);
    // Keep the decoder's Unknown region: the product fast path proves the
    // effective RAM address at runtime, including its observer and mapping
    // generation. A synthetic NormalRam proof would select a different path.
    return katana::ir::lower_program(lines, functions);
}

std::vector<katana::ir::Function> build_fpu_cache_plan_program() {
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(fpu_cache_plan_slot_size) * fpu_cache_plan_slot_count);
    for (std::size_t index = 0; index < bytes.size(); index += 2u) {
        bytes[index] = 0x09u;
        bytes[index + 1u] = 0x00u;
    }

    const auto place = [&bytes](const std::size_t slot, const auto& sequence) {
        std::copy(
            sequence.begin(),
            sequence.end(),
            bytes.begin() + slot * static_cast<std::size_t>(fpu_cache_plan_slot_size));
    };

    constexpr std::array<std::uint8_t, 14> fpul_convert = {
        0x5Au, 0x08u, // sts fpul,r8
        0x01u, 0x78u, // add #1,r8
        0x20u, 0xF4u, // fadd dr2,dr4
        0xBDu, 0xF0u, // fcnvds dr0,fpul
        0x5Au, 0x09u, // sts fpul,r9
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u, // nop
    };
    constexpr std::array<std::uint8_t, 14> fpul_float = {
        0x03u, 0xE8u, // mov #3,r8
        0x5Au, 0x48u, // lds r8,fpul
        0x8Du, 0xF0u, // fldi0 fr0
        0x2Du, 0xF4u, // float fpul,fr4
        0x5Au, 0x09u, // sts fpul,r9
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u, // nop
    };
    constexpr std::array<std::uint8_t, 16> fpul_sca = {
        0x40u, 0xE8u, // mov #0x40,r8
        0x18u, 0x48u, // shll8 r8
        0x5Au, 0x48u, // lds r8,fpul
        0x8Du, 0xF0u, // fldi0 fr0
        0xFDu, 0xF4u, // fsca fpul,dr2
        0x5Au, 0x09u, // sts fpul,r9
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u, // nop
    };
    constexpr std::array<std::uint8_t, 20> compare_and_delay_fmov = {
        0x10u, 0xE8u, // mov #0x10,r8
        0x01u, 0x78u, // add #1,r8
        0x08u, 0x00u, // clrt
        0x05u, 0xF2u, // fcmp/gt fr0,fr2
        0x29u, 0x09u, // movt r9
        0x18u, 0x00u, // sett
        0x04u, 0xF2u, // fcmp/eq fr0,fr2
        0x29u, 0x0Au, // movt r10
        0x0Bu, 0x00u, // rts
        0x0Cu, 0xF6u, // fmov fr0,fr6 (delay slot)
    };
    constexpr std::array<std::uint8_t, 28> vector_and_fpul = {
        0x03u, 0xE8u, // mov #3,r8
        0x02u, 0x78u, // add #2,r8
        0x0Cu, 0xF2u, // fmov fr0,fr2
        0x4Du, 0xF2u, // fneg fr2
        0x5Du, 0xF2u, // fabs fr2
        0x8Du, 0xF4u, // fldi0 fr4
        0x9Du, 0xF5u, // fldi1 fr5
        0x5Eu, 0xF6u, // fmac fr0,fr5,fr6
        0xEDu, 0xF8u, // fipr fv0,fv8
        0xFDu, 0xF9u, // ftrv xmtrx,fv8
        0x1Du, 0xF8u, // flds fr8,fpul
        0x0Du, 0xFCu, // fsts fpul,fr12
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u, // nop
    };
    constexpr std::array<std::uint8_t, 8> delay_fmov = {
        0x01u, 0xE8u, // mov #1,r8
        0x01u, 0x78u, // add #1,r8
        0x0Bu, 0x00u, // rts
        0x0Cu, 0xF2u, // fmov fr0,fr2 (delay slot)
    };
    constexpr std::array<std::uint8_t, 14> unknown_ram_cycle = {
        0x50u, 0xE8u, // mov #0x50,r8
        0x04u, 0xE0u, // mov #4,r0
        0x04u, 0x78u, // add #4,r8
        0x02u, 0x28u, // mov.l r0,@r8
        0x82u, 0x69u, // mov.l @r8,r9
        0x0Bu, 0x00u, // rts
        0x09u, 0x00u, // nop
    };

    place(0u, fpul_convert);
    place(1u, fpul_float);
    place(2u, fpul_sca);
    place(3u, compare_and_delay_fmov);
    place(4u, vector_and_fpul);
    place(5u, delay_fmov);
    place(6u, unknown_ram_cycle);

    const auto lines = katana::sh4::disassemble(bytes, fpu_cache_plan_fixture_address);
    constexpr std::array<std::uint32_t, fpu_cache_plan_slot_count> seeds = {
        fpu_cache_plan_fixture_address + 0x00u,
        fpu_cache_plan_fixture_address + 0x20u,
        fpu_cache_plan_fixture_address + 0x40u,
        fpu_cache_plan_fixture_address + 0x60u,
        fpu_cache_plan_fixture_address + 0x80u,
        fpu_cache_plan_fixture_address + 0xA0u,
        fpu_cache_plan_fixture_address + 0xC0u,
    };
    const auto functions = katana::analysis::discover_functions(lines, seeds);
    return katana::ir::lower_program(lines, functions);
}

std::string emit_cache_variant(
    const std::span<const katana::ir::Function> program,
    const std::uint32_t entry_address,
    const bool localize_registers,
    const std::string_view symbol_namespace) {
    katana::codegen::BackendRequest request{program, entry_address};
    request.symbol_namespace = symbol_namespace;
    request.single_block_execution = true;
    request.guarded_local_block_chaining = true;
    request.conservative_register_localization = localize_registers;
    return katana::codegen::CppBackend{}.emit(request).joined_text();
}

int emit_fixture(const std::string& output_path) {
    const auto program = build_program();
    const auto cache_program = build_fmov_cache_program();
    const auto cache_plan_program = build_fpu_cache_plan_program();
    auto source = katana::codegen::emit_cpp_program(program, base_address);
    // The observer contract forces the independent, per-instruction emission
    // path. Execute it against the optimized path for arithmetic/trap/PC parity.
    katana::codegen::BackendRequest reference{
        std::span<const katana::ir::Function>{program}.last(2u), 0x190u};
    reference.symbol_namespace = "katana_fpu_boundary_reference";
    reference.external_instruction_observer = true;
    source += katana::codegen::CppBackend{}.emit(reference).joined_text();
    source += emit_cache_variant(
        std::span<const katana::ir::Function>{cache_program},
        fmov_cache_fixture_address,
        true,
        "katana_fpu_cache_optimized");
    source += emit_cache_variant(
        std::span<const katana::ir::Function>{cache_program},
        fmov_cache_fixture_address,
        false,
        "katana_fpu_cache_conservative");
    source += emit_cache_variant(
        std::span<const katana::ir::Function>{cache_plan_program},
        fpu_cache_plan_fixture_address,
        true,
        "katana_fpu_cache_plan_optimized");
    source += emit_cache_variant(
        std::span<const katana::ir::Function>{cache_plan_program},
        fpu_cache_plan_fixture_address,
        false,
        "katana_fpu_cache_plan_conservative");
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    return output ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(const int argc, char* argv[]) {
    using katana::sh4::InstructionKind;
    if (argc == 3 && std::string(argv[1]) == "--emit-cpp") {
        return emit_fixture(argv[2]);
    }
    require(argc == 1, "Unerwartete Argumente fuer den FPU-Pipeline-Test.");

    require(katana::sh4::decode(0xF120u).kind == InstructionKind::Fadd,
            "FADD wird nicht dekodiert.");
    require(katana::sh4::decode(0xF123u).kind == InstructionKind::Fdiv,
            "FDIV wird nicht dekodiert.");
    require(katana::sh4::decode(0xF125u).kind == InstructionKind::FcmpGreater,
            "FCMP/GT wird nicht dekodiert.");
    require(katana::sh4::decode(0xF43Du).kind == InstructionKind::Ftrc,
            "FTRC wird nicht dekodiert.");
    require(katana::sh4::decode(0xF4ADu).kind == InstructionKind::FcnvSingleToDouble,
            "FCNVSD wird nicht dekodiert.");
    require(katana::sh4::decode(0xFBFDu).kind == InstructionKind::Frchg,
            "FRCHG wird nicht dekodiert.");
    require(katana::sh4::decode(0xF47Du).kind == InstructionKind::Fsrra,
            "FSRRA wird nicht dekodiert.");
    require(katana::sh4::decode(0xF2FDu).kind == InstructionKind::Fsca,
            "FSCA wird nicht dekodiert.");
    require(katana::sh4::decode(0xF5EDu).kind == InstructionKind::Fipr,
            "FIPR wird nicht dekodiert.");
    require(katana::sh4::decode(0xF9FDu).kind == InstructionKind::Ftrv,
            "FTRV wird nicht dekodiert.");
    require(katana::sh4::decode(0xF0BDu).kind == InstructionKind::FcnvDoubleToSingle,
            "FCNVDS DR0 wird nicht dekodiert.");
    require(katana::sh4::decode(0x085Au).kind == InstructionKind::StoreSpecialRegister &&
                katana::sh4::decode(0x085Au).special_register == katana::sh4::SpecialRegister::Fpul &&
                katana::sh4::decode(0x485Au).kind == InstructionKind::LoadSpecialRegister &&
                katana::sh4::decode(0x485Au).special_register == katana::sh4::SpecialRegister::Fpul,
            "Die FPUL-Transferformen werden falsch dekodiert.");
    require(katana::sh4::decode(0xF4FDu).kind == InstructionKind::Fsca &&
                katana::sh4::decode(0xF65Eu).kind == InstructionKind::Fmac &&
                katana::sh4::decode(0xF8EDu).kind == InstructionKind::Fipr &&
                katana::sh4::decode(0xF9FDu).kind == InstructionKind::Ftrv,
            "Die Vektor-/FSCA-Formen werden falsch dekodiert.");
    require(katana::sh4::decode(0xF205u).kind == InstructionKind::FcmpGreater &&
                katana::sh4::decode(0xF204u).kind == InstructionKind::FcmpEqual,
            "Die FCMP-Formen werden falsch dekodiert.");
    require(katana::sh4::decode(0xF028u).kind == InstructionKind::FmovLoad &&
                katana::sh4::decode(0xF249u).kind == InstructionKind::FmovLoadPostIncrement &&
                katana::sh4::decode(0xF466u).kind == InstructionKind::FmovLoadR0Indexed &&
                katana::sh4::decode(0xF86Au).kind == InstructionKind::FmovStore &&
                katana::sh4::decode(0xFA8Bu).kind == InstructionKind::FmovStorePreDecrement &&
                katana::sh4::decode(0xFCA7u).kind == InstructionKind::FmovStoreR0Indexed,
            "Die sechs FMOV-Memoryformen der Cachefixture werden falsch dekodiert.");
    for (std::uint16_t index = 0u; index < 16u; ++index) {
        require(katana::sh4::decode(static_cast<std::uint16_t>((index << 8u) | 0x0083u)).kind ==
                    InstructionKind::Prefetch,
                "PREF @Rn wird nicht fuer jedes Register dekodiert.");
    }

    const auto program = build_program();
    const auto cache_program = build_fmov_cache_program();
    const auto cache_plan_program = build_fpu_cache_plan_program();
    const auto source = katana::codegen::emit_cpp_program(program, base_address);
    require(source.find("katana::runtime::fpu_binary") != std::string::npos &&
                source.find("cpu.toggle_fpu_register_bank()") != std::string::npos &&
                source.find("write_fpu_pair_bits") != std::string::npos &&
                source.find("raise_fpu_disabled") != std::string::npos &&
                source.find("services->prefetch") != std::string::npos,
            "FPU-IR erreicht den Runtime-basierten C++-Emitter nicht vollstaendig.");

    const auto cache_optimized_source = emit_cache_variant(
        std::span<const katana::ir::Function>{cache_program},
        fmov_cache_fixture_address,
        true,
        "katana_fpu_cache_optimized");
    const auto cache_conservative_source = emit_cache_variant(
        std::span<const katana::ir::Function>{cache_program},
        fmov_cache_fixture_address,
        false,
        "katana_fpu_cache_conservative");
    const auto delay_body = cache_optimized_source.rfind("fn_00000222_with_services");
    const auto delay_body_end = cache_optimized_source.find("\n}\n", delay_body);
    require(cache_optimized_source.find("katana::runtime::NativeAotRegisterFile<") !=
                std::string::npos &&
                cache_conservative_source.find("katana::runtime::NativeAotRegisterFile<") ==
                    std::string::npos &&
                cache_optimized_source.find("fn_00000200_with_services") !=
                    std::string::npos &&
                cache_optimized_source.find("const std::uint32_t address = katana_registers[2];") !=
                    std::string::npos &&
                delay_body != std::string::npos && delay_body_end != std::string::npos &&
                cache_optimized_source.find("katana::runtime::NativeAotRegisterFile<",
                                            delay_body) < delay_body_end,
            "FMOV-Cachefixture erzeugt keinen getrennten lokalisierten und konservativen Emitterpfad.");

    const auto cache_plan_optimized_source = emit_cache_variant(
        std::span<const katana::ir::Function>{cache_plan_program},
        fpu_cache_plan_fixture_address,
        true,
        "katana_fpu_cache_plan_optimized");
    const auto cache_plan_conservative_source = emit_cache_variant(
        std::span<const katana::ir::Function>{cache_plan_program},
        fpu_cache_plan_fixture_address,
        false,
        "katana_fpu_cache_plan_conservative");
    require(cache_plan_optimized_source.find("katana::runtime::NativeAotRegisterFile<") !=
                    std::string::npos &&
                cache_plan_optimized_source.find("katana_registers.fpul()") !=
                    std::string::npos &&
                cache_plan_optimized_source.find("katana_registers.t()") != std::string::npos &&
                cache_plan_optimized_source.find("fn_00000300") != std::string::npos &&
                cache_plan_optimized_source.find("fn_000003C0") != std::string::npos,
            "Der erweiterte FPU-Cacheplan waehlt nicht GPR, FPUL, T und alle Eintraege.");
    require(cache_plan_conservative_source.find("katana::runtime::NativeAotRegisterFile<") ==
                std::string::npos,
            "Der konservative FPU-Cacheplan darf keine NativeRegisterFile waehlen.");

    std::cout << "FPU-Decoder-, IR- und Codegen-Pipeline erfolgreich.\n";
    return EXIT_SUCCESS;
}
