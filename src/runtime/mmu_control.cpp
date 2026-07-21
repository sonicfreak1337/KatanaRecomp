#include "katana/runtime/mmu_control.hpp"

#include <stdexcept>

namespace katana::runtime {
namespace {

constexpr std::uint32_t mmucr_at = 0x00000001u;
constexpr std::uint32_t mmucr_ti = 0x00000004u;
constexpr std::uint32_t mmucr_writable_mask = 0xFCFCFF05u;

std::shared_ptr<MmioMemoryDevice>
word_device(const std::size_t size,
            const MmioReadHandler& read,
            const MmioWriteHandler& write) {
    return std::make_shared<MmioMemoryDevice>(
        size,
        [read](const auto offset, const auto width) {
            if (width != MemoryAccessWidth::Word)
                throw std::invalid_argument("SH-4-MMU-Control-Space verlangt 32-Bit-Zugriffe.");
            return read(offset, width);
        },
        [write](const auto offset, const auto value, const auto width) {
            if (width != MemoryAccessWidth::Word)
                throw std::invalid_argument("SH-4-MMU-Control-Space verlangt 32-Bit-Zugriffe.");
            write(offset, value, width);
        });
}

} // namespace

Sh4MmuControl::Sh4MmuControl(CpuState& cpu, RuntimeAddressSpace& address_space) noexcept
    : cpu_(cpu), address_space_(address_space) {}

std::uint32_t Sh4MmuControl::read(const std::uint32_t offset) const {
    switch (offset) {
    case 0x00u:
        return cpu_.pteh;
    case 0x04u:
        return cpu_.ptel;
    case 0x08u:
        return cpu_.ttb;
    case 0x0Cu:
        return cpu_.tea;
    case 0x10u:
        return cpu_.mmucr;
    default:
        throw std::out_of_range("Unbekannter SH-4-MMU-Control-Offset.");
    }
}

void Sh4MmuControl::write(const std::uint32_t offset, const std::uint32_t value) {
    switch (offset) {
    case 0x00u:
        cpu_.pteh = value;
        address_space_.write_pteh(value);
        return;
    case 0x04u:
        cpu_.ptel = value & 0x1FFFFDFFu;
        return;
    case 0x08u:
        cpu_.ttb = value;
        return;
    case 0x0Cu:
        cpu_.tea = value;
        return;
    case 0x10u: {
        const auto masked = value & mmucr_writable_mask;
        if ((masked & mmucr_ti) != 0u) {
            cpu_.utlb.fill({});
            address_space_.clear_tlb();
        }
        cpu_.mmucr = masked & ~mmucr_ti;
        address_space_.write_mmucr(cpu_.mmucr);
        address_space_.set_mode((cpu_.mmucr & mmucr_at) != 0u ? AddressTranslationMode::Mmu
                                                              : AddressTranslationMode::NoMmu);
        return;
    }
    default:
        throw std::out_of_range("Unbekannter SH-4-MMU-Control-Offset.");
    }
}

std::uint32_t Sh4MmuControl::read_ptea() const noexcept {
    return cpu_.ptea;
}

void Sh4MmuControl::write_ptea(const std::uint32_t value) noexcept {
    cpu_.ptea = value & 0x0000000Fu;
}

void Sh4MmuControl::reset() noexcept {
    cpu_.pteh = 0u;
    cpu_.ptel = 0u;
    cpu_.ptea = 0u;
    cpu_.ttb = 0u;
    cpu_.tea = 0u;
    cpu_.mmucr = 0u;
    cpu_.utlb.fill({});
    address_space_.write_pteh(0u);
    address_space_.write_mmucr(0u);
    address_space_.set_mode(AddressTranslationMode::NoMmu);
    address_space_.clear_tlb();
}

std::shared_ptr<Sh4MmuControl>
map_sh4_mmu_control(Memory& memory, CpuState& cpu, RuntimeAddressSpace& address_space) {
    auto control = std::make_shared<Sh4MmuControl>(cpu, address_space);
    const auto registers = word_device(
        sh4_mmu_control_register_size,
        [control](const auto offset, const auto) { return control->read(offset); },
        [control](const auto offset, const auto value, const auto) { control->write(offset, value); });
    const auto ptea = word_device(
        4u,
        [control](const auto, const auto) { return control->read_ptea(); },
        [control](const auto, const auto value, const auto) { control->write_ptea(value); });
    memory.map_region("sh4-mmu-control-p4", sh4_mmu_control_p4_base, registers);
    memory.map_region("sh4-mmu-control-area7", sh4_mmu_control_area7_base, registers);
    memory.map_region("sh4-ptea-p4", sh4_ptea_p4_address, ptea);
    memory.map_region("sh4-ptea-area7", sh4_ptea_area7_address, ptea);
    return control;
}

} // namespace katana::runtime
