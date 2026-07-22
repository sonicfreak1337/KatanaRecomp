#include "katana/runtime/bios_abi.hpp"
#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/gdrom_controller.hpp"
#include "katana/runtime/platform_services.hpp"
#include "katana/runtime/pvr.hpp"
#include "katana/runtime/system_asic.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace katana::runtime {
namespace {
constexpr std::array kVectors{
    BiosAbiVector{BiosAbiVectorKind::SysInfo, "sysinfo", 0x8C0000B0u, 0x8C001100u},
    BiosAbiVector{BiosAbiVectorKind::RomFont, "romfont", 0x8C0000B4u, 0x8C001120u},
    BiosAbiVector{BiosAbiVectorKind::Flash, "flash", 0x8C0000B8u, 0x8C001140u},
    BiosAbiVector{BiosAbiVectorKind::MiscGdrom, "misc-gdrom", 0x8C0000BCu, 0x8C001160u},
    BiosAbiVector{BiosAbiVectorKind::Gdrom2, "gdrom2", 0x8C0000C0u, 0x8C001180u},
    BiosAbiVector{BiosAbiVectorKind::System, "system", 0x8C0000E0u, 0x8C0011A0u}};
constexpr std::array kDirectAliases{
    BiosAbiVector{BiosAbiVectorKind::Gdrom2,
                  "gdrom2-direct",
                  0u,
                  hle_bios_gdrom2_direct_alias_address}};

struct FlashPartition {
    std::uint32_t offset;
    std::uint32_t size;
    bool writable;
};
constexpr std::array kFlashPartitions{
    FlashPartition{0x0001A000u, 0x00002000u, false},
    FlashPartition{0x00018000u, 0x00002000u, true},
    FlashPartition{0x0001C000u, 0x00004000u, true},
    FlashPartition{0x00010000u, 0x00008000u, true},
    FlashPartition{0x00000000u, 0x00010000u, true},
};

bool valid_range(const std::uint32_t offset,
                 const std::uint32_t size,
                 const std::uint32_t limit) noexcept {
    return offset <= limit && size <= limit - offset;
}

bool flash_write_allowed(const std::uint32_t offset, const std::uint32_t size) noexcept {
    if (size == 0u) return true;
    const auto& factory = kFlashPartitions.front();
    const auto end = static_cast<std::uint64_t>(offset) + size;
    const auto factory_end = static_cast<std::uint64_t>(factory.offset) + factory.size;
    return end <= factory.offset || offset >= factory_end;
}

void flash_unlock(CpuState& cpu, const std::uint8_t command) {
    const auto base = dreamcast_flash_physical_base;
    cpu.memory.write_u8(base + dreamcast_flash_unlock_address_1, 0xAAu);
    cpu.memory.write_u8(base + dreamcast_flash_unlock_address_2, 0x55u);
    cpu.memory.write_u8(base + dreamcast_flash_unlock_address_1, command);
}

std::uint32_t execute_flash_call(CpuState& cpu, const std::uint32_t selector) noexcept {
    try {
        const auto offset = cpu.r[4];
        const auto buffer = cpu.r[5];
        const auto size = cpu.r[6];
        if (selector == 0u) {
            if (offset >= kFlashPartitions.size() || !cpu.memory.contains(buffer, 8u))
                return 0xFFFFFFFFu;
            const auto partition = kFlashPartitions[offset];
            cpu.memory.write_u32(buffer, partition.offset, CodeWriteSource::Copy);
            cpu.memory.write_u32(buffer + 4u, partition.size, CodeWriteSource::Copy);
            return 0u;
        }
        if (selector == 1u) {
            if (!valid_range(offset, size, static_cast<std::uint32_t>(dreamcast_flash_size)) ||
                (size != 0u && !cpu.memory.contains(buffer, size)))
                return 0xFFFFFFFFu;
            std::vector<std::uint8_t> bytes(size);
            for (std::uint32_t index = 0u; index < size; ++index)
                bytes[index] = cpu.memory.read_u8(dreamcast_flash_physical_base + offset + index);
            if (!bytes.empty()) cpu.memory.write_bytes(buffer, bytes, CodeWriteSource::Copy);
            return size;
        }
        if (selector == 2u) {
            if (!valid_range(offset, size, static_cast<std::uint32_t>(dreamcast_flash_size)) ||
                !flash_write_allowed(offset, size) ||
                (size != 0u && !cpu.memory.contains(buffer, size)))
                return 0xFFFFFFFFu;
            std::vector<std::uint8_t> bytes(size);
            for (std::uint32_t index = 0u; index < size; ++index)
                bytes[index] = cpu.memory.read_u8(buffer + index);
            for (std::uint32_t index = 0u; index < size; ++index) {
                flash_unlock(cpu, 0xA0u);
                cpu.memory.write_u8(dreamcast_flash_physical_base + offset + index, bytes[index]);
            }
            return size;
        }
        if (selector == 3u) {
            const auto found =
                std::find_if(kFlashPartitions.begin(),
                             kFlashPartitions.end(),
                             [offset](const auto partition) { return partition.offset == offset; });
            if (found == kFlashPartitions.end() || !found->writable) return 0xFFFFFFFFu;
            for (std::uint32_t sector = 0u; sector < found->size;
                 sector += static_cast<std::uint32_t>(dreamcast_flash_sector_size)) {
                flash_unlock(cpu, 0x80u);
                cpu.memory.write_u8(
                    dreamcast_flash_physical_base + dreamcast_flash_unlock_address_1, 0xAAu);
                cpu.memory.write_u8(
                    dreamcast_flash_physical_base + dreamcast_flash_unlock_address_2, 0x55u);
                cpu.memory.write_u8(dreamcast_flash_physical_base + offset + sector, 0x30u);
            }
            return 0u;
        }
    } catch (...) {
        try {
            cpu.memory.write_u8(dreamcast_flash_physical_base, 0xF0u);
        } catch (...) {
        }
        return 0xFFFFFFFFu;
    }
    return 0xFFFFFFFFu;
}

std::uint32_t execute_sysinfo_call(CpuState& cpu, const std::uint32_t selector) noexcept {
    try {
        if (selector == 0u) {
            std::array<std::uint8_t, 24u> data{};
            for (std::uint32_t index = 0u; index < 8u; ++index)
                data[index] = cpu.memory.read_u8(dreamcast_flash_physical_base + 0x1A056u + index);
            for (std::uint32_t index = 0u; index < 5u; ++index)
                data[8u + index] =
                    cpu.memory.read_u8(dreamcast_flash_physical_base + 0x1A000u + index);
            cpu.memory.write_bytes(0x8C000068u, data, CodeWriteSource::Copy);
            return 0u;
        }
        if (selector == 2u) return 0xFFFFFFFFu;
        if (selector == 3u) return 0x8C000068u;
    } catch (...) {
        return 0xFFFFFFFFu;
    }
    return 0xFFFFFFFFu;
}

std::uint32_t execute_system_call(CpuState& cpu, const std::uint32_t selector) noexcept {
    try {
        if (selector == 0u) {
            constexpr std::uint32_t boot_border_color = 0x00C0BEBCu;
            constexpr std::uint32_t level2_normal_mask = system_asic_physical_base + 0x10u;
            constexpr std::uint32_t border_color =
                pvr_register_physical_base + pvr_register::BorderColor;
            if (cpu.memory.contains(level2_normal_mask, sizeof(std::uint32_t)))
                cpu.memory.write_u32(level2_normal_mask, 0u);
            if (cpu.memory.contains(border_color, sizeof(std::uint32_t)))
                cpu.memory.write_u32(border_color, boot_border_color);
            if (cpu.g1_bus != nullptr) cpu.g1_bus->restore_bios_handoff();
            return boot_border_color;
        }
        if (selector == 2u)
            return cpu.gdrom_services != nullptr &&
                           cpu.gdrom_services->reload_system_bootstrap(cpu)
                       ? 0u
                       : 0xFFFFFFFFu;
    } catch (...) {
        return 0xFFFFFFFFu;
    }
    return 0xFFFFFFFFu;
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return output.str();
}

std::string register_snapshot(const CpuState& cpu) {
    std::ostringstream output;
    for (std::size_t index = 0u; index < cpu.r.size(); ++index)
        output << " r" << index << '=' << hex32(cpu.r[index]);
    return output.str();
}
const BiosAbiVector* vector_for_handler(const std::uint32_t address) noexcept {
    for (const auto& vector : kVectors)
        if (canonical_physical_address(vector.handler_address) ==
            canonical_physical_address(address))
            return &vector;
    for (const auto& alias : kDirectAliases)
        if (canonical_physical_address(alias.handler_address) ==
            canonical_physical_address(address))
            return &alias;
    return nullptr;
}
BiosAbiCall call(const BiosAbiVectorKind vector,
                 const std::uint32_t selector,
                 const std::uint32_t super_selector,
                 const std::string_view service,
                 const BiosAbiServiceStatus status) {
    return {vector, selector, super_selector, service, status};
}
BlockExit bios_abi_block(CpuState& cpu, BlockExecutionContext& context) {
    const auto source = cpu.pc;
    if (cpu.memory.read_u16(source) != 0x000Bu || cpu.memory.read_u16(source + 2u) != 0x0009u)
        throw BiosAbiDispatchError(
            source, cpu.r[7], cpu.r[6], cpu.pr, "handler-bytes-modified" + register_snapshot(cpu));
    const auto routed = route_hle_bios_abi_call(cpu);
    if (routed.vector == BiosAbiVectorKind::System &&
        (routed.selector == 0xFFFFFFFFu || routed.selector == 1u || routed.selector == 3u)) {
        PlatformLifecycleExitEvidence evidence;
        evidence.guest_cycle = context.scheduler_cycle;
        evidence.callsite = source;
        evidence.return_address = cpu.pr;
        evidence.registers = cpu.r;
        if (cpu.gdrom_services != nullptr) {
            const auto& gdrom = cpu.gdrom_services->last_bios_request();
            evidence.last_gdrom_request = gdrom.id;
            evidence.last_gdrom_command = gdrom.command;
            evidence.last_gdrom_state = static_cast<std::uint32_t>(gdrom.state);
            evidence.last_gdrom_status = gdrom.status;
        }
        const auto reason = routed.selector == 0xFFFFFFFFu
                                ? PlatformLifecycleExitReason::Reset
                            : routed.selector == 1u
                                ? PlatformLifecycleExitReason::BiosMenu
                                : PlatformLifecycleExitReason::CdMenu;
        throw PlatformLifecycleExit(reason, std::move(evidence));
    }
    const auto is_gdrom = routed.vector == BiosAbiVectorKind::MiscGdrom ||
                          routed.vector == BiosAbiVectorKind::Gdrom2;
    if (routed.status != BiosAbiServiceStatus::Completed &&
        !(is_gdrom && cpu.gdrom_services != nullptr))
        throw BiosAbiDispatchError(source,
                                   routed.selector,
                                   routed.super_selector,
                                   cpu.pr,
                                   "service-unavailable:" + std::string(routed.service) +
                                       register_snapshot(cpu));
    cpu.r[0] = is_gdrom && cpu.gdrom_services != nullptr
                   ? cpu.gdrom_services->bios_call(cpu, routed.selector, routed.super_selector)
               : routed.vector == BiosAbiVectorKind::Flash ? execute_flash_call(cpu, routed.selector)
               : routed.vector == BiosAbiVectorKind::SysInfo
                   ? execute_sysinfo_call(cpu, routed.selector)
               : routed.vector == BiosAbiVectorKind::System
                   ? execute_system_call(cpu, routed.selector)
                   : 0u;
    cpu.pc = cpu.pr;
    return make_block_exit(cpu,
                           context,
                           BlockEndKind::Return,
                           {source, canonical_physical_address(source)},
                           BlockAddress{cpu.pc, canonical_physical_address(cpu.pc)});
}
bool writable_range(const Memory& memory, const std::uint32_t address, const std::size_t size) {
    for (std::size_t index = 0u; index < memory.region_count(); ++index) {
        const auto& region = memory.region(index);
        const auto end = static_cast<std::uint64_t>(address) + size;
        const auto region_end = static_cast<std::uint64_t>(region.base_address) + region.size;
        if (address >= region.base_address && end <= region_end)
            return region.access == MemoryRegionAccess::ReadWrite;
    }
    return false;
}
} // namespace

