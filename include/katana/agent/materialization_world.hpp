#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace katana::agent {

inline constexpr std::uint32_t materialization_world_schema_version = 2u;
inline constexpr std::uint32_t frontier_schema_version = 1u;
inline constexpr std::uint32_t materialization_world_binary_schema_version = 2u;
inline constexpr std::size_t materialization_world_max_binary_artifact_bytes =
    64u * 1024u * 1024u;

// These limits are an integrity boundary, not a promise about the size of a
// product analysis. A caller that needs more state must publish another
// bounded world shard; silently growing the agent input is not allowed.
inline constexpr std::size_t materialization_world_max_nodes = 4096u;
inline constexpr std::size_t materialization_world_max_edges = 16384u;
inline constexpr std::size_t materialization_world_max_evidence = 8192u;
inline constexpr std::size_t materialization_world_max_frontier = 4096u;
inline constexpr std::size_t materialization_world_max_contracts = 8u;
inline constexpr std::size_t materialization_world_max_evidence_per_item = 16u;
inline constexpr std::size_t materialization_world_max_blocked_items = 32u;
inline constexpr std::size_t materialization_world_max_causal_items = 32u;
inline constexpr std::size_t materialization_world_max_source_items = 32u;
inline constexpr std::size_t materialization_world_max_invariants = 32u;
inline constexpr std::size_t materialization_world_max_acceptance_criteria = 32u;
inline constexpr std::size_t materialization_world_max_text_bytes = 4096u;

struct StableId final {
    std::uint64_t value = 0u;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value != 0u;
    }
    friend constexpr bool operator==(StableId, StableId) noexcept = default;
    friend constexpr bool operator!=(StableId, StableId) noexcept = default;
    friend constexpr bool operator<(StableId lhs, StableId rhs) noexcept {
        return lhs.value < rhs.value;
    }
};

enum class EntityKind : std::uint8_t {
    Node,
    Evidence,
    Frontier,
};

enum class MaterializationNodeKind : std::uint8_t {
    Image,
    Module,
    Function,
    Block,
    Provider,
    Hook,
    HardwareOwner,
    RenderFamily,
    AnalysisRoot,
    Unknown,
};

enum class ProofClass : std::uint8_t {
    // UnknownMaterialization is also the only legacy RuntimeOnly value. It
    // is intentionally not a proof and can never make a complete node.
    UnknownMaterialization,
    // GuardedPartial is a bounded candidate, not a closure proof.
    GuardedPartial,
    // GuardedComplete is statically bounded but still requires its explicit
    // runtime guard contract at the product boundary.
    GuardedComplete,
    // ProvenExact is an immutable exact static proof; NativeReplaced is an
    // identity-bound native replacement proof.
    ProvenExact,
    NativeReplaced,
    Rejected,

    // Source compatibility for the first world-model draft. These aliases
    // intentionally retain the new fail-closed semantics.
    Unknown = UnknownMaterialization,
    StaticCandidate = GuardedPartial,
    StaticProven = ProvenExact,
    IdentityBound = NativeReplaced,
    RuntimeOnly = UnknownMaterialization,
};

enum class Completeness : std::uint8_t {
    Unknown,
    Partial,
    Complete,
    Rejected,
};

enum class EvidenceKind : std::uint8_t {
    Disassembly,
    StaticAnalysis,
    IdentityBinding,
    SourceContract,
    RuntimeObservation,
    UserHint,
};

enum class DependencyKind : std::uint8_t {
    Calls,
    Materializes,
    Replaces,
    Requires,
    Produces,
    Observes,
};

struct ReverseDependency final {
    StableId from{};
    DependencyKind kind = DependencyKind::Requires;

    friend constexpr bool operator==(ReverseDependency,
                                     ReverseDependency) noexcept = default;
};

enum class FrontierBlockKind : std::uint8_t {
    Site,
    Function,
    Root,
    Module,
    Materialization,
    Hardware,
    Unknown,
};

enum class FrontierState : std::uint8_t {
    Open,
    Candidate,
    Blocked,
    Closed,
    ObservedHint,
};

enum class FrontierProof : std::uint8_t {
    None,
    StaticDisassembly,
    StaticAnalyzer,
    IdentityBound,
    RuntimeObservation,
    ExplicitRejection,
};

enum class FrontierSeverity : std::uint8_t {
    P0,
    P1,
    P2,
    P3,
};

enum class AgentDecisionKind : std::uint8_t {
    ContinueStaticIteration,
    RequiresRuntimeEvidence,
    BuildPort,
};

