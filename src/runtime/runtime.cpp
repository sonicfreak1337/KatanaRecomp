#include "katana/runtime/runtime.hpp"

#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace katana::runtime {

std::uint32_t CpuState::read_sr() const noexcept {
    return (sr & ~(sr_m_mask | sr_q_mask | sr_s_mask | sr_t_mask)) | (m ? sr_m_mask : 0u) |
           (q ? sr_q_mask : 0u) | (s ? sr_s_mask : 0u) | (t ? sr_t_mask : 0u);
}

void CpuState::write_sr(const std::uint32_t value) noexcept {
    const std::uint32_t masked = value & sr_writable_mask;
    const bool old_bank_one = (sr & (sr_md_mask | sr_rb_mask)) == (sr_md_mask | sr_rb_mask);
    const bool new_bank_one = (masked & (sr_md_mask | sr_rb_mask)) == (sr_md_mask | sr_rb_mask);
    if (old_bank_one != new_bank_one) {
        for (std::size_t index = 0u; index < r_bank.size(); ++index) {
            std::swap(r[index], r_bank[index]);
        }
    }
    sr = masked;
    m = (masked & sr_m_mask) != 0u;
    q = (masked & sr_q_mask) != 0u;
    s = (masked & sr_s_mask) != 0u;
    t = (masked & sr_t_mask) != 0u;
}

std::uint32_t CpuState::read_fpscr() const noexcept {
    return fpscr;
}

void CpuState::write_fpscr(const std::uint32_t value) noexcept {
    const std::uint32_t masked = value & fpscr_writable_mask;
    const bool old_fr = (fpscr & fpscr_fr_mask) != 0u;
    const bool new_fr = (masked & fpscr_fr_mask) != 0u;
    if (old_fr != new_fr) {
        fr.swap(xf);
    }
    fpscr = masked;
}

void CpuState::toggle_fpu_register_bank() noexcept {
    write_fpscr(read_fpscr() ^ fpscr_fr_mask);
}

bool CpuState::fpu_register_bank_selected() const noexcept {
    return (read_fpscr() & fpscr_fr_mask) != 0u;
}

bool CpuState::fpu_double_precision() const noexcept {
    return (read_fpscr() & fpscr_pr_mask) != 0u;
}

bool CpuState::fpu_transfer_pair() const noexcept {
    return (read_fpscr() & fpscr_sz_mask) != 0u;
}

bool CpuState::fpu_flush_denormals() const noexcept {
    return (read_fpscr() & fpscr_dn_mask) != 0u;
}

std::uint8_t CpuState::interrupt_mask() const noexcept {
    return static_cast<std::uint8_t>((read_sr() & sr_interrupt_mask) >> 4u);
}

void CpuState::set_interrupt_mask(const std::uint8_t level) noexcept {
    const std::uint32_t clamped = level > 15u ? 15u : level;
    write_sr((read_sr() & ~sr_interrupt_mask) | (clamped << 4u));
}

bool CpuState::interrupts_blocked() const noexcept {
    return (read_sr() & sr_bl_mask) != 0u;
}

bool CpuState::privileged_mode() const noexcept {
    return (read_sr() & sr_md_mask) != 0u;
}

bool CpuState::register_bank_selected() const noexcept {
    return (read_sr() & (sr_md_mask | sr_rb_mask)) == (sr_md_mask | sr_rb_mask);
}

bool CpuState::fpu_disabled() const noexcept {
    return (read_sr() & sr_fd_mask) != 0u;
}

