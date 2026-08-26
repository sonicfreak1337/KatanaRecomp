#pragma once

#include "katana/abi_contract.hpp"
#include "katana/ir/ir.hpp"
#include "katana/runtime/abi.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace katana::codegen {

inline constexpr std::uint32_t backend_interface_abi_version =
    abi_contract::backend_interface_abi_version;

enum class BackendCapability : std::uint64_t {
    StructuredSections = 1ull << 0u,
    RuntimeCpuState = 1ull << 1u,
    RuntimeMemory = 1ull << 2u,
    StructuredExceptions = 1ull << 3u,
    Fpu = 1ull << 4u,
    BlockTransitions = 1ull << 5u,
    PlatformServices = 1ull << 6u,
    NativePortServices = 1ull << 7u
};

using BackendCapabilities = std::uint64_t;

[[nodiscard]] constexpr BackendCapabilities capability(const BackendCapability value) noexcept {
    return static_cast<BackendCapabilities>(value);
}

struct BackendRequirements {
    std::uint32_t interface_abi_version = backend_interface_abi_version;
    std::uint32_t runtime_abi_version = katana::runtime::abi_version;
    BackendCapabilities capabilities = capability(BackendCapability::StructuredSections);
};

// A statically discovered native AOT candidate for an indirect guest call.
// This is deliberately separate from IR resolved_targets: the live guest
// register remains authoritative and generated code must compare it before
// entering the candidate, with the ordinary dynamic dispatcher as fallback.
struct GuardedNativeCallTarget {
    std::uint32_t callsite = 0u;
    std::uint32_t target = 0u;
};

// Canonical product-AOT ownership for a guest block. The owner entry names the
// one table-compatible native function that contains the block. Keeping this
// mapping on the backend request lets later finite jump/call lowering reuse the
// same entry contract without teaching the runtime about IR ownership.
struct NativeAotBlockOwnerEntry {
    std::uint32_t block_address = 0u;
    std::uint32_t owner_entry = 0u;
};

enum class BackendRuntimeBinding : std::uint8_t {
    DiagnosticPlatformServices,
    NativePort
};

struct BackendRequest {
    std::span<const katana::ir::Function> functions;
    std::uint32_t entry_address = 0u;
    BackendRequirements requirements;
    std::span<const std::uint32_t> known_function_entries;
    std::string_view symbol_namespace = "katana_generated";
    bool emit_run_functions = true;
    bool external_function_linkage = false;
    std::optional<std::uint32_t> metadata_entry_address;
    bool single_block_execution = false;
    bool external_dynamic_dispatch = false;
    // Emit the sealed NativeBringup preflight only for a NativeBringup
    // product. StrictProduct must not carry its symbols or hot-path branch.
    bool native_bringup_dispatch_validation = false;
    bool guarded_local_block_chaining = false;
    bool external_instruction_observer = false;
    std::span<const GuardedNativeCallTarget> guarded_native_call_targets;
    // Entries that must be observed by the architectural dispatcher (for
    // example external game-project hooks). Native labels, direct calls and
    // guarded singleton calls must return with cpu.pc set instead of crossing
    // these boundaries locally.
    std::span<const std::uint32_t> architectural_boundary_entries;
    // Localize a conservatively selected GPR subset only when the complete
    // emitted guest function is a pure leaf with no architectural boundary.
    bool conservative_register_localization = false;
    std::span<const NativeAotBlockOwnerEntry> native_block_owner_entries;
    BackendRuntimeBinding runtime_binding =
        BackendRuntimeBinding::DiagnosticPlatformServices;
    // Identity-bound indirect callsites selected by the product closure-probe
    // plan. Only these callsites emit the one predictable pending-plan branch;
    // every unrelated guest call remains byte-for-byte free of probe work.
    std::span<const std::uint32_t> closure_probe_callsites;
};

struct BackendEmission {
    std::string declarations;
    std::string functions;
    std::string metadata;

    [[nodiscard]] std::string joined_text() const;
};

class Backend {
  public:
    virtual ~Backend() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t interface_abi_version() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t runtime_abi_version() const noexcept = 0;
    [[nodiscard]] virtual BackendCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual BackendEmission emit(const BackendRequest& request) const = 0;
};

[[nodiscard]] BackendEmission generate_program(const Backend& backend,
                                               const BackendRequest& request);

} // namespace katana::codegen
