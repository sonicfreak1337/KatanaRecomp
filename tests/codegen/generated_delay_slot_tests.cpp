#include "generated_delay_slot_program.cpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using katana::runtime::ExceptionCause;

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void prepare(katana_generated::CpuState& cpu, const std::uint32_t pc) {
    cpu.memory.set_alignment_policy(katana::runtime::MemoryAlignmentPolicy::Strict);
    cpu.vbr = 0x80000000u;
    cpu.pc = pc;
    cpu.r[8] = 1u;
    cpu.r[9] = 0xA5A5A5A5u;
    cpu.r[10] = 0x1F00u;
    cpu.pr = 0xCAFEBABEu;
}

using GeneratedFunction = void (*)(katana_generated::CpuState&);

void test_memory_delay_slots() {
    struct Case {
        const char* name;
        std::uint32_t owner;
        GeneratedFunction function;
    };
    const std::array cases{Case{"BRA", 0x1000u, katana_generated::fn_00001000},
                           Case{"BSR", 0x1100u, katana_generated::fn_00001100},
                           Case{"JMP", 0x1200u, katana_generated::fn_00001200},
                           Case{"JSR", 0x1300u, katana_generated::fn_00001300},
                           Case{"RTS", 0x1400u, katana_generated::fn_00001400}};

    for (const auto& test : cases) {
        katana_generated::CpuState cpu;
        prepare(cpu, test.owner);
        test.function(cpu);
        require(cpu.trap_pending && cpu.last_exception_cause == ExceptionCause::AddressErrorRead &&
                    cpu.exception_in_delay_slot &&
                    cpu.expevt == katana::runtime::event_address_error_read && cpu.tea == 1u &&
                    cpu.spc == test.owner &&
                    cpu.pc == cpu.vbr + katana::runtime::general_exception_vector,
                std::string(test.name) + " verliert die ausgefuehrte Delay-Slot-Exception.");
        require(cpu.r[9] == 0xA5A5A5A5u,
                std::string(test.name) +
                    " veraendert das Zielregister vor dem fehlgeschlagenen Load.");
        if (test.owner == 0x1400u) {
            require(cpu.r[8] == 1u,
                    "RTS inkrementiert den Quellregisterzustand trotz fehlgeschlagenem Load.");
        }
        if (test.owner == 0x1100u || test.owner == 0x1300u) {
            require(cpu.pr == 0xCAFEBABEu,
                    std::string(test.name) +
                        " restauriert PR nach dem fehlgeschlagenen Delay Slot nicht.");
        }
    }
}

void test_call_fpu_delay_slot_exception() {
    katana_generated::CpuState cpu;
    prepare(cpu, 0x1900u);
    cpu.write_sr(katana::runtime::sr_fd_mask);
    cpu.fr[0] = 0x3F800000u;
    cpu.fr[1] = 0x40000000u;

    katana_generated::fn_00001900(cpu);

    require(cpu.trap_pending && cpu.last_exception_cause == ExceptionCause::SlotFpuDisabled &&
                cpu.expevt == katana::runtime::event_slot_fpu_disabled &&
                cpu.exception_in_delay_slot && cpu.spc == 0x1900u && cpu.pr == 0xCAFEBABEu &&
                cpu.fr[1] == 0x40000000u,
            "BSR restauriert PR nach einer FPU-Disable-Ausnahme im Delay Slot nicht.");
}

