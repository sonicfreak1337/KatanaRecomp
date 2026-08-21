#include "katana/agent/materialization_world.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace katana::agent {
namespace {

constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ull;
constexpr std::uint64_t fnv_prime = 1099511628211ull;
constexpr std::uint64_t evidence_digest_seed = 0xA0761D6478BD642Full;

[[nodiscard]] constexpr std::uint64_t hash_byte(std::uint64_t hash,
                                                 const std::uint8_t byte) noexcept {
    return (hash ^ byte) * fnv_prime;
}

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30u;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27u;
    value *= 0x94D049BB133111EBull;
    return value ^ (value >> 31u);
}

[[nodiscard]] std::uint64_t digest_ids(
    const std::vector<StableId>& ids) noexcept {
    if (ids.empty()) return 0u;
    std::uint64_t digest = 0xD6E8FEB86659FD93ull;
    for (const auto id : ids) digest ^= mix64(id.value + 0x9E3779B97F4A7C15ull);
    return digest == 0u ? 1u : digest;
}

[[nodiscard]] constexpr std::uint64_t evidence_digest_component(
    const EvidenceRecord& item) noexcept {
    const auto component = item.id.value ^
                           (static_cast<std::uint64_t>(item.kind) << 56u) ^
                           (item.immutable ? 0xF00DCAFEu : 0u);
    return mix64(component + 0x517CC1B727220A95ull);
}

[[nodiscard]] constexpr std::uint64_t canonical_evidence_digest(
    const std::uint64_t accumulator,
    const bool nonempty) noexcept {
    if (!nonempty) return 0u;
    return accumulator == 0u ? 1u : accumulator;
}

[[nodiscard]] bool valid_text(const std::string& value) noexcept {
    return value.size() <= materialization_world_max_text_bytes;
}

template <typename T>
[[nodiscard]] bool valid_text_vector(const std::vector<T>& values,
                                     const std::size_t limit) noexcept {
    if (values.size() > limit) return false;
    for (const auto& value : values) {
        if (!valid_text(value)) return false;
    }
    return true;
}

[[nodiscard]] bool runtime_only_evidence(const EvidenceKind kind) noexcept {
    return kind == EvidenceKind::RuntimeObservation ||
           kind == EvidenceKind::UserHint;
}

[[nodiscard]] bool valid_node_kind_value(
    const MaterializationNodeKind kind) noexcept {
    return static_cast<std::uint8_t>(kind) <=
           static_cast<std::uint8_t>(MaterializationNodeKind::Unknown);
}

[[nodiscard]] bool valid_proof_value(const ProofClass proof) noexcept {
    return static_cast<std::uint8_t>(proof) <=
           static_cast<std::uint8_t>(ProofClass::Rejected);
}

[[nodiscard]] bool valid_completeness_value(
    const Completeness completeness) noexcept {
    return static_cast<std::uint8_t>(completeness) <=
           static_cast<std::uint8_t>(Completeness::Rejected);
}

[[nodiscard]] bool valid_evidence_kind_value(const EvidenceKind kind) noexcept {
    return static_cast<std::uint8_t>(kind) <=
           static_cast<std::uint8_t>(EvidenceKind::UserHint);
}

[[nodiscard]] bool valid_dependency_kind_value(
    const DependencyKind kind) noexcept {
    return static_cast<std::uint8_t>(kind) <=
           static_cast<std::uint8_t>(DependencyKind::Observes);
}

[[nodiscard]] bool valid_frontier_state_value(
    const FrontierState state) noexcept {
    return static_cast<std::uint8_t>(state) <=
           static_cast<std::uint8_t>(FrontierState::ObservedHint);
}

[[nodiscard]] bool valid_frontier_proof_value(
    const FrontierProof proof) noexcept {
    return static_cast<std::uint8_t>(proof) <=
           static_cast<std::uint8_t>(FrontierProof::ExplicitRejection);
}

[[nodiscard]] bool valid_frontier_severity_value(
    const FrontierSeverity severity) noexcept {
    return static_cast<std::uint8_t>(severity) <=
           static_cast<std::uint8_t>(FrontierSeverity::P3);
}

[[nodiscard]] bool valid_frontier_block_value(
    const FrontierBlockKind kind) noexcept {
    return static_cast<std::uint8_t>(kind) <=
           static_cast<std::uint8_t>(FrontierBlockKind::Unknown);
}

[[nodiscard]] bool unique_id_vector(
    const std::vector<StableId>& ids) noexcept {
    for (std::size_t index = 0u; index < ids.size(); ++index) {
        for (std::size_t other = index + 1u; other < ids.size(); ++other) {
            if (ids[index] == ids[other]) return false;
        }
    }
    return true;
}

[[nodiscard]] bool unique_reverse_dependency_vector(
    const std::vector<ReverseDependency>& values) {
    std::vector<std::tuple<StableId, std::uint8_t>> keys;
    keys.reserve(values.size());
    for (const auto& value : values) {
        if (!value.from || !valid_dependency_kind_value(value.kind))
            return false;
        keys.emplace_back(value.from,
                          static_cast<std::uint8_t>(value.kind));
    }
    std::sort(keys.begin(), keys.end());
    return std::adjacent_find(keys.begin(), keys.end()) == keys.end();
}

[[nodiscard]] std::uint64_t hash_key(const EntityKind kind,
                                     const std::string_view key) noexcept {
    std::uint64_t hash = fnv_offset_basis;
    hash = hash_byte(hash, static_cast<std::uint8_t>(kind));
    hash = hash_byte(hash, 0u);
    for (const auto byte : key) {
        hash = hash_byte(hash, static_cast<std::uint8_t>(byte));
    }
    // Zero is reserved as the invalid/sentinel ID. The second mix also makes
    // the entity namespace part of the value even for very short keys.
    hash ^= (static_cast<std::uint64_t>(kind) + 1u) * 0x9E3779B97F4A7C15ull;
    hash *= fnv_prime;
    return hash == 0u ? 1u : hash;
}

[[nodiscard]] bool has_id(const std::vector<StableId>& ids,
                          const StableId id) noexcept {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

[[nodiscard]] bool reverse_dependency_less(const ReverseDependency& lhs,
                                            const ReverseDependency& rhs) noexcept {
    if (lhs.from != rhs.from) return lhs.from < rhs.from;
    return static_cast<std::uint8_t>(lhs.kind) <
           static_cast<std::uint8_t>(rhs.kind);
}

[[nodiscard]] bool valid_node_proof(
    const MaterializationNode& node,
    const bool check_reverse_dependencies = true) {
    if (!valid_node_kind_value(node.kind) || !valid_proof_value(node.proof) ||
        !valid_completeness_value(node.completeness) ||
        node.canonical_identity.empty() ||
        !valid_text(node.canonical_identity) || !valid_text(node.source_identity))
        return false;
    if (node.evidence.size() > materialization_world_max_evidence_per_item ||
        node.reverse_dependencies.size() > materialization_world_max_edges ||
        !unique_id_vector(node.evidence) ||
        (check_reverse_dependencies &&
         !unique_reverse_dependency_vector(node.reverse_dependencies)))
        return false;
    if (node.evidence_digest != 0u && node.evidence_digest != digest_ids(node.evidence))
        return false;
    if (node.runtime_hint_only && is_static_proof(node.proof)) return false;
    if (node.proof == ProofClass::Rejected)
        return node.completeness == Completeness::Rejected;
    if (node.completeness == Completeness::Complete &&
        !is_static_proof(node.proof))
        return false;
    if (node.runtime_hint_only && node.completeness == Completeness::Complete)
        return false;
    return true;
}

[[nodiscard]] bool valid_frontier_entry(const FrontierEntry& entry) noexcept {
    if (!valid_frontier_state_value(entry.state) ||
        !valid_frontier_proof_value(entry.proof) ||
        !valid_frontier_severity_value(entry.severity) ||
        !valid_frontier_block_value(entry.blocked_kind) ||
        entry.family.empty() || entry.owner.empty() || entry.site.empty() ||
        !valid_text(entry.family) || !valid_text(entry.owner) ||
        !valid_text(entry.site) || !valid_text(entry.missing_proof))
        return false;
    if (entry.evidence.size() > materialization_world_max_evidence_per_item ||
        entry.contracts.size() > materialization_world_max_contracts ||
        !unique_id_vector(entry.evidence))
        return false;
    if (entry.evidence_digest != 0u && entry.evidence_digest != digest_ids(entry.evidence))
        return false;
    if (!valid_text_vector(entry.contracts, materialization_world_max_contracts) ||
        !valid_text_vector(entry.blocked_sites, materialization_world_max_blocked_items) ||
        !valid_text_vector(entry.blocked_functions, materialization_world_max_blocked_items) ||
        !valid_text_vector(entry.blocked_roots, materialization_world_max_blocked_items) ||
        !valid_text_vector(entry.blocked_modules, materialization_world_max_blocked_items) ||
        !valid_text_vector(entry.blocked_materializations,
                           materialization_world_max_blocked_items) ||
        !valid_text_vector(entry.blocked_hardware, materialization_world_max_blocked_items) ||
        !valid_text_vector(entry.causal_chain, materialization_world_max_causal_items) ||
        !valid_text_vector(entry.source_paths, materialization_world_max_source_items) ||
        !valid_text_vector(entry.source_symbols, materialization_world_max_source_items) ||
        !valid_text_vector(entry.invariants, materialization_world_max_invariants) ||
        !valid_text_vector(entry.acceptance_criteria,
                           materialization_world_max_acceptance_criteria))
        return false;

    const bool runtime_proof = entry.proof == FrontierProof::RuntimeObservation;
    const bool observed_hint = entry.state == FrontierState::ObservedHint;
    const bool static_proof = is_static_frontier_proof(entry.proof);
    if (runtime_proof != observed_hint)
        return false;
    if (observed_hint &&
        (!entry.runtime_evidence_required || entry.static_complete))
        return false;
    if (entry.state == FrontierState::Closed &&
        (!entry.static_complete || !static_proof || entry.runtime_evidence_required))
        return false;
    if (entry.static_complete &&
        (!static_proof || entry.runtime_evidence_required ||
         entry.state != FrontierState::Closed))
        return false;
    if (entry.proof == FrontierProof::ExplicitRejection &&
        entry.state != FrontierState::Blocked)
        return false;
    return true;
}

[[nodiscard]] int frontier_state_rank(const FrontierState state) noexcept {
    switch (state) {
    case FrontierState::Open:
        return 0;
    case FrontierState::Blocked:
        return 1;
    case FrontierState::Candidate:
        return 2;
    case FrontierState::ObservedHint:
        return 3;
    case FrontierState::Closed:
        return 4;
    }
    return 5;
}

[[nodiscard]] bool frontier_has_concrete_task_evidence(
    const FrontierEntry& entry) noexcept {
    return !entry.source_paths.empty() || !entry.source_symbols.empty() ||
           !entry.blocked_sites.empty() || !entry.blocked_functions.empty() ||
           !entry.blocked_roots.empty() || !entry.blocked_modules.empty() ||
           !entry.blocked_hardware.empty() || !entry.causal_chain.empty();
}

[[nodiscard]] bool frontier_is_aggregate_task(
    const FrontierEntry& entry) noexcept {
    return !frontier_has_concrete_task_evidence(entry) &&
           entry.blocked_kind == FrontierBlockKind::Materialization &&
           !entry.blocked_materializations.empty();
}

[[nodiscard]] bool frontier_is_native_provider_input_read(
    const FrontierEntry& entry) noexcept {
    return std::ranges::find(
               entry.contracts,
               "task-role:native-provider-input-read") !=
           entry.contracts.end();
}

[[nodiscard]] bool frontier_task_priority_less(
    const FrontierEntry& lhs,
    const FrontierEntry& rhs) noexcept {
    const bool lhs_aggregate = frontier_is_aggregate_task(lhs);
    const bool rhs_aggregate = frontier_is_aggregate_task(rhs);
    if (lhs_aggregate != rhs_aggregate) return !lhs_aggregate;
    if (lhs.severity != rhs.severity)
        return static_cast<std::uint8_t>(lhs.severity) <
               static_cast<std::uint8_t>(rhs.severity);
    const int lhs_state = frontier_state_rank(lhs.state);
    const int rhs_state = frontier_state_rank(rhs.state);
    if (lhs_state != rhs_state) return lhs_state < rhs_state;
    const bool lhs_provider_read =
        frontier_is_native_provider_input_read(lhs);
    const bool rhs_provider_read =
        frontier_is_native_provider_input_read(rhs);
    if (lhs_provider_read != rhs_provider_read) return lhs_provider_read;
    if (lhs.runtime_evidence_required != rhs.runtime_evidence_required)
        return lhs.runtime_evidence_required < rhs.runtime_evidence_required;
    if (lhs.fanout != rhs.fanout) return lhs.fanout > rhs.fanout;
    return lhs.id < rhs.id;
}

struct JsonWriter final {
    std::span<char> output;
    std::size_t position = 0u;
    bool overflow = false;

    void put(const char value) noexcept {
        if (overflow) return;
        if (position >= output.size()) {
            overflow = true;
            return;
        }
        output[position++] = value;
    }

    void raw(const std::string_view value) noexcept {
        if (overflow) return;
        if (value.size() > output.size() - position) {
            overflow = true;
            return;
        }
        std::copy(value.begin(), value.end(), output.begin() + position);
        position += value.size();
    }

    void string(const std::string_view value) noexcept {
        put('"');
        for (const unsigned char byte : value) {
            if (overflow) return;
            switch (byte) {
            case '"':
                raw("\\\"");
                break;
            case '\\':
                raw("\\\\");
                break;
            case '\b':
                raw("\\b");
                break;
            case '\f':
                raw("\\f");
                break;
            case '\n':
                raw("\\n");
                break;
            case '\r':
                raw("\\r");
                break;
            case '\t':
                raw("\\t");
                break;
            default:
                if (byte < 0x20u) {
                    constexpr char digits[] = "0123456789abcdef";
                    put('\\');
                    put('u');
                    put('0');
                    put('0');
                    put(digits[(byte >> 4u) & 0x0Fu]);
                    put(digits[byte & 0x0Fu]);
                } else {
                    put(static_cast<char>(byte));
                }
                break;
            }
        }
        put('"');
    }

    void boolean(const bool value) noexcept { raw(value ? "true" : "false"); }

    void number(std::uint64_t value) noexcept {
        char buffer[32]{};
        std::size_t length = 0u;
        do {
            buffer[length++] = static_cast<char>('0' + (value % 10u));
            value /= 10u;
        } while (value != 0u && length < sizeof(buffer));
        while (length != 0u) put(buffer[--length]);
    }

    void id(const StableId value) noexcept { number(value.value); }
};

void write_id_array(JsonWriter& writer, const std::vector<StableId>& ids) noexcept {
    writer.put('[');
    for (std::size_t index = 0u; index < ids.size(); ++index) {
        if (index != 0u) writer.put(',');
        writer.id(ids[index]);
    }
    writer.put(']');
}

void write_string_array(JsonWriter& writer,
                        const std::vector<std::string>& values) noexcept {
    writer.put('[');
    for (std::size_t index = 0u; index < values.size(); ++index) {
        if (index != 0u) writer.put(',');
        writer.string(values[index]);
    }
    writer.put(']');
}

template <typename ReverseWriter>
void write_node(JsonWriter& writer,
                const MaterializationNode& node,
                ReverseWriter&& reverse_writer) noexcept {
    writer.raw("{\"id\":");
    writer.id(node.id);
    writer.raw(",\"kind\":");
    writer.string(materialization_node_kind_name(node.kind));
    writer.raw(",\"identity\":");
    writer.string(node.canonical_identity);
    writer.raw(",\"source\":");
    writer.string(node.source_identity);
    writer.raw(",\"proof\":");
    writer.string(proof_class_name(node.proof));
    writer.raw(",\"completeness\":");
    writer.string(completeness_name(node.completeness));
    writer.raw(",\"runtime_hint_only\":");
    writer.boolean(node.runtime_hint_only);
    writer.raw(",\"dependency_generation\":");
    writer.number(node.dependency_generation);
    writer.raw(",\"evidence_digest\":");
    writer.number(node.evidence_digest);
    writer.raw(",\"evidence\":");
    write_id_array(writer, node.evidence);
    writer.raw(",\"reverse_dependencies\":[");
    reverse_writer(writer, node);
    writer.raw("]}");
}

void write_frontier(JsonWriter& writer, const FrontierEntry& entry) noexcept {
    writer.raw("{\"id\":");
    writer.id(entry.id);
    writer.raw(",\"family\":");
    writer.string(entry.family);
    writer.raw(",\"owner\":");
    writer.string(entry.owner);
    writer.raw(",\"site\":");
    writer.string(entry.site);
    writer.raw(",\"state\":");
    writer.string(frontier_state_name(entry.state));
    writer.raw(",\"proof\":");
    writer.string(frontier_proof_name(entry.proof));
    writer.raw(",\"severity\":");
    writer.string(frontier_severity_name(entry.severity));
    writer.raw(",\"missing_proof\":");
    writer.string(entry.missing_proof);
    writer.raw(",\"fanout\":");
    writer.number(entry.fanout);
    writer.raw(",\"runtime_evidence_required\":");
    writer.boolean(entry.runtime_evidence_required);
    writer.raw(",\"static_complete\":");
    writer.boolean(entry.static_complete);
    writer.raw(",\"blocked_kind\":");
    writer.string(frontier_block_kind_name(entry.blocked_kind));
    writer.raw(",\"dependency_generation\":");
    writer.number(entry.dependency_generation);
    writer.raw(",\"evidence_digest\":");
    writer.number(entry.evidence_digest);
    writer.raw(",\"evidence\":");
    write_id_array(writer, entry.evidence);
    writer.raw(",\"contracts\":[");
    for (std::size_t index = 0u; index < entry.contracts.size(); ++index) {
        if (index != 0u) writer.put(',');
        writer.string(entry.contracts[index]);
    }
    writer.raw("],\"blocked_sites\":");
    write_string_array(writer, entry.blocked_sites);
    writer.raw(",\"blocked_functions\":");
    write_string_array(writer, entry.blocked_functions);
    writer.raw(",\"blocked_roots\":");
    write_string_array(writer, entry.blocked_roots);
    writer.raw(",\"blocked_modules\":");
    write_string_array(writer, entry.blocked_modules);
    writer.raw(",\"blocked_materializations\":");
    write_string_array(writer, entry.blocked_materializations);
    writer.raw(",\"blocked_hardware\":");
    write_string_array(writer, entry.blocked_hardware);
    writer.raw(",\"causal_chain\":");
    write_string_array(writer, entry.causal_chain);
    writer.raw(",\"source_paths\":");
    write_string_array(writer, entry.source_paths);
    writer.raw(",\"source_symbols\":");
    write_string_array(writer, entry.source_symbols);
    writer.raw(",\"invariants\":");
    write_string_array(writer, entry.invariants);
    writer.raw(",\"acceptance_criteria\":");
    write_string_array(writer, entry.acceptance_criteria);
    writer.put('}');
}

} // namespace