void reset_cpu(CpuState& cpu, const ResetState& state) noexcept {
    cpu.r.fill(0u);
    cpu.r_bank.fill(0u);
    cpu.fr.fill(0u);
    cpu.xf.fill(0u);

    cpu.pc = state.program_counter;
    cpu.pr = 0u;
    cpu.gbr = 0u;
    cpu.vbr = state.vector_base;
    cpu.ssr = 0u;
    cpu.spc = 0u;
    cpu.sgr = 0u;
    cpu.dbr = 0u;
    cpu.tra = 0u;
    cpu.tea = 0u;
    cpu.expevt = 0u;
    cpu.intevt = 0u;
    cpu.pteh = 0u;
    cpu.ptel = 0u;
    cpu.ptea = 0u;
    cpu.ttb = 0u;
    cpu.mmucr = 0u;
    cpu.utlb.fill({});
    cpu.tlb_load_count = 0u;
    cpu.mach = 0u;
    cpu.macl = 0u;
    cpu.fpul = 0u;
    cpu.fpscr = 0u;
    cpu.write_fpscr(state.fpscr);

    cpu.sr = 0u;
    cpu.t = false;
    cpu.s = false;
    cpu.q = false;
    cpu.m = false;
    cpu.trap_pending = false;
    cpu.exception_generation = 0u;
    cpu.last_exception_cause = ExceptionCause::None;
    cpu.exception_in_delay_slot = false;
    cpu.last_exception_instruction_pc = 0u;
    cpu.last_exception_instruction_physical_pc = 0u;
    cpu.last_exception_owner_pc = 0u;
    cpu.last_exception_generation = 0u;
    cpu.sleeping = false;
    cpu.last_prefetch_address = 0u;
    cpu.prefetch_count = 0u;
    cpu.attempted_guest_instructions = 0u;
    cpu.retired_guest_instructions = 0u;
    cpu.total_guest_cycles = 0u;
    cpu.pending_guest_cycles = 0u;
    cpu.active_instruction_pc = 0u;
    cpu.active_instruction_physical_pc = 0u;
    cpu.active_block_virtual_start = 0u;
    cpu.active_block_physical_start = 0u;
    cpu.active_block_size = 0u;
    cpu.last_prefetch_was_store_queue = false;

    if (cpu.address_space) {
        cpu.address_space->set_mode(AddressTranslationMode::NoMmu);
        cpu.address_space->write_mmucr(0u);
        cpu.address_space->write_pteh(0u);
        cpu.address_space->clear_tlb();
    }

    cpu.r[15] = state.stack_pointer;
    cpu.write_sr(state.status_register);
}

void begin_guest_instruction(CpuState& cpu,
                             const std::uint32_t instruction_pc,
                             const std::uint64_t guest_cycles) noexcept {
    ++cpu.attempted_guest_instructions;
    cpu.active_instruction_pc = instruction_pc;
    const auto block_offset = instruction_pc - cpu.active_block_virtual_start;
    cpu.active_instruction_physical_pc =
        cpu.active_block_size != 0u && block_offset < cpu.active_block_size
            ? cpu.active_block_physical_start + block_offset
            : canonical_physical_address(instruction_pc);
    cpu.pending_guest_cycles += guest_cycles;
}

void add_guest_instruction_cycles(CpuState& cpu,
                                  const std::uint64_t guest_cycles) noexcept {
    cpu.pending_guest_cycles += guest_cycles;
}

void retire_guest_instruction(CpuState& cpu) noexcept {
    ++cpu.retired_guest_instructions;
}

std::uint64_t take_pending_guest_cycles(CpuState& cpu) noexcept {
    const auto cycles = cpu.pending_guest_cycles;
    cpu.pending_guest_cycles = 0u;
    cpu.total_guest_cycles += cycles;
    return cycles;
}

std::uint64_t elapsed_guest_cycles(const CpuState& cpu) noexcept {
    return cpu.total_guest_cycles + cpu.pending_guest_cycles;
}

GuestInstructionAttempt::GuestInstructionAttempt(CpuState& cpu,
                                                 const std::uint32_t instruction_pc,
                                                 const std::uint64_t guest_cycles) noexcept
    : cpu_(cpu),
      exception_generation_on_entry_(cpu.exception_generation),
      uncaught_exceptions_on_entry_(std::uncaught_exceptions()) {
    begin_guest_instruction(cpu_, instruction_pc, guest_cycles);
}

GuestInstructionAttempt::~GuestInstructionAttempt() noexcept {
    complete();
}

void GuestInstructionAttempt::complete() noexcept {
    if (!completed_ && cpu_.exception_generation == exception_generation_on_entry_ &&
        std::uncaught_exceptions() == uncaught_exceptions_on_entry_) {
        retire_guest_instruction(cpu_);
        completed_ = true;
    }
}

void prefetch(CpuState& cpu, const std::uint32_t address) noexcept {
    cpu.last_prefetch_address = address;
    ++cpu.prefetch_count;
    cpu.last_prefetch_was_store_queue = address >= 0xE0000000u && address <= 0xE3FFFFFFu;
}

