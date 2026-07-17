#pragma once

#include "katana/ir/ir.hpp"
#include "katana/runtime/abi.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace katana::codegen {

inline constexpr std::uint32_t backend_interface_abi_version = 1u;

enum class BackendCapability : std::uint64_t {
    StructuredSections = 1ull << 0u,
    RuntimeCpuState = 1ull << 1u,
    RuntimeMemory = 1ull << 2u,
    StructuredExceptions = 1ull << 3u,
    Fpu = 1ull << 4u,
    BlockTransitions = 1ull << 5u,
    PlatformServices = 1ull << 6u
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
