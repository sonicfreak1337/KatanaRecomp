#include "katana/runtime/indirect_dispatch.hpp"

#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/exception.hpp"
#include "katana/runtime/executable_modules.hpp"

#include <algorithm>
#include <array>
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
    case MaterializationFailure::AotTemplateMismatch:
        return DispatchDiagnosticError::ByteIdentityMismatch;
    case MaterializationFailure::MissingAot:
        return DispatchDiagnosticError::MissingAot;
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

constexpr std::size_t dispatch_inline_cache_size = 4096u;
static_assert((dispatch_inline_cache_size & (dispatch_inline_cache_size - 1u)) == 0u);

struct MonomorphicDispatchCacheEntry {
    const RuntimeBlockTable* table = nullptr;
    const DemandBlockMaterializer* materializer = nullptr;
    std::uint32_t callsite = 0u;
    std::uint32_t target = 0u;
    std::uint32_t physical_target = 0u;
    IndirectDispatchKind kind = IndirectDispatchKind::TailJump;
    RuntimeDispatchClass dispatch_class = RuntimeDispatchClass::GuardedFallback;
    BlockVariantKey effective_variant;
    ValidatedBlockExecution execution;
    bool direct_p1_p2 = false;
    bool valid = false;
};

thread_local std::array<MonomorphicDispatchCacheEntry, dispatch_inline_cache_size>
    dispatch_inline_cache;

std::size_t dispatch_inline_cache_index(const std::uint32_t callsite,
                                        const IndirectDispatchKind kind) noexcept {
    auto key = (callsite >> 1u) ^
               (static_cast<std::uint32_t>(kind) * 0x9E3779B9u);
    key *= 0x85EBCA6Bu;
    return static_cast<std::size_t>(key) & (dispatch_inline_cache_size - 1u);
}