void load_tlb(CpuState& cpu) noexcept {
    constexpr std::uint32_t mmucr_urc_shift = 10u;
    constexpr std::uint32_t mmucr_urc_mask = 0x3Fu;
    const auto index = static_cast<std::size_t>((cpu.mmucr >> mmucr_urc_shift) & mmucr_urc_mask);
    cpu.utlb[index] = {cpu.pteh, cpu.ptel, cpu.ptea};
    if (cpu.address_space) {
        const auto size_code = ((cpu.ptel >> 6u) & 2u) | ((cpu.ptel >> 4u) & 1u);
        constexpr std::array<std::uint32_t, 4u> page_sizes{1024u, 4096u, 65536u, 1048576u};
        const auto protection = static_cast<std::uint8_t>((cpu.ptel >> 5u) & 3u);
        cpu.address_space->ldtlb(TlbMapping{cpu.pteh & 0xFFFFFC00u,
                                           cpu.ptel & 0x1FFFFC00u,
                                           page_sizes[size_code],
                                           static_cast<std::uint8_t>(cpu.pteh & 0xFFu),
                                           static_cast<std::uint8_t>(index),
                                           (cpu.ptel & 0x00000100u) != 0u,
                                           true,
                                           (protection & 1u) != 0u,
                                           true,
                                           protection >= 2u,
                                           (cpu.ptel & 0x00000004u) != 0u,
                                           (cpu.ptel & 0x00000002u) != 0u});
    }
    ++cpu.tlb_load_count;
}

void advance_utlb_access_counter(CpuState& cpu, const std::uint64_t accesses) noexcept {
    if (accesses == 0u) return;
    constexpr std::uint32_t mmucr_urc_shift = 10u;
    constexpr std::uint32_t mmucr_urc_mask = 0x3Fu;
    constexpr std::uint32_t mmucr_urb_shift = 18u;
    const auto urc = (cpu.mmucr >> mmucr_urc_shift) & mmucr_urc_mask;
    const auto urb = (cpu.mmucr >> mmucr_urb_shift) & mmucr_urc_mask;
    const auto boundary = urb == 0u ? 64u : urb;
    const auto next =
        urc >= boundary
            ? static_cast<std::uint32_t>((accesses - 1u) % boundary)
            : static_cast<std::uint32_t>(
                  (static_cast<std::uint64_t>(urc) + (accesses % boundary)) % boundary);
    cpu.mmucr = (cpu.mmucr & ~(mmucr_urc_mask << mmucr_urc_shift)) |
                (next << mmucr_urc_shift);
    if (cpu.address_space) cpu.address_space->write_mmucr(cpu.mmucr);
}