enum class WorldModelError : std::uint8_t {
    None,
    CapacityExceeded,
    InvalidIdentity,
    InvalidStableId,
    StableIdCollision,
    MissingReference,
    DuplicateEntry,
    InvalidProof,
    InvalidCompleteness,
    InvalidFrontier,
    InvalidDependency,
    InvalidArtifact,
    ArtifactChecksumMismatch,
    ArtifactSchemaMismatch,
    ArtifactTooLarge,
};

[[nodiscard]] StableId stable_id(EntityKind kind,
                                 std::string_view canonical_key) noexcept;
[[nodiscard]] StableId frontier_id(std::string_view family,
                                   std::string_view owner,
                                   std::string_view site) noexcept;

[[nodiscard]] const char* materialization_node_kind_name(
    MaterializationNodeKind kind) noexcept;
[[nodiscard]] const char* proof_class_name(ProofClass proof) noexcept;
[[nodiscard]] const char* completeness_name(Completeness completeness) noexcept;
[[nodiscard]] const char* evidence_kind_name(EvidenceKind kind) noexcept;
[[nodiscard]] const char* dependency_kind_name(DependencyKind kind) noexcept;
[[nodiscard]] const char* frontier_block_kind_name(FrontierBlockKind kind) noexcept;
[[nodiscard]] const char* frontier_state_name(FrontierState state) noexcept;
[[nodiscard]] const char* frontier_proof_name(FrontierProof proof) noexcept;
[[nodiscard]] const char* frontier_severity_name(FrontierSeverity severity) noexcept;
[[nodiscard]] const char* agent_decision_kind_name(AgentDecisionKind kind) noexcept;
[[nodiscard]] const char* world_model_error_name(WorldModelError error) noexcept;

[[nodiscard]] constexpr bool is_static_proof(ProofClass proof) noexcept {
    return proof == ProofClass::ProvenExact ||
           proof == ProofClass::GuardedComplete ||
           proof == ProofClass::NativeReplaced;
}

[[nodiscard]] constexpr bool is_static_frontier_proof(FrontierProof proof) noexcept {
    return proof == FrontierProof::StaticDisassembly ||
           proof == FrontierProof::StaticAnalyzer ||
           proof == FrontierProof::IdentityBound;
}

struct EvidenceRecord final {
    StableId id{};
    EvidenceKind kind = EvidenceKind::Disassembly;
    std::string canonical_identity;
    bool immutable = false;
};

struct MaterializationNode final {
    StableId id{};
    MaterializationNodeKind kind = MaterializationNodeKind::Unknown;
    std::string canonical_identity;
    std::string source_identity;
    ProofClass proof = ProofClass::Unknown;
    Completeness completeness = Completeness::Unknown;
    bool runtime_hint_only = false;
    std::uint64_t dependency_generation = 0u;
    std::uint64_t evidence_digest = 0u;
    std::vector<StableId> evidence;
    // Reverse edges retain their kind. The same source/target pair may have
    // one reverse entry for each distinct DependencyKind.
    std::vector<ReverseDependency> reverse_dependencies;
};

struct DependencyEdge final {
    StableId from{};
    StableId to{};
    DependencyKind kind = DependencyKind::Requires;
    bool statically_proven = false;
};

struct FrontierEntry final {
    StableId id{};
    std::string family;
    std::string owner;
    std::string site;
    FrontierState state = FrontierState::Open;
    FrontierProof proof = FrontierProof::None;
    FrontierSeverity severity = FrontierSeverity::P1;
    std::string missing_proof;
    std::uint32_t fanout = 0u;
    bool runtime_evidence_required = false;
    bool static_complete = false;
    FrontierBlockKind blocked_kind = FrontierBlockKind::Unknown;
    std::uint64_t dependency_generation = 0u;
    std::uint64_t evidence_digest = 0u;
    std::vector<StableId> evidence;
    std::vector<std::string> contracts;
    std::vector<std::string> blocked_sites;
    std::vector<std::string> blocked_functions;
    std::vector<std::string> blocked_roots;
    std::vector<std::string> blocked_modules;
    std::vector<std::string> blocked_materializations;
    std::vector<std::string> blocked_hardware;
    std::vector<std::string> causal_chain;
    std::vector<std::string> source_paths;
    std::vector<std::string> source_symbols;
    std::vector<std::string> invariants;
    std::vector<std::string> acceptance_criteria;
};

struct AgentDecision final {
    AgentDecisionKind kind = AgentDecisionKind::ContinueStaticIteration;
    StableId focus{};
    std::string_view reason;
    std::size_t actionable_frontier = 0u;
};

struct BoundedJsonResult final {
    std::size_t bytes = 0u;
    bool complete = false;
    bool truncated = false;
};

struct BoundedBinaryResult final {
    std::size_t bytes = 0u;
    bool complete = false;
    bool truncated = false;
};

