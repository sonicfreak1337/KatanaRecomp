#include "katana/runtime/indirect_dispatch.hpp"

#include "katana/runtime/exception.hpp"
#include "katana/runtime/executable_modules.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace katana::runtime {
namespace {

const char* kind_name(const IndirectDispatchKind kind) noexcept {
    switch (kind) {
    case IndirectDispatchKind::Call:
        return "call";
    case IndirectDispatchKind::TailJump:
        return "tail-jump";
    case IndirectDispatchKind::Return:
        return "return";
    }
    return "unknown";
}

std::string describe(const IndirectDispatchRequest& request, const std::uint32_t target) {
    std::ostringstream out;
    out << kind_name(request.kind) << " callsite=0x" << std::hex << std::setw(8)
        << std::setfill('0') << request.callsite << " target=0x" << std::setw(8) << target
        << " pr=0x" << std::setw(8) << request.return_address
        << " source=" << stable_block_identity(request.source)
        << " class=" << runtime_dispatch_class_name(request.dispatch_class);
    return out.str();
}

BlockEndKind block_end(const IndirectDispatchKind kind) noexcept {
    switch (kind) {
    case IndirectDispatchKind::Call:
        return BlockEndKind::Call;
    case IndirectDispatchKind::TailJump:
        return BlockEndKind::DynamicBranch;
    case IndirectDispatchKind::Return:
        return BlockEndKind::Return;
    }
    return BlockEndKind::DynamicBranch;
}

void diagnose(const IndirectDispatchRequest& request,
              const std::uint32_t target,
              const std::uint32_t pr,
              const bool alias_lookup,
              const bool resolved,
              const DispatchDiagnosticError error = DispatchDiagnosticError::None) noexcept {
    if (request.diagnostics == nullptr) return;
    static_cast<void>(request.diagnostics->try_record(
        {request.callsite,
         request.source.virtual_address,
         canonical_physical_address(request.source.physical_address),
         target,
         canonical_physical_address(target),
         pr,
         block_end(request.kind),
         request.resolution_origin,
         resolved ? (alias_lookup ? DispatchAliasOrigin::CanonicalPhysical
                                  : DispatchAliasOrigin::ExactVirtual)
                  : DispatchAliasOrigin::None,
         resolved ? DispatchFallbackReason::None : DispatchFallbackReason::UnresolvedControlFlow,
         resolved ? DispatchFallbackAction::None : DispatchFallbackAction::Abort,
         0u,
         resolved ? target : request.callsite,
         resolved ? DispatchDiagnosticError::None : error}));
}

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

DispatchDiagnosticError materialization_error(const MaterializationFailure failure) noexcept {
    switch (failure) {
    case MaterializationFailure::Misaligned:
        return DispatchDiagnosticError::Misaligned;
    case MaterializationFailure::Uncommitted:
        return DispatchDiagnosticError::UnmappedMemory;
    case MaterializationFailure::PermissionDenied:
        return DispatchDiagnosticError::PermissionDenied;
    case MaterializationFailure::ProvenNonCode:
        return DispatchDiagnosticError::ProvenNonCode;
    case MaterializationFailure::BudgetExhausted:
    case MaterializationFailure::RepeatedMissLimit:
        return DispatchDiagnosticError::MaterializationBudget;
    case MaterializationFailure::ByteIdentityMismatch:
        return DispatchDiagnosticError::ByteIdentityMismatch;
    case MaterializationFailure::GenerationMismatch:
    case MaterializationFailure::ModuleUnloaded:
        return DispatchDiagnosticError::GenerationMismatch;
    case MaterializationFailure::RelocationMismatch:
        return DispatchDiagnosticError::RelocationMismatch;
    case MaterializationFailure::StaleHandle:
        return DispatchDiagnosticError::StaleBlock;
    case MaterializationFailure::None:
    case MaterializationFailure::Disabled:
    case MaterializationFailure::UnknownSource:
    case MaterializationFailure::DecodeRejected:
    case MaterializationFailure::AnalysisIncomplete:
    case MaterializationFailure::IrVerificationFailed:
    case MaterializationFailure::CodeGenerationFailed:
    case MaterializationFailure::InvalidBlock:
        return DispatchDiagnosticError::UnknownTarget;
    }
    return DispatchDiagnosticError::UnknownTarget;
}

} // namespace

