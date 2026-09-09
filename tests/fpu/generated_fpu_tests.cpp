#include "generated_fpu_program.cpp"
#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/store_queue.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace katana_fpu_boundary_reference {
std::vector<std::uint32_t> observed_entries;
void note_instruction_entry(const std::uint32_t address, const bool) noexcept {
    observed_entries.push_back(address);
}
} // namespace katana_fpu_boundary_reference

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void prepare_fmov_cache_fixture(katana_generated::CpuState& cpu,
                                const std::uint32_t fpscr) {
    cpu.memory = katana::runtime::Memory(0u);
    auto backing = std::make_shared<katana::runtime::LinearMemoryDevice>(0x100u);
    cpu.memory.map_region("fixture-physical", 0u, backing);
    cpu.memory.set_lookup_mode(katana::runtime::MemoryLookupMode::Indexed);
    cpu.memory.bind_direct_linear_alias_window(0u, 0x100u, *backing);
    std::uint32_t guard_offset = 0u;
    require(katana::runtime::direct_linear_guard_offset(
                cpu.memory.direct_linear_memory_guard(false), 0x80000020u, 4u, guard_offset),
            "Die FMOV-Fixture muss den direkten Produkt-RAM-Pfad aktivieren.");
    cpu.memory.set_alignment_policy(katana::runtime::MemoryAlignmentPolicy::Strict);
    cpu.write_sr(0u);
    cpu.write_fpscr(fpscr);
    cpu.pr = 0x4000u;
    cpu.r[1] = 0xDEADBEEFu;
    cpu.fr[0] = 0xAAAAAAAAu;
    cpu.fr[2] = 0xBBBBBBBBu;
    cpu.fr[4] = 0xCCCCCCCCu;
    cpu.fr[6] = 0x40800000u;
    cpu.fr[8] = 0x40A00000u;
    cpu.fr[10] = 0x40C00000u;
    cpu.fr[12] = 0xDDDDDDDDu;
    cpu.memory.write_u32(0x20u, 0x3F800000u);
    cpu.memory.write_u32(0x24u, 0x3F900000u);
    cpu.memory.write_u32(0x34u, 0x40000000u);
    cpu.memory.write_u32(0x38u, 0x40100000u);
    cpu.memory.write_u32(0x48u, 0x40400000u);
    cpu.memory.write_u32(0x4Cu, 0x40500000u);
    cpu.memory.write_u32(0x50u, 0u);
    cpu.memory.write_u32(0x54u, 0u);
    cpu.memory.write_u32(0x58u, 0u);
    cpu.memory.write_u32(0x5Cu, 0u);
    cpu.memory.write_u32(0x78u, 0u);
    cpu.memory.write_u32(0x7Cu, 0u);
}

[[nodiscard]] bool same_fmov_cache_architecture(
    const katana_generated::CpuState& a,
    const katana_generated::CpuState& b) {
    return a.r == b.r && a.r_bank == b.r_bank && a.fr == b.fr && a.xf == b.xf &&
           a.pc == b.pc && a.pr == b.pr && a.fpscr == b.fpscr && a.sr == b.sr &&
           a.spc == b.spc && a.ssr == b.ssr && a.sgr == b.sgr &&
           a.fpul == b.fpul && a.t == b.t &&
           a.tea == b.tea && a.expevt == b.expevt &&
           a.trap_pending == b.trap_pending &&
           a.exception_generation == b.exception_generation &&
           a.last_exception_cause == b.last_exception_cause &&
           a.exception_in_delay_slot == b.exception_in_delay_slot &&
           a.last_exception_instruction_pc == b.last_exception_instruction_pc &&
           a.last_exception_instruction_physical_pc ==
               b.last_exception_instruction_physical_pc &&
           a.attempted_guest_instructions == b.attempted_guest_instructions &&
           a.retired_guest_instructions == b.retired_guest_instructions &&
           a.pending_guest_cycles == b.pending_guest_cycles;
}

template <typename Function>
void run_fmov_cache_fixture(katana_generated::CpuState& cpu, Function function) {
    // Reenter only the same statically generated function after its exact
    // store/MMIO resume edges. This is compiled execution, with no decoding.
    for (unsigned boundary = 0u; boundary < 16u; ++boundary) {
        function(cpu);
        if (cpu.trap_pending || cpu.pc < 0x200u || cpu.pc >= 0x22Au) return;
    }
    require(false, "Die kompilierte FMOV-Fixture hat ihren begrenzten Resume-Pfad nicht beendet.");
}

