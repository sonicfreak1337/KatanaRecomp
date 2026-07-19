#include "katana/runtime/bios_abi.hpp"
#include "katana/runtime/code_invalidation.hpp"

#include <array>
#include <iomanip>
#include <sstream>

namespace katana::runtime {
namespace {
constexpr std::array kVectors{
    BiosAbiVector{BiosAbiVectorKind::SysInfo, "sysinfo", 0x8C0000B0u, 0x8C000100u},
    BiosAbiVector{BiosAbiVectorKind::RomFont, "romfont", 0x8C0000B4u, 0x8C000120u},
    BiosAbiVector{BiosAbiVectorKind::Flash, "flash", 0x8C0000B8u, 0x8C000140u},
    BiosAbiVector{BiosAbiVectorKind::MiscGdrom, "misc-gdrom", 0x8C0000BCu, 0x8C000160u},
    BiosAbiVector{BiosAbiVectorKind::Gdrom2, "gdrom2", 0x8C0000C0u, 0x8C000180u},
    BiosAbiVector{BiosAbiVectorKind::System, "system", 0x8C0000E0u, 0x8C0001A0u}};

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return output.str();
}
const BiosAbiVector* vector_for_handler(const std::uint32_t address) noexcept {
    for (const auto& vector : kVectors)
        if (canonical_physical_address(vector.handler_address) ==
            canonical_physical_address(address))
            return &vector;
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
    const auto routed = route_hle_bios_abi_call(cpu);
    if (routed.status != BiosAbiServiceStatus::Completed)
        throw BiosAbiDispatchError(source,
                                   routed.selector,
                                   routed.super_selector,
                                   "service-unavailable:" + std::string(routed.service));
    cpu.r[0] = 0u;
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
                                           std::string reason)
    : std::runtime_error("BIOS-ABI-Aufruf abgewiesen: handler=" + hex32(handler_address) +
                         " selector=" + hex32(selector) + " super=" + hex32(super_selector) +
                         " reason=" + std::move(reason)) {}

std::span<const BiosAbiVector> hle_bios_abi_vectors() noexcept {
    return kVectors;
}

BiosAbiCall route_hle_bios_abi_call(const CpuState& cpu) {
    const auto* vector = vector_for_handler(cpu.pc);
    if (!vector) throw BiosAbiDispatchError(cpu.pc, cpu.r[7], cpu.r[6], "unknown-vector");
    const auto selector = vector->kind == BiosAbiVectorKind::RomFont ? cpu.r[1] : cpu.r[7];
    const auto super_selector = cpu.r[6];
    using Status = BiosAbiServiceStatus;
    switch (vector->kind) {
    case BiosAbiVectorKind::SysInfo:
        if (selector == 0u)
            return call(vector->kind, selector, super_selector, "sysinfo-init", Status::Completed);
        if (selector == 2u)
            return call(
                vector->kind, selector, super_selector, "sysinfo-icon", Status::ServiceUnavailable);
        if (selector == 3u)
            return call(
                vector->kind, selector, super_selector, "sysinfo-id", Status::ServiceUnavailable);
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
                        Status::ServiceUnavailable);
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
        if (selector == 0xFFFFFFFFu || selector == 1u || selector == 3u)
            return call(vector->kind,
                        selector,
                        super_selector,
                        "system-lifecycle",
                        Status::ServiceUnavailable);
        break;
    }
    throw BiosAbiDispatchError(cpu.pc, selector, super_selector, "unknown-function");
}

void install_hle_bios_abi(Memory& memory,
                          RuntimeBlockTable& blocks,
                          FirmwareHandoffMap& handoff,
                          const BlockVariantKey& variant,
                          const std::uint64_t guest_cycle,
                          ExecutableCodeTracker* const code_tracker) {
    for (const auto& vector : kVectors) {
        if (!writable_range(memory, vector.slot_address, 4u) ||
            !writable_range(memory, vector.handler_address, 4u))
            throw std::invalid_argument("BIOS-ABI-Vektor liegt nicht in schreibbarem RAM.");
        if (blocks.lookup(vector.handler_address, variant).has_value() ||
            blocks.lookup_physical(vector.handler_address, variant).has_value())
            throw std::invalid_argument("BIOS-ABI-Handler kollidiert mit Runtimeblock.");
        for (const auto& symbol : handoff.runtime_symbols()) {
            if (symbol.virtual_address == vector.slot_address ||
                symbol.virtual_address == vector.handler_address ||
                symbol.physical_address == canonical_physical_address(vector.slot_address) ||
                symbol.physical_address == canonical_physical_address(vector.handler_address))
                throw std::invalid_argument("BIOS-ABI-Vektor kollidiert mit Laufzeitsymbol.");
        }
    }
    for (const auto& vector : kVectors) {
        memory.write_u32(vector.slot_address, vector.handler_address, CodeWriteSource::Copy);
        memory.write_u16(vector.handler_address, 0x000Bu, CodeWriteSource::Copy);
        memory.write_u16(vector.handler_address + 2u, 0x0009u, CodeWriteSource::Copy);
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
            static_cast<void>(code_tracker->register_block(
                {identity,
                 canonical_physical_address(vector.handler_address),
                 4u,
                 "hle-generated-handler",
                 {},
                 ExecutableBlockOrigin::RomRamCopy}));
        }
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
    }
}

const char* bios_abi_service_status_name(const BiosAbiServiceStatus status) noexcept {
    return status == BiosAbiServiceStatus::Completed ? "completed" : "service-unavailable";
}
std::string format_hle_bios_abi_contract_json() {
    std::ostringstream output;
    output << "{\"schema\":\"katana-bios-abi\",\"version\":" << bios_abi_contract_version
           << ",\"selector_register\":\"r7\",\"romfont_selector_register\":\"r1\",\"super_selector_"
              "register\":\"r6\",\"vectors\":[";
    for (std::size_t index = 0u; index < kVectors.size(); ++index) {
        if (index) output << ',';
        output << "{\"name\":\"" << kVectors[index].name << "\",\"slot\":\""
               << hex32(kVectors[index].slot_address) << "\",\"handler\":\""
               << hex32(kVectors[index].handler_address) << "\"}";
    }
    output << "]}";
    return output.str();
}
} // namespace katana::runtime
