#include "katana/runtime/dynamic_interpreter.hpp"

#include "katana/runtime/block_abi.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {

using namespace katana::runtime;

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class Services final : public PlatformServices {
  public:
    [[nodiscard]] std::string_view name() const noexcept override { return "dynamic-test"; }
    [[nodiscard]] std::uint32_t abi_version() const noexcept override {
        return platform_services_abi_version;
    }
    [[nodiscard]] std::uint32_t guest_cycle_contract() const noexcept override {
        return guest_cycle_contract_version;
    }
    [[nodiscard]] PlatformCapabilities capabilities() const noexcept override {
        return core_platform_capabilities;
    }
    void read_memory(std::uint32_t, std::span<std::uint8_t>) override {}
    void write_memory(std::uint32_t, std::span<const std::uint8_t>) override {}
    [[nodiscard]] std::uint64_t scheduler_cycle() const noexcept override { return cycles_; }
    [[nodiscard]] std::optional<std::uint64_t> next_scheduler_event_cycle() const noexcept override {
        return std::nullopt;
    }
    [[nodiscard]] PlatformSchedulerResult consume_guest_cycles(
        const std::uint64_t cycles, std::size_t) override {
        cycles_ += cycles;
        return {cycles_, 0u, false, false};
    }
    [[nodiscard]] std::optional<PlatformInterruptRequest> poll_interrupt() override {
        return std::nullopt;
    }
    [[nodiscard]] PlatformDmaResult start_dma(const PlatformDmaRequest&) override {
        return {};
    }
    [[nodiscard]] PlatformFallbackResult controlled_fallback(
        CpuState&, const PlatformFallbackRequest&) override {
        return {};
    }
    [[nodiscard]] bool prefetch(CpuState&,
                                const GuestInstructionOrigin instruction,
                                const std::uint32_t address) override {
        last_prefetch_instruction = instruction;
        last_prefetch_address = address;
        ++prefetch_count;
        return false;
    }

    GuestInstructionOrigin last_prefetch_instruction;
    std::uint32_t last_prefetch_address = 0u;
    std::size_t prefetch_count = 0u;

  private:
    std::uint64_t cycles_ = 0u;
};

template <std::size_t Capacity> struct AccessCapture {
    std::array<GuestMemoryAccessEvent, Capacity> events{};
    std::size_t count = 0u;
    std::size_t dropped = 0u;

    static void callback(void* const context,
                         const GuestMemoryAccessEvent& event) noexcept {
        auto& capture = *static_cast<AccessCapture*>(context);
        if (capture.count < capture.events.size()) {
            capture.events[capture.count++] = event;
        } else {
            ++capture.dropped;
        }
    }

    [[nodiscard]] GuestMemoryAccessSink sink() noexcept {
        return {this, &AccessCapture::callback};
    }

    [[nodiscard]] std::size_t semantic_count() const noexcept {
        std::size_t result = 0u;
        for (std::size_t index = 0u; index < count; ++index)
            result += events[index].instruction.valid ? 1u : 0u;
        return result;
    }

    [[nodiscard]] const GuestMemoryAccessEvent& semantic(const std::size_t wanted) const {
        std::size_t found = 0u;
        for (std::size_t index = 0u; index < count; ++index) {
            if (!events[index].instruction.valid) continue;
            if (found++ == wanted) return events[index];
        }
        throw std::out_of_range("Fehlendes semantisches Speicherereignis.");
    }
};

void prepare_provenance_program(CpuState& cpu) {
    cpu.pc = 0x2000u;
    cpu.retired_guest_instructions = 77u;
    cpu.r[0] = 0x10u;
    cpu.r[1] = 0x0300u;
    cpu.r[3] = 0x11223344u;
    cpu.r[4] = 0x0304u;
    cpu.r[5] = 0x0310u;
    cpu.r[6] = 0x0314u;
    cpu.r[7] = 0xE0000000u;
    cpu.r[8] = 0x0318u;
    cpu.fr[5] = 0x3F800000u;
    cpu.gbr = 0x0300u;
    cpu.memory.write_u32(0x0300u, 0xAABBCCDDu);
    cpu.memory.write_u8(0x0310u, 0xF3u);
    cpu.memory.write_u16(0x2000u, 0x6212u); // MOV.L @R1,R2
    cpu.memory.write_u16(0x2002u, 0x2432u); // MOV.L R3,@R4
    cpu.memory.write_u16(0x2004u, 0x451Bu); // TAS.B @R5
    cpu.memory.write_u16(0x2006u, 0xF65Au); // FMOV.S FR5,@R6
    cpu.memory.write_u16(0x2008u, 0x0783u); // PREF @R7
    cpu.memory.write_u16(0x200Au, 0x08C3u); // MOVCA.L R0,@R8
}