IndirectDispatchError::IndirectDispatchError(const IndirectDispatchKind kind,
                                             const std::uint32_t callsite,
                                             const std::uint32_t target,
                                             const BlockAddress source,
                                             const DispatchDiagnosticError error,
                                             const RuntimeDispatchClass dispatch_class,
                                             std::string metrics_json)
    : std::runtime_error([&] {
          IndirectDispatchRequest request;
          request.kind = kind;
          request.callsite = callsite;
          request.return_address = 0u;
          request.source = source;
          request.dispatch_class = dispatch_class;
          return "Ungueltiges indirektes Ziel (" +
                 std::string(dispatch_diagnostic_error_name(error)) +
                 "): " + describe(request, target);
      }()),
      metrics_json_(std::move(metrics_json)) {
    if (metrics_json_.empty()) {
        IndirectDispatchMetrics metrics;
        metrics.record_miss(dispatch_class, error, callsite, target);
        metrics_json_ = metrics.serialize_json();
    }
}

const std::string& IndirectDispatchError::metrics_json() const noexcept {
    return metrics_json_;
}

namespace {
void record_target(RuntimeOnlySiteMetrics& site, const std::uint32_t target) noexcept {
    if (std::find(site.targets.begin(), site.targets.end(), target) != site.targets.end()) return;
    if (site.targets.size() >= 16u) {
        site.targets_truncated = true;
        return;
    }
    site.targets.push_back(target);
    std::sort(site.targets.begin(), site.targets.end());
}
} // namespace

RuntimeTargetStability RuntimeOnlySiteMetrics::stability() const noexcept {
    if (hits == 0u) return RuntimeTargetStability::NeverHit;
    if (!targets_truncated && targets.size() == 1u) return RuntimeTargetStability::Monomorphic;
    if (!targets_truncated && targets.size() <= 4u) return RuntimeTargetStability::SmallPolymorphic;
    return RuntimeTargetStability::Dynamic;
}

void IndirectDispatchMetrics::record_hit(const RuntimeDispatchClass dispatch_class,
                                         const std::uint32_t callsite,
                                         const std::uint32_t target,
                                         const bool materialized) noexcept {
    increment(hits_);
    if (dispatch_class == RuntimeDispatchClass::RuntimeOnly) {
        increment(runtime_only_hits_);
        auto& site = runtime_only_sites_[callsite];
        site.callsite = callsite;
        increment(site.calls);
        increment(site.hits);
        if (materialized) increment(site.materializations);
        record_target(site, target);
    }
}

void IndirectDispatchMetrics::record_miss(const RuntimeDispatchClass dispatch_class,
                                          const DispatchDiagnosticError error,
                                          const std::uint32_t callsite,
                                          const std::uint32_t target) noexcept {
    increment(misses_);
    if (dispatch_class == RuntimeDispatchClass::RuntimeOnly) {
        increment(runtime_only_misses_);
        auto& site = runtime_only_sites_[callsite];
        site.callsite = callsite;
        increment(site.calls);
        increment(site.misses);
        record_target(site, target);
    }
    if (!first_error_.has_value())
        first_error_ = IndirectDispatchFirstError{error, dispatch_class, callsite, target};
}

void IndirectDispatchMetrics::record_fallback(const RuntimeDispatchClass dispatch_class) noexcept {
    increment(fallbacks_);
    if (dispatch_class == RuntimeDispatchClass::RuntimeOnly) increment(runtime_only_fallbacks_);
}

void IndirectDispatchMetrics::record_invalidation(const std::uint32_t callsite) noexcept {
    auto& site = runtime_only_sites_[callsite];
    site.callsite = callsite;
    increment(site.invalidations);
}