namespace {

MemoryAccessErrorReason translation_reason(const ExceptionCause cause) noexcept {
    switch (cause) {
    case ExceptionCause::TlbMissRead:
    case ExceptionCause::TlbMissWrite:
        return MemoryAccessErrorReason::TlbMiss;
    case ExceptionCause::TlbMultipleHit:
        return MemoryAccessErrorReason::TlbMultipleHit;
    case ExceptionCause::InitialPageWrite:
        return MemoryAccessErrorReason::InitialPageWrite;
    case ExceptionCause::TlbProtectionRead:
    case ExceptionCause::TlbProtectionWrite:
        return MemoryAccessErrorReason::TlbProtection;
    default:
        return MemoryAccessErrorReason::Unmapped;
    }
}

void validate_virtual_alignment(const std::uint32_t address,
                                const MemoryAccessOperation operation,
                                const MemoryAccessWidth width,
                                const bool instruction) {
    const auto alignment = instruction ? 2u : static_cast<std::uint32_t>(width);
    if ((address & (alignment - 1u)) == 0u) return;
    throw MemoryAccessError(MemoryAccessErrorReason::Misaligned,
                            operation,
                            address,
                            width,
                            "sh4-virtual-address");
}

void note_translation(CpuState& cpu, const TranslationResult& translated) noexcept {
    if (translated.utlb_slot != 0xFFu) advance_utlb_access_counter(cpu, 1u);
}

template <typename Function>
decltype(auto) with_guest_virtual_address(const std::uint32_t virtual_address,
                                          Function&& function) {
    try {
        return std::forward<Function>(function)();
    } catch (const MemoryAccessError& error) {
        if (error.address() == virtual_address) throw;
        throw MemoryAccessError(error.reason(),
                                error.operation(),
                                virtual_address,
                                error.width(),
                                error.region_name(),
                                error.instruction());
    }
}

template <typename Function>
decltype(auto) with_guest_instruction_origin(
    const std::uint32_t virtual_address,
    const GuestInstructionOrigin origin,
    Function&& function) {
    try {
        return with_guest_virtual_address(
            virtual_address, std::forward<Function>(function));
    } catch (const MemoryAccessError& error) {
        if (error.instruction().valid) throw;
        throw MemoryAccessError(error.reason(),
                                error.operation(),
                                error.address(),
                                error.width(),
                                error.region_name(),
                                origin);
    }
}

bool direct_untranslated_data_physical(
    CpuState& cpu,
    const std::uint32_t virtual_address,
    const MemoryAccessWidth width,
    std::uint32_t& physical_address) noexcept {
    const auto alignment = static_cast<std::uint32_t>(width);
    if ((virtual_address & (alignment - 1u)) != 0u)
        return false;

    const auto segment = virtual_address >> 29u;
    if (segment == 4u || segment == 5u) {
        if (!cpu.privileged_mode()) return false;
    } else {
        const bool no_mmu = cpu.address_space == nullptr ||
                            cpu.address_space->mode() ==
                                AddressTranslationMode::NoMmu;
        if (!no_mmu || segment >= 7u ||
            (!cpu.privileged_mode() && virtual_address >= 0x80000000u))
            return false;
    }
    physical_address = canonical_physical_address(virtual_address);
    constexpr auto physical_main_ram_base = dreamcast_main_ram_area_bases.front();
    constexpr auto physical_main_ram_end =
        physical_main_ram_base +
        static_cast<std::uint32_t>(dreamcast_main_ram_size *
                                   dreamcast_main_ram_mirrors_per_area);
    return physical_address >= physical_main_ram_base &&
           physical_address < physical_main_ram_end;
}

} // namespace

std::uint32_t translate_guest_address(CpuState& cpu,
                                      const std::uint32_t address,
                                      const MemoryAccessOperation operation,
                                       const MemoryAccessWidth width,
                                       const bool instruction) {
    validate_virtual_alignment(address, operation, width, instruction);
    if (!cpu.address_space) {
        if (!cpu.privileged_mode() && address >= 0x80000000u)
            throw MemoryAccessError(MemoryAccessErrorReason::Unmapped,
                                    operation,
                                    address,
                                    width,
                                    "sh4-user-segment");
        if (instruction && (address >> 29u) >= 7u)
            throw MemoryAccessError(MemoryAccessErrorReason::Unmapped,
                                    operation,
                                    address,
                                    width,
                                    "sh4-instruction-segment");
        return canonical_physical_address(address);
    }
    const auto access = instruction ? TranslationAccess::Instruction
                                    : operation == MemoryAccessOperation::Write
                                          ? TranslationAccess::Write
                                          : TranslationAccess::Read;
    try {
        const auto translated =
            cpu.address_space->translate(address, access, cpu.privileged_mode());
        note_translation(cpu, translated);
        return translated.physical_address;
    } catch (const TranslationError& error) {
        throw MemoryAccessError(
            translation_reason(error.cause()), operation, address, width, "sh4-address-space");
    }
}

std::uint32_t peek_guest_u32(
    const CpuState& cpu,
    const std::uint32_t address,
    const std::span<const MemoryDevice* const> permitted_devices) {
    std::uint32_t physical = 0u;
    try {
        physical = cpu.address_space
                       ? cpu.address_space
                             ->translate(address, TranslationAccess::Read, cpu.privileged_mode())
                             .physical_address
                       : canonical_physical_address(address);
    } catch (const TranslationError& error) {
        throw MemoryAccessError(translation_reason(error.cause()),
                                MemoryAccessOperation::Read,
                                address,
                                MemoryAccessWidth::Word,
                                "sh4-address-space-diagnostic-peek");
    }
    return cpu.memory.peek_u32(physical, permitted_devices);
}

