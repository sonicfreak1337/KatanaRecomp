#pragma once

#include "katana/runtime/native_port.hpp"
#include "katana/runtime/native_port_cpu_control.hpp"
#include "katana/runtime/runtime.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::runtime {

class NativePortImmutableWriteGuard;
struct CrashCapsule;

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
    explicit NativePortMemory(
        std::uint32_t initial_cache_control_value = 0u);

    NativePortMemory(const NativePortMemory&) = delete;
    NativePortMemory& operator=(const NativePortMemory&) = delete;
    NativePortMemory(NativePortMemory&&) = delete;
    NativePortMemory& operator=(NativePortMemory&&) = delete;

    [[nodiscard]] CpuState& cpu() noexcept;
    [[nodiscard]] const CpuState& cpu() const noexcept;
    [[nodiscard]] NativePortCpuControl& cpu_control() noexcept;
    [[nodiscard]] const NativePortCpuControl& cpu_control() const noexcept;

    void load_verified_images(
        const std::filesystem::path& content_root,
        std::span<const NativePortImageBinding> images);

  private:
    CpuState cpu_;
    std::shared_ptr<LinearMemoryDevice> main_memory_;
    std::shared_ptr<NativePortCpuControl> cpu_control_;
};

// Export-time analyzed code for a disc file is represented at a synthetic
// source address.  The native product may later load those exact bytes into
// ordinary title RAM.  These views let the dispatcher bind that runtime copy
// to the already generated AOT code; they never decode or generate guest code
// at runtime.
struct NativePortLoadedAotBlockIdentityView {
    std::uint32_t source_offset = 0u;
    std::uint32_t byte_size = 0u;
    std::string_view sha256;
};

struct NativePortLoadedAotModuleView {
    std::uint32_t source_start = 0u;
    std::uint32_t byte_size = 0u;
    // Identity of the decoded source module consumed by analysis. A runtime
    // copy may legitimately mutate non-code data before its first dispatch;
    // in that case the binder requires every emitted AOT block identity to
    // remain exact before installing the mapping.
    std::string_view sha256;
    std::span<const NativePortLoadedAotBlockIdentityView> block_identities;
};

// A fixed-address title image is executable only while its exact identity is
// active at the declared runtime range.  Unlike bootstrap code, such ranges
// may be replaced by a later title overlay through this explicit lifecycle.
struct NativePortRuntimeImageView final {
    std::string_view image_id;
    std::uint32_t source_start = 0u;
    std::uint32_t runtime_start = 0u;
    std::uint32_t byte_size = 0u;
    // Identity of the immutable source image consumed by analysis/export.
    // Runtime data words may legitimately differ after bootstrap; every AOT
    // entry is therefore validated separately against its exact runtime bytes.
    std::string_view sha256;
    std::span<const NativePortLoadedAotBlockIdentityView> block_identities;
};

class NativePortRuntimeImageBindings final {
  public:
    NativePortRuntimeImageBindings(
        CpuState& cpu,
        std::span<const NativePortRuntimeImageView> images,
        NativePortImmutableWriteGuard& immutable_guard);
    ~NativePortRuntimeImageBindings() noexcept;

    NativePortRuntimeImageBindings(const NativePortRuntimeImageBindings&) = delete;
    NativePortRuntimeImageBindings& operator=(
        const NativePortRuntimeImageBindings&) = delete;
    NativePortRuntimeImageBindings(NativePortRuntimeImageBindings&&) = delete;
    NativePortRuntimeImageBindings& operator=(
        NativePortRuntimeImageBindings&&) = delete;

    void activate(std::string_view image_id);
    // The requested interval must fully cover every overlapping active image;
    // partial replacement is rejected. Returns the number deactivated.
    void validate_deactivate_runtime_range(
        std::uint32_t runtime_start,
        std::size_t byte_size) const;
    [[nodiscard]] std::size_t deactivate_runtime_range(
        std::uint32_t runtime_start,
        std::size_t byte_size);
    [[nodiscard]] bool active(std::string_view image_id) const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class NativePortLoadedAotBinder final {
  public:
    NativePortLoadedAotBinder(
        CpuState& cpu,
        std::span<const NativePortLoadedAotModuleView> modules,
        NativePortImmutableWriteGuard& immutable_guard,
        CrashCapsule* crash_capsule = nullptr);
    ~NativePortLoadedAotBinder() noexcept;

    NativePortLoadedAotBinder(const NativePortLoadedAotBinder&) = delete;
    NativePortLoadedAotBinder& operator=(
        const NativePortLoadedAotBinder&) = delete;
    NativePortLoadedAotBinder(NativePortLoadedAotBinder&&) = delete;
    NativePortLoadedAotBinder& operator=(
        NativePortLoadedAotBinder&&) = delete;

    // Returns true only when target belongs to an already active mapping.
    // An exact block-identity mismatch is a typed AOT-contract failure.
    [[nodiscard]] bool validate_bound_entry(std::uint32_t target) const;
    // Installs one unambiguous exact executable-closure mapping for target.
    // False means no analyzed module matches; ambiguous, malformed, or stale
    // generated-code state fails closed.
    [[nodiscard]] bool bind_entry(std::uint32_t target);
    // Retires every active loaded module fully covered by the requested
    // runtime interval. Partial replacement and replacement while the CPU is
    // executing or returning into that module fail closed. Returns the
    // number retired.
    void validate_deactivate_runtime_range(
        std::uint32_t runtime_start,
        std::size_t byte_size) const;
    [[nodiscard]] std::size_t deactivate_runtime_range(
        std::uint32_t runtime_start,
        std::size_t byte_size);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct NativePortExecutableRetirement final {
    std::size_t runtime_images = 0u;
    std::size_t loaded_aot_modules = 0u;
};

// One native content/transform boundary must retire both fixed runtime images
// and dynamically bound latent-AOT modules before replacing executable bytes.
// This is a lifecycle operation over statically generated host code; it never
// decodes or recompiles guest instructions at runtime.
[[nodiscard]] NativePortExecutableRetirement
deactivate_native_port_executable_range(
    NativePortContext& context,
    std::uint32_t runtime_start,
    std::size_t byte_size);

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

// Stable identity over every execution-relevant CpuState field. The native
// product owns only its bounded RAM and therefore rejects an attached MMU
// address-space object, reset callback or historical Dreamcast device pointer
// instead of silently omitting those semantics from the digest.
[[nodiscard]] std::string
native_port_cpu_state_identity(const CpuState& cpu);

} // namespace katana::runtime