enum class AgentDiffEntityKind : std::uint8_t {
    WorldMetadata,
    Evidence,
    Node,
    Dependency,
    Frontier,
};

enum class AgentDiffChange : std::uint8_t {
    Added,
    Removed,
    Changed,
};

struct AgentDiffEntry final {
    AgentDiffEntityKind entity = AgentDiffEntityKind::WorldMetadata;
    AgentDiffChange change = AgentDiffChange::Changed;
    StableId id{};
    StableId related{};
    std::uint8_t variant = 0u;
};

struct AgentDiffResult final {
    std::size_t written = 0u;
    std::size_t total = 0u;
    bool complete = false;
    bool truncated = false;
    bool before_valid = false;
    bool after_valid = false;
};

struct AgentTaskView final {
    AgentDecision decision{};
    const FrontierEntry* frontier = nullptr;
};

struct FrontierExplanationView final {
    const FrontierEntry* frontier = nullptr;
    std::size_t related_nodes = 0u;
    std::size_t related_evidence = 0u;
};

class ExecutableMaterializationWorld;

[[nodiscard]] BoundedBinaryResult serialize_agent_world_binary(
    const ExecutableMaterializationWorld& world,
    std::span<std::uint8_t> output) noexcept;
// The binary format is little-endian, self-sized and CRC32-protected. A
// parser validates the complete graph before replacing output; checksum,
// schema, bounds, references and runtime-only proof invariants are all
// fail-closed. Unknown/ObservedHint entries remain non-static hints.
[[nodiscard]] bool parse_agent_world_binary(
    std::span<const std::uint8_t> input,
    ExecutableMaterializationWorld& output) noexcept;
// Serializes a complete, deterministic snapshot into caller-owned storage.
// The function never allocates, sorts, locks, or treats runtime observations
// as static proof. On overflow it returns {0,false,true}; partial bytes are
// not valid JSON and must be discarded by the caller.
[[nodiscard]] BoundedJsonResult serialize_agent_world_json(
    const ExecutableMaterializationWorld& world,
    std::span<char> output) noexcept;
[[nodiscard]] bool next_agent_task(const ExecutableMaterializationWorld& world,
                                   AgentTaskView& output) noexcept;
// Selects a deterministic bounded batch of independent tasks. Entries sharing
// one owner or one concrete blocked site/function/materialization/hardware item
// are not emitted together, so parallel agents cannot race on the same proof
// contract. The first item is identical to next_agent_task().
[[nodiscard]] bool next_agent_tasks(
    const ExecutableMaterializationWorld& world,
    std::span<AgentTaskView> output,
    std::size_t& written) noexcept;
[[nodiscard]] bool explain_frontier(const ExecutableMaterializationWorld& world,
                                    StableId frontier,
                                    FrontierExplanationView& output) noexcept;
[[nodiscard]] AgentDiffResult diff_agent_worlds(
    const ExecutableMaterializationWorld& before,
    const ExecutableMaterializationWorld& after,
    std::span<AgentDiffEntry> output) noexcept;

class ExecutableMaterializationWorld final {
public:
    ExecutableMaterializationWorld() = default;

    [[nodiscard]] bool add_evidence(EvidenceRecord evidence) noexcept;
    [[nodiscard]] bool add_node(MaterializationNode node) noexcept;
    [[nodiscard]] bool add_dependency(DependencyEdge edge) noexcept;
    [[nodiscard]] bool add_frontier(FrontierEntry entry) noexcept;

    // Runtime observations are deliberately represented as hints. They can
    // select work and explain a miss, but can never close a static frontier.
    [[nodiscard]] bool add_runtime_hint(std::string_view family,
                                         std::string_view owner,
                                         std::string_view site,
                                         std::string_view observation) noexcept;

    [[nodiscard]] bool attach_node_evidence(StableId node,
                                            StableId evidence) noexcept;
    [[nodiscard]] bool attach_frontier_evidence(StableId frontier,
                                                StableId evidence) noexcept;

    [[nodiscard]] const std::vector<EvidenceRecord>& evidence() const noexcept {
        return evidence_;
    }
    [[nodiscard]] const std::vector<MaterializationNode>& nodes() const noexcept {
        synchronize_dependency_generation();
        return nodes_;
    }
    [[nodiscard]] const std::vector<DependencyEdge>& dependencies() const noexcept {
        return dependencies_;
    }
    [[nodiscard]] const std::vector<FrontierEntry>& frontier() const noexcept {
        synchronize_dependency_generation();
        return frontier_;
    }