class GeneratedFpuServices final : public katana::runtime::PlatformServices {
  public:
    explicit GeneratedFpuServices(katana::runtime::CpuState& cpu)
        : cpu_(cpu), queues_(cpu.memory, [this](const auto& transfer) {
              if (transfer.target == katana::runtime::StoreQueueTarget::Ram) {
                  cpu_.memory.write_bytes_at(
                      transfer.target_address,
                      transfer.bytes,
                      katana::runtime::GuestMemoryAccessContext{
                          transfer.target_address,
                          transfer.instruction,
                          transfer.retired_guest_instructions},
                      katana::runtime::CodeWriteSource::StoreQueue);
              } else {
                  transfers.push_back(transfer);
              }
          }) {
        const auto registration = tracker_.register_block(
            {fmov_fixture_identity,
             fmov_fixture_start,
             fmov_fixture_size,
             "generated-fpu-test",
             {}});
        if (registration != katana::runtime::BlockRegistrationResult::Inserted) {
            throw std::runtime_error("Generated-FPU-Fixture konnte nicht registriert werden.");
        }
        cpu_.memory.set_guest_write_observer([this](const auto& event) {
            static_cast<void>(tracker_.observe_write(
                event.address, event.size, event.source, event.bytes_changed));
        });
    }

    ~GeneratedFpuServices() override {
        cpu_.memory.clear_guest_write_observer();
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "generated-sq";
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
            destination[index] = cpu_.memory.read_u8(address + static_cast<std::uint32_t>(index));
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
        if (on_consume_cycles) on_consume_cycles();
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
    [[nodiscard]] bool prefetch(katana::runtime::CpuState& cpu,
                                const katana::runtime::GuestInstructionOrigin instruction,
                                const std::uint32_t address) override {
        katana::runtime::prefetch(cpu, address);
        return queues_.prefetch(address,
                                instruction,
                                cpu.retired_guest_instructions,
                                cpu.attempted_guest_instructions);
    }
    [[nodiscard]] katana::runtime::ExecutableCodeTracker*
    executable_code_tracker() noexcept override {
        return &tracker_;
    }
    [[nodiscard]] bool fmov_fixture_intact() const noexcept {
        return tracker_.valid(fmov_fixture_identity) &&
               tracker_.tracks_address(fmov_fixture_start, fmov_fixture_size) &&
               !tracker_.tracks_address(fmov_fixture_start - 1u) &&
               !tracker_.tracks_address(fmov_fixture_start + fmov_fixture_size) &&
               tracker_.invalidation_count() == 0u;
    }

    katana::runtime::CpuState& cpu_;
    katana::runtime::Sh4StoreQueues queues_;
    std::vector<katana::runtime::StoreQueueTransfer> transfers;
    std::uint64_t scheduler_cycle_ = 0u;
    std::function<void()> on_consume_cycles;

  private:
    static constexpr const char* fmov_fixture_identity = "generated-fpu-fmov-fixture";
    static constexpr std::uint32_t fmov_fixture_start = 0x12Cu;
    static constexpr std::uint32_t fmov_fixture_size = 0x12u;
    katana::runtime::ExecutableCodeTracker tracker_;
};

} // namespace