void test_calls_write_pr_before_delay_slot() {
    struct Case {
        const char* name;
        std::uint32_t owner;
        GeneratedFunction store_pr;
        GeneratedFunction load_pr;
        bool pc_relative;
    };
    const std::array cases{
        Case{"BSR", 0x1B00u, katana_generated::fn_00001B00, katana_generated::fn_00001C00, false},
        Case{"BSRF", 0x1D00u, katana_generated::fn_00001D00, katana_generated::fn_00001E00, true},
        Case{"JSR", 0x1F00u, katana_generated::fn_00001F00, katana_generated::fn_00002000, false},
    };

    for (const auto& test : cases) {
        katana_generated::CpuState cpu;
        prepare(cpu, test.owner);
        cpu.r[10] = test.pc_relative ? 0x3000u - (test.owner + 4u) : 0x3000u;
        cpu.r[12] = 0u;
        test.store_pr(cpu);
        require(!cpu.trap_pending && cpu.r[12] == test.owner + 4u && cpu.pr == test.owner + 4u &&
                    cpu.pc == test.owner + 6u,
                std::string(test.name) + " macht den neuen PR-Wert im Delay Slot nicht sichtbar.");

        const auto load_owner = test.owner + 0x100u;
        prepare(cpu, load_owner);
        cpu.r[10] = test.pc_relative ? 0x3000u - (load_owner + 4u) : 0x3000u;
        cpu.r[11] = 0xDEADBEE0u;
        test.load_pr(cpu);
        require(!cpu.trap_pending && cpu.pr == 0xDEADBEE0u && cpu.pc == 0xDEADBEE2u,
                std::string(test.name) + " verwirft den LDS-Schreibzugriff aus dem Delay Slot.");
    }
}

void test_rts_latches_pr_before_delay_slot() {
    katana_generated::CpuState cpu;
    prepare(cpu, 0x1A00u);
    cpu.pr = 0x12345678u;
    cpu.r[11] = 0xDEADBEE0u;

    katana_generated::fn_00001A00(cpu);

    require(!cpu.trap_pending && cpu.pc == 0x12345678u && cpu.pr == 0xDEADBEE0u,
            "RTS liest ein durch LDS im Delay Slot veraendertes PR als Sprungziel.");
}

void test_illegal_delay_slot() {
    katana_generated::CpuState cpu;
    prepare(cpu, 0x1600u);
    katana_generated::fn_00001600(cpu);
    require(cpu.trap_pending &&
                cpu.last_exception_cause == ExceptionCause::SlotIllegalInstruction &&
                cpu.expevt == katana::runtime::event_slot_illegal_instruction &&
                cpu.exception_in_delay_slot && cpu.spc == 0x1600u,
            "Illegale Delay-Slot-Instruktion verliert Ursache oder Owner-PC.");
}

void test_rte_delay_slot_exception() {
    katana_generated::CpuState cpu;
    prepare(cpu, 0x1500u);
    cpu.write_sr(katana::runtime::sr_md_mask | katana::runtime::sr_rb_mask |
                 katana::runtime::sr_bl_mask);
    cpu.r[0] = 4u;
    cpu.r_bank[0] = 1u;
    cpu.ssr = 0u;
    cpu.spc = 0x44556677u;
    cpu.trap_pending = true;

    katana_generated::fn_00001500(cpu);

    require(cpu.trap_pending && cpu.last_exception_cause == ExceptionCause::AddressErrorRead &&
                cpu.tea == 1u && cpu.spc == 0x1500u && cpu.ssr == 0u && cpu.exception_in_delay_slot,
            "RTE-Delay-Slot sieht nicht den restaurierten SR-/Registerbankzustand.");
}

void test_nested_exception_propagation() {
    katana_generated::CpuState cpu;
    prepare(cpu, 0x1700u);
    cpu.r[8] = 3u;
    cpu.r[9] = 0x11223344u;

    katana_generated::fn_00001700(cpu);

    require(cpu.trap_pending && cpu.last_exception_cause == ExceptionCause::AddressErrorWrite &&
                cpu.tea == 0xFFFFFFFFu && cpu.spc == 0x1800u &&
                cpu.pc == cpu.vbr + katana::runtime::general_exception_vector && cpu.pr == 0x1704u,
            "Verschachtelter generierter Aufruf propagiert die Delay-Slot-Exception nicht.");
    require(cpu.r[8] == 3u && cpu.r[9] == 0x11223344u,
            "Fehlgeschlagenes Pre-Decrement schreibt vorbereiteten Registerzustand zurueck.");
}

} // namespace

int main() {
    test_memory_delay_slots();
    test_illegal_delay_slot();
    test_rte_delay_slot_exception();
    test_nested_exception_propagation();
    test_call_fpu_delay_slot_exception();
    test_calls_write_pr_before_delay_slot();
    test_rts_latches_pr_before_delay_slot();
    std::cout << "Ausfuehrbare Delay-Slot-Exception-Regression erfolgreich.\n";
    return EXIT_SUCCESS;
}
