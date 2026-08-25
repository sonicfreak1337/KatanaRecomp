#pragma once

#include "katana/runtime/native_port.hpp"
#include "katana/runtime/native_port_cpu_control.hpp"
#include "katana/runtime/runtime.hpp"

#include <filesystem>
#include <memory>
#include <optional>
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

enum class NativePortLoadedAotSourceTransform : std::uint8_t {
    Identity,
    SegaPrs,
};

// Exact source provenance retained from the export-time latent-AOT hint. It
// lets a title loader bind an authenticated encoded extent to the generated
// decoded module without copying title paths or addresses into the runtime.
// A zero runtime_start means that no loader placement was proven and is never
// eligible for automatic PRS staging.
struct NativePortLoadedAotSourceBindingView final {
    NativePortLoadedAotSourceTransform transform =
        NativePortLoadedAotSourceTransform::Identity;
    std::string_view sha256;
    std::uint64_t disc_byte_offset = 0u;
    std::uint32_t byte_size = 0u;
    std::uint32_t runtime_start = 0u;
};

struct NativePortLoadedAotModuleView {
    std::uint32_t source_start = 0u;
    std::uint32_t byte_size = 0u;
    // Identity of the decoded source module consumed by analysis. A runtime
    // copy may legitimately mutate non-code data before its first dispatch;
    // in that case the binder requires every emitted AOT block identity to
    // remain exact before installing the mapping.
    std::string_view sha256;
    std::span<const NativePortLoadedAotSourceBindingView> source_bindings;
    std::span<const NativePortLoadedAotBlockIdentityView> block_identities;
};

// Identity supplied by a title-owned loader before a decoded module becomes
// executable.  Staging does not authorize code: the binder still verifies the
// exact generated module and every emitted block against the bytes resident at
// runtime_start before it installs a mapping.  It only carries the already
// authenticated loader identity across the decode-to-first-dispatch gap.
struct NativePortLoadedAotModuleActivation final {
    std::string_view sha256;
    std::uint32_t source_start = 0u;
    std::uint32_t runtime_start = 0u;
    std::uint32_t byte_size = 0u;
};

// Identity of one currently active loaded-AOT module. This view is suitable
// for title-owned data-lifecycle bindings inside that module; it does not
// authorize a code entry or expose mutable module bytes.
struct NativePortLoadedAotActiveModuleView final {
    std::string_view sha256;
    std::uint32_t source_start = 0u;
    std::uint32_t runtime_start = 0u;
    std::uint32_t byte_size = 0u;
    std::uint64_t lifecycle_generation = 0u;
};

struct NativePortExecutableRange final {
    std::uint32_t runtime_start = 0u;
    std::size_t byte_size = 0u;
};

// One context-owned range ledger arbitrates every executable title mapping,
// regardless of whether it is a fixed runtime image or a loader-staged AOT
// overlay. Its bounded capacity is reserved at construction, so range
// admission and retirement never allocate after product startup.
class NativePortExecutableLifecycleLedger final {
  public:
    explicit NativePortExecutableLifecycleLedger(
        std::size_t maximum_active_ranges);
    ~NativePortExecutableLifecycleLedger() noexcept;

    NativePortExecutableLifecycleLedger(
        const NativePortExecutableLifecycleLedger&) = delete;
    NativePortExecutableLifecycleLedger& operator=(
        const NativePortExecutableLifecycleLedger&) = delete;
    NativePortExecutableLifecycleLedger(
        NativePortExecutableLifecycleLedger&&) = delete;
    NativePortExecutableLifecycleLedger& operator=(
        NativePortExecutableLifecycleLedger&&) = delete;

    void validate_available(
        std::uint32_t runtime_start,
        std::size_t byte_size) const;
    [[nodiscard]] std::uint64_t acquire(
        std::uint32_t runtime_start,
        std::size_t byte_size);
    void validate_release(std::uint64_t generation) const;
    [[nodiscard]] std::uint64_t release(std::uint64_t generation);
    [[nodiscard]] std::uint64_t release_committed(
        std::uint64_t generation) noexcept;
    [[nodiscard]] std::uint64_t release_noexcept(
        std::uint64_t generation) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
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
        NativePortImmutableWriteGuard& immutable_guard,
        NativePortExecutableLifecycleLedger& lifecycle_ledger,
        CrashCapsule* crash_capsule = nullptr);
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
    [[nodiscard]] NativePortExecutableRange expand_deactivate_runtime_range(
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
        NativePortExecutableLifecycleLedger& lifecycle_ledger,
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
    // Returns the unique active module containing address after validating
    // that its executable blocks still belong to the current immutable-code
    // generation. The address may point at code or module-owned data.
    [[nodiscard]] std::optional<NativePortLoadedAotActiveModuleView>
    active_module_for_address(std::uint32_t address) const;
    // Records one exact loader-selected module at its destination. The module
    // remains non-executable until bind_entry verifies its current bytes and
    // an exact emitted block at the requested entry. Returns the independent,
    // monotone overlay-lifecycle generation assigned to this activation.
    [[nodiscard]] std::uint64_t stage_runtime_module(
        const NativePortLoadedAotModuleActivation& activation);
    // Resolves one exact, export-authenticated Sega PRS disc extent to its
    // generated decoded module and loader-proven runtime placement. No match
    // is an ordinary non-executable PRS asset; ambiguity fails closed.
    [[nodiscard]] std::optional<NativePortLoadedAotModuleActivation>
    resolve_prs_module_source(
        std::string_view encoded_sha256,
        std::uint64_t disc_byte_offset,
        std::size_t encoded_byte_size) const;
    [[nodiscard]] std::uint32_t resolve_module_source_start(
        std::string_view sha256,
        std::size_t byte_size) const;
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
    [[nodiscard]] NativePortExecutableRange expand_deactivate_runtime_range(
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

// Explicit loader replacement transaction. The requested write range is
// expanded to the exact active executable lifetimes it overlaps, then both
// owner domains are validated before either is retired. Arbitrary partial
// retirement remains rejected by deactivate_native_port_executable_range.
[[nodiscard]] NativePortExecutableRetirement
deactivate_native_port_executable_overlaps(
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
