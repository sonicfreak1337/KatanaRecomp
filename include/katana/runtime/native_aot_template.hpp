#pragma once

#include "katana/runtime/executable_modules.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace katana::runtime {

struct NativeAotTemplatePatchTarget {
    std::uint32_t live_value = 0u;
    std::uint32_t block_address = 0u;
};

struct NativeAotTemplatePatch {
    std::uint32_t source_offset = 0u;
    std::vector<NativeAotTemplatePatchTarget> allowed_targets;
};

enum class NativeAotTemplateDestination : std::uint8_t {
    VbrRelative,
    LoadedModule
};

// Describes proof metadata only. The original template bytes stay in the local
// disc-backed ExecutableModuleCatalog and are never embedded in an exported port.
struct NativeAotTemplate {
    std::string source_module_id;
    std::string expected_source_identity;
    std::uint32_t source_start = 0u;
    std::uint32_t extent = 0u;
    std::int32_t destination_vbr_delta = 0;
    std::vector<NativeAotTemplatePatch> patches;
    NativeAotTemplateDestination destination = NativeAotTemplateDestination::VbrRelative;
    std::string expected_runtime_content_identity;
    std::string expected_runtime_byte_identity;
};

enum class NativeAotTemplateBindFailure : std::uint8_t {
    None,
    NoMatchingDestination,
    AmbiguousDestination,
    InvalidDefinition,
    SourceModuleMissing,
    SourceIdentityMismatch,
    RuntimeBytesMismatch,
    PatchTargetRejected,
    SourceBlockMissing,
    RuntimeContentIdentityMismatch,
    MissingAot
};

struct NativeAotTemplateBindResult {
    MaterializedBlockCandidate candidate;
    NativeAotTemplateBindFailure failure = NativeAotTemplateBindFailure::None;

    [[nodiscard]] explicit operator bool() const noexcept {
        return failure == NativeAotTemplateBindFailure::None &&
               candidate.decode_candidate_validated;
    }
};

// Binds a runtime copy to already generated native code. It never decodes,
// interprets or generates guest code at runtime.
class NativeAotTemplateBinder final {
  public:
    NativeAotTemplateBinder(CpuState& cpu,
                            const ExecutableModuleCatalog& modules,
                            const RuntimeBlockTable& blocks,
                            std::span<const NativeAotTemplate> templates) noexcept;

    [[nodiscard]] NativeAotTemplateBindResult
    bind(std::uint32_t target,
         std::uint32_t physical_origin,
         std::span<const std::uint8_t> target_suffix,
         const BlockVariantKey& variant) const;

  private:
    CpuState& cpu_;
    const ExecutableModuleCatalog& modules_;
    const RuntimeBlockTable& blocks_;
    std::span<const NativeAotTemplate> templates_;
};

[[nodiscard]] const char*
native_aot_template_bind_failure_name(NativeAotTemplateBindFailure failure) noexcept;

} // namespace katana::runtime