std::uint64_t IndirectDispatchMetrics::hits() const noexcept {
    return hits_;
}
std::uint64_t IndirectDispatchMetrics::misses() const noexcept {
    return misses_;
}
std::uint64_t IndirectDispatchMetrics::fallbacks() const noexcept {
    return fallbacks_;
}
std::uint64_t IndirectDispatchMetrics::runtime_only_hits() const noexcept {
    return runtime_only_hits_;
}
std::uint64_t IndirectDispatchMetrics::runtime_only_misses() const noexcept {
    return runtime_only_misses_;
}
std::uint64_t IndirectDispatchMetrics::runtime_only_fallbacks() const noexcept {
    return runtime_only_fallbacks_;
}
std::size_t IndirectDispatchMetrics::runtime_only_site_count() const noexcept {
    return runtime_only_sites_.size();
}
std::uint64_t IndirectDispatchMetrics::runtime_only_dispatch_share_ppm() const noexcept {
    return hits_ + misses_ == 0u
               ? 0u
               : ((runtime_only_hits_ + runtime_only_misses_) * 1'000'000u) / (hits_ + misses_);
}
const std::optional<IndirectDispatchFirstError>&
IndirectDispatchMetrics::first_error() const noexcept {
    return first_error_;
}

const std::map<std::uint32_t, RuntimeOnlySiteMetrics>&
IndirectDispatchMetrics::runtime_only_sites() const noexcept {
    return runtime_only_sites_;
}

std::string IndirectDispatchMetrics::serialize_json(const bool include_site_details) const {
    std::ostringstream output;
    output << "{\"schema\":\"katana-indirect-dispatch-v1\",\"report_version\":1"
           << ",\"report_type\":\"indirect-dispatch\",\"status\":\"success\",\"hits\":" << hits_
           << ",\"misses\":" << misses_ << ",\"fallbacks\":" << fallbacks_
           << ",\"runtime_only_hits\":" << runtime_only_hits_
           << ",\"runtime_only_misses\":" << runtime_only_misses_
           << ",\"runtime_only_fallbacks\":" << runtime_only_fallbacks_
           << ",\"runtime_only_sites\":" << runtime_only_site_count()
           << ",\"runtime_only_dispatch_share_ppm\":" << runtime_only_dispatch_share_ppm()
           << ",\"first_error\":";
    if (!first_error_) {
        output << "null";
    } else {
        output << "{\"error\":\"" << dispatch_diagnostic_error_name(first_error_->error)
               << "\",\"class\":\"" << runtime_dispatch_class_name(first_error_->dispatch_class)
               << "\",\"callsite\":\"0x" << std::hex << std::uppercase << std::setw(8)
               << std::setfill('0') << first_error_->callsite << "\",\"target\":\"0x"
               << std::setw(8) << first_error_->target << "\"}";
    }
    output << ",\"site_profiles\":[";
    if (include_site_details) {
        std::size_t current = 0u;
        for (const auto& [callsite, site] : runtime_only_sites_) {
            if (current++ != 0u) output << ',';
            output << "{\"callsite\":\"0x" << std::hex << std::uppercase << std::setw(8)
                   << std::setfill('0') << callsite << std::dec << "\",\"calls\":" << site.calls
                   << ",\"hits\":" << site.hits << ",\"misses\":" << site.misses
                   << ",\"materializations\":" << site.materializations
                   << ",\"invalidations\":" << site.invalidations << ",\"stability\":\""
                   << runtime_target_stability_name(site.stability()) << "\",\"targets\":[";
            for (std::size_t target = 0u; target < site.targets.size(); ++target) {
                if (target != 0u) output << ',';
                output << "\"0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
                       << site.targets[target] << std::dec << '"';
            }
            output << "],\"targets_truncated\":" << (site.targets_truncated ? "true" : "false")
                   << '}';
        }
    }
    output << "]}";
    return output.str();
}

const char* runtime_target_stability_name(const RuntimeTargetStability value) noexcept {
    switch (value) {
    case RuntimeTargetStability::NeverHit:
        return "never-hit";
    case RuntimeTargetStability::Monomorphic:
        return "monomorphic";
    case RuntimeTargetStability::SmallPolymorphic:
        return "small-polymorphic";
    case RuntimeTargetStability::Dynamic:
        return "dynamic";
    }
    return "dynamic";
}

