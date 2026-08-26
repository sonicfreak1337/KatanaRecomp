#include "katana/codegen/native_aot_profile.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>

namespace katana::codegen {
namespace {

constexpr NativeAotEmissionContract product_contract{};
constexpr NativeAotEmissionContract external_conformance_contract{
    .conservative_register_localization = false};

std::optional<std::size_t>
memory_access_width_bytes(const katana::ir::OperandWidth width) noexcept {
    using Width = katana::ir::OperandWidth;
    switch (width) {
    case Width::Bits8:
        return 1u;
    case Width::Bits16:
        return 2u;
    case Width::Bits32:
        return 4u;
    case Width::Bits64:
        return 8u;
    case Width::None:
    case Width::Bit1:
    case Width::Bits4:
    case Width::Bits12:
        return std::nullopt;
    }
    return std::nullopt;
}

bool statically_proves_direct_main_ram_access(const katana::ir::Instruction& instruction) noexcept {
    if (instruction.memory_effects.access == katana::ir::MemoryAccessKind::None ||
        instruction.memory_effects.access_count != 1u || !instruction.effective_address)
        return false;
    const auto width = memory_access_width_bytes(instruction.memory_effects.width);
    if (!width) return false;

    const auto address = *instruction.effective_address;
    const auto last = static_cast<std::uint64_t>(address) + *width - 1u;
    if (last > std::numeric_limits<std::uint32_t>::max()) return false;
    const auto segment = address & 0xE0000000u;
    if ((segment != 0x80000000u && segment != 0xA0000000u) ||
        (static_cast<std::uint32_t>(last) & 0xE0000000u) != segment)
        return false;

    constexpr std::uint32_t physical_begin = 0x0C000000u;
    constexpr std::uint32_t physical_end = 0x10000000u;
    const auto physical = address & 0x1FFFFFFFu;
    return physical >= physical_begin && physical < physical_end &&
           *width <= static_cast<std::size_t>(physical_end - physical);
}

} // namespace

const char* native_aot_emission_profile_name(const NativeAotEmissionProfile profile) noexcept {
    switch (profile) {
    case NativeAotEmissionProfile::Product:
        return "product";
    case NativeAotEmissionProfile::ExternalConformance:
        return "external-conformance";
    }
    return "unknown";
}

const NativeAotEmissionContract&
native_aot_emission_contract(const NativeAotEmissionProfile profile) {
    switch (profile) {
    case NativeAotEmissionProfile::Product:
        return product_contract;
    case NativeAotEmissionProfile::ExternalConformance:
        return external_conformance_contract;
    }
    throw std::invalid_argument("Unbekanntes natives AOT-Emissionsprofil.");
}

BackendRequest
make_native_aot_backend_request(const NativeAotEmissionProfile profile,
                                const std::span<const katana::ir::Function> functions,
                                const std::uint32_t entry_address,
                                const NativeAotBackendRequestOptions& options) {
    if (options.symbol_namespace.empty()) {
        throw std::invalid_argument("Natives AOT-Profil braucht einen Symbolnamensraum.");
    }
    if (options.external_instruction_observer &&
        profile != NativeAotEmissionProfile::ExternalConformance) {
        throw std::invalid_argument(
            "Externe Instruktionsbeobachtung ist nur im Konformitaetsprofil erlaubt.");
    }
    if (options.native_bringup_dispatch_validation &&
        (profile != NativeAotEmissionProfile::Product ||
         options.runtime_binding != BackendRuntimeBinding::NativePort)) {
        throw std::invalid_argument(
            "NativeBringup-Dispatchvalidierung braucht das native Produktprofil.");
    }

    const auto& contract = native_aot_emission_contract(profile);
    BackendRequest request{functions, entry_address};
    request.symbol_namespace = options.symbol_namespace;
    request.emit_run_functions = options.emit_run_functions;
    request.external_function_linkage = contract.external_function_linkage;
    request.metadata_entry_address = options.metadata_entry_address;
    request.single_block_execution = contract.single_block_execution;
    request.external_dynamic_dispatch = contract.external_dynamic_dispatch;
    request.native_bringup_dispatch_validation =
        options.native_bringup_dispatch_validation;
    request.guarded_local_block_chaining = contract.guarded_local_block_chaining;
    request.external_instruction_observer = options.external_instruction_observer;
    request.conservative_register_localization =
        contract.conservative_register_localization && !options.external_instruction_observer;
    request.runtime_binding = options.runtime_binding;
    return request;
}

void annotate_proven_linear_ram_accesses(const std::span<katana::ir::Function> program) noexcept {
    for (auto& function : program) {
        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.memory_effects.region == katana::ir::MemoryRegionKind::Unknown &&
                    statically_proves_direct_main_ram_access(instruction))
                    instruction.memory_effects.region = katana::ir::MemoryRegionKind::NormalRam;
            }
        }
    }
}

} // namespace katana::codegen
