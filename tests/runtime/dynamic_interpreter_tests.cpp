#include "katana/runtime/dynamic_interpreter.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
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
    [[nodiscard]] bool prefetch(CpuState&, std::uint32_t) override { return false; }

  private:
    std::uint64_t cycles_ = 0u;
};

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

    return EXIT_SUCCESS;
}
