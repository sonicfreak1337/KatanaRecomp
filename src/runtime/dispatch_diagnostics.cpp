#include "katana/runtime/dispatch_diagnostics.hpp"

#include "katana/runtime/block_table.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace katana::runtime {
namespace {

bool same_event(const DispatchDiagnosticEvent& left,
                const DispatchDiagnosticEvent& right) noexcept {
    return left.callsite == right.callsite && left.source_virtual == right.source_virtual &&
           left.source_physical == right.source_physical &&
           left.virtual_target == right.virtual_target &&
           left.canonical_target == right.canonical_target && left.pr == right.pr &&
           left.block_end == right.block_end && left.origin == right.origin &&
           left.alias_origin == right.alias_origin &&
           left.fallback_reason == right.fallback_reason &&
           left.fallback_action == right.fallback_action &&
           left.guest_instructions == right.guest_instructions && left.exit_pc == right.exit_pc &&
           left.error == right.error;
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

void optional_address(std::ostringstream& output, const std::optional<std::uint32_t> value) {
    value.has_value() ? output << '"' << hex32(*value) << '"' : output << "null";
}

const char* block_end_name(const BlockEndKind kind) noexcept {
    switch (kind) {
    case BlockEndKind::Fallthrough:
        return "fallthrough";
    case BlockEndKind::StaticBranch:
        return "static-branch";
    case BlockEndKind::ConditionalBranch:
        return "conditional-branch";
    case BlockEndKind::DynamicBranch:
        return "dynamic-branch";
    case BlockEndKind::Call:
        return "call";
    case BlockEndKind::Return:
        return "return";
    case BlockEndKind::ExceptionReturn:
        return "exception-return";
    case BlockEndKind::Sleep:
        return "sleep";
    case BlockEndKind::Exception:
        return "exception";
    case BlockEndKind::InterruptSafepoint:
        return "interrupt-safepoint";
    }
    return "unknown";
}

} // namespace

DispatchDiagnosticRecorder::DispatchDiagnosticRecorder(const std::size_t capacity)
    : capacity_(capacity) {
    if (capacity_ == 0u) {
        throw std::invalid_argument("Dispatchdiagnostik braucht eine positive Kapazitaet.");
    }
    events_.reserve(capacity_);
}

void DispatchDiagnosticRecorder::record(DispatchDiagnosticEvent event) {
    if (event.occurrences != 1u ||
        canonical_physical_address(event.source_physical) != event.source_physical ||
        event.virtual_target.has_value() != event.canonical_target.has_value() ||
        (event.virtual_target.has_value() &&
         canonical_physical_address(*event.virtual_target) != *event.canonical_target)) {
        throw std::invalid_argument("Dispatchdiagnose enthaelt einen unvollstaendigen Kontext.");
    }
    if (total_occurrences_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Dispatchdiagnosezaehler ist uebergelaufen.");
    }
    const auto duplicate =
        std::find_if(events_.begin(), events_.end(), [&event](const auto& current) {
            return same_event(current, event);
        });
    if (duplicate != events_.end() &&
        duplicate->occurrences == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Dispatchereigniszaehler ist uebergelaufen.");
    }
    if (duplicate != events_.end()) {
        ++duplicate->occurrences;
        ++total_occurrences_;
    } else {
        if (events_.size() < capacity_) {
            events_.push_back(std::move(event));
        } else if (dropped_unique_events_ != std::numeric_limits<std::uint64_t>::max()) {
            ++dropped_unique_events_;
        }
        ++total_occurrences_;
    }
}

bool DispatchDiagnosticRecorder::try_record(DispatchDiagnosticEvent event) noexcept {
    try {
        record(std::move(event));
        return true;
    } catch (...) {
        return false;
    }
}

void DispatchDiagnosticRecorder::clear() noexcept {
    events_.clear();
    total_occurrences_ = 0u;
    dropped_unique_events_ = 0u;
}

const std::vector<DispatchDiagnosticEvent>& DispatchDiagnosticRecorder::events() const noexcept {
    return events_;
}

std::uint64_t DispatchDiagnosticRecorder::total_occurrences() const noexcept {
    return total_occurrences_;
}

std::uint64_t DispatchDiagnosticRecorder::dropped_unique_events() const noexcept {
    return dropped_unique_events_;
}

std::size_t DispatchDiagnosticRecorder::capacity() const noexcept {
    return capacity_;
}

std::string DispatchDiagnosticRecorder::serialize_json() const {
    std::ostringstream output;
    output << "{\"schema\":\"katana-dispatch-diagnostic\",\"report_version\":1"
           << ",\"diagnostic_version\":" << dispatch_diagnostic_schema_version
           << ",\"status\":\"success\",\"total_occurrences\":" << total_occurrences_
           << ",\"unique_events\":" << events_.size()
           << ",\"dropped_unique_events\":" << dropped_unique_events_ << ",\"events\":[";
    for (std::size_t index = 0u; index < events_.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& event = events_[index];
        output << "{\"callsite\":\"" << hex32(event.callsite) << "\",\"source_virtual\":\""
               << hex32(event.source_virtual) << "\",\"source_physical\":\""
               << hex32(event.source_physical) << "\",\"virtual_target\":";
        optional_address(output, event.virtual_target);
        output << ",\"canonical_target\":";
        optional_address(output, event.canonical_target);
        output << ",\"pr\":\"" << hex32(event.pr) << "\",\"block_end\":\""
               << block_end_name(event.block_end) << "\",\"origin\":\""
               << dispatch_resolution_origin_name(event.origin) << "\",\"alias_origin\":\""
               << dispatch_alias_origin_name(event.alias_origin) << "\",\"fallback_reason\":\""
               << dispatch_fallback_reason_name(event.fallback_reason)
               << "\",\"fallback_action\":\""
               << dispatch_fallback_action_name(event.fallback_action)
               << "\",\"guest_instructions\":" << event.guest_instructions << ",\"exit_pc\":\""
               << hex32(event.exit_pc) << "\",\"error\":\""
               << dispatch_diagnostic_error_name(event.error)
               << "\",\"occurrences\":" << event.occurrences << '}';
    }
    output << "]}";
    return output.str();
}

const char* dispatch_resolution_origin_name(const DispatchResolutionOrigin value) noexcept {
    switch (value) {
    case DispatchResolutionOrigin::StaticProof:
        return "static-proof";
    case DispatchResolutionOrigin::Override:
        return "override";
    case DispatchResolutionOrigin::TableLookup:
        return "table-lookup";
    case DispatchResolutionOrigin::RuntimeOnly:
        return "runtime-only";
    case DispatchResolutionOrigin::InlineCache:
        return "inline-cache";
    case DispatchResolutionOrigin::Fallback:
        return "fallback";
    }
    return "unknown";
}

const char* dispatch_alias_origin_name(const DispatchAliasOrigin value) noexcept {
    switch (value) {
    case DispatchAliasOrigin::None:
        return "none";
    case DispatchAliasOrigin::ExactVirtual:
        return "exact-virtual";
    case DispatchAliasOrigin::CanonicalPhysical:
        return "canonical-physical";
    }
    return "unknown";
}

const char* dispatch_fallback_reason_name(const DispatchFallbackReason value) noexcept {
    switch (value) {
    case DispatchFallbackReason::None:
        return "none";
    case DispatchFallbackReason::UnknownOpcode:
        return "unknown-opcode";
    case DispatchFallbackReason::UnresolvedControlFlow:
        return "unresolved-control-flow";
    case DispatchFallbackReason::DynamicCode:
        return "dynamic-code";
    case DispatchFallbackReason::ManifestDenied:
        return "manifest-denied";
    }
    return "unknown";
}

const char* dispatch_fallback_action_name(const DispatchFallbackAction value) noexcept {
    switch (value) {
    case DispatchFallbackAction::None:
        return "none";
    case DispatchFallbackAction::Abort:
        return "abort";
    case DispatchFallbackAction::Diagnose:
        return "diagnose";
    case DispatchFallbackAction::Interpreter:
        return "interpreter";
    case DispatchFallbackAction::UserHook:
        return "user-hook";
    }
    return "unknown";
}

const char* dispatch_diagnostic_error_name(const DispatchDiagnosticError value) noexcept {
    switch (value) {
    case DispatchDiagnosticError::None:
        return "none";
    case DispatchDiagnosticError::UnknownCode:
        return "unknown-code";
    case DispatchDiagnosticError::UnknownTarget:
        return "unknown-target";
    case DispatchDiagnosticError::UnmappedMemory:
        return "unmapped-memory";
    case DispatchDiagnosticError::FirmwareDenied:
        return "firmware-denied";
    case DispatchDiagnosticError::Misaligned:
        return "misaligned";
    case DispatchDiagnosticError::InvalidBoundary:
        return "invalid-boundary";
    case DispatchDiagnosticError::PermissionDenied:
        return "permission-denied";
    case DispatchDiagnosticError::ProvenNonCode:
        return "proven-non-code";
    case DispatchDiagnosticError::MaterializationBudget:
        return "materialization-budget";
    case DispatchDiagnosticError::ByteIdentityMismatch:
        return "byte-identity-mismatch";
    case DispatchDiagnosticError::GenerationMismatch:
        return "generation-mismatch";
    case DispatchDiagnosticError::RelocationMismatch:
        return "relocation-mismatch";
    case DispatchDiagnosticError::StaleBlock:
        return "stale-block";
    }
    return "unknown";
}

} // namespace katana::runtime
