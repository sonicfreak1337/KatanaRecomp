#pragma once

#include "katana/runtime/block_table.hpp"
#include "katana/runtime/dispatch_diagnostics.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace katana::runtime {

class DemandBlockMaterializer;

enum class IndirectDispatchKind : std::uint8_t { Call, TailJump, Return };
enum class RuntimeDispatchClass : std::uint8_t { GuardedFallback, RuntimeOnly };

enum class RuntimeTargetStability : std::uint8_t {
    NeverHit,
    Monomorphic,
    SmallPolymorphic,
    Dynamic
};

struct RuntimeOnlySiteMetrics {
    std::uint32_t callsite = 0u;
    std::uint64_t calls = 0u;
    std::uint64_t hits = 0u;
    std::uint64_t misses = 0u;
    std::uint64_t materializations = 0u;
    std::uint64_t invalidations = 0u;
    std::vector<std::uint32_t> targets;
    bool targets_truncated = false;
    [[nodiscard]] RuntimeTargetStability stability() const noexcept;
};

struct IndirectDispatchFirstError {
    DispatchDiagnosticError error = DispatchDiagnosticError::None;
    RuntimeDispatchClass dispatch_class = RuntimeDispatchClass::GuardedFallback;
    std::uint32_t callsite = 0u;
    std::uint32_t target = 0u;
};

class IndirectDispatchMetrics final {
  public:
    void record_hit(RuntimeDispatchClass dispatch_class,
                    std::uint32_t callsite = 0u,
                    std::uint32_t target = 0u,
                    bool materialized = false) noexcept;
    void record_miss(RuntimeDispatchClass dispatch_class,
                     DispatchDiagnosticError error,
                     std::uint32_t callsite,
                     std::uint32_t target) noexcept;
    void record_fallback(RuntimeDispatchClass dispatch_class) noexcept;
    void record_invalidation(std::uint32_t callsite) noexcept;
    [[nodiscard]] std::uint64_t hits() const noexcept;
    [[nodiscard]] std::uint64_t misses() const noexcept;
    [[nodiscard]] std::uint64_t fallbacks() const noexcept;
    [[nodiscard]] std::uint64_t runtime_only_hits() const noexcept;
    [[nodiscard]] std::uint64_t runtime_only_misses() const noexcept;
    [[nodiscard]] std::uint64_t runtime_only_fallbacks() const noexcept;
    [[nodiscard]] std::size_t runtime_only_site_count() const noexcept;
    [[nodiscard]] std::uint64_t runtime_only_dispatch_share_ppm() const noexcept;
    [[nodiscard]] const std::optional<IndirectDispatchFirstError>& first_error() const noexcept;
    [[nodiscard]] const std::map<std::uint32_t, RuntimeOnlySiteMetrics>&
    runtime_only_sites() const noexcept;
    [[nodiscard]] std::string serialize_json(bool include_site_details = false) const;

  private:
    std::uint64_t hits_ = 0u;
    std::uint64_t misses_ = 0u;
    std::uint64_t fallbacks_ = 0u;
    std::uint64_t runtime_only_hits_ = 0u;
    std::uint64_t runtime_only_misses_ = 0u;
    std::uint64_t runtime_only_fallbacks_ = 0u;
    std::optional<IndirectDispatchFirstError> first_error_;
    std::map<std::uint32_t, RuntimeOnlySiteMetrics> runtime_only_sites_;
};

struct IndirectDispatchRequest {
    IndirectDispatchKind kind = IndirectDispatchKind::TailJump;
    std::uint32_t callsite = 0u;
    std::uint32_t target = 0u;
    std::uint32_t return_address = 0u;
    BlockAddress source;
    BlockVariantKey variant;
    DispatchResolutionOrigin resolution_origin = DispatchResolutionOrigin::TableLookup;
    DispatchDiagnosticRecorder* diagnostics = nullptr;
    RuntimeDispatchClass dispatch_class = RuntimeDispatchClass::GuardedFallback;
    IndirectDispatchMetrics* metrics = nullptr;
    DemandBlockMaterializer* materializer = nullptr;
};

struct IndirectDispatchResult {
    RuntimeBlockHandle block;
    std::uint32_t diagnostic_target = 0u;
    std::uint32_t physical_target = 0u;
    std::uint32_t resulting_pc = 0u;
    std::uint32_t resulting_pr = 0u;
    bool alias_lookup = false;
    std::string diagnostic;
};

class IndirectDispatchError final : public std::runtime_error {
  public:
    IndirectDispatchError(
        IndirectDispatchKind kind,
        std::uint32_t callsite,
        std::uint32_t target,
        BlockAddress source,
        DispatchDiagnosticError error = DispatchDiagnosticError::UnknownTarget,
        RuntimeDispatchClass dispatch_class = RuntimeDispatchClass::GuardedFallback,
        std::string metrics_json = {});
    [[nodiscard]] const std::string& metrics_json() const noexcept;

  private:
    std::string metrics_json_;
};

[[nodiscard]] const char* runtime_dispatch_class_name(RuntimeDispatchClass value) noexcept;
[[nodiscard]] const char* runtime_target_stability_name(RuntimeTargetStability value) noexcept;

[[nodiscard]] IndirectDispatchResult dispatch_indirect(CpuState& cpu,
                                                       const RuntimeBlockTable& table,
                                                       const IndirectDispatchRequest& request);

} // namespace katana::runtime
