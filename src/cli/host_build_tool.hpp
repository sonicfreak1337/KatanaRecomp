#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace katana::cli {

enum class HostBuildToolKind : std::uint8_t {
    Compile,
    Archive,
    Link,
};

// Minimal compiler/linker launcher contract. The implementation deliberately
// has no dependency on the Katana CLI, analyzer, runtime, or progress stack so
// every host-tool edge starts only the small launcher executable.
[[nodiscard]] int run_host_build_tool_launcher(
    HostBuildToolKind kind,
    const std::filesystem::path& event_root,
    std::string tool,
    std::span<const char* const> arguments) noexcept;

[[nodiscard]] std::string_view host_build_tool_kind_name(
    HostBuildToolKind kind) noexcept;

} // namespace katana::cli