    [[nodiscard]] bool contains_node(StableId id) const noexcept;
    [[nodiscard]] bool contains_evidence(StableId id) const noexcept;
    [[nodiscard]] bool contains_frontier(StableId id) const noexcept;
    [[nodiscard]] bool collect_reverse_dependencies(
        StableId target,
        std::span<ReverseDependency> output,
        std::size_t& written) const noexcept;

    [[nodiscard]] bool validate() const noexcept;
    [[nodiscard]] WorldModelError last_error() const noexcept {
        return last_error_;
    }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    void set_revision(std::uint64_t revision) noexcept {
        revision_ = revision;
        validation_cached_ = false;
    }
    [[nodiscard]] std::uint64_t dependency_generation() const noexcept {
        return dependency_generation_;
    }
    [[nodiscard]] std::uint64_t evidence_digest() const noexcept {
        return evidence_digest_;
    }

private:
    friend BoundedBinaryResult serialize_agent_world_binary(
        const ExecutableMaterializationWorld& world,
        std::span<std::uint8_t> output) noexcept;
    friend bool parse_agent_world_binary(
        std::span<const std::uint8_t> input,
        ExecutableMaterializationWorld& output) noexcept;
    friend BoundedJsonResult serialize_agent_world_json(
        const ExecutableMaterializationWorld& world,
        std::span<char> output) noexcept;
    friend AgentDiffResult diff_agent_worlds(
        const ExecutableMaterializationWorld& before,
        const ExecutableMaterializationWorld& after,
        std::span<AgentDiffEntry> output) noexcept;

    using EntityIndex = std::map<StableId, std::size_t>;
    using DependencyIndexKey =
        std::tuple<StableId, StableId, std::uint8_t>;
    using DependencyIndex = std::map<DependencyIndexKey, std::size_t>;
    using ReverseDependencyIndexKey =
        std::tuple<StableId, StableId, std::uint8_t>;
    using ReverseDependencyIndex =
        std::map<ReverseDependencyIndexKey, std::size_t>;
    using FrontierOrderKey = std::tuple<std::uint8_t,
                                        std::uint8_t,
                                        std::uint8_t,
                                        std::uint32_t,
                                        StableId>;
    using FrontierOrderIndex = std::map<FrontierOrderKey, std::size_t>;

    static DependencyIndexKey dependency_index_key(
        const DependencyEdge& edge) noexcept;
    static ReverseDependencyIndexKey reverse_dependency_index_key(
        StableId target,
        const ReverseDependency& dependency) noexcept;
    static FrontierOrderKey frontier_order_key(
        const FrontierEntry& entry) noexcept;
    [[nodiscard]] bool rebuild_indexes() noexcept;
    void synchronize_dependency_generation() const noexcept;
    [[nodiscard]] bool validate_uncached() const noexcept;

    template <typename Callback>
    void for_each_reverse_dependency(StableId target,
                                     Callback&& callback) const noexcept {
        const auto node_index = node_index_.find(target);
        if (node_index == node_index_.end() ||
            node_index->second >= nodes_.size())
            return;
        const auto begin = reverse_dependency_index_.lower_bound(
            std::make_tuple(target, StableId{}, std::uint8_t{0u}));
        for (auto item = begin; item != reverse_dependency_index_.end();
             ++item) {
            if (std::get<0>(item->first) != target) break;
            const auto reverse_index = item->second;
            if (reverse_index >=
                nodes_[node_index->second].reverse_dependencies.size())
                return;
            callback(nodes_[node_index->second]
                         .reverse_dependencies[reverse_index]);
        }
    }

    std::vector<EvidenceRecord> evidence_;
    mutable std::vector<MaterializationNode> nodes_;
    std::vector<DependencyEdge> dependencies_;
    mutable std::vector<FrontierEntry> frontier_;
    EntityIndex evidence_index_;
    EntityIndex node_index_;
    EntityIndex frontier_index_;
    DependencyIndex dependency_index_;
    ReverseDependencyIndex reverse_dependency_index_;
    FrontierOrderIndex frontier_order_index_;
    WorldModelError last_error_ = WorldModelError::None;
    std::uint64_t revision_ = 0u;
    std::uint64_t dependency_generation_ = 1u;
    std::uint64_t evidence_digest_ = 0u;
    std::uint64_t evidence_digest_accumulator_ = 0u;
    mutable bool dependency_generation_dirty_ = false;
    mutable bool validation_cached_ = false;
    mutable bool validation_result_ = false;

    void fail(WorldModelError error) noexcept { last_error_ = error; }
};

[[nodiscard]] bool frontier_priority_less(const FrontierEntry& lhs,
                                          const FrontierEntry& rhs) noexcept;
[[nodiscard]] AgentDecision evaluate_agent_decision(
    const ExecutableMaterializationWorld& world) noexcept;

} // namespace katana::agent