const char* runtime_dispatch_class_name(const RuntimeDispatchClass value) noexcept {
    switch (value) {
    case RuntimeDispatchClass::GuardedFallback:
        return "guarded-fallback";
    case RuntimeDispatchClass::RuntimeOnly:
        return "runtime-only";
    }
    return "guarded-fallback";
}

IndirectDispatchResult dispatch_indirect(CpuState& cpu,
                                         const RuntimeBlockTable& table,
                                         const IndirectDispatchRequest& request) {
    const auto requested_target =
        request.kind == IndirectDispatchKind::Return ? cpu.pr : request.target;
    auto target = requested_target;
    std::uint32_t physical = 0u;
    try {
        physical = translate_guest_address(cpu,
                                           target,
                                           MemoryAccessOperation::Read,
                                           MemoryAccessWidth::Halfword,
                                           true);
    } catch (const MemoryAccessError& error) {
        enter_memory_exception(cpu, error, request.callsite);
        target = cpu.pc;
        physical = translate_guest_address(cpu,
                                           target,
                                           MemoryAccessOperation::Read,
                                           MemoryAccessWidth::Halfword,
                                           true);
    }
    const auto reject = [&](const DispatchDiagnosticError error) {
        table.mark_rejected(target, request.variant);
        if (request.metrics != nullptr)
            request.metrics->record_miss(request.dispatch_class, error, request.callsite, target);
        diagnose(request, target, cpu.pr, false, false, error);
        throw IndirectDispatchError(request.kind,
                                    request.callsite,
                                    target,
                                    request.source,
                                    error,
                                    request.dispatch_class,
                                    request.metrics != nullptr ? request.metrics->serialize_json()
                                                               : std::string{});
    };
    if ((target & 1u) != 0u) {
        if (request.materializer != nullptr) {
            static_cast<void>(request.materializer->try_materialize(
                cpu, target, request.variant, request.callsite));
            reject(materialization_error(request.materializer->last_failure()));
        }
        reject(DispatchDiagnosticError::Misaligned);
    }
    auto block = table.lookup(target, request.variant);
    bool alias_lookup = false;
    if (!block) {
        block = table.lookup_physical(physical, request.variant);
        alias_lookup = block.has_value();
    }
    bool materialized = false;
    if (!block && request.materializer != nullptr) {
        block =
            request.materializer->try_materialize(cpu, target, request.variant, request.callsite);
        alias_lookup = false;
        materialized = block.has_value();
    }
    if (!block)
        reject(request.materializer != nullptr
                   ? materialization_error(request.materializer->last_failure())
                   : DispatchDiagnosticError::UnknownTarget);
    const auto resolved = table.resolve(*block);
    if (!resolved || resolved->get().function == nullptr || resolved->get().size < 2u ||
        (resolved->get().virtual_start & 1u) != 0u ||
        (alias_lookup ? resolved->get().physical_origin != physical
                      : resolved->get().virtual_start != target))
        reject(DispatchDiagnosticError::InvalidBoundary);

    if (resolved->get().runtime_registered &&
        (request.materializer == nullptr ||
         !request.materializer->validate_for_dispatch(cpu, *block, target)))
        reject(request.materializer != nullptr
                   ? materialization_error(request.materializer->last_failure())
                   : DispatchDiagnosticError::StaleBlock);

    if (request.kind == IndirectDispatchKind::Call) {
        cpu.pr = request.return_address;
    }
    cpu.pc = alias_lookup ? resolved->get().virtual_start : target;
    if (request.metrics != nullptr)
        request.metrics->record_hit(request.dispatch_class, request.callsite, target, materialized);
    diagnose(request, target, cpu.pr, alias_lookup, true);
    return {*block,
            target,
            physical,
            cpu.pc,
            cpu.pr,
            alias_lookup,
            describe(request, target) + (alias_lookup ? " alias=physical" : " alias=exact")};
}

} // namespace katana::runtime