StoreQueuePrefetchTranslation translate_store_queue_prefetch(CpuState& cpu,
                                                              const std::uint32_t address) {
    try {
        StoreQueuePrefetchTranslation translated;
        if (!cpu.address_space) {
            RuntimeAddressSpace fallback_address_space;
            fallback_address_space.write_mmucr(cpu.mmucr);
            fallback_address_space.set_mode((cpu.mmucr & 1u) != 0u
                                                ? AddressTranslationMode::Mmu
                                                : AddressTranslationMode::NoMmu);
            translated = fallback_address_space.translate_store_queue_prefetch(
                address, cpu.privileged_mode());
        } else {
            translated = cpu.address_space->translate_store_queue_prefetch(
                address, cpu.privileged_mode());
        }
        if (translated.addressing == StoreQueueAddressingMode::Utlb)
            advance_utlb_access_counter(cpu, 1u);
        return translated;
    } catch (const TranslationError& error) {
        throw MemoryAccessError((address & 3u) != 0u
                                    ? MemoryAccessErrorReason::Misaligned
                                    : translation_reason(error.cause()),
                                MemoryAccessOperation::Write,
                                address,
                                MemoryAccessWidth::Word,
                                "sh4-store-queue-prefetch");
    }
}

std::uint16_t guest_fetch_u16(CpuState& cpu, const std::uint32_t address) {
    const auto physical = translate_guest_address(
        cpu, address, MemoryAccessOperation::Read, MemoryAccessWidth::Halfword, true);
    cpu.active_instruction_physical_pc = physical;
    return with_guest_virtual_address(
        address, [&] { return cpu.memory.read_u16(physical); });
}

std::uint8_t guest_read_u8(CpuState& cpu, const std::uint32_t address) {
    std::uint32_t direct_physical = 0u;
    std::uint8_t direct_value = 0u;
    if (direct_untranslated_data_physical(
            cpu, address, MemoryAccessWidth::Byte, direct_physical) &&
        cpu.memory.try_read_direct_linear_u8(direct_physical, direct_value))
        return direct_value;
    const auto physical = translate_guest_address(
        cpu, address, MemoryAccessOperation::Read, MemoryAccessWidth::Byte);
    return with_guest_virtual_address(
        address, [&] { return cpu.memory.read_u8(physical); });
}

std::uint16_t guest_read_u16(CpuState& cpu, const std::uint32_t address) {
    std::uint32_t direct_physical = 0u;
    std::uint16_t direct_value = 0u;
    if (direct_untranslated_data_physical(
            cpu, address, MemoryAccessWidth::Halfword, direct_physical) &&
        cpu.memory.try_read_direct_linear_u16(direct_physical, direct_value))
        return direct_value;
    const auto physical = translate_guest_address(
        cpu, address, MemoryAccessOperation::Read, MemoryAccessWidth::Halfword);
    return with_guest_virtual_address(
        address, [&] { return cpu.memory.read_u16(physical); });
}

std::uint32_t guest_read_u32(CpuState& cpu, const std::uint32_t address) {
    std::uint32_t direct_physical = 0u;
    std::uint32_t direct_value = 0u;
    if (direct_untranslated_data_physical(
            cpu, address, MemoryAccessWidth::Word, direct_physical) &&
        cpu.memory.try_read_direct_linear_u32(direct_physical, direct_value))
        return direct_value;
    const auto physical = translate_guest_address(
        cpu, address, MemoryAccessOperation::Read, MemoryAccessWidth::Word);
    return with_guest_virtual_address(
        address, [&] { return cpu.memory.read_u32(physical); });
}

std::int32_t guest_read_s8(CpuState& cpu, const std::uint32_t address) {
    return static_cast<std::int8_t>(guest_read_u8(cpu, address));
}

std::int32_t guest_read_s16(CpuState& cpu, const std::uint32_t address) {
    return static_cast<std::int16_t>(guest_read_u16(cpu, address));
}

std::uint8_t guest_read_u8_at(CpuState& cpu,
                              const GuestInstructionOrigin origin,
                              const std::uint32_t virtual_address) {
    return with_guest_instruction_origin(virtual_address, origin, [&] {
        std::uint32_t direct_physical = 0u;
        std::uint8_t direct_value = 0u;
        if (direct_untranslated_data_physical(
                cpu,
                virtual_address,
                MemoryAccessWidth::Byte,
                direct_physical) &&
            cpu.memory.try_read_direct_linear_u8(
                direct_physical, direct_value))
            return direct_value;
        const auto physical_address = translate_guest_address(
            cpu,
            virtual_address,
            MemoryAccessOperation::Read,
            MemoryAccessWidth::Byte);
        return cpu.memory.read_u8_at(
            physical_address,
            GuestMemoryAccessContext{
                virtual_address,
                origin,
                cpu.retired_guest_instructions,
                GuestMemoryAccessOrigin::Memory,
                cpu.attempted_guest_instructions});
    });
}

