#pragma once

#include "katana/codegen/backend.hpp"
#include "katana/codegen/partition.hpp"
#include "katana/ir/optimize.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace katana::codegen {

inline constexpr std::uint32_t native_aot_emission_profile_version = 43u;

enum class NativeAotEmissionProfile : std::uint8_t { Product, ExternalConformance };

struct NativeAotEmissionContract {
    std::uint32_t version = native_aot_emission_profile_version;
    katana::ir::OptimizationOptions optimization_options;
    PartitionOptions partition_options;
    bool external_function_linkage = true;
    bool single_block_execution = true;
    bool external_dynamic_dispatch = true;
    bool guarded_local_block_chaining = true;
    bool conservative_register_localization = true;
};

struct NativeAotBackendRequestOptions {
    std::string_view symbol_namespace;
    bool emit_run_functions = true;
    std::optional<std::uint32_t> metadata_entry_address;
    bool external_instruction_observer = false;
    BackendRuntimeBinding runtime_binding =
        BackendRuntimeBinding::DiagnosticPlatformServices;
};

[[nodiscard]] const char*
native_aot_emission_profile_name(NativeAotEmissionProfile profile) noexcept;

[[nodiscard]] const NativeAotEmissionContract&
native_aot_emission_contract(NativeAotEmissionProfile profile);

[[nodiscard]] BackendRequest
make_native_aot_backend_request(NativeAotEmissionProfile profile,
                                std::span<const katana::ir::Function> functions,
                                std::uint32_t entry_address,
                                const NativeAotBackendRequestOptions& options);

void annotate_proven_linear_ram_accesses(std::span<katana::ir::Function> program) noexcept;

} // namespace katana::codegen