void require_instruction_origin(const GuestMemoryAccessEvent& event,
                                const std::uint32_t source_pc,
                                const std::uint32_t runtime_pc,
                                const std::uint64_t retired_guest_instructions,
                                const char* const message) {
    require(event.instruction.valid && event.instruction.source_pc == source_pc &&
                event.instruction.runtime_pc == runtime_pc &&
                event.retired_guest_instructions == retired_guest_instructions,
            message);
}

} // namespace

int main() {
    Services services;

    CpuState same_register;
    same_register.pc = 0u;
    same_register.r[1] = 0x104u;
    same_register.memory.write_u16(0u, 0x2116u);
    const auto predecrement = execute_dynamic_sh4_block(same_register, services, 1u);
    require(same_register.r[1] == 0x100u &&
                same_register.memory.read_u32(0x100u) == 0x104u &&
                predecrement.instructions == 1u,
            "MOV.L Rn,@-Rn speichert nicht den alten Wert.");

    CpuState faulting_predecrement;
    faulting_predecrement.pc = 0u;
    faulting_predecrement.write_sr(sr_md_mask | sr_rb_mask);
    faulting_predecrement.r[1] = 2u;
    faulting_predecrement.memory.write_u16(0u, 0x2116u);
    const auto fault = execute_dynamic_sh4_block(faulting_predecrement, services, 1u);
    require(fault.end_kind == BlockEndKind::Exception && faulting_predecrement.r[1] == 2u,
            "Fehlgeschlagenes Pre-Decrement mutiert Rn vor dem Speicherzugriff.");

    CpuState arithmetic_shift;
    arithmetic_shift.pc = 0u;
    arithmetic_shift.r[1] = 0xFFFFFFE0u;
    arithmetic_shift.r[2] = 0x80000000u;
    arithmetic_shift.memory.write_u16(0u, 0x421Cu);
    static_cast<void>(execute_dynamic_sh4_block(arithmetic_shift, services, 1u));
    require(arithmetic_shift.r[2] == 0xFFFFFFFFu,
            "SHAD -32 bildet den architektonischen Vorzeichenfall nicht ab.");

    CpuState logical_shift;
    logical_shift.pc = 0u;
    logical_shift.r[1] = 0xFFFFFFE0u;
    logical_shift.r[2] = 0x80000000u;
    logical_shift.memory.write_u16(0u, 0x421Du);
    static_cast<void>(execute_dynamic_sh4_block(logical_shift, services, 1u));
    require(logical_shift.r[2] == 0u, "SHLD -32 bildet den Nullfall nicht ab.");

    CpuState slot_fault;
    slot_fault.pc = 0x100u;
    slot_fault.write_sr(sr_md_mask | sr_rb_mask);
    slot_fault.pr = 0xDEADBEEFu;
    slot_fault.r[1] = 0xE0000002u;
    slot_fault.memory.write_u16(0x100u, 0xB000u);
    slot_fault.memory.write_u16(0x102u, 0x6212u);
    const auto call = execute_dynamic_sh4_block(slot_fault, services, 4u);
    require(call.end_kind == BlockEndKind::Exception && call.instructions == 2u &&
                slot_fault.pr == 0xDEADBEEFu && slot_fault.exception_in_delay_slot,
            "Call-Slotexception restauriert PR oder Instruktionsbudget nicht.");

    ScopedCodeAddressMapping provenance_mapping({0x1000u, 0x2000u, 0x100u});
    CpuState traced;
    CpuState diagnostics_off;
    prepare_provenance_program(traced);
    prepare_provenance_program(diagnostics_off);
    AccessCapture<16u> accesses;
    traced.memory.set_guest_memory_access_sink(accesses.sink());
    Services traced_services;
    Services diagnostics_off_services;
    const auto traced_result = execute_dynamic_sh4_block(traced, traced_services, 6u);
    const auto diagnostics_off_result =
        execute_dynamic_sh4_block(diagnostics_off, diagnostics_off_services, 6u);
    require(accesses.dropped == 0u && accesses.semantic_count() == 6u,
            "Dynamischer Interpreter erfasst Datenzugriffe nicht vollstaendig oder doppelt.");

    const auto& load = accesses.semantic(0u);
    require_instruction_origin(
        load, 0x1000u, 0x2000u, 78u, "MOV.L-Read verliert Source-/Runtime-PC.");
    require(load.operation == MemoryAccessOperation::Read &&
                load.virtual_address == 0x0300u && load.width == MemoryAccessWidth::Word,
            "MOV.L-Read meldet falsche Adresse oder Breite.");

    const auto& store = accesses.semantic(1u);
    require_instruction_origin(
        store, 0x1002u, 0x2002u, 79u, "MOV.L-Write verliert Source-/Runtime-PC.");
    require(store.operation == MemoryAccessOperation::Write &&
                store.virtual_address == 0x0304u &&
                store.write_source == CodeWriteSource::Cpu,
            "MOV.L-Write meldet falsche Adresse oder Writer-Quelle.");

    const auto& rmw_read = accesses.semantic(2u);
    const auto& rmw_write = accesses.semantic(3u);
    require_instruction_origin(
        rmw_read, 0x1004u, 0x2004u, 80u, "RMW-Read verliert Instruktionsherkunft.");
    require_instruction_origin(
        rmw_write, 0x1004u, 0x2004u, 80u, "RMW-Write verliert Instruktionsherkunft.");
    require(rmw_read.operation == MemoryAccessOperation::Read &&
                rmw_write.operation == MemoryAccessOperation::Write &&
                rmw_read.virtual_address == 0x0310u &&
                rmw_write.virtual_address == 0x0310u,
            "TAS.B-RMW besitzt keine gemeinsame Quellregisteradresse.");

    const auto& fmov = accesses.semantic(4u);
    require_instruction_origin(
        fmov, 0x1006u, 0x2006u, 81u, "FMOV-Store verliert Instruktionsherkunft.");
    require(fmov.operation == MemoryAccessOperation::Write &&
                fmov.virtual_address == 0x0314u &&
                fmov.write_source == CodeWriteSource::Fpu,
            "FMOV-Store meldet nicht die FPU als Writer.");

    const auto& movca = accesses.semantic(5u);
    require_instruction_origin(
        movca, 0x100Au, 0x200Au, 83u, "MOVCA.L verliert Instruktionsherkunft.");
    require(movca.operation == MemoryAccessOperation::Write &&
                movca.virtual_address == 0x0318u &&
                movca.write_source == CodeWriteSource::StoreQueue,
            "MOVCA.L meldet nicht die Store Queue als Writer.");
    require(traced_services.prefetch_count == 1u &&
                traced_services.last_prefetch_address == 0xE0000000u,
            "PREF erreicht den Plattformdienst nicht mit der Gastadresse.");
    require(traced_services.last_prefetch_instruction.source_pc == 0x1008u &&
                traced_services.last_prefetch_instruction.runtime_pc == 0x2008u &&
                traced_services.last_prefetch_instruction.valid,
            "PREF erhaelt nicht dieselbe lazy Instruktionsherkunft.");

    traced.memory.clear_guest_memory_access_sink();
    require(!diagnostics_off.memory.has_guest_memory_access_sink() &&
                !diagnostics_off_services.last_prefetch_instruction.valid &&
                traced.retired_guest_instructions == 83u &&
                diagnostics_off.retired_guest_instructions == 83u &&
                traced_result.instructions == diagnostics_off_result.instructions &&
                traced_result.guest_cycles == diagnostics_off_result.guest_cycles &&
                traced_result.end_kind == diagnostics_off_result.end_kind &&
                traced.pc == diagnostics_off.pc && traced.r[2] == diagnostics_off.r[2] &&
                traced.memory.read_u32(0x0304u) ==
                    diagnostics_off.memory.read_u32(0x0304u) &&
                traced.memory.read_u8(0x0310u) ==
                    diagnostics_off.memory.read_u8(0x0310u) &&
                traced.memory.read_u32(0x0314u) ==
                    diagnostics_off.memory.read_u32(0x0314u) &&
                traced.memory.read_u32(0x0318u) ==
                    diagnostics_off.memory.read_u32(0x0318u),
            "Diagnostics-on/off veraendert den architektonischen Interpreterzustand.");

    CpuState pair_loads;
    pair_loads.pc = 0x2060u;
    pair_loads.retired_guest_instructions = 77u;
    pair_loads.write_fpscr(fpscr_sz_mask);
    pair_loads.r[0] = 0x10u;
    pair_loads.r[2] = 0x0380u;
    pair_loads.r[3] = 0x0388u;
    pair_loads.r[4] = 0x0380u;
    for (std::uint32_t offset = 0u; offset < 24u; offset += 4u)
        pair_loads.memory.write_u32(0x0380u + offset, 0x11110000u + offset);
    pair_loads.memory.write_u16(0x2060u, 0xF028u); // FMOV.D @R2,DR0
    pair_loads.memory.write_u16(0x2062u, 0xF039u); // FMOV.D @R3+,DR0
    pair_loads.memory.write_u16(0x2064u, 0xF046u); // FMOV.D @(R0,R4),DR0
    AccessCapture<12u> pair_accesses;
    pair_loads.memory.set_guest_memory_access_sink(pair_accesses.sink());
    Services pair_services;
    static_cast<void>(execute_dynamic_sh4_block(pair_loads, pair_services, 3u));
    constexpr std::array<std::uint32_t, 6u> expected_pair_addresses{
        0x0380u, 0x0384u, 0x0388u, 0x038Cu, 0x0390u, 0x0394u};
    require(pair_accesses.semantic_count() == expected_pair_addresses.size(),
            "FMOV-64-Loads emittieren nicht je zwei Datenreads.");
    for (std::size_t index = 0u; index < expected_pair_addresses.size(); ++index) {
        const auto& event = pair_accesses.semantic(index);
        require(event.operation == MemoryAccessOperation::Read &&
                    event.virtual_address == expected_pair_addresses[index] &&
                    event.instruction.source_pc == 0x1060u + (index / 2u) * 2u &&
                    event.instruction.runtime_pc == 0x2060u + (index / 2u) * 2u,
                "FMOV-64-Load liest High vor Low oder verliert seine Instruktionsherkunft.");
    }

    CpuState delay_slot;
    delay_slot.pc = 0x2040u;
    delay_slot.retired_guest_instructions = 77u;
    delay_slot.r[3] = 0x55667788u;
    delay_slot.r[4] = 0x0340u;
    delay_slot.memory.write_u16(0x2040u, 0xA001u); // BRA 0x2046
    delay_slot.memory.write_u16(0x2042u, 0x2432u); // MOV.L R3,@R4
    AccessCapture<4u> slot_accesses;
    delay_slot.memory.set_guest_memory_access_sink(slot_accesses.sink());
    Services slot_services;
    const auto slot_result = execute_dynamic_sh4_block(delay_slot, slot_services, 2u);
    require(slot_result.instructions == 2u && delay_slot.pc == 0x2046u &&
                slot_accesses.semantic_count() == 1u,
            "Delay-Slot-Test erreicht nicht exakt den Datenzugriff im Slot.");
    const auto& slot_write = slot_accesses.semantic(0u);
    require_instruction_origin(
        slot_write,
        0x1042u,
        0x2042u,
        79u,
        "Delay-Slot benutzt den Branch-PC als Herkunft.");
    require(slot_write.operation == MemoryAccessOperation::Write &&
                slot_write.virtual_address == 0x0340u &&
                slot_write.write_source == CodeWriteSource::Cpu,
            "Delay-Slot-Write besitzt falsche Adresse oder Writer-Quelle.");

    return EXIT_SUCCESS;
}