std::uint16_t guest_read_u16_at(CpuState& cpu,
                                const GuestInstructionOrigin origin,
                                const std::uint32_t virtual_address) {
    return with_guest_instruction_origin(virtual_address, origin, [&] {
        std::uint32_t direct_physical = 0u;
        std::uint16_t direct_value = 0u;
        if (direct_untranslated_data_physical(
                cpu,
                virtual_address,
                MemoryAccessWidth::Halfword,
                direct_physical) &&
            cpu.memory.try_read_direct_linear_u16(
                direct_physical, direct_value))
            return direct_value;
        const auto physical_address = translate_guest_address(
            cpu,
            virtual_address,
            MemoryAccessOperation::Read,
            MemoryAccessWidth::Halfword);
        return cpu.memory.read_u16_at(
            physical_address,
            GuestMemoryAccessContext{
                virtual_address,
                origin,
                cpu.retired_guest_instructions,
                GuestMemoryAccessOrigin::Memory,
                cpu.attempted_guest_instructions});
    });
}

std::uint32_t guest_read_u32_at(CpuState& cpu,
                                const GuestInstructionOrigin origin,
                                const std::uint32_t virtual_address) {
    return with_guest_instruction_origin(virtual_address, origin, [&] {
        std::uint32_t direct_physical = 0u;
        std::uint32_t direct_value = 0u;
        if (direct_untranslated_data_physical(
                cpu,
                virtual_address,
                MemoryAccessWidth::Word,
                direct_physical) &&
            cpu.memory.try_read_direct_linear_u32(
                direct_physical, direct_value))
            return direct_value;
        const auto physical_address = translate_guest_address(
            cpu,
            virtual_address,
            MemoryAccessOperation::Read,
            MemoryAccessWidth::Word);
        return cpu.memory.read_u32_at(
            physical_address,
            GuestMemoryAccessContext{
                virtual_address,
                origin,
                cpu.retired_guest_instructions,
                GuestMemoryAccessOrigin::Memory,
                cpu.attempted_guest_instructions});
    });
}

std::int32_t guest_read_s8_at(CpuState& cpu,
                              const GuestInstructionOrigin origin,
                              const std::uint32_t virtual_address) {
    return static_cast<std::int8_t>(guest_read_u8_at(cpu, origin, virtual_address));
}

std::int32_t guest_read_s16_at(CpuState& cpu,
                               const GuestInstructionOrigin origin,
                               const std::uint32_t virtual_address) {
    return static_cast<std::int16_t>(guest_read_u16_at(cpu, origin, virtual_address));
}

void guest_write_u8(CpuState& cpu,
                    const std::uint32_t address,
                    const std::uint8_t value,
                    const CodeWriteSource source) {
    std::uint32_t direct_physical = 0u;
    if (direct_untranslated_data_physical(
            cpu, address, MemoryAccessWidth::Byte, direct_physical) &&
        cpu.memory.try_write_direct_linear_u8(direct_physical, value, source))
        return;
    const auto physical = translate_guest_address(
        cpu, address, MemoryAccessOperation::Write, MemoryAccessWidth::Byte);
    with_guest_virtual_address(
        address, [&] { cpu.memory.write_u8(physical, value, source); });
}

void guest_write_u16(CpuState& cpu,
                     const std::uint32_t address,
                     const std::uint16_t value,
                     const CodeWriteSource source) {
    std::uint32_t direct_physical = 0u;
    if (direct_untranslated_data_physical(
            cpu, address, MemoryAccessWidth::Halfword, direct_physical) &&
        cpu.memory.try_write_direct_linear_u16(direct_physical, value, source))
        return;
    const auto physical = translate_guest_address(
        cpu, address, MemoryAccessOperation::Write, MemoryAccessWidth::Halfword);
    with_guest_virtual_address(
        address, [&] { cpu.memory.write_u16(physical, value, source); });
}

