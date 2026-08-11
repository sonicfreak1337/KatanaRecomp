#pragma once

#include "katana/runtime/native_port.hpp"
#include "katana/runtime/runtime.hpp"

#include <filesystem>
#include <memory>
#include <span>

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

} // namespace katana::runtime
