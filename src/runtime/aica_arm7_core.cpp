#include "aica_arm7_core.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4018 4100 4101 4189 4244 4245 4267 4389 4505 4701 4703 4805)
#endif

#if defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#else
#define FORCE_INLINE inline
#endif
#define SB_UNLIKELY(value) (value)
#define SB_LIKELY(value) (value)
#define SB_BFE(value, offset, size) (((value) >> (offset)) & ((1ull << (size)) - 1ull))
#include "skyemu/arm7.h"
#undef SB_BFE
#undef SB_LIKELY
#undef SB_UNLIKELY
#undef FORCE_INLINE

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace katana::runtime {
namespace {

constexpr std::uint32_t arm_address_mask = 0x00FFFFFFu;
constexpr std::uint32_t arm_register_base = 0x00800000u;
constexpr std::uint32_t arm_register_mask = 0x00007FFFu;
constexpr std::uint32_t arm_ram_mask = 0x001FFFFFu;
constexpr std::size_t required_arm_ram_size = 0x00200000u;
constexpr std::uint32_t arm_pc = 15u;
constexpr std::uint32_t arm_cpsr = 16u;
constexpr std::uint32_t arm_r13_fiq = 22u;
constexpr std::uint32_t arm_r14_fiq = 23u;
constexpr std::uint32_t arm_r13_irq = 24u;
constexpr std::uint32_t arm_r13_svc = 26u;
constexpr std::uint32_t arm_spsr_fiq = 32u;
constexpr std::uint32_t arm_phase_none = 0u;
constexpr std::uint32_t arm_phase_fill_pipeline = 1u;
constexpr std::uint32_t arm_phase_block_transfer = 2u;
constexpr std::uint32_t arm_mode_mask = 0x1Fu;
constexpr std::uint32_t arm_mode_fiq = 0x11u;
constexpr std::uint32_t arm_mode_svc = 0x13u;
constexpr std::uint32_t arm_thumb_mask = 1u << 5u;
constexpr std::uint32_t arm_fiq_disable_mask = 1u << 6u;
constexpr std::uint32_t arm_irq_disable_mask = 1u << 7u;

[[nodiscard]] bool valid_arm_mode(const std::uint32_t mode) noexcept {
    switch (mode) {
    case 0x10u:
    case 0x11u:
    case 0x12u:
    case 0x13u:
    case 0x17u:
    case 0x1Bu:
    case 0x1Fu:
        return true;
    }
    return false;
}

} // namespace

struct AicaArm7Core::Impl final {
    std::weak_ptr<AicaRegisterFile> registers;
    std::weak_ptr<LinearMemoryDevice> ram;
    arm7_t cpu{};
    std::uint64_t bus_cycles = 0u;
    std::uint64_t executed_cycles = 0u;
    std::uint64_t executed_instructions = 0u;
    std::uint64_t cycle_debt = 0u;
    bool enabled = false;
    bool fiq_line = false;
    bool faulted = false;

    static void initialize_tables() {
        static std::once_flag once;
        std::call_once(once, [] {
            static_cast<void>(arm7_init(nullptr));
        });
    }

    void install_callbacks() noexcept {
        cpu.user_data = this;
        cpu.read32 = &read32;
        cpu.read16 = &read16;
        cpu.read32_seq = &read32_sequential;
        cpu.read16_seq = &read16_sequential;
        cpu.read8 = &read8;
        cpu.write32 = &write32;
        cpu.write16 = &write16;
        cpu.write8 = &write8;
        cpu.coprocessor_read = &coprocessor_read;
        cpu.coprocessor_write = &coprocessor_write;
        cpu.trigger_breakpoint = &breakpoint;
    }

    void reset(const bool next_enabled) noexcept {
        cpu = {};
        cpu.prefetch_pc = std::numeric_limits<std::uint32_t>::max();
        cpu.phased_op_id = arm_phase_fill_pipeline;
        cpu.registers[arm_cpsr] = arm_mode_svc | arm_fiq_disable_mask;
        cpu.registers[arm_r13_svc] = 0x03007F00u;
        cpu.registers[arm_r13_irq] = 0x03007FA0u;
        cpu.registers[arm_r13_fiq] = 0x03007FC0u;
        install_callbacks();
        bus_cycles = 0u;
        executed_cycles = 0u;
        executed_instructions = 0u;
        cycle_debt = 0u;
        enabled = next_enabled;
        fiq_line = false;
        faulted = false;
    }

    [[nodiscard]] std::shared_ptr<LinearMemoryDevice> lock_ram() noexcept {
        auto value = ram.lock();
        if (!value || value->size() < required_arm_ram_size) faulted = true;
        return value;
    }

    [[nodiscard]] std::shared_ptr<AicaRegisterFile> lock_registers() noexcept {
        auto value = registers.lock();
        if (!value) faulted = true;
        return value;
    }

    template <typename Value>
    [[nodiscard]] Value read_memory(const std::uint32_t source_address) noexcept {
        ++bus_cycles;
        const auto address = source_address & arm_address_mask;
        try {
            if (address < arm_register_base) {
                const auto memory = lock_ram();
                if (!memory) return 0u;
                if constexpr (sizeof(Value) == 1u) {
                    return memory->read_u8(address & arm_ram_mask);
                } else if constexpr (sizeof(Value) == 2u) {
                    return memory->read_u16((address & arm_ram_mask) & ~1u);
                } else {
                    return memory->read_u32((address & arm_ram_mask) & ~3u);
                }
            }
            const auto register_file = lock_registers();
            if (!register_file) return 0u;
            const auto offset = address & arm_register_mask;
            if constexpr (sizeof(Value) == 1u) {
                return static_cast<Value>(
                    register_file->read_arm(offset, MemoryAccessWidth::Byte));
            } else if constexpr (sizeof(Value) == 2u) {
                return static_cast<Value>(
                    register_file->read_arm(offset & ~1u, MemoryAccessWidth::Halfword));
            } else {
                // AICA's ARM-side 32-bit register accesses use the low 16-bit port.
                return static_cast<Value>(
                    register_file->read_arm(offset & ~1u, MemoryAccessWidth::Halfword));
            }
        } catch (...) {
            faulted = true;
            return 0u;
        }
    }

    template <typename Value>
    void write_memory(const std::uint32_t target_address, const Value value) noexcept {
        ++bus_cycles;
        const auto address = target_address & arm_address_mask;
        try {
            if (address < arm_register_base) {
                const auto memory = lock_ram();
                if (!memory) return;
                if constexpr (sizeof(Value) == 1u)
                    memory->write_u8(address & arm_ram_mask, value);
                else if constexpr (sizeof(Value) == 2u)
                    memory->write_u16((address & arm_ram_mask) & ~1u, value);
                else
                    memory->write_u32((address & arm_ram_mask) & ~3u, value);
                return;
            }
            const auto register_file = lock_registers();
            if (!register_file) return;
            const auto offset = address & arm_register_mask;
            if constexpr (sizeof(Value) == 1u)
                register_file->write_arm(offset, value, MemoryAccessWidth::Byte);
            else if constexpr (sizeof(Value) == 2u)
                register_file->write_arm(offset & ~1u, value, MemoryAccessWidth::Halfword);
            else
                register_file->write_arm(
                    offset & ~1u,
                    static_cast<std::uint16_t>(value),
                    MemoryAccessWidth::Halfword);
        } catch (...) {
            faulted = true;
        }
    }

    static std::uint32_t read32(void* const context, const std::uint32_t address) {
        return static_cast<Impl*>(context)->read_memory<std::uint32_t>(address);
    }
    static std::uint32_t read16(void* const context, const std::uint32_t address) {
        return static_cast<Impl*>(context)->read_memory<std::uint16_t>(address);
    }
    static std::uint32_t read32_sequential(void* const context,
                                           const std::uint32_t address,
                                           const bool) {
        return read32(context, address);
    }
    static std::uint32_t read16_sequential(void* const context,
                                           const std::uint32_t address,
                                           const bool) {
        return read16(context, address);
    }
    static std::uint8_t read8(void* const context, const std::uint32_t address) {
        return static_cast<Impl*>(context)->read_memory<std::uint8_t>(address);
    }
    static void write32(void* const context,
                        const std::uint32_t address,
                        const std::uint32_t value) {
        static_cast<Impl*>(context)->write_memory(address, value);
    }
    static void write16(void* const context,
                        const std::uint32_t address,
                        const std::uint16_t value) {
        static_cast<Impl*>(context)->write_memory(address, value);
    }
    static void write8(void* const context,
                       const std::uint32_t address,
                       const std::uint8_t value) {
        static_cast<Impl*>(context)->write_memory(address, value);
    }
    static std::uint32_t coprocessor_read(void* const context,
                                          int,
                                          int,
                                          int,
                                          int,
                                          int) {
        static_cast<Impl*>(context)->faulted = true;
        return 0u;
    }
    static void coprocessor_write(void* const context,
                                  int,
                                  int,
                                  int,
                                  int,
                                  int,
                                  std::uint32_t) {
        static_cast<Impl*>(context)->faulted = true;
    }
    static void breakpoint(void* const context) {
        static_cast<Impl*>(context)->faulted = true;
    }

    void enter_fiq() noexcept {
        if (!fiq_line || (cpu.registers[arm_cpsr] & arm_fiq_disable_mask) != 0u ||
            cpu.phased_op_id != arm_phase_none)
            return;
        const auto cpsr = cpu.registers[arm_cpsr];
        cpu.registers[arm_r14_fiq] = cpu.registers[arm_pc] + 4u;
        cpu.registers[arm_spsr_fiq] = cpsr;
        cpu.registers[arm_pc] = cpu.irq_table_address + 0x1Cu;
        cpu.registers[arm_cpsr] =
            (cpsr & ~arm_mode_mask & ~arm_thumb_mask) |
            arm_mode_fiq | arm_fiq_disable_mask | arm_irq_disable_mask;
        ++cpu.i_cycles;
        cpu.phased_op_id = arm_phase_fill_pipeline;
        cpu.phase = 0u;
    }

    void run_cycles(const std::uint64_t requested_cycles) noexcept {
        if (!enabled || faulted || requested_cycles == 0u) return;
        if (cycle_debt >= requested_cycles) {
            cycle_debt -= requested_cycles;
            return;
        }
        const auto target = requested_cycles - cycle_debt;
        cycle_debt = 0u;
        std::uint64_t consumed = 0u;
        while (consumed < target && enabled && !faulted) {
            cpu.i_cycles = 0u;
            bus_cycles = 0u;
            enter_fiq();
            const auto executes_instruction =
                cpu.phased_op_id == arm_phase_none && !cpu.wait_for_interrupt;
            arm7_exec_instruction(&cpu);
            if (faulted) enabled = false;
            const auto step_cycles = std::max<std::uint64_t>(
                1u, bus_cycles + static_cast<std::uint64_t>(cpu.i_cycles));
            consumed += step_cycles;
            if (executes_instruction) ++executed_instructions;
        }
        executed_cycles += consumed;
        cycle_debt = consumed > target ? consumed - target : 0u;
        cpu.i_cycles = 0u;
        cpu.executed_instructions = executed_instructions;
    }
};

AicaArm7Core::AicaArm7Core() : impl_(std::make_unique<Impl>()) {
    Impl::initialize_tables();
    impl_->reset(false);
}

AicaArm7Core::~AicaArm7Core() = default;

void AicaArm7Core::bind_bus(const std::shared_ptr<AicaRegisterFile>& registers,
                            const std::shared_ptr<LinearMemoryDevice>& ram) {
    if (!registers || !ram || ram->size() < required_arm_ram_size)
        throw std::invalid_argument("AICA-ARM7-Bus benoetigt Register und 2 MiB Sound-RAM.");
    impl_->registers = registers;
    impl_->ram = ram;
}

void AicaArm7Core::reset(const bool enabled) noexcept {
    impl_->reset(enabled);
}

void AicaArm7Core::set_enabled(const bool enabled) noexcept {
    impl_->enabled = enabled && !impl_->faulted;
}

bool AicaArm7Core::enabled() const noexcept {
    return impl_->enabled && !impl_->faulted;
}

bool AicaArm7Core::bus_bound() const noexcept {
    return !impl_->registers.expired() && !impl_->ram.expired();
}

bool AicaArm7Core::faulted() const noexcept {
    return impl_->faulted;
}

void AicaArm7Core::mark_faulted() noexcept {
    impl_->enabled = false;
    impl_->faulted = true;
}

void AicaArm7Core::set_fiq_line(const bool asserted) noexcept {
    impl_->fiq_line = asserted;
}

void AicaArm7Core::run_cycles(const std::uint64_t cycles) noexcept {
    impl_->run_cycles(cycles);
}

AicaArm7Snapshot AicaArm7Core::snapshot() const noexcept {
    AicaArm7Snapshot result;
    std::copy_n(std::begin(impl_->cpu.registers), result.registers.size(), result.registers.begin());
    std::copy_n(std::begin(impl_->cpu.prefetch_opcode),
                result.prefetch_opcodes.size(),
                result.prefetch_opcodes.begin());
    result.prefetch_pc = impl_->cpu.prefetch_pc;
    result.instruction_cycles = impl_->cpu.i_cycles;
    result.phased_opcode = impl_->cpu.phased_opcode;
    result.phased_operation = impl_->cpu.phased_op_id;
    result.phase = impl_->cpu.phase;
    result.block = {
        impl_->cpu.block.addr,
        impl_->cpu.block.r15_off,
        impl_->cpu.block.last_bank,
        impl_->cpu.block.base_addr,
        impl_->cpu.block.cycle,
        impl_->cpu.block.num_regs,
    };
    result.executed_instructions = impl_->executed_instructions;
    result.executed_cycles = impl_->executed_cycles;
    result.cycle_debt = impl_->cycle_debt;
    result.next_fetch_sequential = impl_->cpu.next_fetch_sequential;
    result.waiting_for_interrupt = impl_->cpu.wait_for_interrupt;
    result.enabled = impl_->enabled;
    result.faulted = impl_->faulted;
    return result;
}

void AicaArm7Core::validate_state_restore(const AicaArm7Snapshot& state) const {
    if (!valid_arm_mode(state.registers[arm_cpsr] & arm_mode_mask) ||
        state.phased_operation > arm_phase_block_transfer ||
        state.phase > 16u || state.block.cycle > 16u ||
        state.block.register_count > 16u || state.instruction_cycles > 64u ||
        state.cycle_debt > 64u || (state.enabled && state.faulted))
        throw std::invalid_argument("AICA-ARM7-Handoff besitzt ungueltigen CPU-Zustand.");
    if (state.enabled && (impl_->registers.expired() || impl_->ram.expired()))
        throw std::invalid_argument("AICA-ARM7-Handoff besitzt keinen gebundenen Bus.");
}

void AicaArm7Core::commit_validated_state_restore(AicaArm7Snapshot state) noexcept {
    std::copy(state.registers.begin(), state.registers.end(), std::begin(impl_->cpu.registers));
    std::copy(state.prefetch_opcodes.begin(),
              state.prefetch_opcodes.end(),
              std::begin(impl_->cpu.prefetch_opcode));
    impl_->cpu.prefetch_pc = state.prefetch_pc;
    impl_->cpu.i_cycles = state.instruction_cycles;
    impl_->cpu.phased_opcode = state.phased_opcode;
    impl_->cpu.phased_op_id = state.phased_operation;
    impl_->cpu.phase = state.phase;
    impl_->cpu.block.addr = state.block.address;
    impl_->cpu.block.r15_off = state.block.r15_offset;
    impl_->cpu.block.last_bank = state.block.last_bank;
    impl_->cpu.block.base_addr = state.block.base_address;
    impl_->cpu.block.cycle = state.block.cycle;
    impl_->cpu.block.num_regs = state.block.register_count;
    impl_->cpu.next_fetch_sequential = state.next_fetch_sequential;
    impl_->cpu.wait_for_interrupt = state.waiting_for_interrupt;
    impl_->executed_instructions = state.executed_instructions;
    impl_->executed_cycles = state.executed_cycles;
    impl_->cycle_debt = state.cycle_debt;
    impl_->enabled = state.enabled;
    impl_->faulted = state.faulted;
    impl_->cpu.executed_instructions = state.executed_instructions;
    impl_->install_callbacks();
}

} // namespace katana::runtime