void guest_write_u32(CpuState& cpu,
                     const std::uint32_t address,
                     const std::uint32_t value,
                     const CodeWriteSource source) {
    std::uint32_t direct_physical = 0u;
    if (direct_untranslated_data_physical(
            cpu, address, MemoryAccessWidth::Word, direct_physical) &&
        cpu.memory.try_write_direct_linear_u32(direct_physical, value, source))
        return;
    const auto physical = translate_guest_address(
        cpu, address, MemoryAccessOperation::Write, MemoryAccessWidth::Word);
    with_guest_virtual_address(
        address, [&] { cpu.memory.write_u32(physical, value, source); });
}

void guest_write_u8_at(CpuState& cpu,
                       const GuestInstructionOrigin origin,
                       const std::uint32_t virtual_address,
                       const std::uint8_t value,
                       const CodeWriteSource source) {
    with_guest_instruction_origin(virtual_address, origin, [&] {
        std::uint32_t direct_physical = 0u;
        if (direct_untranslated_data_physical(
                cpu,
                virtual_address,
                MemoryAccessWidth::Byte,
                direct_physical) &&
            cpu.memory.try_write_direct_linear_u8(
                direct_physical, value, source))
            return;
        const auto physical_address = translate_guest_address(
            cpu,
            virtual_address,
            MemoryAccessOperation::Write,
            MemoryAccessWidth::Byte);
        cpu.memory.write_u8_at(
            physical_address,
            value,
            GuestMemoryAccessContext{
                virtual_address,
                origin,
                cpu.retired_guest_instructions,
                GuestMemoryAccessOrigin::Memory,
                cpu.attempted_guest_instructions},
            source);
    });
}

void guest_write_u16_at(CpuState& cpu,
                        const GuestInstructionOrigin origin,
                        const std::uint32_t virtual_address,
                        const std::uint16_t value,
                        const CodeWriteSource source) {
    with_guest_instruction_origin(virtual_address, origin, [&] {
        std::uint32_t direct_physical = 0u;
        if (direct_untranslated_data_physical(
                cpu,
                virtual_address,
                MemoryAccessWidth::Halfword,
                direct_physical) &&
            cpu.memory.try_write_direct_linear_u16(
                direct_physical, value, source))
            return;
        const auto physical_address = translate_guest_address(
            cpu,
            virtual_address,
            MemoryAccessOperation::Write,
            MemoryAccessWidth::Halfword);
        cpu.memory.write_u16_at(
            physical_address,
            value,
            GuestMemoryAccessContext{
                virtual_address,
                origin,
                cpu.retired_guest_instructions,
                GuestMemoryAccessOrigin::Memory,
                cpu.attempted_guest_instructions},
            source);
    });
}

void guest_write_u32_at(CpuState& cpu,
                        const GuestInstructionOrigin origin,
                        const std::uint32_t virtual_address,
                        const std::uint32_t value,
                        const CodeWriteSource source) {
    with_guest_instruction_origin(virtual_address, origin, [&] {
        std::uint32_t direct_physical = 0u;
        if (direct_untranslated_data_physical(
                cpu,
                virtual_address,
                MemoryAccessWidth::Word,
                direct_physical) &&
            cpu.memory.try_write_direct_linear_u32(
                direct_physical, value, source))
            return;
        const auto physical_address = translate_guest_address(
            cpu,
            virtual_address,
            MemoryAccessOperation::Write,
            MemoryAccessWidth::Word);
        cpu.memory.write_u32_at(
            physical_address,
            value,
            GuestMemoryAccessContext{
                virtual_address,
                origin,
                cpu.retired_guest_instructions,
                GuestMemoryAccessOrigin::Memory,
                cpu.attempted_guest_instructions},
            source);
    });
}

OperandCacheMaintenanceResult
maintain_coherent_operand_cache(const OperandCacheOperation operation,
                                const std::uint32_t address) noexcept {
    return OperandCacheMaintenanceResult{operation, address, false, false};
}

[[noreturn]] void unresolved_call(CpuState& cpu, const std::uint32_t target) {
    cpu.pc = target;
    throw std::runtime_error("Nicht aufgeloester Aufruf");
}

[[noreturn]] void unresolved_jump(CpuState& cpu, const std::uint32_t target) {
    cpu.pc = target;
    throw std::runtime_error("Nicht aufgeloester Sprung");
}

} // namespace katana::runtime