BiosAbiDispatchError::BiosAbiDispatchError(const std::uint32_t handler_address,
                                           const std::uint32_t selector,
                                           const std::uint32_t super_selector,
                                           const std::uint32_t return_address,
                                           std::string reason)
    : std::runtime_error("BIOS-ABI-Aufruf abgewiesen: handler=" + hex32(handler_address) +
                         " selector=" + hex32(selector) + " super=" + hex32(super_selector) +
                         " return=" + hex32(return_address) + " reason=" + std::move(reason)) {}

std::span<const BiosAbiVector> hle_bios_abi_vectors() noexcept {
    return kVectors;
}

void refresh_hle_bios_abi_memory(Memory& memory) {
    std::vector<std::uint8_t> erased(hle_bios_ram_size, 0xFFu);
    memory.write_bytes(hle_bios_ram_base, erased, CodeWriteSource::Copy);
    for (const auto& vector : kVectors) {
        memory.write_u32(vector.slot_address, vector.handler_address, CodeWriteSource::Copy);
        memory.write_u16(vector.handler_address, 0x000Bu, CodeWriteSource::Copy);
        memory.write_u16(vector.handler_address + 2u, 0x0009u, CodeWriteSource::Copy);
    }
    for (const auto& alias : kDirectAliases) {
        memory.write_u16(alias.handler_address, 0x000Bu, CodeWriteSource::Copy);
        memory.write_u16(alias.handler_address + 2u, 0x0009u, CodeWriteSource::Copy);
    }
}

