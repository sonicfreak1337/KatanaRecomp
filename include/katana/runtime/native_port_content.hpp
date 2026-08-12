#pragma once

#include "katana/runtime/native_port.hpp"
#include "katana/runtime/runtime.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t native_port_main_memory_physical_base =
    0x0C000000u;
inline constexpr std::uint32_t native_port_main_memory_backing_size =
    16u * 1024u * 1024u;
inline constexpr std::uint32_t native_port_main_memory_physical_span =
    64u * 1024u * 1024u;

// Owns only the ordinary guest-memory compatibility state required by the
// statically recompiled game. It does not construct firmware or a Dreamcast
// device map.
class NativePortMemory final {
  public:
    NativePortMemory();

    NativePortMemory(const NativePortMemory&) = delete;
    NativePortMemory& operator=(const NativePortMemory&) = delete;
    NativePortMemory(NativePortMemory&&) = delete;
    NativePortMemory& operator=(NativePortMemory&&) = delete;

    [[nodiscard]] CpuState& cpu() noexcept;
    [[nodiscard]] const CpuState& cpu() const noexcept;

    void load_verified_images(
        const std::filesystem::path& content_root,
        std::span<const NativePortImageBinding> images);

  private:
    CpuState cpu_;
    std::shared_ptr<LinearMemoryDevice> main_memory_;
};

// Bootstrap validation deliberately observes the complete aliased 16-MiB
// backing rather than only Memory API calls: a private adapter may use a raw
// span for fast checkpoint materialization, and those writes must still be
// covered by the same product contract.
[[nodiscard]] std::vector<std::uint8_t>
capture_native_port_main_memory(const CpuState& cpu);

void validate_native_port_bootstrap_memory_transition(
    const CpuState& cpu,
    std::span<const std::uint8_t> before,
    std::span<const NativePortBootstrapWriteBinding> writes,
    std::span<const NativePortImmutableRange> immutable_ranges);

// Stable, pointer-free identity over every execution-relevant CpuState field.
// Memory, host callbacks and device pointers are intentionally excluded.
[[nodiscard]] std::string
native_port_cpu_state_identity(const CpuState& cpu);

} // namespace katana::runtime
