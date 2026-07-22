#pragma once

#include "katana/runtime/abi.hpp"
#include "katana/runtime/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace katana::runtime {

inline constexpr std::uint32_t sr_t_mask = 0x00000001u;
inline constexpr std::uint32_t sr_s_mask = 0x00000002u;
inline constexpr std::uint32_t sr_interrupt_mask = 0x000000F0u;
inline constexpr std::uint32_t sr_q_mask = 0x00000100u;
inline constexpr std::uint32_t sr_m_mask = 0x00000200u;
inline constexpr std::uint32_t sr_fd_mask = 0x00008000u;
inline constexpr std::uint32_t sr_bl_mask = 0x10000000u;
inline constexpr std::uint32_t sr_rb_mask = 0x20000000u;
inline constexpr std::uint32_t sr_md_mask = 0x40000000u;
inline constexpr std::uint32_t sr_writable_mask = 0x700083F3u;

inline constexpr std::uint32_t fpscr_rounding_mode_mask = 0x00000003u;
inline constexpr std::uint32_t fpscr_dn_mask = 0x00040000u;
inline constexpr std::uint32_t fpscr_pr_mask = 0x00080000u;
inline constexpr std::uint32_t fpscr_sz_mask = 0x00100000u;
inline constexpr std::uint32_t fpscr_fr_mask = 0x00200000u;
inline constexpr std::uint32_t fpscr_writable_mask = 0x003FFFFFu;

inline constexpr std::size_t general_register_count = 16u;
inline constexpr std::size_t banked_register_count = 8u;
inline constexpr std::size_t fpu_register_count = 16u;

enum class ExceptionCause : std::uint8_t {
    None,
    Trap,
    IllegalInstruction,
    SlotIllegalInstruction,
    FpuDisabled,
    SlotFpuDisabled,
    AddressErrorRead,
    AddressErrorWrite,
    TlbMissRead,
    TlbMissWrite,
    InitialPageWrite,
    TlbProtectionRead,
    TlbProtectionWrite,
    TlbMultipleHit,
    BusErrorRead,
    BusErrorWrite,
    Interrupt
};

class RuntimeAddressSpace;
class DreamcastGdRomController;

enum class OperandCacheOperation : std::uint8_t { Invalidate, Purge, WriteBack };

struct Sh4TlbEntry {
    std::uint32_t pteh = 0u;
    std::uint32_t ptel = 0u;
    std::uint32_t ptea = 0u;
};

struct OperandCacheMaintenanceResult {
    OperandCacheOperation operation = OperandCacheOperation::Purge;
    std::uint32_t address = 0u;
    bool dirty_line_present = false;
    bool wrote_memory = false;
};

struct ResetState {
    std::uint32_t program_counter = 0u;
    std::uint32_t stack_pointer = 0u;
    std::uint32_t vector_base = 0u;
    std::uint32_t status_register = 0u;
    std::uint32_t fpscr = 0u;
};

struct CpuState {
    std::array<std::uint32_t, general_register_count> r{};
    std::array<std::uint32_t, banked_register_count> r_bank{};
    std::array<std::uint32_t, fpu_register_count> fr{};
    std::array<std::uint32_t, fpu_register_count> xf{};
    std::uint32_t pc = 0u;
    std::uint32_t pr = 0u;
    std::uint32_t gbr = 0u;
    std::uint32_t vbr = 0u;
    std::uint32_t ssr = 0u;
    std::uint32_t spc = 0u;
    std::uint32_t sgr = 0u;
    std::uint32_t dbr = 0u;
    std::uint32_t tra = 0u;
    std::uint32_t tea = 0u;
    std::uint32_t expevt = 0u;
    std::uint32_t intevt = 0u;
    std::uint32_t pteh = 0u;
    std::uint32_t ptel = 0u;
    std::uint32_t ptea = 0u;
    std::uint32_t ttb = 0u;
    std::uint32_t mmucr = 0u;
    std::array<Sh4TlbEntry, 64u> utlb{};
    std::uint64_t tlb_load_count = 0u;
    std::uint32_t mach = 0u;
    std::uint32_t macl = 0u;
    std::uint32_t fpul = 0u;
    std::uint32_t fpscr = 0u;
    std::uint32_t sr = 0u;
    bool t = false;
    bool s = false;
    bool q = false;
    bool m = false;
    bool trap_pending = false;
    ExceptionCause last_exception_cause = ExceptionCause::None;
    bool exception_in_delay_slot = false;
    bool sleeping = false;
    std::uint32_t last_prefetch_address = 0u;
    std::uint64_t prefetch_count = 0u;
    bool last_prefetch_was_store_queue = false;
    std::shared_ptr<RuntimeAddressSpace> address_space;
    DreamcastGdRomController* gdrom_services = nullptr;
    std::function<void(CpuState&)> disc_reboot;
    Memory memory{1024u * 1024u, MemoryAlignmentPolicy::Permissive};