void refresh_hle_bios_abi_memory(LinearMemoryDevice& main_ram, const std::size_t erased_size) {
    if (erased_size > hle_bios_ram_size || main_ram.size() < hle_bios_ram_size)
        throw std::invalid_argument("BIOS-RAM-Grundzustand passt nicht in den Hauptspeicher.");
    auto bytes = main_ram.writable_bytes();
    std::fill_n(bytes.begin(), erased_size, static_cast<std::uint8_t>(0xFFu));
    const auto write_u16 = [&bytes](const std::uint32_t address, const std::uint16_t value) {
        const auto offset = static_cast<std::size_t>(address - hle_bios_ram_base);
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto write_u32 = [&bytes](const std::uint32_t address, const std::uint32_t value) {
        const auto offset = static_cast<std::size_t>(address - hle_bios_ram_base);
        for (std::size_t index = 0u; index < 4u; ++index)
            bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
    };
    for (const auto& vector : kVectors) {
        write_u32(vector.slot_address, vector.handler_address);
        write_u16(vector.handler_address, 0x000Bu);
        write_u16(vector.handler_address + 2u, 0x0009u);
    }
    for (const auto& alias : kDirectAliases) {
        write_u16(alias.handler_address, 0x000Bu);
        write_u16(alias.handler_address + 2u, 0x0009u);
    }
}

BiosAbiCall route_hle_bios_abi_call(const CpuState& cpu) {
    const auto* vector = vector_for_handler(cpu.pc);
    if (!vector)
        throw BiosAbiDispatchError(
            cpu.pc, cpu.r[7], cpu.r[6], cpu.pr, "unknown-vector" + register_snapshot(cpu));
    const auto selector = vector->kind == BiosAbiVectorKind::RomFont ? cpu.r[1]
                          : vector->kind == BiosAbiVectorKind::System ? cpu.r[4]
                                                                     : cpu.r[7];
    const auto super_selector = cpu.r[6];
    using Status = BiosAbiServiceStatus;
    switch (vector->kind) {
    case BiosAbiVectorKind::SysInfo:
        if (selector == 0u)
            return call(vector->kind, selector, super_selector, "sysinfo-init", Status::Completed);
        if (selector == 2u)
            return call(vector->kind,
                        selector,
                        super_selector,
                        "sysinfo-icon",
                        Status::ServiceUnavailable);
        if (selector == 3u)
            return call(vector->kind, selector, super_selector, "sysinfo-id", Status::Completed);
        break;
    case BiosAbiVectorKind::RomFont:
        if (selector == 0u)
            return call(vector->kind,
                        selector,
                        super_selector,
                        "romfont-address",
                        Status::ServiceUnavailable);
        if (selector == 1u)
            return call(vector->kind, selector, super_selector, "romfont-lock", Status::Completed);
        if (selector == 2u)
            return call(
                vector->kind, selector, super_selector, "romfont-unlock", Status::Completed);
        break;
    case BiosAbiVectorKind::Flash:
        if (selector <= 3u)
            return call(vector->kind,
                        selector,
                        super_selector,
                        selector == 0u   ? "flash-info"
                        : selector == 1u ? "flash-read"
                        : selector == 2u ? "flash-write"
                                         : "flash-delete",
                        Status::Completed);
        break;
    case BiosAbiVectorKind::MiscGdrom:
        if (super_selector == 0xFFFFFFFFu && selector == 0u)
            return call(vector->kind, selector, super_selector, "misc-init", Status::Completed);
        if (super_selector == 0xFFFFFFFFu && selector == 1u)
            return call(vector->kind,
                        selector,
                        super_selector,
                        "misc-setvector",
                        Status::ServiceUnavailable);
        if (super_selector == 0u && selector <= 15u)
            return call(vector->kind,
                        selector,
                        super_selector,
                        "gdrom-service",
                        Status::ServiceUnavailable);
        break;
    case BiosAbiVectorKind::Gdrom2:
        return call(vector->kind,
                    selector,
                    super_selector,
                    "gdrom2-undocumented",
                    Status::ServiceUnavailable);
    case BiosAbiVectorKind::System:
        if (selector == 0u)
            return call(vector->kind,
                        selector,
                        super_selector,
                        "system-normal-init",
                        Status::Completed);
        if (selector == 2u)
            return call(vector->kind,
                        selector,
                        super_selector,
                        "system-check-disc",
                        Status::Completed);
        if (selector == 1u)
            return call(vector->kind,
                        selector,
                        super_selector,
                        "system-bios-menu",
                        Status::Completed);
        if (selector == 0xFFFFFFFFu || selector == 3u)
            return call(vector->kind,
                        selector,
                        super_selector,
                        "system-lifecycle",
                        Status::Completed);
        break;
    }
    throw BiosAbiDispatchError(cpu.pc,
                               selector,
                               super_selector,
                               cpu.pr,
                               "unknown-function" + register_snapshot(cpu));
}

void install_hle_bios_abi(Memory& memory,
                          RuntimeBlockTable& blocks,
                          FirmwareHandoffMap& handoff,
                          const BlockVariantKey& variant,
                          const std::uint64_t guest_cycle,
                          ExecutableCodeTracker* const code_tracker) {
    const auto validate = [&](const BiosAbiVector& vector, const bool has_slot) {
        if ((has_slot && !writable_range(memory, vector.slot_address, 4u)) ||
            !writable_range(memory, vector.handler_address, 4u))
            throw std::invalid_argument("BIOS-ABI-Vektor liegt nicht in schreibbarem RAM.");
        if (blocks.lookup(vector.handler_address, variant).has_value() ||
            blocks.lookup_physical(vector.handler_address, variant).has_value())
            throw std::invalid_argument("BIOS-ABI-Handler kollidiert mit Runtimeblock.");
        for (const auto& symbol : handoff.runtime_symbols()) {
            if ((has_slot && symbol.virtual_address == vector.slot_address) ||
                symbol.virtual_address == vector.handler_address ||
                (has_slot &&
                 symbol.physical_address == canonical_physical_address(vector.slot_address)) ||
                symbol.physical_address == canonical_physical_address(vector.handler_address))
                throw std::invalid_argument("BIOS-ABI-Vektor kollidiert mit Laufzeitsymbol.");
        }
    };
    for (const auto& vector : kVectors) validate(vector, true);
    for (const auto& alias : kDirectAliases) validate(alias, false);
    refresh_hle_bios_abi_memory(memory);
    const auto register_handler = [&](const BiosAbiVector& vector, const bool has_slot) {
        RuntimeBlock block{vector.handler_address,
                           canonical_physical_address(vector.handler_address),
                           4u,
                           BlockEndKind::Return,
                           variant,
                           &bios_abi_block,
                           "hle-bios-abi:" + std::string(vector.name)};
        const auto identity = stable_runtime_block_identity(block);
        static_cast<void>(blocks.register_bootstrap_static(std::move(block)));
        if (code_tracker != nullptr) {
            static_cast<void>(
                code_tracker->register_block({identity,
                                              canonical_physical_address(vector.handler_address),
                                              4u,
                                              "hle-generated-handler",
                                              {},
                                              ExecutableBlockOrigin::RomRamCopy}));
        }
        if (has_slot)
            handoff.install_runtime_symbol({"bios-vector-" + std::string(vector.name),
                                            vector.slot_address,
                                            canonical_physical_address(vector.slot_address),
                                            "hle-generated-vector",
                                            guest_cycle});
        handoff.install_runtime_symbol({"bios-handler-" + std::string(vector.name),
                                        vector.handler_address,
                                        canonical_physical_address(vector.handler_address),
                                        "hle-generated-handler",
                                        guest_cycle});
    };
    for (const auto& vector : kVectors) register_handler(vector, true);
    for (const auto& alias : kDirectAliases) register_handler(alias, false);
}

const char* bios_abi_service_status_name(const BiosAbiServiceStatus status) noexcept {
    return status == BiosAbiServiceStatus::Completed ? "completed" : "service-unavailable";
}
std::string format_hle_bios_abi_contract_json() {
    std::ostringstream output;
    output << "{\"schema\":\"katana-bios-abi\",\"version\":" << bios_abi_contract_version
           << ",\"selector_register\":\"r7\",\"romfont_selector_register\":\"r1\","
              "\"system_selector_register\":\"r4\",\"super_selector_register\":\"r6\","
              "\"vectors\":[";
    for (std::size_t index = 0u; index < kVectors.size(); ++index) {
        if (index) output << ',';
        output << "{\"name\":\"" << kVectors[index].name << "\",\"slot\":\""
               << hex32(kVectors[index].slot_address) << "\",\"handler\":\""
               << hex32(kVectors[index].handler_address) << "\"}";
    }
    output << "],\"direct_aliases\":[";
    for (std::size_t index = 0u; index < kDirectAliases.size(); ++index) {
        if (index) output << ',';
        output << "{\"name\":\"" << kDirectAliases[index].name << "\",\"handler\":\""
               << hex32(kDirectAliases[index].handler_address) << "\"}";
    }
    output << "]}";
    return output.str();
}
} // namespace katana::runtime