int main() {
    using katana::runtime::read_dr_double;

    // The observer-emitted reference retains every architectural attempt. This
    // exercises a Sonic-like arithmetic run under normal, masked, trapping and
    // invalid modes, including a newer mapping overlapping its final opcodes.
    for (const auto entry : {0x190u, 0x19Cu})
    for (unsigned mapping_case = 0u; mapping_case != 3u; ++mapping_case) {
        const auto byte_size = entry == 0x190u ? 12u : 20u;
        const auto final_pc = entry + byte_size - 2u;
        std::optional<katana::runtime::ScopedCodeAddressMapping> outer;
        std::optional<katana::runtime::ScopedCodeAddressMapping> inner;
        if (mapping_case != 0u) outer.emplace(
            katana::runtime::CodeAddressMapping{0x190u, 0x8C010190u, 0x40u});
        if (mapping_case == 2u) inner.emplace(
            katana::runtime::CodeAddressMapping{final_pc - 2u,
                0xAC020000u + final_pc - 2u, 4u});
        constexpr std::array<std::uint32_t, 9> modes = {
            katana::runtime::fpscr_dn_mask,
            katana::runtime::fpscr_dn_mask | 1u,
            katana::runtime::fpscr_dn_mask | 2u,
            katana::runtime::fpscr_dn_mask | 3u,
            0u,
            katana::runtime::fpscr_dn_mask | katana::runtime::fpscr_exception_enable_mask,
            katana::runtime::fpscr_dn_mask | katana::runtime::fpscr_pr_mask,
            katana::runtime::fpscr_dn_mask | katana::runtime::fpscr_sz_mask,
            katana::runtime::fpscr_dn_mask | katana::runtime::fpscr_pr_mask |
                katana::runtime::fpscr_sz_mask};
        constexpr std::array<std::uint32_t, 6> inputs = {
            0x3FC00000u, 0u, 1u, 0x7F800000u, 0x7FBFFFFFu, 0x7F7FFFFFu};
        for (const auto mode : modes) for (const auto input : inputs)
        for (const bool disabled : {false, true}) {
            auto optimized = std::make_unique<katana_generated::CpuState>();
            auto reference = std::make_unique<katana_generated::CpuState>();
            for (auto* cpu : {optimized.get(), reference.get()}) {
                cpu->write_sr(disabled ? katana::runtime::sr_fd_mask : 0u);
                cpu->fpscr = mode;
                cpu->pc = katana::runtime::relocate_code_address(entry);
                cpu->vbr = 0x8000u;
                cpu->fr[0] = 0x3F800000u;
                cpu->fr[1] = 0x40000000u;
                cpu->fr[2] = 0x40000000u;
                cpu->fr[3] = input;
                cpu->fr[4] = 0x3F000000u;
                cpu->fr[5] = 0x3F800000u;
                cpu->fr[6] = 0x3F000000u;
                cpu->active_block_virtual_start = cpu->pc;
                cpu->active_block_physical_start =
                    katana::runtime::canonical_physical_address_inline(cpu->pc);
                cpu->active_block_size = byte_size;
            }
            katana_fpu_boundary_reference::observed_entries.clear();
            if (entry == 0x190u) {
                katana_generated::fn_00000190(*optimized);
                katana_fpu_boundary_reference::fn_00000190(*reference);
            } else {
                katana_generated::fn_0000019C(*optimized);
                katana_fpu_boundary_reference::fn_0000019C(*reference);
            }
            const auto& a = *optimized;
            const auto& b = *reference;
            require(a.fr == b.fr && a.fpscr == b.fpscr && a.sr == b.sr &&
                        a.pc == b.pc && a.spc == b.spc && a.expevt == b.expevt &&
                        a.trap_pending == b.trap_pending &&
                        a.exception_generation == b.exception_generation &&
                        a.last_exception_cause == b.last_exception_cause &&
                        a.last_exception_instruction_pc == b.last_exception_instruction_pc &&
                        a.exception_in_delay_slot == b.exception_in_delay_slot &&
                        a.active_instruction_pc == b.active_instruction_pc &&
                        a.active_instruction_physical_pc == b.active_instruction_physical_pc &&
                        a.attempted_guest_instructions == b.attempted_guest_instructions &&
                        a.retired_guest_instructions == b.retired_guest_instructions &&
                        a.pending_guest_cycles == b.pending_guest_cycles,
                    "FPU epoch diverged from per-instruction arithmetic, exception or PC state.");
            require(katana_fpu_boundary_reference::observed_entries.size() ==
                        b.attempted_guest_instructions,
                    "FPU reference lost an observable instruction boundary.");
            if (!a.trap_pending) require(
                a.retired_guest_instructions == byte_size / 2u &&
                    a.active_instruction_pc == katana::runtime::relocate_code_address(final_pc) &&
                    a.pc == katana::runtime::relocate_code_address(entry + byte_size),
                "FPU epoch lost its final opcode or fallthrough PC.");
        }
    }

    for (const auto entry : {0x176u, 0x17Eu, 0x188u}) {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        cpu.write_sr(0u);
        cpu.vbr = 0x8000u;
        cpu.pc = entry;
        cpu.fpscr = entry == 0x188u ? katana::runtime::fpscr_enable_inexact_mask :
                                      katana::runtime::fpscr_enable_divide_by_zero_mask;
        cpu.fr[0] = 0u;
        cpu.fr[2] = 0x40800000u;
        cpu.r[0] = 0x1234u;
        const auto before = cpu.fr;
        if (entry == 0x176u) katana_generated::fn_00000176(cpu);
        else if (entry == 0x17Eu) katana_generated::fn_0000017E(cpu);
        else katana_generated::fn_00000188(cpu);
        require(cpu.trap_pending && cpu.fr == before && cpu.spc == entry &&
                    cpu.r_bank[0] == 0x1234u &&
                    cpu.last_exception_instruction_pc == (entry == 0x17Eu ? 0x180u : entry) &&
                    cpu.exception_in_delay_slot == (entry == 0x17Eu),
                "Generated arithmetic continued after exception or lost delay owner.");
    }

    {
        auto prepare_split_store_fixture = [](katana_generated::CpuState& cpu) {
            cpu.memory = katana::runtime::Memory(0u);
            cpu.memory.set_alignment_policy(katana::runtime::MemoryAlignmentPolicy::Strict);
            cpu.memory.map_region(
                "loads_a", 0x20u, std::make_shared<katana::runtime::LinearMemoryDevice>(0x20u));
            cpu.memory.map_region(
                "loads_b", 0x40u, std::make_shared<katana::runtime::LinearMemoryDevice>(0x18u));
            cpu.memory.map_region(
                "first_store", 0x58u, std::make_shared<katana::runtime::LinearMemoryDevice>(4u));
            cpu.memory.map_region(
                "indexed_store", 0x78u,
                std::make_shared<katana::runtime::LinearMemoryDevice>(8u));
            cpu.write_sr(0u);
            cpu.write_fpscr(katana::runtime::fpscr_sz_mask);
            cpu.pr = 0x4000u;
            cpu.r[1] = 0xDEADBEEFu;
            cpu.fr[0] = 0xAAAAAAAAu;
            cpu.fr[2] = 0xBBBBBBBBu;
            cpu.fr[4] = 0xCCCCCCCCu;
            cpu.fr[6] = 0x40800000u;
            cpu.fr[8] = 0x40A00000u;
            cpu.fr[9] = 0x40B00000u;
            cpu.fr[10] = 0x40C00000u;
            cpu.fr[11] = 0x40D00000u;
            cpu.fr[12] = 0xDDDDDDDDu;
            cpu.memory.write_u32(0x20u, 0x3F800000u);
            cpu.memory.write_u32(0x24u, 0x3F900000u);
            cpu.memory.write_u32(0x34u, 0x40000000u);
            cpu.memory.write_u32(0x38u, 0x40100000u);
            cpu.memory.write_u32(0x48u, 0x40400000u);
            cpu.memory.write_u32(0x4Cu, 0x40500000u);
            cpu.memory.write_u32(0x58u, 0u);
            cpu.memory.write_u32(0x78u, 0u);
            cpu.pc = 0x200u;
        };
        auto optimized = std::make_unique<katana_generated::CpuState>();
        auto conservative = std::make_unique<katana_generated::CpuState>();
        prepare_split_store_fixture(*optimized);
        prepare_split_store_fixture(*conservative);
        run_fmov_cache_fixture(*optimized, katana_fpu_cache_optimized::fn_00000200);
        run_fmov_cache_fixture(*conservative, katana_fpu_cache_conservative::fn_00000200);
        require(same_fmov_cache_architecture(*optimized, *conservative) &&
                    optimized->last_exception_cause ==
                        katana::runtime::ExceptionCause::AddressErrorWrite &&
                    optimized->spc == 0x21Au && optimized->r[10] == 0x60u &&
                    optimized->memory.read_u32(0x58u) == 0x40A00000u &&
                    optimized->memory.read_u32(0x78u) == 0u,
                "SZ-FMOV-Predecrement schreibt nicht nur die gueltige erste Haelfte vor dem zweiten Halbwortfehler.");
    }

    {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        cpu.write_sr(katana::runtime::sr_fd_mask);
        cpu.vbr = 0x8000u;
        cpu.fr[0] = 0x3F800000u;
        cpu.fr[1] = 0x40000000u;
        cpu.pc = 0x100u;
        katana_generated::fn_00000100(cpu);
        require(cpu.last_exception_cause == katana::runtime::ExceptionCause::FpuDisabled &&
                    cpu.expevt == katana::runtime::event_fpu_disabled && cpu.spc == 0x100u &&
                    cpu.pc == 0x8100u && cpu.fr[1] == 0x40000000u,
                "SR.FD sperrt einen generierten FPU-Pfad nicht vor Zustandsaenderungen.");
    }

    {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        GeneratedFpuServices services(cpu);
        cpu.r[3] = 0x8C010000u;
        cpu.pc = 0x170u;
        katana_generated::fn_00000170_with_services(cpu, &services);
        require(cpu.prefetch_count == 1u && cpu.last_prefetch_address == 0x8C010000u &&
                    !cpu.last_prefetch_was_store_queue && services.queues_.transfer_count() == 0u,
                "Generiertes normales PREF erreicht die Runtime nicht.");
        services.queues_.write_qacr(0u, 0u);
        for (std::uint32_t offset = 0u; offset < 32u; offset += 4u) {
            services.queues_.write_p4(0xE0000000u + offset,
                                      0xA5000000u + offset,
                                      katana::runtime::MemoryAccessWidth::Word);
        }
        cpu.r[3] = 0xE0000000u;
        cpu.pc = 0x170u;
        katana_generated::fn_00000170_with_services(cpu, &services);
        require(cpu.prefetch_count == 2u && cpu.last_prefetch_was_store_queue &&
                    services.queues_.transfer_count() == 1u &&
                    cpu.memory.read_u32(0x00u) == 0xA5000000u &&
                    cpu.memory.read_u32(0x1Cu) == 0xA500001Cu,
                "Generiertes PREF uebertraegt SQ0 nicht ueber Plattformdienste in RAM.");

        services.queues_.write_qacr(1u, 0x10u);
        for (std::uint32_t offset = 0u; offset < 32u; offset += 4u) {
            services.queues_.write_p4(0xE2000020u + offset,
                                      0x5A000000u + offset,
                                      katana::runtime::MemoryAccessWidth::Word);
        }
        cpu.r[3] = 0xE2000020u;
        cpu.pc = 0x170u;
        katana_generated::fn_00000170_with_services(cpu, &services);
        require(services.queues_.transfer_count() == 2u && services.transfers.size() == 1u &&
                    services.transfers.front().queue == 1u &&
                    services.transfers.front().target ==
                        katana::runtime::StoreQueueTarget::TileAccelerator &&
                    services.transfers.front().target_address == 0x12000020u &&
                    services.transfers.front().bytes[0] == 0x00u &&
                    services.transfers.front().bytes[31] == 0x5Au,
                "Generiertes PREF uebertraegt SQ1 nicht ueber Plattformdienste zum TA-Sink.");
    }

    {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        cpu.fpul = 0x2000u;
        cpu.pc = 0x158u;
        katana_generated::fn_00000158(cpu);
        require(std::fabs(std::bit_cast<float>(cpu.fr[2]) - 0.70710677f) <= 2.0e-7f &&
                    std::fabs(std::bit_cast<float>(cpu.fr[3]) - 0.70710677f) <= 2.0e-7f,
                "Generiertes FSCA verlaesst die Konformanztoleranz.");

        cpu.fr[4] = std::bit_cast<std::uint32_t>(4.0f);
        cpu.pc = 0x15Eu;
        katana_generated::fn_0000015E(cpu);
        require(std::bit_cast<float>(cpu.fr[4]) == 0.5f,
                "Generiertes FSRRA liefert ein falsches Ergebnis.");

        for (std::uint8_t i = 0; i < 4u; ++i) {
            cpu.fr[i] = std::bit_cast<std::uint32_t>(static_cast<float>(i + 1u));
            cpu.fr[4u + i] = std::bit_cast<std::uint32_t>(1.0f);
        }
        cpu.pc = 0x164u;
        katana_generated::fn_00000164(cpu);
        require(std::bit_cast<float>(cpu.fr[3]) == 10.0f,
                "Generiertes FIPR liest die Vektoransichten falsch.");

        for (std::uint8_t i = 0; i < 16u; ++i) {
            cpu.xf[i] = std::bit_cast<std::uint32_t>(0.0f);
        }
        cpu.xf[0] = cpu.xf[5] = cpu.xf[10] = cpu.xf[15] = std::bit_cast<std::uint32_t>(1.0f);
        for (std::uint8_t i = 8u; i < 12u; ++i) {
            cpu.fr[i] = std::bit_cast<std::uint32_t>(static_cast<float>(i));
        }
        cpu.pc = 0x16Au;
        katana_generated::fn_0000016A(cpu);
        require(std::bit_cast<float>(cpu.fr[8]) == 8.0f &&
                    std::bit_cast<float>(cpu.fr[11]) == 11.0f,
                "Generiertes FTRV nutzt XMTRX oder FV8 falsch.");
    }

    {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        cpu.write_sr(katana::runtime::sr_fd_mask);
        cpu.memory.set_alignment_policy(katana::runtime::MemoryAlignmentPolicy::Strict);
        cpu.r[4] = 1u;
        cpu.pc = 0x12Cu;
        katana_generated::fn_0000012C(cpu);
        require(cpu.last_exception_cause == katana::runtime::ExceptionCause::FpuDisabled &&
                    cpu.expevt == katana::runtime::event_fpu_disabled && cpu.spc == 0x12Cu &&
                    cpu.tea == 0u,
                "SR.FD wird bei einem FPU-Speicheroperand nicht vor dem Adressfehler geprueft.");
    }

    {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        cpu.write_sr(katana::runtime::sr_fd_mask);
        cpu.vbr = 0x9000u;
        cpu.fr[0] = 0x3F800000u;
        cpu.fr[1] = 0x40000000u;
        cpu.pc = 0x13Eu;
        katana_generated::fn_0000013E(cpu);
        require(cpu.last_exception_cause == katana::runtime::ExceptionCause::SlotFpuDisabled &&
                    cpu.expevt == katana::runtime::event_slot_fpu_disabled && cpu.spc == 0x13Eu &&
                    cpu.pc == 0x9100u && cpu.exception_in_delay_slot && cpu.fr[1] == 0x40000000u,
                "FPU-Sperre im generierten BRA-Delay-Slot verliert Owner-PC oder Zustand.");
    }

    {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        cpu.fpul = 0xA5A5A5A5u;
        cpu.fr[3] = 0x3FC00000u;
        cpu.pc = 0x100u;
        katana_generated::fn_00000100(cpu);
        require(std::bit_cast<float>(cpu.fr[1]) == 2.0f && cpu.fr[2] == 0xA5A5A5A5u &&
                    cpu.fpul == 0x3FC00000u,
                "Generierte Single-Precision-Arithmetik oder FPUL-Transfers sind falsch.");
    }

    {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        cpu.fr[0] = 0x11111111u;
        cpu.xf[0] = 0x22222222u;
        cpu.pc = 0x110u;
        katana_generated::fn_00000110(cpu);
        require(cpu.fpu_register_bank_selected() && cpu.fpu_transfer_pair() &&
                    cpu.fr[0] == 0x22222222u && cpu.xf[0] == 0x11111111u,
                "Generiertes FRCHG oder FSCHG ist falsch.");
    }

    {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        cpu.write_fpscr(katana::runtime::fpscr_pr_mask);
        cpu.fr[0] = 0x11111111u;
        cpu.xf[0] = 0x22222222u;
        cpu.pc = 0x110u;
        katana_generated::fn_00000110(cpu);
        require(cpu.last_exception_cause == katana::runtime::ExceptionCause::IllegalInstruction &&
                    cpu.expevt == katana::runtime::event_illegal_instruction && cpu.spc == 0x110u &&
                    !cpu.fpu_register_bank_selected() && cpu.fr[0] == 0x11111111u &&
                    cpu.xf[0] == 0x22222222u,
                "FRCHG wird im Double-Precision-Modus nicht vor Bankaenderungen abgewiesen.");
    }

    {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        cpu.write_fpscr(katana::runtime::fpscr_pr_mask);
        cpu.fr[2] = 0xAAAAAAAAu;
        cpu.fr[3] = 0xBBBBBBBBu;
        cpu.pc = 0x146u;
        katana_generated::fn_00000146(cpu);
        require(cpu.last_exception_cause == katana::runtime::ExceptionCause::IllegalInstruction &&
                    cpu.expevt == katana::runtime::event_illegal_instruction && cpu.spc == 0x146u &&
                    cpu.fr[2] == 0xAAAAAAAAu && cpu.fr[3] == 0xBBBBBBBBu,
                "Ungerade Double-Precision-Register werden nicht vor Teilwirkungen abgewiesen.");
    }

    for (const std::uint32_t reserved_rm : {2u, 3u}) {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        cpu.write_fpscr(reserved_rm);
        cpu.fr[1] = 0x3F800000u;
        cpu.fr[3] = 0x40000000u;
        cpu.pc = 0x146u;
        katana_generated::fn_00000146(cpu);
        require(cpu.last_exception_cause == katana::runtime::ExceptionCause::IllegalInstruction &&
                    cpu.expevt == katana::runtime::event_illegal_instruction && cpu.spc == 0x146u &&
                    cpu.fr[3] == 0x40000000u,
                "Ein reservierter FPSCR.RM-Wert erreicht die FPU-Ausfuehrung.");
    }

    {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        cpu.fr[0] = std::bit_cast<std::uint32_t>(1.0f);
        cpu.fr[1] = std::bit_cast<std::uint32_t>(2.0f);
        cpu.fpul = 42u;
        cpu.pc = 0x118u;
        katana_generated::fn_00000118(cpu);
        require(cpu.t && cpu.fpul == 42u, "Generiertes FCMP/FLOAT/FTRC ist falsch.");
    }

    {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        cpu.write_fpscr(katana::runtime::fpscr_pr_mask);
        katana::runtime::write_dr_double(cpu, 0u, 1.5);
        katana::runtime::write_dr_double(cpu, 2u, 2.25);
        cpu.pc = 0x122u;
        katana_generated::fn_00000122(cpu);
        require(read_dr_double(cpu, 2u) == 3.75 && read_dr_double(cpu, 4u) == 3.75,
                "Generierte Double-Precision-Arithmetik oder FCNV-Konvertierung ist falsch.");
    }

    {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        cpu.memory.set_alignment_policy(katana::runtime::MemoryAlignmentPolicy::Strict);
        cpu.r[0] = 4u;
        cpu.r[4] = 0x40u;
        cpu.r[6] = 0x80u;
        cpu.memory.write_u32(0x40u, 0x40400000u);
        cpu.memory.write_u32(0x48u, 0x40400000u);
        cpu.fr[7] = 0x40800000u;
        cpu.pc = 0x12Cu;
        GeneratedFpuServices services(cpu);
        katana_generated::fn_0000012C_with_services(cpu, &services);
        require(cpu.fr[5] == 0x40400000u && cpu.fr[8] == 0x40400000u && cpu.fr[9] == 0x40400000u &&
                    cpu.r[4] == 0x44u && cpu.r[6] == 0x7Cu &&
                    cpu.memory.read_u32(0x7Cu) == 0x40400000u &&
                    cpu.memory.read_u32(0x80u) == 0x40400000u && services.fmov_fixture_intact(),
                "Generierte FMOV-Register- oder Speicherformen sind falsch.");
    }

    {
        auto optimized = std::make_unique<katana_generated::CpuState>();
        auto conservative = std::make_unique<katana_generated::CpuState>();
        prepare_fmov_cache_fixture(*optimized, 0u);
        prepare_fmov_cache_fixture(*conservative, 0u);
        optimized->pc = conservative->pc = 0x200u;
        run_fmov_cache_fixture(*optimized, katana_fpu_cache_optimized::fn_00000200);
        run_fmov_cache_fixture(*conservative, katana_fpu_cache_conservative::fn_00000200);
        require(same_fmov_cache_architecture(*optimized, *conservative) &&
                    optimized->r[0] == 0x08u && optimized->r[2] == 0x20u &&
                    optimized->r[4] == 0x38u && optimized->r[6] == 0x40u &&
                    optimized->r[8] == 0x50u && optimized->r[10] == 0x5Cu &&
                    optimized->r[12] == 0x70u && optimized->fr[0] == 0x3F800000u &&
                    optimized->fr[2] == 0x40000000u && optimized->fr[4] == 0x40400000u &&
                    optimized->fr[12] == 0x3F800000u &&
                    optimized->memory.read_u32(0x50u) == 0x40800000u &&
                    optimized->memory.read_u32(0x5Cu) == 0x40A00000u &&
                    optimized->memory.read_u32(0x78u) == 0x40C00000u,
                "Lokalisierte FMOV-Cachefolge verliert GPR-Mutationen, sechs Memoryformen oder den Delay-Load.");
    }

    {
        auto optimized = std::make_unique<katana_generated::CpuState>();
        auto conservative = std::make_unique<katana_generated::CpuState>();
        prepare_fmov_cache_fixture(
            *optimized,
            katana::runtime::fpscr_pr_mask | katana::runtime::fpscr_sz_mask);
        prepare_fmov_cache_fixture(
            *conservative,
            katana::runtime::fpscr_pr_mask | katana::runtime::fpscr_sz_mask);
        optimized->pc = conservative->pc = 0x200u;
        run_fmov_cache_fixture(*optimized, katana_fpu_cache_optimized::fn_00000200);
        run_fmov_cache_fixture(*conservative, katana_fpu_cache_conservative::fn_00000200);
        require(same_fmov_cache_architecture(*optimized, *conservative) &&
                    optimized->last_exception_cause ==
                        katana::runtime::ExceptionCause::IllegalInstruction &&
                    optimized->spc == 0x212u && optimized->r_bank[0] == 0x08u &&
                    optimized->r_bank[4] == 0x34u && optimized->fr[0] == 0xAAAAAAAAu &&
                    optimized->memory.read_u32(0x50u) == 0u &&
                    optimized->memory.read_u32(0x5Cu) == 0u,
                "FMOV bei gleichzeitigem PR+SZ wird nicht vor dem ersten Speicherzugriff abgewiesen.");
    }

    {
        auto optimized = std::make_unique<katana_generated::CpuState>();
        auto conservative = std::make_unique<katana_generated::CpuState>();
        prepare_fmov_cache_fixture(*optimized, 0u);
        prepare_fmov_cache_fixture(*conservative, 0u);
        optimized->write_sr(katana::runtime::sr_fd_mask);
        conservative->write_sr(katana::runtime::sr_fd_mask);
        optimized->r[2] = conservative->r[2] = 0x20u;
        optimized->fr[6] = conservative->fr[6] = 0xEEEEEEEEu;
        optimized->pc = conservative->pc = 0x222u;
        katana_fpu_cache_optimized::fn_00000222(*optimized);
        katana_fpu_cache_conservative::fn_00000222(*conservative);
        require(same_fmov_cache_architecture(*optimized, *conservative) &&
                    optimized->last_exception_cause ==
                        katana::runtime::ExceptionCause::SlotFpuDisabled &&
                    optimized->exception_in_delay_slot && optimized->spc == 0x226u &&
                    optimized->r_bank[2] == 0x20u &&
                    optimized->fr[6] == 0xEEEEEEEEu &&
                    optimized->memory.read_u32(0x20u) == 0x3F800000u,
                "FPU-disabled FMOV im RTS-Delay-Slot veraendert den Zielzustand.");
    }

    {
        auto optimized = std::make_unique<katana_generated::CpuState>();
        auto conservative = std::make_unique<katana_generated::CpuState>();
        prepare_fmov_cache_fixture(*optimized, 0u);
        prepare_fmov_cache_fixture(*conservative, 0u);
        std::array<bool, 2> observed_current_registers{};
        const auto attach_observer = [&](auto& cpu, const std::size_t index) {
            cpu.memory.set_guest_write_observer([&, index](const auto& event) {
                if (event.address != 0x50u) return;
                observed_current_registers[index] =
                    cpu.r[0] == 0x08u && cpu.r[4] == 0x38u;
                cpu.r[2] = 0x24u;
            });
        };
        attach_observer(*optimized, 0u);
        attach_observer(*conservative, 1u);
        optimized->pc = conservative->pc = 0x200u;
        run_fmov_cache_fixture(*optimized, katana_fpu_cache_optimized::fn_00000200);
        run_fmov_cache_fixture(*conservative, katana_fpu_cache_conservative::fn_00000200);
        require(same_fmov_cache_architecture(*optimized, *conservative) &&
                    observed_current_registers[0] && observed_current_registers[1] &&
                    optimized->r[2] == 0x24u && optimized->fr[12] == 0x3F900000u &&
                    optimized->memory.read_u32(0x50u) == 0x40800000u,
                "FMOV-Fallback zeigt dem allgemeinen Observer veraltete GPRs oder verliert seine Registermutation.");
    }

    {
        auto optimized = std::make_unique<katana_generated::CpuState>();
        auto conservative = std::make_unique<katana_generated::CpuState>();
        prepare_fmov_cache_fixture(*optimized, 0u);
        prepare_fmov_cache_fixture(*conservative, 0u);
        GeneratedFpuServices optimized_services(*optimized);
        GeneratedFpuServices conservative_services(*conservative);
        std::array<bool, 2> updated_address{};
        const auto attach_provider = [&](auto& cpu, auto& services, const std::size_t index) {
            auto* state = &cpu;
            services.on_consume_cycles = [state, index, &updated_address] {
                if (updated_address[index]) return;
                require(state->r[8] == 0x50u && state->r[4] == 0x38u,
                        "FMOV-Cycleprovider sieht veraltete lokalisierte Register.");
                state->r[8] = 0x54u;
                updated_address[index] = true;
            };
        };
        attach_provider(*optimized, optimized_services, 0u);
        attach_provider(*conservative, conservative_services, 1u);
        optimized->pc = conservative->pc = 0x200u;
        run_fmov_cache_fixture(*optimized, [&](auto& cpu) {
            katana_fpu_cache_optimized::fn_00000200_with_services(cpu, &optimized_services);
        });
        run_fmov_cache_fixture(*conservative, [&](auto& cpu) {
            katana_fpu_cache_conservative::fn_00000200_with_services(cpu, &conservative_services);
        });
        require(same_fmov_cache_architecture(*optimized, *conservative) &&
                    updated_address[0] && updated_address[1] &&
                    optimized->memory.read_u32(0x50u) == 0u &&
                    optimized->memory.read_u32(0x54u) == 0x40800000u,
                "FMOV verwendet nach dem Cycleprovider eine veraltete Storeadresse.");
    }

    {
        auto cpu_storage = std::make_unique<katana_generated::CpuState>();
        auto& cpu = *cpu_storage;
        cpu.memory = katana::runtime::Memory(0u);
        cpu.memory.map_region(
            "left", 0x1000u, std::make_shared<katana::runtime::LinearMemoryDevice>(16u));
        cpu.memory.map_region(
            "right", 0x1010u, std::make_shared<katana::runtime::LinearMemoryDevice>(16u));
        cpu.write_fpscr(katana::runtime::fpscr_sz_mask);
        cpu.memory.write_u32(0x100Cu, 0x89ABCDEFu);
        cpu.memory.write_u32(0x1010u, 0x01234567u);
        cpu.r[4] = 0x100Cu;
        cpu.pc = 0x14Cu;
        katana_generated::fn_0000014C(cpu);
        require(katana::runtime::read_fpu_pair_bits(cpu, 4u) == 0x0123456789ABCDEFull &&
                    cpu.r[4] == 0x1014u,
                "64-Bit-FMOV @R4+,FR4 scheitert an einer angrenzenden Regionsgrenze.");

        katana::runtime::write_fpu_pair_bits(cpu, 4u, 0xFEDCBA9876543210ull);
        cpu.r[4] = 0x1014u;
        cpu.pc = 0x152u;
        katana_generated::fn_00000152(cpu);
        require(cpu.r[4] == 0x100Cu && cpu.memory.read_u32(0x100Cu) == 0x76543210u &&
                    cpu.memory.read_u32(0x1010u) == 0xFEDCBA98u,
                "64-Bit-FMOV FR4,@-R4 scheitert bei identischem Registerindex.");

        cpu.r[4] = 0x101Cu;
        cpu.fr[4] = 0xAAAAAAAAu;
        cpu.fr[5] = 0xBBBBBBBBu;
        cpu.pc = 0x14Cu;
        katana_generated::fn_0000014C(cpu);
        require(cpu.last_exception_cause == katana::runtime::ExceptionCause::AddressErrorRead &&
                    cpu.spc == 0x14Cu && cpu.r_bank[4] == 0x101Cu && cpu.fr[4] == 0xAAAAAAAAu &&
                    cpu.fr[5] == 0xBBBBBBBBu,
                "Fehlgeschlagenes 64-Bit-FMOV an der Speichergrenze hinterlaesst Teilzustand.");
    }

    std::cout << "Generierter FPU-Pfad erfolgreich ausgefuehrt.\n";
    return EXIT_SUCCESS;
}