StableId stable_id(const EntityKind kind,
                   const std::string_view canonical_key) noexcept {
    if (canonical_key.empty()) return {};
    return StableId{hash_key(kind, canonical_key)};
}

StableId frontier_id(const std::string_view family,
                     const std::string_view owner,
                     const std::string_view site) noexcept {
    if (family.empty() || owner.empty() || site.empty()) return {};
    std::uint64_t hash = fnv_offset_basis;
    hash = hash_byte(hash, static_cast<std::uint8_t>(EntityKind::Frontier));
    for (const auto part : {family, owner, site}) {
        for (const auto byte : part) hash = hash_byte(hash, static_cast<std::uint8_t>(byte));
        hash = hash_byte(hash, 0u);
    }
    hash ^= 0x9E3779B97F4A7C15ull;
    hash *= fnv_prime;
    return StableId{hash == 0u ? 1u : hash};
}

const char* materialization_node_kind_name(const MaterializationNodeKind kind) noexcept {
    switch (kind) {
    case MaterializationNodeKind::Image:
        return "image";
    case MaterializationNodeKind::Module:
        return "module";
    case MaterializationNodeKind::Function:
        return "function";
    case MaterializationNodeKind::Block:
        return "block";
    case MaterializationNodeKind::Provider:
        return "provider";
    case MaterializationNodeKind::Hook:
        return "hook";
    case MaterializationNodeKind::HardwareOwner:
        return "hardware-owner";
    case MaterializationNodeKind::RenderFamily:
        return "render-family";
    case MaterializationNodeKind::AnalysisRoot:
        return "analysis-root";
    case MaterializationNodeKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

const char* proof_class_name(const ProofClass proof) noexcept {
    switch (proof) {
    case ProofClass::UnknownMaterialization:
        return "unknown-materialization";
    case ProofClass::GuardedPartial:
        return "guarded-partial";
    case ProofClass::GuardedComplete:
        return "guarded-complete";
    case ProofClass::ProvenExact:
        return "proven-exact";
    case ProofClass::NativeReplaced:
        return "native-replaced";
    case ProofClass::Rejected:
        return "rejected";
    }
    return "unknown";
}

const char* completeness_name(const Completeness completeness) noexcept {
    switch (completeness) {
    case Completeness::Unknown:
        return "unknown";
    case Completeness::Partial:
        return "partial";
    case Completeness::Complete:
        return "complete";
    case Completeness::Rejected:
        return "rejected";
    }
    return "unknown";
}

const char* evidence_kind_name(const EvidenceKind kind) noexcept {
    switch (kind) {
    case EvidenceKind::Disassembly:
        return "disassembly";
    case EvidenceKind::StaticAnalysis:
        return "static-analysis";
    case EvidenceKind::IdentityBinding:
        return "identity-binding";
    case EvidenceKind::SourceContract:
        return "source-contract";
    case EvidenceKind::RuntimeObservation:
        return "runtime-observation";
    case EvidenceKind::UserHint:
        return "user-hint";
    }
    return "unknown";
}

const char* dependency_kind_name(const DependencyKind kind) noexcept {
    switch (kind) {
    case DependencyKind::Calls:
        return "calls";
    case DependencyKind::Materializes:
        return "materializes";
    case DependencyKind::Replaces:
        return "replaces";
    case DependencyKind::Requires:
        return "requires";
    case DependencyKind::Produces:
        return "produces";
    case DependencyKind::Observes:
        return "observes";
    }
    return "unknown";
}

const char* frontier_block_kind_name(const FrontierBlockKind kind) noexcept {
    switch (kind) {
    case FrontierBlockKind::Site:
        return "site";
    case FrontierBlockKind::Function:
        return "function";
    case FrontierBlockKind::Root:
        return "root";
    case FrontierBlockKind::Module:
        return "module";
    case FrontierBlockKind::Materialization:
        return "materialization";
    case FrontierBlockKind::Hardware:
        return "hardware";
    case FrontierBlockKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

const char* frontier_state_name(const FrontierState state) noexcept {
    switch (state) {
    case FrontierState::Open:
        return "open";
    case FrontierState::Candidate:
        return "candidate";
    case FrontierState::Blocked:
        return "blocked";
    case FrontierState::Closed:
        return "closed";
    case FrontierState::ObservedHint:
        return "observed-hint";
    }
    return "open";
}

const char* frontier_proof_name(const FrontierProof proof) noexcept {
    switch (proof) {
    case FrontierProof::None:
        return "none";
    case FrontierProof::StaticDisassembly:
        return "static-disassembly";
    case FrontierProof::StaticAnalyzer:
        return "static-analyzer";
    case FrontierProof::IdentityBound:
        return "identity-bound";
    case FrontierProof::RuntimeObservation:
        return "runtime-observation";
    case FrontierProof::ExplicitRejection:
        return "explicit-rejection";
    }
    return "none";
}

const char* frontier_severity_name(const FrontierSeverity severity) noexcept {
    switch (severity) {
    case FrontierSeverity::P0:
        return "P0";
    case FrontierSeverity::P1:
        return "P1";
    case FrontierSeverity::P2:
        return "P2";
    case FrontierSeverity::P3:
        return "P3";
    }
    return "P3";
}

const char* agent_decision_kind_name(const AgentDecisionKind kind) noexcept {
    switch (kind) {
    case AgentDecisionKind::ContinueStaticIteration:
        return "continue_static_iteration";
    case AgentDecisionKind::RequiresRuntimeEvidence:
        return "requires_runtime_evidence";
    case AgentDecisionKind::BuildPort:
        return "build_port";
    }
    return "continue_static_iteration";
}

const char* world_model_error_name(const WorldModelError error) noexcept {
    switch (error) {
    case WorldModelError::None:
        return "none";
    case WorldModelError::CapacityExceeded:
        return "capacity-exceeded";
    case WorldModelError::InvalidIdentity:
        return "invalid-identity";
    case WorldModelError::InvalidStableId:
        return "invalid-stable-id";
    case WorldModelError::StableIdCollision:
        return "stable-id-collision";
    case WorldModelError::MissingReference:
        return "missing-reference";
    case WorldModelError::DuplicateEntry:
        return "duplicate-entry";
    case WorldModelError::InvalidProof:
        return "invalid-proof";
    case WorldModelError::InvalidCompleteness:
        return "invalid-completeness";
    case WorldModelError::InvalidFrontier:
        return "invalid-frontier";
    case WorldModelError::InvalidDependency:
        return "invalid-dependency";
    case WorldModelError::InvalidArtifact:
        return "invalid-artifact";
    case WorldModelError::ArtifactChecksumMismatch:
        return "artifact-checksum-mismatch";
    case WorldModelError::ArtifactSchemaMismatch:
        return "artifact-schema-mismatch";
    case WorldModelError::ArtifactTooLarge:
        return "artifact-too-large";
    }
    return "invalid";
}

ExecutableMaterializationWorld::DependencyIndexKey
ExecutableMaterializationWorld::dependency_index_key(
    const DependencyEdge& edge) noexcept {
    return std::make_tuple(edge.from,
                           edge.to,
                           static_cast<std::uint8_t>(edge.kind));
}

ExecutableMaterializationWorld::ReverseDependencyIndexKey
ExecutableMaterializationWorld::reverse_dependency_index_key(
    const StableId target,
    const ReverseDependency& dependency) noexcept {
    return std::make_tuple(target,
                           dependency.from,
                           static_cast<std::uint8_t>(dependency.kind));
}

ExecutableMaterializationWorld::FrontierOrderKey
ExecutableMaterializationWorld::frontier_order_key(
    const FrontierEntry& entry) noexcept {
    return std::make_tuple(
        static_cast<std::uint8_t>(entry.severity),
        static_cast<std::uint8_t>(frontier_state_rank(entry.state)),
        static_cast<std::uint8_t>(entry.runtime_evidence_required),
        0xFFFFFFFFu - entry.fanout,
        entry.id);
}

void ExecutableMaterializationWorld::synchronize_dependency_generation()
    const noexcept {
    if (!dependency_generation_dirty_) return;
    for (auto& node : nodes_)
        node.dependency_generation = dependency_generation_;
    for (auto& entry : frontier_)
        entry.dependency_generation = dependency_generation_;
    dependency_generation_dirty_ = false;
    validation_cached_ = false;
}

bool ExecutableMaterializationWorld::rebuild_indexes() noexcept {
    const auto clear_indexes = [&]() noexcept {
        evidence_index_.clear();
        node_index_.clear();
        frontier_index_.clear();
        dependency_index_.clear();
        reverse_dependency_index_.clear();
        frontier_order_index_.clear();
        evidence_digest_accumulator_ = 0u;
        dependency_generation_dirty_ = false;
        validation_cached_ = false;
    };
    try {
        clear_indexes();
        evidence_digest_accumulator_ = evidence_.empty()
                                          ? 0u
                                          : evidence_digest_seed;

        for (std::size_t index = 0u; index < evidence_.size(); ++index) {
            const auto& item = evidence_[index];
            if (!evidence_index_.emplace(item.id, index).second) {
                clear_indexes();
                return false;
            }
            evidence_digest_accumulator_ ^= evidence_digest_component(item);
        }
        for (std::size_t index = 0u; index < nodes_.size(); ++index) {
            const auto& node = nodes_[index];
            if (!node_index_.emplace(node.id, index).second) {
                clear_indexes();
                return false;
            }
        }
        for (std::size_t index = 0u; index < frontier_.size(); ++index) {
            const auto& entry = frontier_[index];
            if (!frontier_index_.emplace(entry.id, index).second ||
                !frontier_order_index_.emplace(frontier_order_key(entry), index)
                     .second) {
                clear_indexes();
                return false;
            }
        }
        for (std::size_t index = 0u; index < dependencies_.size(); ++index) {
            if (!dependency_index_.emplace(
                    dependency_index_key(dependencies_[index]), index)
                     .second) {
                clear_indexes();
                return false;
            }
        }
        for (const auto& node : nodes_) {
            for (std::size_t index = 0u;
                 index < node.reverse_dependencies.size();
                 ++index) {
                if (!reverse_dependency_index_.emplace(
                        reverse_dependency_index_key(
                            node.id, node.reverse_dependencies[index]),
                        index)
                         .second) {
                    clear_indexes();
                    return false;
                }
            }
        }
        dependency_generation_dirty_ = false;
        validation_cached_ = false;
        return true;
    } catch (...) {
        clear_indexes();
        return false;
    }
}

bool ExecutableMaterializationWorld::add_evidence(EvidenceRecord evidence) noexcept {
    try {
        if (!valid_evidence_kind_value(evidence.kind) ||
            evidence.canonical_identity.empty() ||
            !valid_text(evidence.canonical_identity)) {
            fail(WorldModelError::InvalidIdentity);
            return false;
        }
        const auto expected = stable_id(EntityKind::Evidence, evidence.canonical_identity);
        if (!expected) {
            fail(WorldModelError::InvalidIdentity);
            return false;
        }
        if (!evidence.id) evidence.id = expected;
        if (evidence.id != expected) {
            fail(WorldModelError::InvalidStableId);
            return false;
        }
        if (evidence_.size() >= materialization_world_max_evidence) {
            fail(WorldModelError::CapacityExceeded);
            return false;
        }
        const auto existing = evidence_index_.find(evidence.id);
        if (existing != evidence_index_.end()) {
            if (existing->second >= evidence_.size()) {
                fail(WorldModelError::InvalidArtifact);
                return false;
            }
            const auto& prior = evidence_[existing->second];
            fail(prior.canonical_identity == evidence.canonical_identity
                     ? WorldModelError::DuplicateEntry
                     : WorldModelError::StableIdCollision);
            return false;
        }
        evidence_.push_back(std::move(evidence));
        const auto index = evidence_.size() - 1u;
        try {
            if (!evidence_index_.emplace(evidence_[index].id, index).second) {
                evidence_.pop_back();
                fail(WorldModelError::DuplicateEntry);
                return false;
            }
        } catch (...) {
            evidence_.pop_back();
            fail(WorldModelError::CapacityExceeded);
            return false;
        }
        if (evidence_.size() == 1u) evidence_digest_accumulator_ = evidence_digest_seed;
        evidence_digest_accumulator_ ^= evidence_digest_component(evidence_.back());
        evidence_digest_ = canonical_evidence_digest(evidence_digest_accumulator_, true);
        validation_cached_ = false;
        last_error_ = WorldModelError::None;
        ++revision_;
        return true;
    } catch (...) {
        fail(WorldModelError::CapacityExceeded);
        return false;
    }
}

bool ExecutableMaterializationWorld::add_node(MaterializationNode node) noexcept {
    try {
        if (!valid_node_proof(node)) {
            fail(node.canonical_identity.empty() ? WorldModelError::InvalidIdentity
                                                  : WorldModelError::InvalidProof);
            return false;
        }
        const auto expected = stable_id(EntityKind::Node, node.canonical_identity);
        if (!expected) {
            fail(WorldModelError::InvalidIdentity);
            return false;
        }
        if (!node.id) node.id = expected;
        if (node.id != expected) {
            fail(WorldModelError::InvalidStableId);
            return false;
        }
        std::sort(node.evidence.begin(), node.evidence.end());
        if (node.dependency_generation == 0u)
            node.dependency_generation = dependency_generation_;
        if (node.evidence_digest == 0u)
            node.evidence_digest = digest_ids(node.evidence);
        bool has_runtime_evidence = false;
        for (const auto evidence_id : node.evidence) {
            const auto evidence_index = evidence_index_.find(evidence_id);
            if (evidence_index == evidence_index_.end() ||
                evidence_index->second >= evidence_.size()) {
                fail(WorldModelError::MissingReference);
                return false;
            }
            const auto& evidence = evidence_[evidence_index->second];
            if (runtime_only_evidence(evidence.kind)) {
                if (!node.runtime_hint_only || is_static_proof(node.proof) ||
                    node.completeness == Completeness::Complete) {
                    fail(WorldModelError::InvalidProof);
                    return false;
                }
                has_runtime_evidence = true;
            }
            if (node.completeness == Completeness::Complete &&
                (!evidence.immutable || runtime_only_evidence(evidence.kind))) {
                fail(WorldModelError::InvalidProof);
                return false;
            }
        }
        if (node.completeness == Completeness::Complete && node.evidence.empty()) {
            fail(WorldModelError::InvalidProof);
            return false;
        }
        if (node.runtime_hint_only && !has_runtime_evidence) {
            fail(WorldModelError::InvalidProof);
            return false;
        }
        if (nodes_.size() >= materialization_world_max_nodes) {
            fail(WorldModelError::CapacityExceeded);
            return false;
        }
        const auto existing = node_index_.find(node.id);
        if (existing != node_index_.end()) {
            if (existing->second >= nodes_.size()) {
                fail(WorldModelError::InvalidArtifact);
                return false;
            }
            const auto& prior = nodes_[existing->second];
            fail(prior.canonical_identity == node.canonical_identity
                     ? WorldModelError::DuplicateEntry
                     : WorldModelError::StableIdCollision);
            return false;
        }
        nodes_.push_back(std::move(node));
        const auto index = nodes_.size() - 1u;
        const auto node_key = nodes_[index].id;
        bool node_index_added = false;
        std::size_t reverse_indexes_added = 0u;
        const auto rollback_node = [&]() noexcept {
            for (std::size_t reverse_index = 0u;
                 reverse_index < reverse_indexes_added;
                 ++reverse_index) {
                reverse_dependency_index_.erase(
                    reverse_dependency_index_key(
                        node_key, nodes_[index].reverse_dependencies[reverse_index]));
            }
            if (node_index_added) node_index_.erase(node_key);
            nodes_.pop_back();
        };
        try {
            if (!node_index_.emplace(node_key, index).second) {
                rollback_node();
                fail(WorldModelError::DuplicateEntry);
                return false;
            }
            node_index_added = true;
            for (std::size_t reverse_index = 0u;
                 reverse_index < nodes_[index].reverse_dependencies.size();
                 ++reverse_index) {
                const auto& reverse = nodes_[index].reverse_dependencies[reverse_index];
                if (!reverse_dependency_index_
                         .emplace(reverse_dependency_index_key(node_key, reverse),
                                  reverse_index)
                         .second) {
                    rollback_node();
                    fail(WorldModelError::DuplicateEntry);
                    return false;
                }
                ++reverse_indexes_added;
            }
        } catch (...) {
            rollback_node();
            fail(WorldModelError::CapacityExceeded);
            return false;
        }
        validation_cached_ = false;
        last_error_ = WorldModelError::None;
        ++revision_;
        return true;
    } catch (...) {
        fail(WorldModelError::CapacityExceeded);
        return false;
    }
}

bool ExecutableMaterializationWorld::add_dependency(DependencyEdge edge) noexcept {
    if (!valid_dependency_kind_value(edge.kind) || !edge.from || !edge.to ||
        edge.from == edge.to) {
        fail(WorldModelError::InvalidDependency);
        return false;
    }
    const auto from = node_index_.find(edge.from);
    const auto to = node_index_.find(edge.to);
    if (from == node_index_.end() || to == node_index_.end() ||
        from->second >= nodes_.size() || to->second >= nodes_.size()) {
        fail(WorldModelError::InvalidDependency);
        return false;
    }
    if (dependencies_.size() >= materialization_world_max_edges) {
        fail(WorldModelError::CapacityExceeded);
        return false;
    }
    const auto dependency_key = dependency_index_key(edge);
    if (dependency_index_.find(dependency_key) != dependency_index_.end()) {
        fail(WorldModelError::DuplicateEntry);
        return false;
    }
    const ReverseDependency reverse{edge.from, edge.kind};
    const auto reverse_key = reverse_dependency_index_key(edge.to, reverse);
    if (reverse_dependency_index_.find(reverse_key) !=
        reverse_dependency_index_.end()) {
        fail(WorldModelError::DuplicateEntry);
        return false;
    }
    if (dependency_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(WorldModelError::CapacityExceeded);
        return false;
    }
    auto& target = nodes_[to->second];
    if (target.reverse_dependencies.size() >= materialization_world_max_edges) {
        fail(WorldModelError::CapacityExceeded);
        return false;
    }
    bool reverse_added = false;
    bool dependency_added = false;
    bool dependency_index_added = false;
    bool reverse_index_added = false;
    const auto rollback_dependency = [&]() noexcept {
        if (reverse_index_added) reverse_dependency_index_.erase(reverse_key);
        if (dependency_index_added) dependency_index_.erase(dependency_key);
        if (dependency_added && !dependencies_.empty()) dependencies_.pop_back();
        if (reverse_added && !target.reverse_dependencies.empty() &&
            target.reverse_dependencies.back() == reverse)
            target.reverse_dependencies.pop_back();
    };
    try {
        target.reverse_dependencies.push_back(reverse);
        reverse_added = true;
        dependencies_.push_back(edge);
        dependency_added = true;
        if (!dependency_index_.emplace(dependency_key, dependencies_.size() - 1u)
                 .second) {
            rollback_dependency();
            fail(WorldModelError::DuplicateEntry);
            return false;
        }
        dependency_index_added = true;
        if (!reverse_dependency_index_
                 .emplace(reverse_key, target.reverse_dependencies.size() - 1u)
                 .second) {
            rollback_dependency();
            fail(WorldModelError::DuplicateEntry);
            return false;
        }
        reverse_index_added = true;
        ++dependency_generation_;
        dependency_generation_dirty_ = true;
        validation_cached_ = false;
        last_error_ = WorldModelError::None;
        ++revision_;
        return true;
    } catch (...) {
        rollback_dependency();
        fail(WorldModelError::CapacityExceeded);
        return false;
    }
}

bool ExecutableMaterializationWorld::add_frontier(FrontierEntry entry) noexcept {
    try {
        if (!valid_frontier_entry(entry)) {
            fail(WorldModelError::InvalidFrontier);
            return false;
        }
        const auto expected = frontier_id(entry.family, entry.owner, entry.site);
        if (!expected) {
            fail(WorldModelError::InvalidIdentity);
            return false;
        }
        if (!entry.id) entry.id = expected;
        if (entry.id != expected) {
            fail(WorldModelError::InvalidStableId);
            return false;
        }
        std::sort(entry.evidence.begin(), entry.evidence.end());
        if (entry.dependency_generation == 0u)
            entry.dependency_generation = dependency_generation_;
        if (entry.evidence_digest == 0u)
            entry.evidence_digest = digest_ids(entry.evidence);
        bool has_runtime_evidence = false;
        for (const auto evidence_id : entry.evidence) {
            const auto evidence_index = evidence_index_.find(evidence_id);
            if (evidence_index == evidence_index_.end() ||
                evidence_index->second >= evidence_.size()) {
                fail(WorldModelError::MissingReference);
                return false;
            }
            const auto& evidence = evidence_[evidence_index->second];
            if (runtime_only_evidence(evidence.kind)) {
                has_runtime_evidence = true;
                if (!entry.runtime_evidence_required ||
                    entry.state != FrontierState::ObservedHint ||
                    entry.proof != FrontierProof::RuntimeObservation ||
                    entry.static_complete) {
                    fail(WorldModelError::InvalidProof);
                    return false;
                }
            }
            if (entry.static_complete &&
                (!evidence.immutable || runtime_only_evidence(evidence.kind))) {
                fail(WorldModelError::InvalidProof);
                return false;
            }
        }
        if (entry.static_complete && entry.evidence.empty()) {
            fail(WorldModelError::InvalidProof);
            return false;
        }
        if (entry.state == FrontierState::ObservedHint && !has_runtime_evidence) {
            fail(WorldModelError::InvalidProof);
            return false;
        }
        if (frontier_.size() >= materialization_world_max_frontier) {
            fail(WorldModelError::CapacityExceeded);
            return false;
        }
        const auto existing = frontier_index_.find(entry.id);
        if (existing != frontier_index_.end()) {
            if (existing->second >= frontier_.size()) {
                fail(WorldModelError::InvalidArtifact);
                return false;
            }
            const auto& prior = frontier_[existing->second];
            fail(prior.family == entry.family && prior.owner == entry.owner &&
                         prior.site == entry.site
                     ? WorldModelError::DuplicateEntry
                     : WorldModelError::StableIdCollision);
            return false;
        }
        frontier_.push_back(std::move(entry));
        const auto index = frontier_.size() - 1u;
        const auto frontier_key = frontier_[index].id;
        bool id_added = false;
        try {
            if (!frontier_index_.emplace(frontier_[index].id, index).second) {
                frontier_.pop_back();
                fail(WorldModelError::DuplicateEntry);
                return false;
            }
            id_added = true;
            if (!frontier_order_index_
                     .emplace(frontier_order_key(frontier_[index]), index)
                     .second) {
                frontier_index_.erase(frontier_key);
                frontier_.pop_back();
                fail(WorldModelError::DuplicateEntry);
                return false;
            }
        } catch (...) {
            if (id_added) frontier_index_.erase(frontier_key);
            frontier_.pop_back();
            fail(WorldModelError::CapacityExceeded);
            return false;
        }
        validation_cached_ = false;
        last_error_ = WorldModelError::None;
        ++revision_;
        return true;
    } catch (...) {
        fail(WorldModelError::CapacityExceeded);
        return false;
    }
}

bool ExecutableMaterializationWorld::add_runtime_hint(
    const std::string_view family,
    const std::string_view owner,
    const std::string_view site,
    const std::string_view observation) noexcept {
    if (family.empty() || owner.empty() || site.empty() || observation.empty()) {
        fail(WorldModelError::InvalidIdentity);
        return false;
    }
    if (family.size() > materialization_world_max_text_bytes ||
        owner.size() > materialization_world_max_text_bytes ||
        site.size() > materialization_world_max_text_bytes ||
        observation.size() > materialization_world_max_text_bytes ||
        11u + family.size() + owner.size() + site.size() + observation.size() >
            materialization_world_max_text_bytes) {
        fail(WorldModelError::InvalidIdentity);
        return false;
    }
    try {
        const auto frontier = frontier_id(family, owner, site);
        if (!frontier) {
            fail(WorldModelError::InvalidIdentity);
            return false;
        }
        FrontierEntry* existing_frontier = nullptr;
        std::size_t existing_frontier_index = 0u;
        const auto frontier_index = frontier_index_.find(frontier);
        if (frontier_index != frontier_index_.end()) {
            if (frontier_index->second >= frontier_.size()) {
                fail(WorldModelError::InvalidFrontier);
                return false;
            }
            existing_frontier_index = frontier_index->second;
            auto& candidate = frontier_[existing_frontier_index];
            if (candidate.family != family || candidate.owner != owner ||
                candidate.site != site) {
                fail(WorldModelError::StableIdCollision);
                return false;
            }
            existing_frontier = &candidate;
        }
        if (existing_frontier != nullptr &&
            (existing_frontier->state != FrontierState::ObservedHint ||
             existing_frontier->proof != FrontierProof::RuntimeObservation ||
             !existing_frontier->runtime_evidence_required ||
             existing_frontier->static_complete)) {
            fail(WorldModelError::InvalidFrontier);
            return false;
        }
        if (existing_frontier != nullptr &&
            existing_frontier->evidence.size() >=
                materialization_world_max_evidence_per_item) {
            fail(WorldModelError::CapacityExceeded);
            return false;
        }

        std::string evidence_key;
        evidence_key.reserve(11u + family.size() + owner.size() + site.size() +
                             observation.size());
        evidence_key.append("runtime:");
        evidence_key.append(family);
        evidence_key.push_back('|');
        evidence_key.append(owner);
        evidence_key.push_back('|');
        evidence_key.append(site);
        evidence_key.push_back('|');
        evidence_key.append(observation);
        EvidenceRecord evidence{stable_id(EntityKind::Evidence, evidence_key),
                                 EvidenceKind::RuntimeObservation,
                                 std::move(evidence_key), false};
        const auto evidence_id = evidence.id;

        FrontierEntry entry;
        entry.id = frontier;
        entry.family.assign(family);
        entry.owner.assign(owner);
        entry.site.assign(site);
        entry.state = FrontierState::ObservedHint;
        entry.proof = FrontierProof::RuntimeObservation;
        entry.severity = FrontierSeverity::P1;
        entry.missing_proof = "immutable static provenance";
        entry.runtime_evidence_required = true;
        entry.static_complete = false;
        entry.evidence.push_back(evidence_id);

        const auto previous_evidence_size = evidence_.size();
        const auto previous_evidence_digest = evidence_digest_;
        const auto previous_evidence_accumulator = evidence_digest_accumulator_;
        const auto previous_revision = revision_;
        const auto previous_validation_cached = validation_cached_;
        const auto previous_validation_result = validation_result_;
        const auto rollback = [&](const WorldModelError error) noexcept {
            if (evidence_.size() > previous_evidence_size) {
                evidence_index_.erase(evidence_.back().id);
            }
            evidence_.resize(previous_evidence_size);
            evidence_digest_ = previous_evidence_digest;
            evidence_digest_accumulator_ = previous_evidence_accumulator;
            revision_ = previous_revision;
            validation_cached_ = previous_validation_cached;
            validation_result_ = previous_validation_result;
            last_error_ = error;
        };
        if (!add_evidence(std::move(evidence))) return false;
        if (existing_frontier != nullptr) {
            bool frontier_evidence_added = false;
            std::size_t inserted_evidence_index = 0u;
            try {
                const auto position = std::lower_bound(
                    existing_frontier->evidence.begin(),
                    existing_frontier->evidence.end(),
                    evidence_id);
                inserted_evidence_index = static_cast<std::size_t>(
                    position - existing_frontier->evidence.begin());
                existing_frontier->evidence.insert(position, evidence_id);
                frontier_evidence_added = true;
                existing_frontier->evidence_digest =
                    digest_ids(existing_frontier->evidence);
                last_error_ = WorldModelError::None;
                validation_cached_ = false;
                ++revision_;
                return true;
            } catch (...) {
                if (frontier_evidence_added)
                    existing_frontier->evidence.erase(
                        existing_frontier->evidence.begin() +
                        static_cast<std::ptrdiff_t>(inserted_evidence_index));
                rollback(WorldModelError::CapacityExceeded);
                return false;
            }
        }
        if (!add_frontier(std::move(entry))) {
            const auto error = last_error_;
            rollback(error);
            return false;
        }
        return true;
    } catch (...) {
        fail(WorldModelError::CapacityExceeded);
        return false;
    }
}

bool ExecutableMaterializationWorld::attach_node_evidence(const StableId node,
                                                          const StableId evidence) noexcept {
    const auto node_index = node_index_.find(node);
    const auto evidence_index = evidence_index_.find(evidence);
    if (node_index == node_index_.end() || evidence_index == evidence_index_.end() ||
        node_index->second >= nodes_.size() || evidence_index->second >= evidence_.size()) {
        fail(WorldModelError::MissingReference);
        return false;
    }
    auto& item = nodes_[node_index->second];
    if (has_id(item.evidence, evidence)) return true;
    const auto& evidence_item = evidence_[evidence_index->second];
    if (runtime_only_evidence(evidence_item.kind) &&
        (!item.runtime_hint_only || is_static_proof(item.proof) ||
         item.completeness == Completeness::Complete)) {
        fail(WorldModelError::InvalidProof);
        return false;
    }
    if (item.completeness == Completeness::Complete &&
        !evidence_item.immutable) {
        fail(WorldModelError::InvalidProof);
        return false;
    }
    if (item.evidence.size() >= materialization_world_max_evidence_per_item) {
        fail(WorldModelError::CapacityExceeded);
        return false;
    }
    try {
        const auto position = std::lower_bound(item.evidence.begin(),
                                               item.evidence.end(), evidence);
        item.evidence.insert(position, evidence);
        item.evidence_digest = digest_ids(item.evidence);
        validation_cached_ = false;
        ++revision_;
        last_error_ = WorldModelError::None;
        return true;
    } catch (...) {
        fail(WorldModelError::CapacityExceeded);
        return false;
    }
}

bool ExecutableMaterializationWorld::attach_frontier_evidence(
    const StableId frontier,
    const StableId evidence) noexcept {
    const auto frontier_index = frontier_index_.find(frontier);
    const auto evidence_index = evidence_index_.find(evidence);
    if (frontier_index == frontier_index_.end() ||
        evidence_index == evidence_index_.end() ||
        frontier_index->second >= frontier_.size() ||
        evidence_index->second >= evidence_.size()) {
        fail(WorldModelError::MissingReference);
        return false;
    }
    auto& item = frontier_[frontier_index->second];
    if (has_id(item.evidence, evidence)) return true;
    const auto& evidence_item = evidence_[evidence_index->second];
    if (runtime_only_evidence(evidence_item.kind) &&
        (item.state != FrontierState::ObservedHint ||
         item.proof != FrontierProof::RuntimeObservation ||
         !item.runtime_evidence_required || item.static_complete)) {
        fail(WorldModelError::InvalidProof);
        return false;
    }
    if (item.static_complete && !evidence_item.immutable) {
        fail(WorldModelError::InvalidProof);
        return false;
    }
    if (item.evidence.size() >= materialization_world_max_evidence_per_item) {
        fail(WorldModelError::CapacityExceeded);
        return false;
    }
    try {
        const auto position = std::lower_bound(item.evidence.begin(),
                                               item.evidence.end(), evidence);
        item.evidence.insert(position, evidence);
        item.evidence_digest = digest_ids(item.evidence);
        validation_cached_ = false;
        ++revision_;
        last_error_ = WorldModelError::None;
        return true;
    } catch (...) {
        fail(WorldModelError::CapacityExceeded);
        return false;
    }
}

bool ExecutableMaterializationWorld::contains_node(const StableId id) const noexcept {
    const auto item = node_index_.find(id);
    return item != node_index_.end() && item->second < nodes_.size() &&
           nodes_[item->second].id == id;
}

bool ExecutableMaterializationWorld::contains_evidence(const StableId id) const noexcept {
    const auto item = evidence_index_.find(id);
    return item != evidence_index_.end() && item->second < evidence_.size() &&
           evidence_[item->second].id == id;
}

bool ExecutableMaterializationWorld::contains_frontier(const StableId id) const noexcept {
    const auto item = frontier_index_.find(id);
    return item != frontier_index_.end() && item->second < frontier_.size() &&
           frontier_[item->second].id == id;
}

bool ExecutableMaterializationWorld::collect_reverse_dependencies(
    const StableId target,
    const std::span<ReverseDependency> output,
    std::size_t& written) const noexcept {
    const auto node_index = node_index_.find(target);
    if (node_index == node_index_.end() || node_index->second >= nodes_.size()) {
        written = 0u;
        return false;
    }
    const auto& node = nodes_[node_index->second];
    if (node.reverse_dependencies.size() > output.size()) {
        written = node.reverse_dependencies.size();
        return false;
    }
    written = 0u;
    for_each_reverse_dependency(target, [&](const ReverseDependency& dependency) {
        if (written < output.size()) output[written++] = dependency;
    });
    if (written != node.reverse_dependencies.size()) {
        written = node.reverse_dependencies.size();
        return false;
    }
    return true;
}

bool ExecutableMaterializationWorld::validate() const noexcept {
    synchronize_dependency_generation();
    if (validation_cached_) return validation_result_;
    validation_result_ = validate_uncached();
    validation_cached_ = true;
    return validation_result_;
}

bool ExecutableMaterializationWorld::validate_uncached() const noexcept {
    try {
        if (dependency_generation_ == 0u ||
            nodes_.size() > materialization_world_max_nodes ||
            dependencies_.size() > materialization_world_max_edges ||
            evidence_.size() > materialization_world_max_evidence ||
            frontier_.size() > materialization_world_max_frontier)
            return false;
        if (evidence_index_.size() != evidence_.size() ||
            node_index_.size() != nodes_.size() ||
            frontier_index_.size() != frontier_.size() ||
            frontier_order_index_.size() != frontier_.size() ||
            dependency_index_.size() != dependencies_.size() ||
            reverse_dependency_index_.size() != dependencies_.size())
            return false;

        for (std::size_t index = 0u; index < evidence_.size(); ++index) {
            const auto& item = evidence_[index];
            if (!valid_evidence_kind_value(item.kind) ||
                item.canonical_identity.empty() ||
                !valid_text(item.canonical_identity) ||
                item.id != stable_id(EntityKind::Evidence, item.canonical_identity))
                return false;
            const auto indexed = evidence_index_.find(item.id);
            if (indexed == evidence_index_.end() || indexed->second != index)
                return false;
        }
        if (evidence_digest_ != canonical_evidence_digest(
                                  evidence_digest_accumulator_,
                                  !evidence_.empty()))
            return false;

        for (std::size_t index = 0u; index < nodes_.size(); ++index) {
            const auto& node = nodes_[index];
            const auto indexed = node_index_.find(node.id);
            if (indexed == node_index_.end() || indexed->second != index ||
                !valid_node_proof(node, false) ||
                node.id != stable_id(EntityKind::Node, node.canonical_identity) ||
                node.dependency_generation != dependency_generation_ ||
                node.evidence_digest != digest_ids(node.evidence))
                return false;
            for (const auto evidence : node.evidence) {
                const auto item_index = evidence_index_.find(evidence);
                if (item_index == evidence_index_.end() ||
                    item_index->second >= evidence_.size())
                    return false;
                const auto& item = evidence_[item_index->second];
                if (runtime_only_evidence(item.kind) &&
                    (!node.runtime_hint_only || is_static_proof(node.proof) ||
                     node.completeness == Completeness::Complete))
                    return false;
                if (node.completeness == Completeness::Complete &&
                    !item.immutable)
                    return false;
            }
            if (node.completeness == Completeness::Complete && node.evidence.empty())
                return false;
            if (node.runtime_hint_only) {
                const bool has_runtime = std::any_of(
                    node.evidence.begin(), node.evidence.end(), [&](const auto id) {
                        const auto item_index = evidence_index_.find(id);
                        return item_index != evidence_index_.end() &&
                               item_index->second < evidence_.size() &&
                               runtime_only_evidence(
                                   evidence_[item_index->second].kind);
                    });
                if (!has_runtime) return false;
            }
            for (std::size_t reverse_index = 0u;
                 reverse_index < node.reverse_dependencies.size();
                 ++reverse_index) {
                const auto& reverse = node.reverse_dependencies[reverse_index];
                const auto source = node_index_.find(reverse.from);
                if (source == node_index_.end() || source->second >= nodes_.size())
                    return false;
                const auto reverse_key = reverse_dependency_index_key(node.id, reverse);
                const auto reverse_indexed = reverse_dependency_index_.find(reverse_key);
                if (reverse_indexed == reverse_dependency_index_.end() ||
                    reverse_indexed->second != reverse_index)
                    return false;
                const auto edge = dependency_index_.find(
                    std::make_tuple(reverse.from,
                                    node.id,
                                    static_cast<std::uint8_t>(reverse.kind)));
                if (edge == dependency_index_.end()) return false;
            }
        }

        for (std::size_t index = 0u; index < dependencies_.size(); ++index) {
            const auto& edge = dependencies_[index];
            if (!valid_dependency_kind_value(edge.kind) || !edge.from || !edge.to ||
                edge.from == edge.to)
                return false;
            const auto indexed = dependency_index_.find(dependency_index_key(edge));
            if (indexed == dependency_index_.end() || indexed->second != index)
                return false;
            const auto from = node_index_.find(edge.from);
            const auto to = node_index_.find(edge.to);
            if (from == node_index_.end() || to == node_index_.end() ||
                from->second >= nodes_.size() || to->second >= nodes_.size())
                return false;
            const auto reverse = reverse_dependency_index_.find(
                std::make_tuple(edge.to,
                                edge.from,
                                static_cast<std::uint8_t>(edge.kind)));
            if (reverse == reverse_dependency_index_.end() ||
                reverse->second >= nodes_[to->second].reverse_dependencies.size() ||
                nodes_[to->second].reverse_dependencies[reverse->second] !=
                    ReverseDependency{edge.from, edge.kind})
                return false;
        }

        for (std::size_t index = 0u; index < frontier_.size(); ++index) {
            const auto& entry = frontier_[index];
            const auto indexed = frontier_index_.find(entry.id);
            if (indexed == frontier_index_.end() || indexed->second != index ||
                !valid_frontier_entry(entry) ||
                entry.id != frontier_id(entry.family, entry.owner, entry.site) ||
                entry.dependency_generation != dependency_generation_ ||
                entry.evidence_digest != digest_ids(entry.evidence))
                return false;
            const auto ordered = frontier_order_index_.find(frontier_order_key(entry));
            if (ordered == frontier_order_index_.end() || ordered->second != index)
                return false;
            for (const auto evidence : entry.evidence) {
                const auto item_index = evidence_index_.find(evidence);
                if (item_index == evidence_index_.end() ||
                    item_index->second >= evidence_.size())
                    return false;
                const auto& item = evidence_[item_index->second];
                if (runtime_only_evidence(item.kind) &&
                    (entry.state != FrontierState::ObservedHint ||
                     entry.proof != FrontierProof::RuntimeObservation ||
                     !entry.runtime_evidence_required || entry.static_complete))
                    return false;
                if (entry.static_complete && !item.immutable) return false;
            }
            if (entry.static_complete && entry.evidence.empty()) return false;
            if (entry.state == FrontierState::ObservedHint) {
                const bool has_runtime = std::any_of(
                    entry.evidence.begin(), entry.evidence.end(), [&](const auto id) {
                        const auto item_index = evidence_index_.find(id);
                        return item_index != evidence_index_.end() &&
                               item_index->second < evidence_.size() &&
                               runtime_only_evidence(
                                   evidence_[item_index->second].kind);
                    });
                if (!has_runtime) return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool frontier_priority_less(const FrontierEntry& lhs,
                            const FrontierEntry& rhs) noexcept {
    if (lhs.severity != rhs.severity)
        return static_cast<std::uint8_t>(lhs.severity) <
               static_cast<std::uint8_t>(rhs.severity);
    const int lhs_state = frontier_state_rank(lhs.state);
    const int rhs_state = frontier_state_rank(rhs.state);
    if (lhs_state != rhs_state) return lhs_state < rhs_state;
    if (lhs.runtime_evidence_required != rhs.runtime_evidence_required)
        return lhs.runtime_evidence_required < rhs.runtime_evidence_required;
    if (lhs.fanout != rhs.fanout) return lhs.fanout > rhs.fanout;
    return lhs.id < rhs.id;
}

namespace {

AgentDecision evaluate_agent_decision_unchecked(
    const ExecutableMaterializationWorld& world) noexcept {
    StableId first_static{};
    const FrontierEntry* first_static_entry = nullptr;
    StableId first_runtime{};
    const FrontierEntry* first_runtime_entry = nullptr;
    std::size_t actionable = 0u;
    bool runtime_required = false;
    for (const auto& entry : world.frontier()) {
        if (entry.state == FrontierState::Closed && entry.static_complete) continue;
        if (entry.proof == FrontierProof::ExplicitRejection) continue;
        const bool runtime = entry.runtime_evidence_required ||
                             entry.state == FrontierState::ObservedHint ||
                             entry.proof == FrontierProof::RuntimeObservation;
        if (runtime) {
            runtime_required = true;
            if (first_runtime_entry == nullptr ||
                frontier_task_priority_less(entry, *first_runtime_entry)) {
                first_runtime = entry.id;
                first_runtime_entry = &entry;
            }
        } else {
            ++actionable;
            if (first_static_entry == nullptr ||
                frontier_task_priority_less(entry, *first_static_entry)) {
                first_static = entry.id;
                first_static_entry = &entry;
            }
        }
    }

    bool graph_complete = !world.nodes().empty();
    for (const auto& node : world.nodes()) {
        if (!is_static_proof(node.proof) ||
            node.completeness != Completeness::Complete || node.runtime_hint_only) {
            graph_complete = false;
            break;
        }
    }
    if (actionable != 0u)
        return {AgentDecisionKind::ContinueStaticIteration,
                first_static,
                "static-frontier-remains",
                actionable};
    if (!graph_complete)
        return {AgentDecisionKind::ContinueStaticIteration,
                {},
                "materialization-graph-incomplete",
                actionable};
    if (runtime_required)
        return {AgentDecisionKind::RequiresRuntimeEvidence,
                first_runtime,
                "static-proof-exhausted-runtime-evidence-required",
                actionable};
    if (world.frontier().empty())
        return {AgentDecisionKind::ContinueStaticIteration,
                {},
                "no-frontier-published",
                actionable};
    return {AgentDecisionKind::BuildPort, {}, "static-world-closed", actionable};
}

} // namespace

AgentDecision evaluate_agent_decision(
    const ExecutableMaterializationWorld& world) noexcept {
    if (!world.validate())
        return {AgentDecisionKind::ContinueStaticIteration,
                {},
                "world-model-invalid-or-incomplete",
                0u};
    return evaluate_agent_decision_unchecked(world);
}

BoundedJsonResult serialize_agent_world_json(
    const ExecutableMaterializationWorld& world,
    const std::span<char> output) noexcept {
    JsonWriter writer{output};
    const bool valid = world.validate();
    const auto decision = valid
                              ? evaluate_agent_decision_unchecked(world)
                              : AgentDecision{AgentDecisionKind::ContinueStaticIteration,
                                              {},
                                              "world-model-invalid-or-incomplete",
                                              0u};
    writer.raw("{\"schema\":");
    writer.number(materialization_world_schema_version);
    writer.raw(",\"frontier_schema\":");
    writer.number(frontier_schema_version);
    writer.raw(",\"revision\":");
    writer.number(world.revision());
    writer.raw(",\"dependency_generation\":");
    writer.number(world.dependency_generation());
    writer.raw(",\"evidence_digest\":");
    writer.number(world.evidence_digest());
    writer.raw(",\"valid\":");
    writer.boolean(valid);
    writer.raw(",\"error\":");
    writer.string(valid ? "none" : world_model_error_name(world.last_error()));
    writer.raw(",\"decision\":{\"kind\":");
    writer.string(agent_decision_kind_name(decision.kind));
    writer.raw(",\"focus\":");
    writer.id(decision.focus);
    writer.raw(",\"reason\":");
    writer.string(decision.reason);
    writer.raw(",\"actionable_frontier\":");
    writer.number(decision.actionable_frontier);
    writer.raw("},\"evidence\":[");
    bool first = true;
    for (const auto& indexed : world.evidence_index_) {
        if (indexed.second >= world.evidence_.size()) continue;
        const auto& item = world.evidence_[indexed.second];
        if (!first) writer.put(',');
        first = false;
        writer.raw("{\"id\":");
        writer.id(item.id);
        writer.raw(",\"kind\":");
        writer.string(evidence_kind_name(item.kind));
        writer.raw(",\"identity\":");
        writer.string(item.canonical_identity);
        writer.raw(",\"immutable\":");
        writer.boolean(item.immutable);
        writer.put('}');
    }
    writer.raw("],\"nodes\":[");
    first = true;
    for (const auto& indexed : world.node_index_) {
        if (indexed.second >= world.nodes_.size()) continue;
        const auto& item = world.nodes_[indexed.second];
        if (!first) writer.put(',');
        first = false;
        write_node(writer, item, [&](JsonWriter& reverse_writer,
                                     const MaterializationNode& node) noexcept {
            bool first_reverse = true;
            world.for_each_reverse_dependency(
                node.id, [&](const ReverseDependency& reverse) noexcept {
                    if (!first_reverse) reverse_writer.put(',');
                    first_reverse = false;
                    reverse_writer.raw("{\"from\":");
                    reverse_writer.id(reverse.from);
                    reverse_writer.raw(",\"kind\":");
                    reverse_writer.string(dependency_kind_name(reverse.kind));
                    reverse_writer.put('}');
                });
        });
    }
    writer.raw("],\"dependencies\":[");
    first = true;
    for (const auto& indexed : world.dependency_index_) {
        if (indexed.second >= world.dependencies_.size()) continue;
        const auto& item = world.dependencies_[indexed.second];
        if (!first) writer.put(',');
        first = false;
        writer.raw("{\"from\":");
        writer.id(item.from);
        writer.raw(",\"to\":");
        writer.id(item.to);
        writer.raw(",\"kind\":");
        writer.string(dependency_kind_name(item.kind));
        writer.raw(",\"statically_proven\":");
        writer.boolean(item.statically_proven);
        writer.put('}');
    }
    writer.raw("],\"frontier\":[");
    first = true;
    for (const auto& indexed : world.frontier_order_index_) {
        if (indexed.second >= world.frontier_.size()) continue;
        const auto& item = world.frontier_[indexed.second];
        if (!first) writer.put(',');
        first = false;
        write_frontier(writer, item);
    }
    writer.raw("]}");
    if (writer.overflow) return {0u, false, true};
    return {writer.position, true, false};
}

namespace {

constexpr std::size_t binary_header_bytes = 80u;
constexpr std::size_t binary_checksum_offset = 32u;
constexpr std::size_t binary_checksum_bytes = sizeof(std::uint32_t);
constexpr std::size_t binary_parser_reserve_budget_bytes =
    materialization_world_max_binary_artifact_bytes * 2u;
constexpr char binary_magic[] = "KATWAG01";

struct BinaryWriter final {
    std::span<std::uint8_t> output;
    std::size_t position = 0u;
    bool overflow = false;

    void bytes(const std::span<const std::uint8_t> value) noexcept {
        if (overflow) return;
        if (position > output.size() || value.size() > output.size() - position) {
            overflow = true;
            return;
        }
        std::copy(value.begin(), value.end(), output.begin() + position);
        position += value.size();
    }

    void u8(const std::uint8_t value) noexcept {
        bytes(std::span<const std::uint8_t>(&value, 1u));
    }

    void u16(const std::uint16_t value) noexcept {
        const std::uint8_t encoded[] = {
            static_cast<std::uint8_t>(value & 0xFFu),
            static_cast<std::uint8_t>((value >> 8u) & 0xFFu),
        };
        bytes(encoded);
    }

    void u32(const std::uint32_t value) noexcept {
        const std::uint8_t encoded[] = {
            static_cast<std::uint8_t>(value & 0xFFu),
            static_cast<std::uint8_t>((value >> 8u) & 0xFFu),
            static_cast<std::uint8_t>((value >> 16u) & 0xFFu),
            static_cast<std::uint8_t>((value >> 24u) & 0xFFu),
        };
        bytes(encoded);
    }

    void u64(const std::uint64_t value) noexcept {
        const std::uint8_t encoded[] = {
            static_cast<std::uint8_t>(value & 0xFFu),
            static_cast<std::uint8_t>((value >> 8u) & 0xFFu),
            static_cast<std::uint8_t>((value >> 16u) & 0xFFu),
            static_cast<std::uint8_t>((value >> 24u) & 0xFFu),
            static_cast<std::uint8_t>((value >> 32u) & 0xFFu),
            static_cast<std::uint8_t>((value >> 40u) & 0xFFu),
            static_cast<std::uint8_t>((value >> 48u) & 0xFFu),
            static_cast<std::uint8_t>((value >> 56u) & 0xFFu),
        };
        bytes(encoded);
    }

    void text(const std::string_view value) noexcept {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            overflow = true;
            return;
        }
        u32(static_cast<std::uint32_t>(value.size()));
        bytes(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    }

    void patch_u32(const std::size_t offset, const std::uint32_t value) noexcept {
        if (offset > output.size() || sizeof(value) > output.size() - offset) {
            overflow = true;
            return;
        }
        output[offset + 0u] = static_cast<std::uint8_t>(value & 0xFFu);
        output[offset + 1u] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
        output[offset + 2u] = static_cast<std::uint8_t>((value >> 16u) & 0xFFu);
        output[offset + 3u] = static_cast<std::uint8_t>((value >> 24u) & 0xFFu);
    }

    void patch_u64(const std::size_t offset, const std::uint64_t value) noexcept {
        if (offset > output.size() || sizeof(value) > output.size() - offset) {
            overflow = true;
            return;
        }
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            output[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
    }
};

struct BinaryReader final {
    std::span<const std::uint8_t> input;
    std::size_t position = 0u;
    std::size_t reserve_budget = binary_parser_reserve_budget_bytes;
    bool failed = false;

    [[nodiscard]] bool reserve_bytes(const std::size_t bytes) noexcept {
        if (bytes > reserve_budget) {
            failed = true;
            return false;
        }
        reserve_budget -= bytes;
        return true;
    }

    [[nodiscard]] bool bytes(std::span<std::uint8_t> output) noexcept {
        if (failed || position > input.size() || output.size() > input.size() - position) {
            failed = true;
            return false;
        }
        std::copy(input.begin() + position, input.begin() + position + output.size(),
                  output.begin());
        position += output.size();
        return true;
    }

    [[nodiscard]] bool raw(const std::span<const std::uint8_t> expected) noexcept {
        if (failed || position > input.size() ||
            expected.size() > input.size() - position) {
            failed = true;
            return false;
        }
        if (!std::equal(expected.begin(), expected.end(), input.begin() + position)) {
            failed = true;
            return false;
        }
        position += expected.size();
        return true;
    }

    [[nodiscard]] bool u8(std::uint8_t& value) noexcept {
        if (failed || position >= input.size()) {
            failed = true;
            return false;
        }
        value = input[position++];
        return true;
    }

    [[nodiscard]] bool u16(std::uint16_t& value) noexcept {
        std::uint8_t encoded[2]{};
        if (!bytes(encoded)) return false;
        value = static_cast<std::uint16_t>(encoded[0]) |
                (static_cast<std::uint16_t>(encoded[1]) << 8u);
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& value) noexcept {
        std::uint8_t encoded[4]{};
        if (!bytes(encoded)) return false;
        value = static_cast<std::uint32_t>(encoded[0]) |
                (static_cast<std::uint32_t>(encoded[1]) << 8u) |
                (static_cast<std::uint32_t>(encoded[2]) << 16u) |
                (static_cast<std::uint32_t>(encoded[3]) << 24u);
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& value) noexcept {
        std::uint8_t encoded[8]{};
        if (!bytes(encoded)) return false;
        value = 0u;
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            value |= static_cast<std::uint64_t>(encoded[byte]) << (byte * 8u);
        return true;
    }

    [[nodiscard]] bool text(std::string& value, const std::size_t maximum) noexcept {
        std::uint32_t length = 0u;
        if (!u32(length) || length > maximum || position > input.size() ||
            length > input.size() - position) {
            failed = true;
            return false;
        }
        try {
            value.assign(reinterpret_cast<const char*>(input.data() + position), length);
        } catch (...) {
            failed = true;
            return false;
        }
        position += length;
        return true;
    }
};

[[nodiscard]] std::uint32_t crc32_without_checksum(
    const std::span<const std::uint8_t> input) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t index = 0u; index < input.size(); ++index) {
        const auto byte = index >= binary_checksum_offset &&
                                  index < binary_checksum_offset + binary_checksum_bytes
                              ? 0u
                              : input[index];
        crc ^= byte;
        for (unsigned bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ (0xEDB88320u & static_cast<std::uint32_t>(-
                                              static_cast<std::int32_t>(crc & 1u)));
    }
    return ~crc;
}

template <typename T>
void write_id_vector(BinaryWriter& writer, const std::vector<T>& values) noexcept {
    writer.u32(static_cast<std::uint32_t>(values.size()));
    for (const auto value : values) writer.u64(value.value);
}

void write_text_vector(BinaryWriter& writer,
                       const std::vector<std::string>& values) noexcept {
    writer.u32(static_cast<std::uint32_t>(values.size()));
    for (const auto& value : values) writer.text(value);
}

template <typename T>
[[nodiscard]] bool read_id_vector(BinaryReader& reader,
                                  std::vector<T>& values,
                                  const std::size_t maximum) {
    std::uint32_t count = 0u;
    if (!reader.u32(count) || count > maximum) return false;
    if (reader.position > reader.input.size() ||
        count > (reader.input.size() - reader.position) / sizeof(std::uint64_t) ||
        count > std::numeric_limits<std::size_t>::max() / sizeof(T) ||
        !reader.reserve_bytes(static_cast<std::size_t>(count) * sizeof(T))) {
        reader.failed = true;
        return false;
    }
    try {
        values.clear();
        values.reserve(count);
        for (std::uint32_t index = 0u; index < count; ++index) {
            std::uint64_t id = 0u;
            if (!reader.u64(id)) return false;
            values.push_back(StableId{id});
        }
    } catch (...) {
        reader.failed = true;
        return false;
    }
    return true;
}

[[nodiscard]] bool read_text_vector(BinaryReader& reader,
                                    std::vector<std::string>& values,
                                    const std::size_t maximum) {
    std::uint32_t count = 0u;
    if (!reader.u32(count) || count > maximum) return false;
    if (reader.position > reader.input.size() ||
        count > (reader.input.size() - reader.position) / sizeof(std::uint32_t) ||
        count > std::numeric_limits<std::size_t>::max() / sizeof(std::string) ||
        !reader.reserve_bytes(static_cast<std::size_t>(count) * sizeof(std::string))) {
        reader.failed = true;
        return false;
    }
    try {
        values.clear();
        values.reserve(count);
        for (std::uint32_t index = 0u; index < count; ++index) {
            std::string value;
            if (!reader.text(value, materialization_world_max_text_bytes)) return false;
            values.push_back(std::move(value));
        }
    } catch (...) {
        reader.failed = true;
        return false;
    }
    return true;
}

[[nodiscard]] bool valid_node_kind_byte(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(MaterializationNodeKind::Unknown);
}

[[nodiscard]] bool valid_proof_byte(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(ProofClass::Rejected);
}

[[nodiscard]] bool valid_completeness_byte(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(Completeness::Rejected);
}

[[nodiscard]] bool valid_evidence_kind_byte(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(EvidenceKind::UserHint);
}

[[nodiscard]] bool valid_dependency_kind_byte(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(DependencyKind::Observes);
}

[[nodiscard]] bool read_reverse_dependency_vector(
    BinaryReader& reader,
    std::vector<ReverseDependency>& values,
    const std::size_t maximum) {
    constexpr std::size_t wire_bytes = sizeof(std::uint64_t) +
                                        sizeof(std::uint8_t) +
                                        sizeof(std::uint8_t) +
                                        sizeof(std::uint16_t);
    std::uint32_t count = 0u;
    if (!reader.u32(count) || count > maximum) return false;
    if (reader.position > reader.input.size() ||
        count > (reader.input.size() - reader.position) / wire_bytes ||
        count > std::numeric_limits<std::size_t>::max() /
                    sizeof(ReverseDependency) ||
        !reader.reserve_bytes(static_cast<std::size_t>(count) *
                              sizeof(ReverseDependency))) {
        reader.failed = true;
        return false;
    }
    try {
        values.clear();
        values.reserve(count);
        for (std::uint32_t index = 0u; index < count; ++index) {
            ReverseDependency value;
            std::uint8_t kind = 0u;
            std::uint8_t reserved8 = 0u;
            std::uint16_t reserved16 = 0u;
            if (!reader.u64(value.from.value) || !reader.u8(kind) ||
                !reader.u8(reserved8) || !reader.u16(reserved16) ||
                reserved8 != 0u || reserved16 != 0u || !value.from ||
                !valid_dependency_kind_byte(kind))
                return false;
            value.kind = static_cast<DependencyKind>(kind);
            values.push_back(value);
        }
    } catch (...) {
        reader.failed = true;
        return false;
    }
    return true;
}

[[nodiscard]] bool valid_frontier_state_byte(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(FrontierState::ObservedHint);
}

[[nodiscard]] bool valid_frontier_proof_byte(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(FrontierProof::ExplicitRejection);
}

[[nodiscard]] bool valid_frontier_severity_byte(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(FrontierSeverity::P3);
}

[[nodiscard]] bool valid_frontier_block_byte(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(FrontierBlockKind::Unknown);
}

[[nodiscard]] bool valid_world_error_byte(const std::uint32_t value) noexcept {
    return value <= static_cast<std::uint32_t>(WorldModelError::ArtifactTooLarge);
}

template <typename T>
[[nodiscard]] bool unique_ids(const std::vector<T>& values) noexcept {
    for (std::size_t index = 0u; index < values.size(); ++index)
        for (std::size_t other = index + 1u; other < values.size(); ++other)
            if (values[index] == values[other]) return false;
    return true;
}

[[nodiscard]] const FrontierEntry* find_frontier(
    const ExecutableMaterializationWorld& world, const StableId id) noexcept {
    for (const auto& item : world.frontier())
        if (item.id == id) return &item;
    return nullptr;
}

[[nodiscard]] bool equal_evidence(const EvidenceRecord& lhs,
                                  const EvidenceRecord& rhs) noexcept {
    return lhs.id == rhs.id && lhs.kind == rhs.kind &&
           lhs.canonical_identity == rhs.canonical_identity &&
           lhs.immutable == rhs.immutable;
}

[[nodiscard]] bool equal_node(const MaterializationNode& lhs,
                              const MaterializationNode& rhs) noexcept {
    return lhs.id == rhs.id && lhs.kind == rhs.kind &&
           lhs.canonical_identity == rhs.canonical_identity &&
           lhs.source_identity == rhs.source_identity && lhs.proof == rhs.proof &&
           lhs.completeness == rhs.completeness &&
           lhs.runtime_hint_only == rhs.runtime_hint_only &&
           lhs.dependency_generation == rhs.dependency_generation &&
           lhs.evidence_digest == rhs.evidence_digest && lhs.evidence == rhs.evidence;
}

[[nodiscard]] bool equal_dependency(const DependencyEdge& lhs,
                                    const DependencyEdge& rhs) noexcept {
    return lhs.from == rhs.from && lhs.to == rhs.to && lhs.kind == rhs.kind &&
           lhs.statically_proven == rhs.statically_proven;
}

[[nodiscard]] bool equal_frontier(const FrontierEntry& lhs,
                                  const FrontierEntry& rhs) noexcept {
    return lhs.id == rhs.id && lhs.family == rhs.family && lhs.owner == rhs.owner &&
           lhs.site == rhs.site && lhs.state == rhs.state && lhs.proof == rhs.proof &&
           lhs.severity == rhs.severity && lhs.missing_proof == rhs.missing_proof &&
           lhs.fanout == rhs.fanout &&
           lhs.runtime_evidence_required == rhs.runtime_evidence_required &&
           lhs.static_complete == rhs.static_complete &&
           lhs.blocked_kind == rhs.blocked_kind &&
           lhs.dependency_generation == rhs.dependency_generation &&
           lhs.evidence_digest == rhs.evidence_digest && lhs.evidence == rhs.evidence &&
           lhs.contracts == rhs.contracts && lhs.blocked_sites == rhs.blocked_sites &&
           lhs.blocked_functions == rhs.blocked_functions &&
           lhs.blocked_roots == rhs.blocked_roots &&
           lhs.blocked_modules == rhs.blocked_modules &&
           lhs.blocked_materializations == rhs.blocked_materializations &&
           lhs.blocked_hardware == rhs.blocked_hardware &&
           lhs.causal_chain == rhs.causal_chain && lhs.source_paths == rhs.source_paths &&
           lhs.source_symbols == rhs.source_symbols && lhs.invariants == rhs.invariants &&
           lhs.acceptance_criteria == rhs.acceptance_criteria;
}

} // namespace

BoundedBinaryResult serialize_agent_world_binary(
    const ExecutableMaterializationWorld& world,
    const std::span<std::uint8_t> output) noexcept {
    if (!world.validate()) return {};
    if (output.size() < binary_header_bytes) return {0u, false, true};

    try {
        BinaryWriter writer{output};
        writer.bytes(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(binary_magic), 8u));
        writer.u32(materialization_world_binary_schema_version);
        writer.u32(static_cast<std::uint32_t>(binary_header_bytes));
        writer.u64(0u);
        writer.u64(0u);
        writer.u32(0u);
        writer.u32(static_cast<std::uint32_t>(world.last_error()));
        writer.u64(world.revision());
        writer.u64(world.dependency_generation());
        writer.u64(world.evidence_digest());
        writer.u32(static_cast<std::uint32_t>(world.evidence().size()));
        writer.u32(static_cast<std::uint32_t>(world.nodes().size()));
        writer.u32(static_cast<std::uint32_t>(world.dependencies().size()));
        writer.u32(static_cast<std::uint32_t>(world.frontier().size()));

        for (const auto& indexed : world.evidence_index_) {
            if (indexed.second >= world.evidence_.size()) continue;
            const auto& item = world.evidence_[indexed.second];
            writer.u64(item.id.value);
            writer.u8(static_cast<std::uint8_t>(item.kind));
            writer.u8(item.immutable ? 1u : 0u);
            writer.u16(0u);
            writer.text(item.canonical_identity);
        }
        for (const auto& indexed : world.node_index_) {
            if (indexed.second >= world.nodes_.size()) continue;
            const auto& item = world.nodes_[indexed.second];
            writer.u64(item.id.value);
            writer.u8(static_cast<std::uint8_t>(item.kind));
            writer.u8(static_cast<std::uint8_t>(item.proof));
            writer.u8(static_cast<std::uint8_t>(item.completeness));
            writer.u8(item.runtime_hint_only ? 1u : 0u);
            writer.u64(item.dependency_generation);
            writer.u64(item.evidence_digest);
            writer.text(item.canonical_identity);
            writer.text(item.source_identity);
            write_id_vector(writer, item.evidence);
            writer.u32(static_cast<std::uint32_t>(item.reverse_dependencies.size()));
            world.for_each_reverse_dependency(
                item.id, [&](const ReverseDependency& reverse) noexcept {
                    writer.u64(reverse.from.value);
                    writer.u8(static_cast<std::uint8_t>(reverse.kind));
                    writer.u8(0u);
                    writer.u16(0u);
                });
        }
        for (const auto& indexed : world.dependency_index_) {
            if (indexed.second >= world.dependencies_.size()) continue;
            const auto& item = world.dependencies_[indexed.second];
            writer.u64(item.from.value);
            writer.u64(item.to.value);
            writer.u8(static_cast<std::uint8_t>(item.kind));
            writer.u8(item.statically_proven ? 1u : 0u);
            writer.u16(0u);
        }
        for (const auto& indexed : world.frontier_order_index_) {
            if (indexed.second >= world.frontier_.size()) continue;
            const auto& item = world.frontier_[indexed.second];
            writer.u64(item.id.value);
            writer.u8(static_cast<std::uint8_t>(item.state));
            writer.u8(static_cast<std::uint8_t>(item.proof));
            writer.u8(static_cast<std::uint8_t>(item.severity));
            writer.u8(static_cast<std::uint8_t>(item.blocked_kind));
            writer.u8(item.runtime_evidence_required ? 1u : 0u);
            writer.u8(item.static_complete ? 1u : 0u);
            writer.u16(0u);
            writer.u64(item.dependency_generation);
            writer.u64(item.evidence_digest);
            writer.u32(item.fanout);
            writer.text(item.family);
            writer.text(item.owner);
            writer.text(item.site);
            writer.text(item.missing_proof);
            write_id_vector(writer, item.evidence);
            write_text_vector(writer, item.contracts);
            write_text_vector(writer, item.blocked_sites);
            write_text_vector(writer, item.blocked_functions);
            write_text_vector(writer, item.blocked_roots);
            write_text_vector(writer, item.blocked_modules);
            write_text_vector(writer, item.blocked_materializations);
            write_text_vector(writer, item.blocked_hardware);
            write_text_vector(writer, item.causal_chain);
            write_text_vector(writer, item.source_paths);
            write_text_vector(writer, item.source_symbols);
            write_text_vector(writer, item.invariants);
            write_text_vector(writer, item.acceptance_criteria);
        }
        if (writer.overflow || writer.position > materialization_world_max_binary_artifact_bytes ||
            writer.position < binary_header_bytes) {
            if (!output.empty()) output[0] = 0u;
            return {0u, false, true};
        }
        writer.patch_u64(16u, writer.position);
        writer.patch_u64(24u, writer.position - binary_header_bytes);
        writer.patch_u32(binary_checksum_offset,
                         crc32_without_checksum(output.first(writer.position)));
        if (writer.overflow) {
            if (!output.empty()) output[0] = 0u;
            return {0u, false, true};
        }
        return {writer.position, true, false};
    } catch (...) {
        if (!output.empty()) output[0] = 0u;
        return {0u, false, true};
    }
}

bool parse_agent_world_binary(const std::span<const std::uint8_t> input,
                              ExecutableMaterializationWorld& output) noexcept {
    if (input.size() < binary_header_bytes ||
        input.size() > materialization_world_max_binary_artifact_bytes)
        return false;
    try {
        BinaryReader reader{input};
        if (!reader.raw(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(binary_magic), 8u)))
            return false;
        std::uint32_t schema = 0u;
        std::uint32_t header = 0u;
        std::uint64_t total = 0u;
        std::uint64_t payload = 0u;
        std::uint32_t checksum = 0u;
        std::uint32_t persisted_error = 0u;
        std::uint64_t revision = 0u;
        std::uint64_t dependency_generation = 0u;
        std::uint64_t evidence_digest = 0u;
        std::uint32_t evidence_count = 0u;
        std::uint32_t node_count = 0u;
        std::uint32_t dependency_count = 0u;
        std::uint32_t frontier_count = 0u;
        if (!reader.u32(schema) || !reader.u32(header) || !reader.u64(total) ||
            !reader.u64(payload) || !reader.u32(checksum) ||
            !reader.u32(persisted_error) || !reader.u64(revision) ||
            !reader.u64(dependency_generation) || !reader.u64(evidence_digest) ||
            !reader.u32(evidence_count) || !reader.u32(node_count) ||
            !reader.u32(dependency_count) || !reader.u32(frontier_count))
            return false;
        if (schema != materialization_world_binary_schema_version ||
            header != binary_header_bytes || total != input.size() ||
            payload != input.size() - binary_header_bytes ||
            !valid_world_error_byte(persisted_error) ||
            evidence_count > materialization_world_max_evidence ||
            node_count > materialization_world_max_nodes ||
            dependency_count > materialization_world_max_edges ||
            frontier_count > materialization_world_max_frontier)
            return false;
        if (crc32_without_checksum(input) != checksum) return false;
        if (dependency_generation == 0u) return false;

        ExecutableMaterializationWorld parsed;
        const auto reserve_vector = [&](const std::size_t count,
                                        const std::size_t element_size) noexcept {
            return count <= std::numeric_limits<std::size_t>::max() / element_size &&
                   reader.reserve_bytes(count * element_size);
        };
        if (!reserve_vector(evidence_count, sizeof(EvidenceRecord)) ||
            !reserve_vector(node_count, sizeof(MaterializationNode)) ||
            !reserve_vector(dependency_count, sizeof(DependencyEdge)) ||
            !reserve_vector(frontier_count, sizeof(FrontierEntry))) {
            return false;
        }
        parsed.evidence_.reserve(evidence_count);
        parsed.nodes_.reserve(node_count);
        parsed.dependencies_.reserve(dependency_count);
        parsed.frontier_.reserve(frontier_count);
        for (std::uint32_t index = 0u; index < evidence_count; ++index) {
            EvidenceRecord item;
            std::uint8_t kind = 0u;
            std::uint8_t immutable = 0u;
            std::uint16_t reserved = 0u;
            if (!reader.u64(item.id.value) || !reader.u8(kind) || !reader.u8(immutable) ||
                !reader.u16(reserved) || reserved != 0u || immutable > 1u ||
                !valid_evidence_kind_byte(kind) ||
                !reader.text(item.canonical_identity, materialization_world_max_text_bytes))
                return false;
            item.kind = static_cast<EvidenceKind>(kind);
            item.immutable = immutable != 0u;
            parsed.evidence_.push_back(std::move(item));
        }
        for (std::uint32_t index = 0u; index < node_count; ++index) {
            MaterializationNode item;
            std::uint8_t kind = 0u;
            std::uint8_t proof = 0u;
            std::uint8_t completeness = 0u;
            std::uint8_t runtime_hint = 0u;
            if (!reader.u64(item.id.value) || !reader.u8(kind) || !reader.u8(proof) ||
                !reader.u8(completeness) || !reader.u8(runtime_hint) ||
                !valid_node_kind_byte(kind) || !valid_proof_byte(proof) ||
                !valid_completeness_byte(completeness) || runtime_hint > 1u ||
                !reader.u64(item.dependency_generation) ||
                !reader.u64(item.evidence_digest) ||
                !reader.text(item.canonical_identity, materialization_world_max_text_bytes) ||
                !reader.text(item.source_identity, materialization_world_max_text_bytes) ||
                !read_id_vector(reader, item.evidence,
                                materialization_world_max_evidence_per_item) ||
                !read_reverse_dependency_vector(reader, item.reverse_dependencies,
                                                materialization_world_max_edges))
                return false;
            item.kind = static_cast<MaterializationNodeKind>(kind);
            item.proof = static_cast<ProofClass>(proof);
            item.completeness = static_cast<Completeness>(completeness);
            item.runtime_hint_only = runtime_hint != 0u;
            if (!unique_ids(item.evidence) ||
                !unique_reverse_dependency_vector(item.reverse_dependencies))
                return false;
            std::sort(item.evidence.begin(), item.evidence.end());
            std::sort(item.reverse_dependencies.begin(),
                      item.reverse_dependencies.end(),
                      reverse_dependency_less);
            parsed.nodes_.push_back(std::move(item));
        }
        for (std::uint32_t index = 0u; index < dependency_count; ++index) {
            DependencyEdge item;
            std::uint8_t kind = 0u;
            std::uint8_t proven = 0u;
            std::uint16_t reserved = 0u;
            if (!reader.u64(item.from.value) || !reader.u64(item.to.value) ||
                !reader.u8(kind) || !reader.u8(proven) || !reader.u16(reserved) ||
                reserved != 0u || proven > 1u || !valid_dependency_kind_byte(kind))
                return false;
            item.kind = static_cast<DependencyKind>(kind);
            item.statically_proven = proven != 0u;
            parsed.dependencies_.push_back(item);
        }
        for (std::uint32_t index = 0u; index < frontier_count; ++index) {
            FrontierEntry item;
            std::uint8_t state = 0u;
            std::uint8_t proof = 0u;
            std::uint8_t severity = 0u;
            std::uint8_t blocked_kind = 0u;
            std::uint8_t runtime_required = 0u;
            std::uint8_t static_complete = 0u;
            std::uint16_t reserved = 0u;
            if (!reader.u64(item.id.value) || !reader.u8(state) || !reader.u8(proof) ||
                !reader.u8(severity) || !reader.u8(blocked_kind) ||
                !reader.u8(runtime_required) || !reader.u8(static_complete) ||
                !reader.u16(reserved) || reserved != 0u || runtime_required > 1u ||
                static_complete > 1u || !valid_frontier_state_byte(state) ||
                !valid_frontier_proof_byte(proof) ||
                !valid_frontier_severity_byte(severity) ||
                !valid_frontier_block_byte(blocked_kind) ||
                !reader.u64(item.dependency_generation) ||
                !reader.u64(item.evidence_digest) || !reader.u32(item.fanout) ||
                !reader.text(item.family, materialization_world_max_text_bytes) ||
                !reader.text(item.owner, materialization_world_max_text_bytes) ||
                !reader.text(item.site, materialization_world_max_text_bytes) ||
                !reader.text(item.missing_proof, materialization_world_max_text_bytes) ||
                !read_id_vector(reader, item.evidence,
                                materialization_world_max_evidence_per_item) ||
                !read_text_vector(reader, item.contracts,
                                  materialization_world_max_contracts) ||
                !read_text_vector(reader, item.blocked_sites,
                                  materialization_world_max_blocked_items) ||
                !read_text_vector(reader, item.blocked_functions,
                                  materialization_world_max_blocked_items) ||
                !read_text_vector(reader, item.blocked_roots,
                                  materialization_world_max_blocked_items) ||
                !read_text_vector(reader, item.blocked_modules,
                                  materialization_world_max_blocked_items) ||
                !read_text_vector(reader, item.blocked_materializations,
                                  materialization_world_max_blocked_items) ||
                !read_text_vector(reader, item.blocked_hardware,
                                  materialization_world_max_blocked_items) ||
                !read_text_vector(reader, item.causal_chain,
                                  materialization_world_max_causal_items) ||
                !read_text_vector(reader, item.source_paths,
                                  materialization_world_max_source_items) ||
                !read_text_vector(reader, item.source_symbols,
                                  materialization_world_max_source_items) ||
                !read_text_vector(reader, item.invariants,
                                  materialization_world_max_invariants) ||
                !read_text_vector(reader, item.acceptance_criteria,
                                  materialization_world_max_acceptance_criteria))
                return false;
            item.state = static_cast<FrontierState>(state);
            item.proof = static_cast<FrontierProof>(proof);
            item.severity = static_cast<FrontierSeverity>(severity);
            item.blocked_kind = static_cast<FrontierBlockKind>(blocked_kind);
            item.runtime_evidence_required = runtime_required != 0u;
            item.static_complete = static_complete != 0u;
            if (!unique_ids(item.evidence)) return false;
            std::sort(item.evidence.begin(), item.evidence.end());
            parsed.frontier_.push_back(std::move(item));
        }
        if (reader.failed || reader.position != input.size()) return false;
        parsed.last_error_ = static_cast<WorldModelError>(persisted_error);
        parsed.revision_ = revision;
        parsed.dependency_generation_ = dependency_generation;
        parsed.evidence_digest_ = evidence_digest;
        if (!parsed.rebuild_indexes() ||
            parsed.evidence_digest_ != canonical_evidence_digest(
                                           parsed.evidence_digest_accumulator_,
                                           !parsed.evidence_.empty()))
            return false;
        if (!parsed.validate()) return false;
        output = std::move(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool next_agent_task(const ExecutableMaterializationWorld& world,
                     AgentTaskView& output) noexcept {
    std::size_t written = 0u;
    return next_agent_tasks(world, std::span<AgentTaskView>{&output, 1u},
                            written);
}

bool next_agent_tasks(const ExecutableMaterializationWorld& world,
                      const std::span<AgentTaskView> output,
                      std::size_t& written) noexcept {
    written = 0u;
    for (auto& item : output) item = {};
    if (output.empty()) return false;
    if (!world.validate()) return false;
    const auto decision = evaluate_agent_decision_unchecked(world);
    if (!decision.focus) return false;

    const bool select_runtime =
        decision.kind == AgentDecisionKind::RequiresRuntimeEvidence;
    const auto text_sets_overlap = [](const auto& lhs,
                                      const auto& rhs) noexcept {
        return std::any_of(
            lhs.begin(), lhs.end(), [&](const auto& value) {
                return std::find(rhs.begin(), rhs.end(), value) != rhs.end();
            });
    };
    const auto hardware_resources_overlap = [](const auto& lhs,
                                               const auto& rhs) noexcept {
        // blocked_hardware is also the bounded human/agent explanation of a
        // hardware frontier.  Provider roles, expected symbols and missing
        // proofs describe work; they are not shared mutable resources.  The
        // hardware audit's canonical region/access/register descriptor is the
        // only concrete resource identity in this vector.  Owners and sites
        // are already compared through their structured fields above.
        const auto resource_identity = [](const std::string_view value) noexcept
            -> std::optional<
                std::tuple<std::string_view, std::string_view,
                           std::string_view>> {
            constexpr std::string_view region_prefix{"region="};
            constexpr std::string_view access_marker{";access="};
            constexpr std::string_view register_marker{";register="};
            if (!value.starts_with(region_prefix)) return std::nullopt;
            const auto region_end = value.find(';', region_prefix.size());
            if (region_end == std::string_view::npos) return std::nullopt;
            const auto access_begin = value.find(access_marker);
            if (access_begin == std::string_view::npos)
                return std::nullopt;
            const auto access_end = value.find(
                ';', access_begin + access_marker.size());
            if (access_end == std::string_view::npos)
                return std::nullopt;
            const auto register_begin = value.find(register_marker);
            const auto region = value.substr(
                region_prefix.size(), region_end - region_prefix.size());
            const auto access = value.substr(
                access_begin + access_marker.size(),
                access_end - access_begin - access_marker.size());
            const auto hardware_register =
                register_begin == std::string_view::npos
                    ? std::string_view{}
                    : value.substr(register_begin + register_marker.size());
            return std::tuple{region, access, hardware_register};
        };
        return std::any_of(
            lhs.begin(), lhs.end(), [&](const auto& value) {
                const auto left = resource_identity(value);
                if (!left.has_value()) return false;
                return std::any_of(
                    rhs.begin(), rhs.end(), [&](const auto& candidate) {
                        const auto right = resource_identity(candidate);
                        return right.has_value() &&
                               std::get<0>(*left) == std::get<0>(*right) &&
                               std::get<1>(*left) == std::get<1>(*right) &&
                               (std::get<2>(*left).empty() ||
                                std::get<2>(*right).empty() ||
                                std::get<2>(*left) == std::get<2>(*right));
                    });
            });
    };
    const auto task_owner = [](const FrontierEntry& entry)
        -> std::string_view {
        if (entry.family == "replacement-reachability" &&
            entry.blocked_functions.size() == 1u)
            return entry.blocked_functions.front();
        return entry.owner;
    };
    while (written < output.size()) {
        const FrontierEntry* selected = nullptr;
        for (const auto& entry : world.frontier()) {
            if (entry.state == FrontierState::Closed && entry.static_complete)
                continue;
            if (entry.proof == FrontierProof::ExplicitRejection) continue;
            const bool runtime = entry.runtime_evidence_required ||
                                 entry.state == FrontierState::ObservedHint ||
                                 entry.proof == FrontierProof::RuntimeObservation;
            if (runtime != select_runtime) continue;
            bool conflicts = false;
            for (std::size_t index = 0u; index < written; ++index) {
                const auto* const existing = output[index].frontier;
                if (existing == nullptr) continue;
                if (existing->id == entry.id ||
                    task_owner(*existing) == task_owner(entry) ||
                    text_sets_overlap(
                        existing->blocked_sites, entry.blocked_sites) ||
                    text_sets_overlap(
                        existing->blocked_functions,
                        entry.blocked_functions) ||
                    text_sets_overlap(
                        existing->blocked_materializations,
                        entry.blocked_materializations) ||
                    hardware_resources_overlap(
                        existing->blocked_hardware,
                        entry.blocked_hardware)) {
                    conflicts = true;
                    break;
                }
            }
            if (conflicts) continue;
            if (selected == nullptr ||
                frontier_task_priority_less(entry, *selected))
                selected = &entry;
        }
        if (selected == nullptr) break;
        auto item_decision = decision;
        item_decision.focus = selected->id;
        output[written++] = {item_decision, selected};
    }
    return written != 0u;
}

bool explain_frontier(const ExecutableMaterializationWorld& world,
                      const StableId frontier,
                      FrontierExplanationView& output) noexcept {
    output = {};
    if (!world.validate()) return false;
    output.frontier = find_frontier(world, frontier);
    if (output.frontier == nullptr) return false;
    output.related_evidence = output.frontier->evidence.size();
    for (const auto& node : world.nodes()) {
        bool related = false;
        for (const auto evidence : node.evidence)
            if (std::find(output.frontier->evidence.begin(),
                          output.frontier->evidence.end(), evidence) !=
                output.frontier->evidence.end()) {
                related = true;
                break;
            }
        if (related) ++output.related_nodes;
    }
    return true;
}

AgentDiffResult diff_agent_worlds(
    const ExecutableMaterializationWorld& before,
    const ExecutableMaterializationWorld& after,
    const std::span<AgentDiffEntry> output) noexcept {
    AgentDiffResult result;
    result.before_valid = before.validate();
    result.after_valid = after.validate();
    if (!result.before_valid || !result.after_valid) return result;
    auto emit = [&](const AgentDiffEntry& entry) noexcept {
        if (result.written < output.size()) output[result.written++] = entry;
        else result.truncated = true;
        ++result.total;
    };
    if (before.revision() != after.revision() ||
        before.dependency_generation() != after.dependency_generation() ||
        before.evidence_digest() != after.evidence_digest() ||
        before.last_error() != after.last_error())
        emit({AgentDiffEntityKind::WorldMetadata, AgentDiffChange::Changed, {}, {}, 0u});

    const auto equal_nodes = [&](const MaterializationNode& lhs,
                                 const MaterializationNode& rhs) noexcept {
        if (!equal_node(lhs, rhs)) return false;
        const auto lhs_index = before.node_index_.find(lhs.id);
        const auto rhs_index = after.node_index_.find(rhs.id);
        if (lhs_index == before.node_index_.end() ||
            rhs_index == after.node_index_.end())
            return false;
        const auto lhs_begin = before.reverse_dependency_index_.lower_bound(
            std::make_tuple(lhs.id, StableId{}, std::uint8_t{0u}));
        const auto rhs_begin = after.reverse_dependency_index_.lower_bound(
            std::make_tuple(rhs.id, StableId{}, std::uint8_t{0u}));
        auto lhs_reverse = lhs_begin;
        auto rhs_reverse = rhs_begin;
        while (lhs_reverse != before.reverse_dependency_index_.end() &&
               std::get<0>(lhs_reverse->first) == lhs.id &&
               rhs_reverse != after.reverse_dependency_index_.end() &&
               std::get<0>(rhs_reverse->first) == rhs.id) {
            if (std::get<1>(lhs_reverse->first) !=
                    std::get<1>(rhs_reverse->first) ||
                std::get<2>(lhs_reverse->first) !=
                    std::get<2>(rhs_reverse->first))
                return false;
            ++lhs_reverse;
            ++rhs_reverse;
        }
        const bool lhs_done =
            lhs_reverse == before.reverse_dependency_index_.end() ||
            std::get<0>(lhs_reverse->first) != lhs.id;
        const bool rhs_done =
            rhs_reverse == after.reverse_dependency_index_.end() ||
            std::get<0>(rhs_reverse->first) != rhs.id;
        return lhs_done && rhs_done &&
               lhs_index->second < before.nodes_.size() &&
               rhs_index->second < after.nodes_.size();
    };

    const auto diff_entity = [&](const auto& before_index,
                                 const auto& after_index,
                                 const auto& before_values,
                                 const auto& after_values,
                                 const AgentDiffEntityKind entity_kind,
                                 const auto& equal) noexcept {
        auto lhs_index = before_index.begin();
        auto rhs_index = after_index.begin();
        while (lhs_index != before_index.end() || rhs_index != after_index.end()) {
            const bool take_lhs =
                rhs_index == after_index.end() ||
                (lhs_index != before_index.end() &&
                 lhs_index->first < rhs_index->first);
            const bool take_rhs =
                lhs_index == before_index.end() ||
                (rhs_index != after_index.end() &&
                 rhs_index->first < lhs_index->first);
            StableId id{};
            using BeforeValue =
                typename std::decay_t<decltype(before_values)>::value_type;
            using AfterValue =
                typename std::decay_t<decltype(after_values)>::value_type;
            const BeforeValue* lhs = nullptr;
            const AfterValue* rhs = nullptr;
            if (take_lhs) {
                id = lhs_index->first;
                lhs = &before_values[lhs_index->second];
                ++lhs_index;
            } else if (take_rhs) {
                id = rhs_index->first;
                rhs = &after_values[rhs_index->second];
                ++rhs_index;
            } else {
                id = lhs_index->first;
                lhs = &before_values[lhs_index->second];
                rhs = &after_values[rhs_index->second];
                ++lhs_index;
                ++rhs_index;
            }
            if (lhs == nullptr || rhs == nullptr || !equal(*lhs, *rhs))
                emit({entity_kind,
                      lhs == nullptr ? AgentDiffChange::Added
                                     : rhs == nullptr ? AgentDiffChange::Removed
                                                      : AgentDiffChange::Changed,
                      id,
                      {},
                      0u});
        }
    };

    diff_entity(before.evidence_index_,
                after.evidence_index_,
                before.evidence_,
                after.evidence_,
                AgentDiffEntityKind::Evidence,
                equal_evidence);
    diff_entity(before.node_index_,
                after.node_index_,
                before.nodes_,
                after.nodes_,
                AgentDiffEntityKind::Node,
                equal_nodes);
    diff_entity(before.frontier_index_,
                after.frontier_index_,
                before.frontier_,
                after.frontier_,
                AgentDiffEntityKind::Frontier,
                equal_frontier);

    auto lhs_dependency = before.dependency_index_.begin();
    auto rhs_dependency = after.dependency_index_.begin();
    while (lhs_dependency != before.dependency_index_.end() ||
           rhs_dependency != after.dependency_index_.end()) {
        const bool take_lhs =
            rhs_dependency == after.dependency_index_.end() ||
            (lhs_dependency != before.dependency_index_.end() &&
             lhs_dependency->first < rhs_dependency->first);
        const bool take_rhs =
            lhs_dependency == before.dependency_index_.end() ||
            (rhs_dependency != after.dependency_index_.end() &&
             rhs_dependency->first < lhs_dependency->first);
        const DependencyEdge* lhs = nullptr;
        const DependencyEdge* rhs = nullptr;
        std::tuple<StableId, StableId, std::uint8_t> key{};
        if (take_lhs) {
            key = lhs_dependency->first;
            lhs = &before.dependencies_[lhs_dependency->second];
            ++lhs_dependency;
        } else if (take_rhs) {
            key = rhs_dependency->first;
            rhs = &after.dependencies_[rhs_dependency->second];
            ++rhs_dependency;
        } else {
            key = lhs_dependency->first;
            lhs = &before.dependencies_[lhs_dependency->second];
            rhs = &after.dependencies_[rhs_dependency->second];
            ++lhs_dependency;
            ++rhs_dependency;
        }
        if (lhs == nullptr || rhs == nullptr || !equal_dependency(*lhs, *rhs))
            emit({AgentDiffEntityKind::Dependency,
                  lhs == nullptr ? AgentDiffChange::Added
                                 : rhs == nullptr ? AgentDiffChange::Removed
                                                  : AgentDiffChange::Changed,
                  std::get<0>(key),
                  std::get<1>(key),
                  std::get<2>(key)});
    }
    result.complete = !result.truncated;
    return result;
}

} // namespace katana::agent