    [[nodiscard]] std::uint32_t read_sr() const noexcept;
    void write_sr(std::uint32_t value) noexcept;
    [[nodiscard]] std::uint32_t read_fpscr() const noexcept;
    void write_fpscr(std::uint32_t value) noexcept;
    void toggle_fpu_register_bank() noexcept;
    [[nodiscard]] bool fpu_register_bank_selected() const noexcept;
    [[nodiscard]] bool fpu_double_precision() const noexcept;
    [[nodiscard]] bool fpu_transfer_pair() const noexcept;
    [[nodiscard]] bool fpu_flush_denormals() const noexcept;
    [[nodiscard]] std::uint8_t interrupt_mask() const noexcept;
    void set_interrupt_mask(std::uint8_t level) noexcept;
    [[nodiscard]] bool interrupts_blocked() const noexcept;
    [[nodiscard]] bool privileged_mode() const noexcept;
    [[nodiscard]] bool register_bank_selected() const noexcept;
    [[nodiscard]] bool fpu_disabled() const noexcept;
};

void reset_cpu(CpuState& cpu, const ResetState& state = ResetState{}) noexcept;

void prefetch(CpuState& cpu, std::uint32_t address) noexcept;
void load_tlb(CpuState& cpu) noexcept;

[[nodiscard]] std::uint32_t translate_guest_address(CpuState& cpu,
                                                    std::uint32_t address,
                                                    MemoryAccessOperation operation,
                                                    MemoryAccessWidth width,
                                                    bool instruction = false);
[[nodiscard]] std::uint8_t guest_read_u8(CpuState& cpu, std::uint32_t address);
[[nodiscard]] std::uint16_t guest_read_u16(CpuState& cpu, std::uint32_t address);
[[nodiscard]] std::uint32_t guest_read_u32(CpuState& cpu, std::uint32_t address);
[[nodiscard]] std::int32_t guest_read_s8(CpuState& cpu, std::uint32_t address);
[[nodiscard]] std::int32_t guest_read_s16(CpuState& cpu, std::uint32_t address);
void guest_write_u8(CpuState& cpu,
                    std::uint32_t address,
                    std::uint8_t value,
                    CodeWriteSource source = CodeWriteSource::Cpu);
void guest_write_u16(CpuState& cpu,
                     std::uint32_t address,
                     std::uint16_t value,
                     CodeWriteSource source = CodeWriteSource::Cpu);
void guest_write_u32(CpuState& cpu,
                     std::uint32_t address,
                     std::uint32_t value,
                     CodeWriteSource source = CodeWriteSource::Cpu);

// The reference runtime exposes stores directly through Memory and therefore has no hidden dirty
// operand-cache line. OCBI/OCBP/OCBWB still pass through this explicit contract so a future cache
// model can replace it without treating an instruction as an unknown or silent backend omission.
[[nodiscard]] OperandCacheMaintenanceResult
maintain_coherent_operand_cache(OperandCacheOperation operation, std::uint32_t address) noexcept;

[[noreturn]] void unresolved_call(CpuState& cpu, std::uint32_t target);
[[noreturn]] void unresolved_jump(CpuState& cpu, std::uint32_t target);

} // namespace katana::runtime