ValidatedBlockExecution make_validated_execution(
    const RuntimeBlockHandle handle,
    const RuntimeBlock& block,
    const std::optional<BlockDispatchGenerationGuard>& reusable_guard = std::nullopt) noexcept {
    ValidatedBlockExecution execution;
    execution.block = handle;
    execution.function = block.function;
    execution.virtual_start = block.virtual_start;
    execution.physical_origin = block.physical_origin;
    execution.size = block.size;
    execution.variant = block.variant;
    execution.end_kind = block.end_kind;
    execution.runtime_registered = block.runtime_registered;
    execution.provenance = block.provenance;
    execution.aot_template = block.aot_template ? &*block.aot_template : nullptr;
    execution.generation_guard =
        reusable_guard.value_or(BlockDispatchGenerationGuard{
            .block = handle,
            .runtime_registered = block.runtime_registered,
        });
    execution.generation_guard_reusable = reusable_guard.has_value();
    return execution;
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
      metrics_json_(std::move(metrics_json)),
      kind_(kind),
      callsite_(callsite),
      target_(target),
      source_(source),
      error_(error),
      dispatch_class_(dispatch_class) {
    if (metrics_json_.empty()) {
        IndirectDispatchMetrics metrics;
        metrics.record_miss(dispatch_class, error, callsite, target);
        metrics_json_ = metrics.serialize_json();
    }
}

const std::string& IndirectDispatchError::metrics_json() const noexcept {
    return metrics_json_;
}

IndirectDispatchKind IndirectDispatchError::kind() const noexcept {
    return kind_;
}

std::uint32_t IndirectDispatchError::callsite() const noexcept {
    return callsite_;
}

std::uint32_t IndirectDispatchError::target() const noexcept {
    return target_;
}

BlockAddress IndirectDispatchError::source() const noexcept {
    return source_;
}

DispatchDiagnosticError IndirectDispatchError::error() const noexcept {
    return error_;
}

RuntimeDispatchClass IndirectDispatchError::dispatch_class() const noexcept {
    return dispatch_class_;
}

namespace {
void record_target(RuntimeOnlySiteMetrics& site, const std::uint32_t target) noexcept {
    if (site.targets.empty()) {
        site.targets.push_back(target);
        return;
    }
    if (site.targets.front() != target)
        site.targets_truncated = true;
}
} // namespace

RuntimeTargetStability RuntimeOnlySiteMetrics::stability() const noexcept {
    if (hits == 0u) return RuntimeTargetStability::NeverHit;
    if (!targets_truncated && targets.size() == 1u) return RuntimeTargetStability::Monomorphic;
    if (!targets_truncated && targets.size() <= 4u) return RuntimeTargetStability::SmallPolymorphic;
    return RuntimeTargetStability::Dynamic;
}

void IndirectDispatchMetrics::set_site_details_enabled(const bool enabled) noexcept {
    site_details_enabled_ = enabled;
}

bool IndirectDispatchMetrics::site_details_enabled() const noexcept {
    return site_details_enabled_;
}

void IndirectDispatchMetrics::record_hit(const RuntimeDispatchClass dispatch_class,
                                         const std::uint32_t callsite,
                                         const std::uint32_t target,
                                         const bool materialized) noexcept {
    increment(hits_);
    if (dispatch_class == RuntimeDispatchClass::RuntimeOnly) {
        increment(runtime_only_hits_);
        if (!site_details_enabled_) return;
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
        if (site_details_enabled_) {
            auto& site = runtime_only_sites_[callsite];
            site.callsite = callsite;
            increment(site.calls);
            increment(site.misses);
            record_target(site, target);
        }
    }
    if (!first_error_.has_value())
        first_error_ = IndirectDispatchFirstError{error, dispatch_class, callsite, target};
}

void IndirectDispatchMetrics::record_fallback(const RuntimeDispatchClass dispatch_class) noexcept {
    increment(fallbacks_);
    if (dispatch_class == RuntimeDispatchClass::RuntimeOnly) increment(runtime_only_fallbacks_);
}

void IndirectDispatchMetrics::record_invalidation(const std::uint32_t callsite) noexcept {
    if (!site_details_enabled_) return;
    const auto site = runtime_only_sites_.find(callsite);
    if (site != runtime_only_sites_.end()) increment(site->second.invalidations);
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

IndirectDispatchContinuation make_indirect_dispatch_continuation(
    const BlockExit& exit,
    const DynamicDispatchSiteClass site_class) noexcept {
    const bool runtime_only = site_class == DynamicDispatchSiteClass::RuntimeOnly;
    const auto origin = [site_class] {
        switch (site_class) {
        case DynamicDispatchSiteClass::NotDynamic:
            return DispatchResolutionOrigin::StaticProof;
        case DynamicDispatchSiteClass::Guarded:
            return DispatchResolutionOrigin::TableLookup;
        case DynamicDispatchSiteClass::RuntimeOnly:
            return DispatchResolutionOrigin::RuntimeOnly;
        case DynamicDispatchSiteClass::Unresolved:
            return DispatchResolutionOrigin::Fallback;
        }
        return DispatchResolutionOrigin::Fallback;
    }();
    return {exit.kind == BlockEndKind::Call ? IndirectDispatchKind::Call
                                            : IndirectDispatchKind::TailJump,
            exit.source.virtual_address,
            exit.source,
            origin,
            runtime_only ? RuntimeDispatchClass::RuntimeOnly
                         : RuntimeDispatchClass::GuardedFallback,
            site_class != DynamicDispatchSiteClass::NotDynamic};
}

IndirectDispatchResult dispatch_indirect(CpuState& cpu,
                                         RuntimeBlockTable& table,
                                         const IndirectDispatchRequest& request) {
    const auto requested_target =
        request.kind == IndirectDispatchKind::Return ? cpu.pr : request.target;
    auto target = requested_target;
    std::uint32_t physical = 0u;
    auto effective_variant = request.variant;
    bool architectural_target_fetch_fault = false;
    const auto try_cached_dispatch =
        [&](const BlockVariantKey& candidate_variant,
            const std::uint32_t candidate_physical,
            const bool require_direct_p1_p2)
        -> std::optional<IndirectDispatchResult> {
        if (architectural_target_fetch_fault) return std::nullopt;
        auto& cached = dispatch_inline_cache[
            dispatch_inline_cache_index(request.callsite, request.kind)];
        const auto cache_key_matches =
            cached.valid && cached.table == &table &&
            cached.materializer == request.materializer &&
            cached.callsite == request.callsite && cached.kind == request.kind &&
            cached.dispatch_class == request.dispatch_class &&
            (!require_direct_p1_p2 || cached.direct_p1_p2) &&
            cached.target == target && cached.physical_target == candidate_physical &&
            cached.effective_variant == candidate_variant;
        if (!cache_key_matches) return std::nullopt;

        const auto static_alias_matches =
            cached.execution.generation_guard.kind ==
                BlockDispatchGenerationGuardKind::StaticAot &&
            require_direct_p1_p2 &&
            canonical_physical_address(cached.execution.virtual_start) ==
                canonical_physical_address(candidate_physical);
        const auto exact_block_matches =
            cached.execution.function != nullptr && cached.execution.size >= 2u &&
            (cached.execution.virtual_start & 1u) == 0u &&
            (cached.execution.virtual_start == target || static_alias_matches) &&
            canonical_physical_address(cached.execution.physical_origin) ==
                canonical_physical_address(candidate_physical) &&
            cached.execution.variant == candidate_variant &&
            cached.execution.generation_guard_reusable;
        if (!exact_block_matches) {
            cached.valid = false;
            return std::nullopt;
        }
        const auto guard_current = [&] {
            if (cached.execution.generation_guard.kind ==
                BlockDispatchGenerationGuardKind::Materializer)
                return request.materializer != nullptr &&
                       request.materializer->dispatch_generation_guard_current(
                           cpu, cached.execution.generation_guard);
            if (cached.execution.generation_guard.kind ==
                BlockDispatchGenerationGuardKind::StaticAot)
                return table.static_dispatch_generation_guard_current(
                    cached.execution.generation_guard);
            return false;
        }();
        if (guard_current) {
            const bool alias_lookup = cached.execution.virtual_start != target;
            if (request.kind == IndirectDispatchKind::Call)
                cpu.pr = request.return_address;
            cpu.pc = cached.execution.virtual_start;
            if (request.metrics != nullptr)
                request.metrics->record_hit(
                    request.dispatch_class, request.callsite, target, false);
            const bool plain_runtime_hit =
                request.dispatch_class == RuntimeDispatchClass::RuntimeOnly &&
                request.kind == IndirectDispatchKind::TailJump && !alias_lookup;
            if (!plain_runtime_hit)
                diagnose(request, target, cpu.pr, alias_lookup, true);
            return IndirectDispatchResult{
                cached.execution.block,
                cached.execution,
                target,
                candidate_physical,
                cpu.pc,
                cpu.pr,
                alias_lookup,
                false,
                {},
            };
        }
        cached.valid = false;
        return std::nullopt;
    };
    const auto try_static_aot_dispatch =
        [&]() -> std::optional<IndirectDispatchResult> {
        if (architectural_target_fetch_fault) return std::nullopt;
        auto execution =
            table.lookup_static_aot(physical, target, effective_variant);
        if (!execution) return std::nullopt;

        const bool alias_lookup = execution->virtual_start != target;
        if (request.kind == IndirectDispatchKind::Call)
            cpu.pr = request.return_address;
        cpu.pc = execution->virtual_start;
        if (request.metrics != nullptr)
            request.metrics->record_hit(
                request.dispatch_class, request.callsite, target, false);
        const bool plain_runtime_hit =
            request.dispatch_class == RuntimeDispatchClass::RuntimeOnly &&
            request.kind == IndirectDispatchKind::TailJump && !alias_lookup;
        if (!plain_runtime_hit)
            diagnose(request, target, cpu.pr, alias_lookup, true);
        if (execution->generation_guard_reusable) {
            auto& cached = dispatch_inline_cache[
                dispatch_inline_cache_index(request.callsite, request.kind)];
            cached.table = &table;
            cached.materializer = request.materializer;
            cached.callsite = request.callsite;
            cached.target = target;
            cached.physical_target = physical;
            cached.kind = request.kind;
            cached.dispatch_class = request.dispatch_class;
            cached.effective_variant = effective_variant;
            cached.execution = *execution;
            cached.direct_p1_p2 = true;
            cached.valid = true;
        }
        return IndirectDispatchResult{
            execution->block,
            *execution,
            target,
            physical,
            cpu.pc,
            cpu.pr,
            alias_lookup,
            false,
            {},
        };
    };

    bool direct_p1_p2_target = false;
    if (cpu.address_space) {
        const auto segment = target >> 29u;
        if (segment == 4u || segment == 5u) {
            const auto& cached = dispatch_inline_cache[
                dispatch_inline_cache_index(request.callsite, request.kind)];
            if (cached.valid && cached.table == &table &&
                cached.materializer == request.materializer &&
                cached.callsite == request.callsite &&
                cached.kind == request.kind &&
                cached.dispatch_class == request.dispatch_class &&
                cached.direct_p1_p2 && cached.target == target &&
                cpu.address_space->direct_p1_p2_dispatch_guard_current(
                    target,
                    cpu.read_fpscr(),
                    cpu.privileged_mode(),
                    cached.effective_variant,
                    request.variant.runtime_generation)) {
                direct_p1_p2_target = true;
                physical = cached.physical_target;
                effective_variant = cached.effective_variant;
                if (auto hit =
                        try_cached_dispatch(effective_variant, physical, true))
                    return std::move(*hit);
            }
        }
    }
    if (cpu.address_space) {
        if (const auto guard = cpu.address_space->direct_p1_p2_instruction_guard(
                target, cpu.read_fpscr(), cpu.privileged_mode())) {
            direct_p1_p2_target = true;
            physical = canonical_physical_address(target);
            effective_variant =
                block_variant_key(*guard, request.variant.runtime_generation);
            if (auto cached =
                    try_cached_dispatch(effective_variant, physical, true))
                return std::move(*cached);
            if (auto static_aot = try_static_aot_dispatch())
                return std::move(*static_aot);
        }
    }
    const auto translate_target = [&] {
        effective_variant = request.variant;
        physical = translate_guest_address(cpu,
                                           target,
                                           MemoryAccessOperation::Read,
                                           MemoryAccessWidth::Halfword,
                                           true);
        if (cpu.address_space) {
            effective_variant = block_variant_key(
                cpu.address_space->guard_for(
                    target, cpu.read_fpscr(), cpu.privileged_mode()),
                request.variant.runtime_generation);
        }
    };
    try {
        translate_target();
    } catch (const MemoryAccessError& error) {
        GuestInstructionAttempt target_fetch_attempt(cpu, target, 1u);
        // A call and its delay slot have architecturally completed before target
        // fetch. Preserve the resulting PR on that architectural exception edge;
        // ordinary host lookup rejection below remains transactional.
        architectural_target_fetch_fault = true;
        if (request.kind == IndirectDispatchKind::Call) {
            cpu.pr = request.return_address;
        }
        // The architectural faulting instruction fetch is at the target after the
        // branch delay slot, not at the already-retired control-transfer site.
        enter_memory_exception(cpu, error, target);
        target = cpu.pc;
        try {
            translate_target();
        } catch (const MemoryAccessError& handler_error) {
            GuestInstructionAttempt handler_fetch_attempt(cpu, target, 1u);
            // A fault while fetching the exception handler is a second architectural
            // instruction-fetch attempt. SR.BL is already set, so the common exception
            // path converts a general exception into the shared manual-reset transition
            // (and handles a repeated TLB multiple hit through the same reset contract).
            enter_memory_exception(cpu, handler_error, target);
            target = cpu.pc;
            // A valid manual reset always selects the privileged P2 reset vector. Failure
            // to translate that vector is an internal reset-contract violation and must
            // remain visible rather than being turned into an unbounded exception loop.
            translate_target();
        }
    }
    if (!direct_p1_p2_target)
        if (auto cached =
                try_cached_dispatch(effective_variant, physical, false))
            return std::move(*cached);
    const auto reject = [&](const DispatchDiagnosticError error) {
        table.mark_rejected(target, effective_variant);
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
    auto block = table.lookup(target, effective_variant);
    bool alias_lookup = false;
    bool variant_materialized = false;
    bool invalidated = false;
    if (!block) {
        block = table.lookup_physical(physical, effective_variant);
        alias_lookup = block.has_value();
    }
    if (!block && effective_variant != request.variant) {
        block = table.register_static_variant(
            target, physical, request.variant, effective_variant);
        alias_lookup = false;
        variant_materialized = block.has_value();
    }
    bool materialized = false;
    if (!block && request.materializer != nullptr) {
        const auto materializations_before = request.materializer->metrics().materializations;
        block =
            request.materializer->try_materialize(
                cpu, target, physical, effective_variant, request.callsite);
        alias_lookup = false;
        materialized = block.has_value() &&
                       request.materializer->metrics().materializations !=
                           materializations_before;
    }
    if (!block)
        reject(request.materializer != nullptr
                   ? materialization_error(request.materializer->last_failure())
                   : DispatchDiagnosticError::UnknownTarget);
    auto resolved = table.resolve(*block);
    const auto valid_boundary = [&] {
        const auto runtime_interior =
            resolved && resolved->get().runtime_registered && !alias_lookup &&
            static_cast<std::uint64_t>(target) >= resolved->get().virtual_start &&
            static_cast<std::uint64_t>(target) + 2u <=
                static_cast<std::uint64_t>(resolved->get().virtual_start) +
                    resolved->get().size;
        return resolved && resolved->get().function != nullptr && resolved->get().size >= 2u &&
               (resolved->get().virtual_start & 1u) == 0u &&
               (alias_lookup ? resolved->get().physical_origin == physical
                             : resolved->get().virtual_start == target || runtime_interior);
    };
    if (!valid_boundary()) reject(DispatchDiagnosticError::InvalidBoundary);

    if (resolved->get().runtime_registered) {
        if (request.materializer == nullptr)
            reject(DispatchDiagnosticError::StaleBlock);
        if (!request.materializer->validate_for_dispatch(cpu, *block, target, physical)) {
            const auto stale_identity = stable_runtime_block_identity(resolved->get());
            if (!table.erase_identity(stale_identity))
                reject(materialization_error(request.materializer->last_failure()));
            invalidated = true;
            request.materializer->reconcile_inactive_origins(request.metrics);

            const auto materializations_before =
                request.materializer->metrics().materializations;
            block = request.materializer->try_materialize(
                cpu, target, physical, effective_variant, request.callsite);
            alias_lookup = false;
            materialized =
                block.has_value() &&
                request.materializer->metrics().materializations != materializations_before;
            if (!block)
                reject(materialization_error(request.materializer->last_failure()));
            resolved = table.resolve(*block);
            if (!valid_boundary()) reject(DispatchDiagnosticError::InvalidBoundary);
            if (!request.materializer->validate_for_dispatch(cpu, *block, target, physical)) {
                if (resolved) {
                    static_cast<void>(
                        table.erase_identity(stable_runtime_block_identity(resolved->get())));
                    request.materializer->reconcile_inactive_origins(request.metrics);
                }
                reject(materialization_error(request.materializer->last_failure()));
            }
        }
    }

    if (request.kind == IndirectDispatchKind::Call && !architectural_target_fetch_fault) {
        cpu.pr = request.return_address;
    }
    cpu.pc = alias_lookup ? resolved->get().virtual_start : target;
    if (request.metrics != nullptr)
        request.metrics->record_hit(request.dispatch_class, request.callsite, target, materialized);
    const bool plain_runtime_hit =
        request.dispatch_class == RuntimeDispatchClass::RuntimeOnly &&
        request.kind == IndirectDispatchKind::TailJump && !materialized &&
        !variant_materialized && !invalidated && !alias_lookup &&
        !architectural_target_fetch_fault;
    if (!plain_runtime_hit) diagnose(request, target, cpu.pr, alias_lookup, true);
    std::optional<BlockDispatchGenerationGuard> generation_guard;
    if (request.materializer != nullptr) {
        generation_guard = request.materializer->capture_dispatch_generation_guard(
            cpu, *block, resolved->get().runtime_registered);
    }
    const auto execution =
        make_validated_execution(*block, resolved->get(), generation_guard);
    if (!architectural_target_fetch_fault && !alias_lookup &&
        resolved->get().virtual_start == target && request.materializer != nullptr) {
        if (generation_guard) {
            auto& cached = dispatch_inline_cache[
                dispatch_inline_cache_index(request.callsite, request.kind)];
            cached.table = &table;
            cached.materializer = request.materializer;
            cached.callsite = request.callsite;
            cached.target = target;
            cached.physical_target = physical;
            cached.kind = request.kind;
            cached.dispatch_class = request.dispatch_class;
            cached.effective_variant = effective_variant;
            cached.execution = execution;
            cached.direct_p1_p2 = direct_p1_p2_target;
            cached.valid = true;
        }
    }
    return {*block,
            execution,
            target,
            physical,
            cpu.pc,
            cpu.pr,
            alias_lookup,
            materialized,
            {}};
}

} // namespace katana::runtime
