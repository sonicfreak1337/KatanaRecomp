#include "katana/runtime/indirect_dispatch.hpp"

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

void IndirectDispatchMetrics::record_hit(const RuntimeDispatchClass dispatch_class) noexcept {
    increment(hits_);
    if (dispatch_class == RuntimeDispatchClass::RuntimeOnly) increment(runtime_only_hits_);
}

void IndirectDispatchMetrics::record_miss(const RuntimeDispatchClass dispatch_class,
                                          const DispatchDiagnosticError error,
                                          const std::uint32_t callsite,
                                          const std::uint32_t target) noexcept {
    increment(misses_);
    if (dispatch_class == RuntimeDispatchClass::RuntimeOnly) increment(runtime_only_misses_);
    if (!first_error_.has_value())
        first_error_ = IndirectDispatchFirstError{error, dispatch_class, callsite, target};
}

void IndirectDispatchMetrics::record_fallback(const RuntimeDispatchClass dispatch_class) noexcept {
    increment(fallbacks_);
    if (dispatch_class == RuntimeDispatchClass::RuntimeOnly) increment(runtime_only_fallbacks_);
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
const std::optional<IndirectDispatchFirstError>&
IndirectDispatchMetrics::first_error() const noexcept {
    return first_error_;
}

std::string IndirectDispatchMetrics::serialize_json() const {
    std::ostringstream output;
    output << "{\"schema\":\"katana-indirect-dispatch-v1\",\"report_version\":1"
           << ",\"report_type\":\"indirect-dispatch\",\"status\":\"success\",\"hits\":" << hits_
           << ",\"misses\":" << misses_ << ",\"fallbacks\":" << fallbacks_
           << ",\"runtime_only_hits\":" << runtime_only_hits_
           << ",\"runtime_only_misses\":" << runtime_only_misses_
           << ",\"runtime_only_fallbacks\":" << runtime_only_fallbacks_ << ",\"first_error\":";
    if (!first_error_) {
        output << "null";
    } else {
        output << "{\"error\":\"" << dispatch_diagnostic_error_name(first_error_->error)
               << "\",\"class\":\"" << runtime_dispatch_class_name(first_error_->dispatch_class)
               << "\",\"callsite\":\"0x" << std::hex << std::uppercase << std::setw(8)
               << std::setfill('0') << first_error_->callsite << "\",\"target\":\"0x"
               << std::setw(8) << first_error_->target << "\"}";
    }
    output << '}';
    return output.str();
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
    const auto target = request.kind == IndirectDispatchKind::Return ? cpu.pr : request.target;
    const auto physical = canonical_physical_address(target);
    const auto reject = [&](const DispatchDiagnosticError error) {
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
    if ((target & 1u) != 0u) reject(DispatchDiagnosticError::Misaligned);
    auto block = table.lookup(target, request.variant);
    bool alias_lookup = false;
    if (!block) {
        block = table.lookup_physical(physical, request.variant);
        alias_lookup = block.has_value();
    }
    if (!block) reject(DispatchDiagnosticError::UnknownTarget);
    const auto resolved = table.resolve(*block);
    if (!resolved || resolved->get().function == nullptr || resolved->get().size < 2u ||
        (resolved->get().virtual_start & 1u) != 0u ||
        (alias_lookup ? resolved->get().physical_origin != physical
                      : resolved->get().virtual_start != target))
        reject(DispatchDiagnosticError::InvalidBoundary);

    if (request.kind == IndirectDispatchKind::Call) {
        cpu.pr = request.return_address;
    }
    cpu.pc = target;
    if (request.metrics != nullptr) request.metrics->record_hit(request.dispatch_class);
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
