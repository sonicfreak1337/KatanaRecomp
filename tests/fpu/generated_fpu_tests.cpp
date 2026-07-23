#include "generated_fpu_program.cpp"
#include "katana/runtime/store_queue.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class StoreQueueServices final : public katana::runtime::PlatformServices {
  public:
    explicit StoreQueueServices(katana::runtime::CpuState& cpu)
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
          }) {}

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
        return queues_.prefetch(address, instruction, cpu.retired_guest_instructions);
    }

    katana::runtime::CpuState& cpu_;
    katana::runtime::Sh4StoreQueues queues_;
    std::vector<katana::runtime::StoreQueueTransfer> transfers;
    std::uint64_t scheduler_cycle_ = 0u;
};

} // namespace

int main() {
    using katana::runtime::read_dr_double;

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
        StoreQueueServices services(cpu);
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
        katana_generated::fn_0000012C(cpu);
        require(cpu.fr[5] == 0x40400000u && cpu.fr[8] == 0x40400000u && cpu.fr[9] == 0x40400000u &&
                    cpu.r[4] == 0x44u && cpu.r[6] == 0x7Cu &&
                    cpu.memory.read_u32(0x7Cu) == 0x40400000u &&
                    cpu.memory.read_u32(0x80u) == 0x40400000u,
                "Generierte FMOV-Register- oder Speicherformen sind falsch.");
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
