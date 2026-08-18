#include "katana/analysis/recursive_analysis.hpp"
#include "katana/analysis/code_address.hpp"

#include "katana/io/binary_reader.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/sh4/decoder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace katana::analysis {
namespace {

struct PendingAddress {
    std::uint32_t address = 0u;
    std::uint32_t incoming_address = 0u;
    std::optional<std::uint32_t> delay_slot_owner;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;

    bool operator==(const PendingAddress&) const = default;
};

struct PendingAddressHash {
    std::size_t operator()(const PendingAddress& value) const noexcept {
        auto hash = static_cast<std::size_t>(value.address);
        hash ^= static_cast<std::size_t>(value.incoming_address) + 0x9E3779B9u + (hash << 6u) +
                (hash >> 2u);
        hash ^= static_cast<std::size_t>(value.delay_slot_owner.value_or(0u)) + 0x9E3779B9u +
                (hash << 6u) + (hash >> 2u);
        hash ^= static_cast<std::size_t>(value.delay_slot_owner.has_value()) << 1u;
        hash ^= static_cast<std::size_t>(value.evidence) + 0x9E3779B9u + (hash << 6u) +
                (hash >> 2u);
        return hash;
    }
};

struct ExactFunctionRange {
    std::uint32_t start = 0u;
    std::uint64_t end = 0u;
};

const ExactFunctionRange*
containing_function_range(
    const std::span<const ExactFunctionRange> ranges,
    const std::uint32_t address) noexcept {
    const auto after = std::upper_bound(
        ranges.begin(),
        ranges.end(),
        address,
        [](const std::uint32_t candidate,
           const ExactFunctionRange& range) {
            return candidate < range.start;
        });
    if (after == ranges.begin()) return nullptr;
    const auto& range = *std::prev(after);
    return static_cast<std::uint64_t>(address) < range.end ? &range
                                                           : nullptr;
}

[[nodiscard]] bool exact_function_range_strictly_contains(
    const std::span<const ExactFunctionRange> ranges,
    const std::uint32_t address) noexcept {
    const auto* range = containing_function_range(ranges, address);
    return range != nullptr && address != range->start;
}

[[nodiscard]] std::string delay_slot_boundary_error(
    const std::uint32_t transfer_address,
    const std::uint32_t delay_address,
    const ExactFunctionRange& range) {
    std::ostringstream output;
    output << "Explizite Funktionsgrenze trennt einen Delay Slot: transfer=0x"
           << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << transfer_address
           << " delay=0x" << std::setw(8) << delay_address
           << " function=[0x" << std::setw(8) << range.start
           << ",0x" << std::setw(8) << range.end << ").";
    return output.str();
}

void enqueue(std::deque<PendingAddress>& pending,
             std::unordered_set<PendingAddress, PendingAddressHash>& scheduled,
             const std::uint32_t address,
             const std::uint32_t incoming_address,
             const std::optional<std::uint32_t> delay_slot_owner,
             const ControlFlowEvidence evidence) {
    PendingAddress work{address, incoming_address, delay_slot_owner, evidence};
    if (scheduled.insert(work).second) pending.push_back(std::move(work));
}

void enqueue_next(std::deque<PendingAddress>& pending,
                  std::unordered_set<PendingAddress, PendingAddressHash>& scheduled,
                  const std::uint32_t address,
                  const std::uint32_t distance,
                  const ControlFlowEvidence evidence,
                  const std::span<const ExactFunctionRange>
                      exact_function_ranges) {
    if (address <= std::numeric_limits<std::uint32_t>::max() - distance) {
        const auto next = address + distance;
        const auto* range =
            containing_function_range(exact_function_ranges, address);
        if (range == nullptr ||
            static_cast<std::uint64_t>(next) < range->end)
            enqueue(pending,
                    scheduled,
                    next,
                    address,
                    std::nullopt,
                    evidence);
    }
}

void add_range(std::vector<ClassifiedRange>& ranges,
               const std::uint64_t start,
               const std::uint64_t end,
               const DiscoveredByteKind kind) {
    if (start >= end) {
        return;
    }
    if (!ranges.empty()) {
        auto& previous = ranges.back();
        const auto previous_end =
            static_cast<std::uint64_t>(previous.start_address) + previous.size;
        if (previous.kind == kind && previous_end == start) {
            previous.size += end - start;
            return;
        }
    }
    ranges.push_back({static_cast<std::uint32_t>(start), end - start, kind});
}

void classify_image(const katana::io::ExecutableImage& image,
                    const std::span<const katana::sh4::DisassemblyLine> discovered,
                    RecursiveAnalysisResult& result) {
    for (const auto& segment : image.segments()) {
        const auto segment_start = static_cast<std::uint64_t>(segment.virtual_address);
        const auto segment_end = segment.end_address();
        if (segment.kind == katana::io::SegmentKind::Data) {
            add_range(result.ranges, segment_start, segment_end, DiscoveredByteKind::Data);
            continue;
        }
        if ((segment.kind != katana::io::SegmentKind::Code &&
             segment.kind != katana::io::SegmentKind::Mixed) ||
            !segment.permissions.executable) {
            add_range(result.ranges, segment_start, segment_end, DiscoveredByteKind::Unknown);
            continue;
        }

        auto cursor = segment_start;
        const auto committed_end = segment_start + segment.bytes.size();
        for (const auto& line : discovered) {
            const auto address = line.address;
            if (!segment.contains(address, 2u)) {
                continue;
            }
            const auto instruction_start = static_cast<std::uint64_t>(address);
            add_range(result.ranges, cursor, instruction_start, DiscoveredByteKind::Unknown);
            add_range(result.unreachable_code,
                      cursor,
                      std::min(instruction_start, committed_end),
                      DiscoveredByteKind::Unknown);
            add_range(
                result.ranges, instruction_start, instruction_start + 2u, DiscoveredByteKind::Code);
            cursor = instruction_start + 2u;
        }
        add_range(result.ranges, cursor, segment_end, DiscoveredByteKind::Unknown);
        add_range(result.unreachable_code, cursor, committed_end, DiscoveredByteKind::Unknown);
    }
}

void add_function_evidence(std::unordered_map<std::uint32_t, FunctionCandidate>& candidates,
                           const std::uint32_t address,
                           const FunctionOrigin origin,
                           const AnalysisConfidence confidence,
                           const ControlFlowEvidence evidence,
                           const std::uint32_t size = 0u) {
    auto& candidate = candidates[address];
    candidate.address = address;
    if (size != 0u) {
        if (candidate.size != 0u && candidate.size != size)
            throw std::invalid_argument(
                "Explizite Funktionsgrenzen widersprechen sich.");
        candidate.size = size;
    }
    if (static_cast<int>(confidence) > static_cast<int>(candidate.confidence)) {
        candidate.confidence = confidence;
    }
    if (control_flow_evidence_preferred_for_static_decode(evidence, candidate.evidence))
        candidate.evidence = evidence;
    if (std::find(candidate.origins.begin(), candidate.origins.end(), origin) ==
        candidate.origins.end()) {
        candidate.origins.push_back(origin);
        std::sort(candidate.origins.begin(), candidate.origins.end());
    }
}

[[nodiscard]] ControlFlowEvidence effective_seed_evidence(
    const AnalysisSeed& seed) noexcept {
    return seed.guarded_candidate &&
                   seed.evidence == ControlFlowEvidence::ProvenComplete
               ? ControlFlowEvidence::GuardedPartial
               : seed.evidence;
}

[[nodiscard]] std::vector<RecursiveAnalysisSeedContract>
canonical_seed_contract(
    const katana::io::ExecutableImage& image,
    const std::span<const AnalysisSeed> seeds) {
    std::vector<RecursiveAnalysisSeedContract> unmerged;
    unmerged.reserve(seeds.size());
    for (const auto& seed : seeds) {
        const auto validation =
            validate_committed_code_address(image, seed.address);
        if (!validation.valid()) continue;
        unmerged.push_back({validation.resolved_address,
                            seed.function_origins,
                            {effective_seed_evidence(seed)},
                            seed.function_size});
    }
    std::sort(unmerged.begin(),
              unmerged.end(),
              [](const auto& left, const auto& right) {
                  return left.address < right.address;
              });
    std::vector<RecursiveAnalysisSeedContract> result;
    result.reserve(unmerged.size());
    for (auto& contract : unmerged) {
        if (!result.empty() &&
            result.back().address == contract.address) {
            auto& merged = result.back();
            if (merged.function_size != 0u &&
                contract.function_size != 0u &&
                merged.function_size != contract.function_size)
                throw std::invalid_argument(
                    "Explizite Funktionsgrenzen widersprechen sich.");
            if (merged.function_size == 0u)
                merged.function_size = contract.function_size;
            merged.function_origins.insert(
                merged.function_origins.end(),
                contract.function_origins.begin(),
                contract.function_origins.end());
            merged.decode_evidences.insert(
                merged.decode_evidences.end(),
                contract.decode_evidences.begin(),
                contract.decode_evidences.end());
            continue;
        }
        result.push_back(std::move(contract));
    }
    for (auto& contract : result) {
        std::sort(contract.function_origins.begin(),
                  contract.function_origins.end());
        contract.function_origins.erase(
            std::unique(contract.function_origins.begin(),
                        contract.function_origins.end()),
            contract.function_origins.end());
        std::sort(contract.decode_evidences.begin(),
                  contract.decode_evidences.end());
        contract.decode_evidences.erase(
            std::unique(contract.decode_evidences.begin(),
                        contract.decode_evidences.end()),
            contract.decode_evidences.end());
    }
    return result;
}

[[nodiscard]] bool seed_contract_is_monotone_extension(
    const std::span<const RecursiveAnalysisSeedContract> baseline,
    const std::span<const RecursiveAnalysisSeedContract> current,
    bool* const evidence_replaced = nullptr) noexcept {
    if (evidence_replaced != nullptr) *evidence_replaced = false;
    std::size_t current_index = 0u;
    for (const auto& old : baseline) {
        while (current_index < current.size() &&
               current[current_index].address < old.address)
            ++current_index;
        if (current_index == current.size() ||
            current[current_index].address != old.address)
            return false;
        const auto& now = current[current_index];
        const auto evidence_is_covered =
            [&](const ControlFlowEvidence old_evidence) {
                if (std::binary_search(now.decode_evidences.begin(),
                                       now.decode_evidences.end(),
                                       old_evidence))
                    return true;
                const auto strengthened = std::any_of(
                    now.decode_evidences.begin(),
                    now.decode_evidences.end(),
                    [&](const ControlFlowEvidence current_evidence) {
                        return control_flow_evidence_preferred_for_static_decode(
                            current_evidence, old_evidence);
                    });
                if (strengthened && evidence_replaced != nullptr)
                    *evidence_replaced = true;
                return strengthened;
            };
        if (now.function_size != old.function_size ||
            !std::includes(now.function_origins.begin(),
                           now.function_origins.end(),
                           old.function_origins.begin(),
                           old.function_origins.end()) ||
            !std::all_of(old.decode_evidences.begin(),
                         old.decode_evidences.end(),
                         evidence_is_covered))
            return false;
    }
    // Introducing an exact range is structural even at a new target. It can
    // cut through already retained fallthrough/delay-slot contexts.
    for (const auto& now : current) {
        const auto old = std::lower_bound(
            baseline.begin(),
            baseline.end(),
            now.address,
            [](const RecursiveAnalysisSeedContract& contract,
               const std::uint32_t address) {
                return contract.address < address;
            });
        if (old == baseline.end() || old->address != now.address) {
            if (now.function_size != 0u) return false;
        }
    }
    return true;
}

[[nodiscard]] bool seed_contract_payload_is_canonical(
    const std::span<const RecursiveAnalysisSeedContract> contracts) noexcept {
    const auto valid_origin = [](const FunctionOrigin origin) noexcept {
        return origin >= FunctionOrigin::EntryPoint &&
               origin <= FunctionOrigin::StoredCodeAddress;
    };
    const auto valid_evidence =
        [](const ControlFlowEvidence evidence) noexcept {
            return evidence >= ControlFlowEvidence::ProvenComplete &&
                   evidence <= ControlFlowEvidence::Unresolved;
        };
    for (std::size_t index = 0u; index < contracts.size(); ++index) {
        const auto& contract = contracts[index];
        if ((contract.address & 1u) != 0u ||
            (contract.function_size & 1u) != 0u ||
            contract.decode_evidences.empty() ||
            (index != 0u &&
             contracts[index - 1u].address >= contract.address))
            return false;
        for (std::size_t origin = 0u;
             origin < contract.function_origins.size();
             ++origin) {
            if (!valid_origin(contract.function_origins[origin]) ||
                (origin != 0u &&
                 contract.function_origins[origin - 1u] >=
                     contract.function_origins[origin]))
                return false;
        }
        for (std::size_t evidence = 0u;
             evidence < contract.decode_evidences.size();
             ++evidence) {
            if (!valid_evidence(contract.decode_evidences[evidence]) ||
                (evidence != 0u &&
                 contract.decode_evidences[evidence - 1u] >=
                     contract.decode_evidences[evidence]))
                return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<AnalysisSeed> incremental_decode_delta(
    const std::span<const RecursiveAnalysisSeedContract> baseline,
    const std::span<const RecursiveAnalysisSeedContract> current) {
    std::vector<AnalysisSeed> result;
    for (const auto& now : current) {
        const auto old = std::lower_bound(
            baseline.begin(),
            baseline.end(),
            now.address,
            [](const RecursiveAnalysisSeedContract& contract,
               const std::uint32_t address) {
                return contract.address < address;
            });
        for (const auto evidence : now.decode_evidences) {
            if (old != baseline.end() && old->address == now.address &&
                std::binary_search(old->decode_evidences.begin(),
                                   old->decode_evidences.end(),
                                   evidence))
                continue;
            result.push_back(
                {now.address, {}, false, evidence, 0u});
        }
    }
    return result;
}

class BaselinePayloadEncoder final {
  public:
    template <typename T>
    void scalar(const T value) {
        using Value = std::remove_cv_t<T>;
        if constexpr (std::is_enum_v<Value>) {
            scalar(static_cast<std::underlying_type_t<Value>>(value));
        } else {
            static_assert(std::is_integral_v<Value>);
            using Unsigned = std::make_unsigned_t<Value>;
            auto bits = static_cast<Unsigned>(value);
            std::array<char, sizeof(Unsigned)> encoded{};
            for (std::size_t index = 0u; index < sizeof(Unsigned); ++index) {
                encoded[index] = static_cast<char>(bits & 0xFFu);
                bits >>= 8u;
            }
            append(encoded);
        }
    }

    void boolean(const bool value) {
        scalar<std::uint8_t>(value ? 1u : 0u);
    }

    void text(const std::string_view value) {
        scalar<std::uint64_t>(value.size());
        append(value);
    }

    void optional_address(
        const std::optional<std::uint32_t> value) {
        boolean(value.has_value());
        if (value.has_value()) scalar(*value);
    }

    [[nodiscard]] std::string finish() {
        flush();
        return hash_.finish();
    }

    [[nodiscard]] std::size_t encoded_bytes() const noexcept {
        return encoded_bytes_;
    }

  private:
    void append(std::string_view bytes) {
        encoded_bytes_ += bytes.size();
        while (!bytes.empty()) {
            const auto copied =
                std::min(buffer_.size() - buffer_size_, bytes.size());
            std::memcpy(buffer_.data() + buffer_size_, bytes.data(), copied);
            buffer_size_ += copied;
            bytes.remove_prefix(copied);
            if (buffer_size_ == buffer_.size()) flush();
        }
    }

    template <std::size_t Size>
    void append(const std::array<char, Size>& bytes) {
        append(std::string_view{bytes.data(), bytes.size()});
    }

    void flush() {
        if (buffer_size_ == 0u) return;
        hash_.update(std::string_view{buffer_.data(), buffer_size_});
        buffer_size_ = 0u;
    }

    static constexpr std::size_t buffer_capacity = 4096u;
    katana::io::Sha256Accumulator hash_;
    std::array<char, buffer_capacity> buffer_{};
    std::size_t buffer_size_ = 0u;
    std::size_t encoded_bytes_ = 0u;
};

void encode_baseline_line(BaselinePayloadEncoder& encoder,
                          const katana::sh4::DisassemblyLine& line) {
    encoder.scalar(line.address);
    encoder.scalar(line.opcode);
    encoder.scalar(line.instruction.opcode);
    encoder.scalar(line.instruction.kind);
    encoder.scalar(line.instruction.destination_register);
    encoder.scalar(line.instruction.source_register);
    encoder.scalar(line.instruction.branch_register);
    encoder.scalar(line.instruction.immediate);
    encoder.scalar(line.instruction.displacement);
    encoder.scalar(line.instruction.special_register);
    encoder.scalar(line.instruction.control_flow);
    encoder.boolean(line.instruction.has_delay_slot);
    encoder.boolean(line.instruction.is_privileged);
    encoder.text(line.instruction.text);
    encoder.boolean(line.is_delay_slot);
    encoder.optional_address(line.target_address);
}

[[nodiscard]] std::string baseline_payload_digest(
    const RecursiveAnalysisResult& result,
    std::size_t* const encoded_bytes = nullptr) {
    BaselinePayloadEncoder encoder;
    encoder.scalar(result.retained_baseline_contract_version);
    encoder.scalar(result.source_image_identity);
    encoder.scalar(result.source_image_revision);
    encoder.scalar(result.limit);
    encoder.scalar<std::uint64_t>(result.seed_contract.size());
    for (const auto& contract : result.seed_contract) {
        encoder.scalar(contract.address);
        encoder.scalar(contract.function_size);
        encoder.scalar<std::uint64_t>(
            contract.function_origins.size());
        for (const auto origin : contract.function_origins)
            encoder.scalar(origin);
        encoder.scalar<std::uint64_t>(
            contract.decode_evidences.size());
        for (const auto evidence : contract.decode_evidences)
            encoder.scalar(evidence);
    }
    encoder.scalar<std::uint64_t>(result.instructions.size());
    for (const auto& line : result.instructions)
        encode_baseline_line(encoder, line);
    encoder.scalar<std::uint64_t>(
        result.contextual_instructions.size());
    for (const auto& contextual : result.contextual_instructions) {
        encode_baseline_line(encoder, contextual.line);
        encoder.scalar(contextual.incoming_address);
        encoder.optional_address(contextual.delay_slot_owner);
        encoder.scalar(contextual.evidence);
    }
    encoder.scalar<std::uint64_t>(result.functions.size());
    for (const auto& function : result.functions) {
        encoder.scalar(function.address);
        encoder.scalar(function.confidence);
        encoder.scalar(function.evidence);
        encoder.scalar(function.size);
        encoder.scalar<std::uint64_t>(function.origins.size());
        for (const auto origin : function.origins)
            encoder.scalar(origin);
    }
    encoder.scalar<std::uint64_t>(result.diagnostics.size());
    for (const auto& diagnostic : result.diagnostics) {
        encoder.scalar(diagnostic.address);
        encoder.scalar(diagnostic.opcode);
        encoder.scalar(diagnostic.kind);
        encoder.text(diagnostic.reason);
        encoder.scalar(diagnostic.evidence);
    }
    auto digest = encoder.finish();
    if (encoded_bytes != nullptr)
        *encoded_bytes += encoder.encoded_bytes();
    return digest;
}

} // namespace

RecursiveAnalysisResult analyze_reachable_code(const katana::io::ExecutableImage& image,
                                               const RecursiveAnalysisOptions& options) {
    RecursiveAnalysisPhysicalWork physical_work;
    physical_work.seed_contract_items_visited =
        options.additional_seeds.size();
    physical_work.seed_arena_copy_items = options.additional_seeds.size();
    physical_work.seed_arena_copy_bytes =
        options.additional_seeds.size() * sizeof(AnalysisSeed);
    physical_work.public_sort_items = options.additional_seeds.size();
    std::deque<PendingAddress> pending;
    std::unordered_set<PendingAddress, PendingAddressHash> scheduled;
    std::unordered_set<std::uint32_t> delay_slots;
    std::unordered_map<std::uint32_t, katana::sh4::DisassemblyLine> discovered;
    std::unordered_map<std::uint32_t, FunctionCandidate> function_candidates;
    auto seed_contract = canonical_seed_contract(
        image, options.additional_seeds);
    const RecursiveAnalysisResult* baseline = nullptr;
    auto baseline_status = RecursiveAnalysisBaselineStatus::NotRequested;
    bool baseline_evidence_replaced = false;
    if (options.baseline != nullptr) {
        const auto& candidate = *options.baseline;
        if (candidate.retained_baseline_contract_version !=
            RecursiveAnalysisResult::baseline_contract_version) {
            baseline_status =
                RecursiveAnalysisBaselineStatus::IncompleteBaseline;
        } else if (candidate.source_image_identity !=
                   image.analysis_instance_identity()) {
            baseline_status = RecursiveAnalysisBaselineStatus::
                ImageIdentityMismatch;
        } else if (candidate.source_image_revision !=
                   image.analysis_revision()) {
            baseline_status = RecursiveAnalysisBaselineStatus::
                ImageRevisionMismatch;
        } else if (candidate.limit != RecursiveAnalysisLimit::None) {
            baseline_status =
                RecursiveAnalysisBaselineStatus::IncompleteBaseline;
        } else if (candidate.instructions.size() >
                       options.maximum_instructions ||
                   candidate.contextual_instructions.size() >
                       options.maximum_contexts) {
            baseline_status =
                RecursiveAnalysisBaselineStatus::BudgetIncompatible;
        } else if (!seed_contract_payload_is_canonical(
                       candidate.seed_contract)) {
            baseline_status =
                RecursiveAnalysisBaselineStatus::PayloadMismatch;
        } else if (!seed_contract_is_monotone_extension(
                       candidate.seed_contract, seed_contract,
                       &baseline_evidence_replaced)) {
            baseline_status =
                RecursiveAnalysisBaselineStatus::SeedContractMismatch;
        } else {
            const auto digest = baseline_payload_digest(
                candidate, &physical_work.public_baseline_hash_bytes);
            if (candidate.retained_baseline_payload_sha256_.empty() ||
                candidate.retained_baseline_payload_sha256_ != digest) {
                baseline_status =
                    RecursiveAnalysisBaselineStatus::PayloadMismatch;
            } else {
                baseline = &candidate;
                baseline_status = RecursiveAnalysisBaselineStatus::Reused;
            }
        }
    }
    const auto incremental_seeds =
        baseline == nullptr
            ? std::vector<AnalysisSeed>{}
            : incremental_decode_delta(
                  baseline->seed_contract, seed_contract);
    const auto baseline_context_count = baseline == nullptr
                                            ? 0u
                                            : baseline->contextual_instructions.size();
    const auto baseline_instruction_count =
        baseline == nullptr ? 0u : baseline->instructions.size();
    const auto baseline_function_count =
        baseline == nullptr ? 0u : baseline->functions.size();
    delay_slots.reserve(baseline_context_count + 1024u);
    discovered.reserve(baseline_instruction_count + 4096u);
    function_candidates.reserve(baseline_function_count + 256u);

    std::vector<AnalysisDiagnostic> diagnostics;
    std::vector<ContextualInstruction> result_contexts;
    result_contexts.reserve(baseline_context_count + 4096u);
    std::size_t reused_contexts = 0u;
    std::size_t processed_work_items = 0u;
    scheduled.reserve(baseline_context_count + 4096u);
    std::vector<ExactFunctionRange> exact_function_ranges;
    exact_function_ranges.reserve(options.additional_seeds.size());
    for (const auto& seed : options.additional_seeds) {
        if (seed.function_size == 0u) continue;
        if ((seed.function_size & 1u) != 0u)
            throw std::invalid_argument(
                "Explizite Funktionsgrenze besitzt eine ungerade Groesse.");
        const auto validation = validate_committed_code_address(
            image, seed.address, seed.function_size);
        if (!validation.valid())
            throw std::invalid_argument(
                "Explizite Funktionsgrenze ist nicht vollstaendig "
                "dekodierbar: " +
                std::string(
                    code_address_status_name(validation.status)) +
                ".");
        exact_function_ranges.push_back(
            {validation.resolved_address,
             static_cast<std::uint64_t>(
                 validation.resolved_address) +
                 seed.function_size});
    }
    std::sort(
        exact_function_ranges.begin(),
        exact_function_ranges.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.start, left.end) <
                   std::tie(right.start, right.end);
        });
    exact_function_ranges.erase(
        std::unique(
            exact_function_ranges.begin(),
            exact_function_ranges.end(),
            [](const auto& left, const auto& right) {
                return left.start == right.start && left.end == right.end;
            }),
        exact_function_ranges.end());
    for (std::size_t index = 1u;
         index < exact_function_ranges.size();
         ++index) {
        if (exact_function_ranges[index].start <
            exact_function_ranges[index - 1u].end)
            throw std::invalid_argument(
                "Explizite Funktionsgrenzen ueberlappen.");
    }
    if (baseline != nullptr) {
        physical_work.public_baseline_copy_items +=
            baseline->diagnostics.size() +
            baseline->contextual_instructions.size() +
            baseline->instructions.size() + baseline->functions.size();
        diagnostics = baseline->diagnostics;
        result_contexts = baseline->contextual_instructions;
        for (const auto& contextual : baseline->contextual_instructions) {
            scheduled.insert({contextual.line.address,
                              contextual.incoming_address,
                              contextual.delay_slot_owner,
                              contextual.evidence});
            if (contextual.delay_slot_owner.has_value()) {
                delay_slots.insert(contextual.line.address);
            }
        }
        reused_contexts = scheduled.size();
        for (const auto& line : baseline->instructions) {
            discovered.emplace(line.address, line);
        }
        for (const auto& function : baseline->functions) {
            function_candidates.emplace(function.address, function);
        }
    }

    for (const auto entry : image.entry_points()) {
        const auto validation = validate_committed_code_address(image, entry);
        if (!validation.valid()) {
            throw std::invalid_argument("Analyse-Einstiegspunkt ist ungueltig: " +
                                        std::string(code_address_status_name(validation.status)) +
                                        ".");
        }
        const auto resolved_entry = validation.resolved_address;
        add_function_evidence(function_candidates,
                              resolved_entry,
                              FunctionOrigin::EntryPoint,
                              AnalysisConfidence::Certain,
                              ControlFlowEvidence::ProvenComplete);
        enqueue(pending,
                scheduled,
                resolved_entry,
                resolved_entry,
                std::nullopt,
                ControlFlowEvidence::ProvenComplete);
    }
    for (const auto& symbol : image.symbols()) {
        if (symbol.kind != katana::io::SymbolKind::Function || (symbol.address & 1u) != 0u) {
            continue;
        }
        const auto validation = validate_committed_code_address(image, symbol.address);
        if (!validation.valid()) continue;
        const auto resolved_address = validation.resolved_address;
        enqueue(pending,
                scheduled,
                resolved_address,
                resolved_address,
                std::nullopt,
                ControlFlowEvidence::ProvenComplete);
        add_function_evidence(function_candidates,
                              resolved_address,
                              FunctionOrigin::Symbol,
                              AnalysisConfidence::High,
                              ControlFlowEvidence::ProvenComplete);
    }
    // The caller-provided delta is deliberately advisory. A missing baseline,
    // a rejected baseline, or an incomplete hint must never suppress a seed
    // from the complete semantic contract.
    const auto& decode_seeds = baseline == nullptr
                                   ? options.additional_seeds
                                   : incremental_seeds;
    for (const auto& seed : decode_seeds) {
        const auto validation = validate_committed_code_address(image, seed.address);
        if (!validation.valid()) continue;
        const auto resolved_address = validation.resolved_address;
        const auto evidence =
            seed.guarded_candidate && seed.evidence == ControlFlowEvidence::ProvenComplete
                ? ControlFlowEvidence::GuardedPartial
                : seed.evidence;
        enqueue(pending, scheduled, resolved_address, resolved_address, std::nullopt, evidence);
    }
    for (const auto& seed : options.additional_seeds) {
        const auto validation = validate_committed_code_address(image, seed.address);
        if (!validation.valid()) continue;
        const auto resolved_address = validation.resolved_address;
        const auto evidence =
            seed.guarded_candidate && seed.evidence == ControlFlowEvidence::ProvenComplete
                ? ControlFlowEvidence::GuardedPartial
                : seed.evidence;
        if (seed.function_size != 0u) {
            auto& candidate = function_candidates[resolved_address];
            candidate.address = resolved_address;
            if (candidate.size != 0u &&
                candidate.size != seed.function_size)
                throw std::invalid_argument(
                    "Explizite Funktionsgrenzen widersprechen sich.");
            candidate.size = seed.function_size;
        }
        for (const auto origin : seed.function_origins) {
            const auto confidence =
                origin == FunctionOrigin::UserOverride ? AnalysisConfidence::Certain
                : origin == FunctionOrigin::GuardedSnapshot ||
                        origin == FunctionOrigin::RuntimeCopy ||
                        origin == FunctionOrigin::StoredCodeAddress
                    ? AnalysisConfidence::Medium
                    : AnalysisConfidence::High;
            add_function_evidence(
                function_candidates,
                resolved_address,
                origin,
                confidence,
                evidence,
                seed.function_size);
        }
    }

    auto limit = RecursiveAnalysisLimit::None;
    if (discovered.size() > options.maximum_instructions)
        limit = RecursiveAnalysisLimit::InstructionBudgetExceeded;
    else if (result_contexts.size() > options.maximum_contexts)
        limit = RecursiveAnalysisLimit::ContextBudgetExceeded;

    while (!pending.empty() &&
           limit == RecursiveAnalysisLimit::None) {
        const auto work = pending.front();
        pending.pop_front();
        ++processed_work_items;
        ++physical_work.decoded_work_items;
        const auto evidence = work.evidence;
        const auto validation = validate_decode_candidate(image, work.address);
        if (!validation.valid()) {
            continue;
        }
        const auto address = validation.resolved_address;
        const auto* segment = validation.segment;
        const auto offset = *segment->byte_offset(address);
        const auto opcode = katana::io::read_u16_le(segment->bytes, offset);

        katana::sh4::DisassemblyLine line;
        line.address = address;
        line.opcode = opcode;
        line.instruction = katana::sh4::decode(opcode);
        line.is_delay_slot = work.delay_slot_owner.has_value();
        line.target_address =
            katana::sh4::calculate_direct_branch_target(line.instruction, address);
        if (!discovered.contains(address) &&
            discovered.size() >= options.maximum_instructions) {
            limit =
                RecursiveAnalysisLimit::InstructionBudgetExceeded;
            break;
        }
        if (result_contexts.size() >= options.maximum_contexts) {
            limit = RecursiveAnalysisLimit::ContextBudgetExceeded;
            break;
        }
        const auto [legacy, inserted] = discovered.emplace(address, line);
        if (!inserted && !line.is_delay_slot) legacy->second = line;
        if (inserted || !line.is_delay_slot)
            ++physical_work.canonical_instruction_updates;

        ContextualInstruction contextual;
        contextual.line = line;
        contextual.incoming_address = work.incoming_address;
        contextual.delay_slot_owner = work.delay_slot_owner;
        contextual.evidence = evidence;
        result_contexts.push_back(std::move(contextual));
        ++physical_work.canonical_context_updates;

        if (!line.instruction.is_known()) {
            diagnostics.push_back({address,
                                   opcode,
                                   AnalysisDiagnosticKind::UnknownOpcode,
                                   "unknown-opcode",
                                   evidence});
            continue;
        }

        if (line.is_delay_slot) {
            delay_slots.insert(address);
            if (line.instruction.changes_control_flow())
                diagnostics.push_back({address,
                                       opcode,
                                       AnalysisDiagnosticKind::ControlFlowInDelaySlot,
                                       "control-flow-in-delay-slot",
                                       evidence});
            continue;
        }

        const auto fallthrough_distance = line.instruction.has_delay_slot ? 4u : 2u;
        if (line.instruction.has_delay_slot) {
            if (address <= std::numeric_limits<std::uint32_t>::max() - 2u) {
                const auto delay_address = address + 2u;
                if (const auto* range = containing_function_range(
                        exact_function_ranges, address);
                    range != nullptr &&
                    static_cast<std::uint64_t>(delay_address) >=
                        range->end)
                    throw std::invalid_argument(
                        delay_slot_boundary_error(
                            address, delay_address, *range));
                enqueue(pending, scheduled, delay_address, address, address, evidence);
                const auto delay_validation = validate_committed_code_address(image, delay_address);
                if (!delay_validation.valid()) {
                    diagnostics.push_back({address,
                                           opcode,
                                           AnalysisDiagnosticKind::DelaySlotUnavailable,
                                           "delay-slot-unavailable",
                                           evidence});
                    continue;
                }
                const auto delay_offset = *delay_validation.segment->byte_offset(delay_address);
                const auto delay_opcode =
                    katana::io::read_u16_le(delay_validation.segment->bytes, delay_offset);
                if (!katana::sh4::decode(delay_opcode).is_known()) {
                    diagnostics.push_back({address,
                                           opcode,
                                           AnalysisDiagnosticKind::DelaySlotUnknownOpcode,
                                           "delay-slot-unknown-opcode",
                                           evidence});
                    continue;
                }
            }
        }

        switch (line.instruction.control_flow) {
        case katana::sh4::ControlFlowKind::None:
            enqueue_next(pending,
                         scheduled,
                         address,
                         2u,
                         evidence,
                         exact_function_ranges);
            break;
        case katana::sh4::ControlFlowKind::ConditionalBranch:
            if (line.target_address.has_value()) {
                enqueue(pending, scheduled, *line.target_address, address, std::nullopt, evidence);
            }
            enqueue_next(pending,
                         scheduled,
                         address,
                         fallthrough_distance,
                         evidence,
                         exact_function_ranges);
            break;
        case katana::sh4::ControlFlowKind::Call:
            if (line.target_address.has_value()) {
                enqueue(pending, scheduled, *line.target_address, address, std::nullopt, evidence);
                if ((*line.target_address & 1u) == 0u &&
                    validate_committed_code_address(image, *line.target_address).valid() &&
                    !exact_function_range_strictly_contains(
                        exact_function_ranges, *line.target_address)) {
                    add_function_evidence(function_candidates,
                                          *line.target_address,
                                          FunctionOrigin::DirectCall,
                                          AnalysisConfidence::High,
                                          evidence);
                }
            }
            enqueue_next(pending,
                         scheduled,
                         address,
                         fallthrough_distance,
                         evidence,
                         exact_function_ranges);
            break;
        case katana::sh4::ControlFlowKind::IndirectCall:
            enqueue_next(pending,
                         scheduled,
                         address,
                         fallthrough_distance,
                         evidence,
                         exact_function_ranges);
            break;
        case katana::sh4::ControlFlowKind::UnconditionalBranch:
            if (line.target_address.has_value()) {
                enqueue(pending, scheduled, *line.target_address, address, std::nullopt, evidence);
            }
            break;
        case katana::sh4::ControlFlowKind::IndirectBranch:
        case katana::sh4::ControlFlowKind::Return:
        case katana::sh4::ControlFlowKind::Trap:
        case katana::sh4::ControlFlowKind::ExceptionReturn:
        case katana::sh4::ControlFlowKind::Halt:
            break;
        }
    }

    RecursiveAnalysisResult result;
    result.retained_baseline_contract_version =
        RecursiveAnalysisResult::baseline_contract_version;
    result.source_image_identity = image.analysis_instance_identity();
    result.source_image_revision = image.analysis_revision();
    result.seed_contract = std::move(seed_contract);
    result.baseline_status = baseline_status;
    result.processed_work_items = processed_work_items;
    result.reused_contexts = reused_contexts;
    result.limit = limit;
    const auto contextual_order = [](const auto& left, const auto& right) {
            return std::tie(left.line.address,
                            left.delay_slot_owner,
                            left.incoming_address,
                            left.evidence) < std::tie(right.line.address,
                                                      right.delay_slot_owner,
                                                      right.incoming_address,
                                                      right.evidence);
        };
    const auto unsorted_contexts =
        result_contexts.begin() + static_cast<std::ptrdiff_t>(baseline_context_count);
    physical_work.public_sort_items +=
        static_cast<std::size_t>(result_contexts.end() - unsorted_contexts) +
        result_contexts.size();
    std::sort(unsorted_contexts, result_contexts.end(), contextual_order);
    std::inplace_merge(
        result_contexts.begin(), unsorted_contexts, result_contexts.end(), contextual_order);
    // Evidence upgrades deliberately enqueue the stronger variant. Collapse
    // only identical structural contexts to their strongest static-decode
    // evidence so an incremental result is canonical with a fresh run and
    // does not retain weak payload forever. Distinct incoming edges and
    // delay-slot owners remain separate contexts.
    std::vector<ContextualInstruction> canonical_contexts;
    canonical_contexts.reserve(result_contexts.size());
    for (auto& contextual : result_contexts) {
        const auto same_context =
            !canonical_contexts.empty() &&
            canonical_contexts.back().line.address ==
                contextual.line.address &&
            canonical_contexts.back().delay_slot_owner ==
                contextual.delay_slot_owner &&
            canonical_contexts.back().incoming_address ==
                contextual.incoming_address;
        if (!same_context) {
            canonical_contexts.push_back(std::move(contextual));
            continue;
        }
        if (control_flow_evidence_preferred_for_static_decode(
                contextual.evidence,
                canonical_contexts.back().evidence))
            canonical_contexts.back() = std::move(contextual);
    }
    result_contexts = std::move(canonical_contexts);
    result.contextual_instructions = std::move(result_contexts);
    result.instructions.reserve(discovered.size());
    for (auto& [address, line] : discovered) {
        static_cast<void>(address);
        result.instructions.push_back(std::move(line));
    }
    std::sort(result.instructions.begin(),
              result.instructions.end(),
              [](const auto& left, const auto& right) { return left.address < right.address; });
    physical_work.public_sort_items += result.instructions.size();
    result.proven_instruction_addresses.reserve(result.instructions.size());
    result.guarded_candidate_instruction_addresses.reserve(result.instructions.size());
    std::unordered_set<std::uint32_t> proven_addresses;
    std::unordered_set<std::uint32_t> candidate_addresses;
    proven_addresses.reserve(result.contextual_instructions.size());
    candidate_addresses.reserve(result.contextual_instructions.size());
    for (const auto& contextual : result.contextual_instructions) {
        if (control_flow_evidence_proven(contextual.evidence) &&
            contextual.line.instruction.is_known())
            proven_addresses.insert(contextual.line.address);
        else
            candidate_addresses.insert(contextual.line.address);
    }
    result.proven_instruction_addresses.assign(proven_addresses.begin(), proven_addresses.end());
    std::sort(result.proven_instruction_addresses.begin(),
              result.proven_instruction_addresses.end());
    physical_work.public_sort_items +=
        result.proven_instruction_addresses.size();
    for (const auto address : candidate_addresses) {
        if (!proven_addresses.contains(address))
            result.guarded_candidate_instruction_addresses.push_back(address);
    }
    std::sort(result.guarded_candidate_instruction_addresses.begin(),
              result.guarded_candidate_instruction_addresses.end());
    physical_work.public_sort_items +=
        result.guarded_candidate_instruction_addresses.size();
    classify_image(image, result.instructions, result);
    result.diagnostics = std::move(diagnostics);
    std::sort(result.diagnostics.begin(),
              result.diagnostics.end(),
              [](const auto& left, const auto& right) {
                  if (left.address != right.address) {
                      return left.address < right.address;
                  }
                  if (left.opcode != right.opcode) {
                      return left.opcode < right.opcode;
                  }
                  if (left.reason != right.reason) {
                      return left.reason < right.reason;
                  }
                  const auto left_blocks = analysis_diagnostic_blocks_codegen(left);
                  const auto right_blocks = analysis_diagnostic_blocks_codegen(right);
                  if (left_blocks != right_blocks) return left_blocks;
                  return control_flow_evidence_preferred_for_static_decode(left.evidence,
                                                                          right.evidence);
              });
    physical_work.public_sort_items += result.diagnostics.size();
    result.diagnostics.erase(std::unique(result.diagnostics.begin(),
                                         result.diagnostics.end(),
                                         [](const auto& left, const auto& right) {
                                             return left.address == right.address &&
                                                    left.opcode == right.opcode &&
                                                    left.reason == right.reason;
                                         }),
                             result.diagnostics.end());
    result.functions.reserve(function_candidates.size());
    for (auto& [address, candidate] : function_candidates) {
        if (control_flow_evidence_proven(candidate.evidence) && !proven_addresses.contains(address))
            candidate.evidence = ControlFlowEvidence::GuardedPartial;
        if (delay_slots.contains(address)) {
            result.conflicts.push_back(
                {address, 2u, AnalysisConflictKind::FunctionEntryInDelaySlot});
            if (candidate.size != 0u)
                throw std::invalid_argument(
                    "Explizite Funktionsgrenze beginnt in einem Delay Slot.");
        }
        result.functions.push_back(std::move(candidate));
        ++physical_work.canonical_function_updates;
    }
    std::sort(result.functions.begin(),
              result.functions.end(),
              [](const auto& left, const auto& right) { return left.address < right.address; });
    std::sort(result.conflicts.begin(),
              result.conflicts.end(),
              [](const auto& left, const auto& right) { return left.address < right.address; });
    physical_work.public_sort_items +=
        result.functions.size() + result.conflicts.size();
    physical_work.public_materializations = 1u;
    physical_work.public_materialized_items =
        result.seed_contract.size() + result.instructions.size() +
        result.contextual_instructions.size() + result.functions.size() +
        result.diagnostics.size() + result.conflicts.size() +
        result.ranges.size() + result.unreachable_code.size();
    if (baseline != nullptr && baseline_evidence_replaced &&
        result.limit == RecursiveAnalysisLimit::ContextBudgetExceeded) {
        auto retry_options = options;
        retry_options.baseline = nullptr;
        auto retry = analyze_reachable_code(image, retry_options);
        retry.physical_work.add(physical_work);
        retry.baseline_status = RecursiveAnalysisBaselineStatus::
            EvidenceUpgradeContextRetry;
        return retry;
    }
    const auto payload_is_unchanged =
        baseline != nullptr && processed_work_items == 0u &&
        baseline->seed_contract == result.seed_contract;
    result.retained_baseline_payload_sha256_ =
        payload_is_unchanged
            ? baseline->retained_baseline_payload_sha256_
            : baseline_payload_digest(
                  result, &physical_work.public_baseline_hash_bytes);
    result.physical_work = physical_work;
    return result;
}

namespace detail {
namespace {

struct SessionContextKey final {
    std::uint32_t address = 0u;
    std::optional<std::uint32_t> delay_slot_owner;
    std::uint32_t incoming_address = 0u;

    auto operator<=>(const SessionContextKey&) const = default;
};

struct SessionDiagnosticKey final {
    std::uint32_t address = 0u;
    std::uint16_t opcode = 0u;
    std::string reason;

    auto operator<=>(const SessionDiagnosticKey&) const = default;
};

[[nodiscard]] bool same_session_line(
    const katana::sh4::DisassemblyLine& left,
    const katana::sh4::DisassemblyLine& right) noexcept {
    return left.address == right.address && left.opcode == right.opcode &&
           left.is_delay_slot == right.is_delay_slot &&
           left.target_address == right.target_address;
}

[[nodiscard]] bool same_session_function(
    const FunctionCandidate& left,
    const FunctionCandidate& right) noexcept {
    return left.address == right.address &&
           left.confidence == right.confidence &&
           left.evidence == right.evidence &&
           left.origins == right.origins && left.size == right.size;
}

[[nodiscard]] std::vector<ExactFunctionRange> session_exact_ranges(
    const katana::io::ExecutableImage& image,
    const std::span<const AnalysisSeed> seeds) {
    std::vector<ExactFunctionRange> ranges;
    ranges.reserve(seeds.size());
    for (const auto& seed : seeds) {
        if (seed.function_size == 0u) continue;
        if ((seed.function_size & 1u) != 0u)
            throw std::invalid_argument(
                "Explizite Funktionsgrenze besitzt eine ungerade Groesse.");
        const auto validation = validate_committed_code_address(
            image, seed.address, seed.function_size);
        if (!validation.valid())
            throw std::invalid_argument(
                "Explizite Funktionsgrenze ist nicht vollstaendig "
                "dekodierbar: " +
                std::string(code_address_status_name(validation.status)) +
                ".");
        ranges.push_back(
            {validation.resolved_address,
             static_cast<std::uint64_t>(validation.resolved_address) +
                 seed.function_size});
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto& left,
                                                const auto& right) {
        return std::tie(left.start, left.end) <
               std::tie(right.start, right.end);
    });
    ranges.erase(
        std::unique(ranges.begin(), ranges.end(), [](const auto& left,
                                                      const auto& right) {
            return left.start == right.start && left.end == right.end;
        }),
        ranges.end());
    for (std::size_t index = 1u; index < ranges.size(); ++index) {
        if (ranges[index].start < ranges[index - 1u].end)
            throw std::invalid_argument(
                "Explizite Funktionsgrenzen ueberlappen.");
    }
    return ranges;
}

} // namespace

struct SessionEpoch final {
    std::shared_ptr<const SessionEpoch> base;
    std::uint64_t image_identity = 0u;
    std::uint64_t image_revision = 0u;
    std::uint64_t version = 0u;
    std::shared_ptr<const std::vector<ExactFunctionRange>> exact_ranges;
    std::map<std::uint32_t, RecursiveAnalysisSeedContract> seed_updates;
    std::map<std::uint32_t, katana::sh4::DisassemblyLine> line_updates;
    std::map<std::uint32_t, FunctionCandidate> function_candidate_updates;
    std::map<std::uint32_t, FunctionCandidate> function_updates;
    std::map<SessionContextKey, ContextualInstruction> context_updates;
    std::map<SessionDiagnosticKey, AnalysisDiagnostic> diagnostic_updates;
    std::unordered_set<PendingAddress, PendingAddressHash> scheduled_additions;
    std::set<std::uint32_t> delay_slot_additions;
    std::set<std::uint32_t> proven_additions;
    std::map<std::uint32_t, bool> candidate_membership_updates;
    std::size_t instruction_count = 0u;
    std::size_t function_count = 0u;
    std::size_t context_count = 0u;
    RecursiveAnalysisLimit limit = RecursiveAnalysisLimit::None;
    RecursiveAnalysisBaselineStatus baseline_status =
        RecursiveAnalysisBaselineStatus::NotRequested;
    std::size_t processed_work_items = 0u;
    std::size_t reused_contexts = 0u;
    RecursiveAnalysisPhysicalWork work;
    std::vector<std::uint32_t> changed_instruction_addresses;
    std::vector<katana::sh4::DisassemblyLine> changed_instructions;
    std::vector<std::uint32_t> changed_function_entries;
    std::vector<FunctionCandidate> changed_functions;
    std::vector<std::uint32_t> newly_proven_instruction_addresses;
};

struct RecursiveAnalysisSession::Impl final {
    std::shared_ptr<const void> owner_identity =
        std::make_shared<const std::uint8_t>(std::uint8_t{0u});
    std::shared_ptr<const SessionEpoch> published;
    RecursiveAnalysisPhysicalWork aggregate_work;
    std::map<std::uint32_t, RecursiveAnalysisSeedContract> seed_index;
    std::map<std::uint32_t, katana::sh4::DisassemblyLine> line_index;
    std::map<std::uint32_t, FunctionCandidate> function_candidate_index;
    std::map<std::uint32_t, FunctionCandidate> function_index;
    std::map<SessionContextKey, ContextualInstruction> context_index;
    std::map<SessionDiagnosticKey, AnalysisDiagnostic> diagnostic_index;
    std::unordered_set<PendingAddress, PendingAddressHash> scheduled_index;
    std::set<std::uint32_t> delay_slot_index;
    std::set<std::uint32_t> proven_index;
    std::map<std::uint32_t, bool> candidate_membership_index;
};

template <typename Map>
void commit_prepared_map_nodes(Map& current, Map prepared) noexcept {
    while (!prepared.empty()) {
        auto next = prepared.extract(prepared.begin());
        static_cast<void>(current.extract(next.key()));
        current.insert(std::move(next));
    }
}

RecursiveAnalysisSession::RecursiveAnalysisSession()
    : impl_(std::make_unique<Impl>()) {}

RecursiveAnalysisSession::~RecursiveAnalysisSession() = default;
RecursiveAnalysisSession::RecursiveAnalysisSession(
    RecursiveAnalysisSession&&) noexcept = default;
RecursiveAnalysisSession& RecursiveAnalysisSession::operator=(
    RecursiveAnalysisSession&&) noexcept = default;

RecursiveAnalysisSnapshot RecursiveAnalysisSession::analyze(
    const katana::io::ExecutableImage& image,
    const std::span<const AnalysisSeed> complete_seeds,
    const RecursiveAnalysisDeltaJournal& delta,
    const std::size_t maximum_instructions,
    const std::size_t maximum_contexts) {
    const auto published = impl_->published;
    const auto identity = image.analysis_instance_identity();
    const auto revision = image.analysis_revision();
    const bool image_changed =
        published != nullptr &&
        (published->image_identity != identity ||
         published->image_revision != revision);
    const bool budget_incompatible =
        published != nullptr &&
        (published->instruction_count > maximum_instructions ||
         published->context_count > maximum_contexts);
    const bool cold = published == nullptr || image_changed ||
                      budget_incompatible || !delta.complete ||
                      delta.exact_function_boundary_changed;
    const auto baseline_status = [&] {
        if (published == nullptr)
            return RecursiveAnalysisBaselineStatus::NotRequested;
        if (image_changed)
            return published->image_identity != identity
                       ? RecursiveAnalysisBaselineStatus::
                             ImageIdentityMismatch
                       : RecursiveAnalysisBaselineStatus::
                             ImageRevisionMismatch;
        if (budget_incompatible)
            return RecursiveAnalysisBaselineStatus::BudgetIncompatible;
        if (!delta.complete)
            return RecursiveAnalysisBaselineStatus::PayloadMismatch;
        if (delta.exact_function_boundary_changed)
            return RecursiveAnalysisBaselineStatus::SeedContractMismatch;
        return RecursiveAnalysisBaselineStatus::Reused;
    }();
    if (cold && !delta.complete_seed_contract_supplied) {
        // The caller optimistically supplied only a delta, but a trusted
        // session binding was invalidated underneath it. Never publish a
        // partial cold epoch. Ask the owner to retry with its authoritative
        // complete seed contract while the prior epoch remains untouched.
        RecursiveAnalysisSnapshot retry;
        retry.owner_identity_ = impl_->owner_identity;
        retry.epoch_version_ = published == nullptr ? 0u
                                                    : published->version;
        retry.baseline_status_ = baseline_status;
        retry.cold_retry_required_ = true;
        ++retry.physical_work_.trusted_snapshot_validations;
        impl_->aggregate_work.add(retry.physical_work_);
        return retry;
    }
    if (!cold && delta.changed_seeds.empty()) {
        RecursiveAnalysisPhysicalWork work;
        ++work.trusted_snapshot_validations;
        impl_->aggregate_work.add(work);
        RecursiveAnalysisSnapshot snapshot;
        snapshot.owner_identity_ = impl_->owner_identity;
        snapshot.epoch_data_ = published.get();
        snapshot.epoch_keepalive_ = published;
        snapshot.epoch_version_ = published->version;
        snapshot.instruction_count_ = published->instruction_count;
        snapshot.function_count_ = published->function_count;
        snapshot.contextual_instruction_count_ = published->context_count;
        snapshot.limit_ = RecursiveAnalysisLimit::None;
        snapshot.baseline_status_ = RecursiveAnalysisBaselineStatus::Reused;
        snapshot.reused_contexts_ = published->context_count;
        snapshot.physical_work_ = work;
        return snapshot;
    }

    auto staged = std::make_shared<SessionEpoch>();
    staged->base = cold ? nullptr : published;
    staged->image_identity = identity;
    staged->image_revision = revision;
    staged->version = published == nullptr ? 1u : published->version + 1u;
    staged->instruction_count = cold ? 0u : published->instruction_count;
    staged->function_count = cold ? 0u : published->function_count;
    staged->context_count = cold ? 0u : published->context_count;
    staged->reused_contexts = cold ? 0u : published->context_count;
    staged->baseline_status = baseline_status;
    if (cold) {
        staged->exact_ranges = std::make_shared<const std::vector<ExactFunctionRange>>(
            session_exact_ranges(image, complete_seeds));
    } else {
        staged->exact_ranges = published->exact_ranges;
        ++staged->work.trusted_snapshot_validations;
    }

    const auto current_line = [&](const std::uint32_t address)
        -> const katana::sh4::DisassemblyLine* {
        ++staged->work.epoch_index_lookups;
        const auto local = staged->line_updates.find(address);
        if (local != staged->line_updates.end()) return &local->second;
        if (!cold) {
            const auto current = impl_->line_index.find(address);
            if (current != impl_->line_index.end()) return &current->second;
        }
        return static_cast<const katana::sh4::DisassemblyLine*>(nullptr);
    };
    const auto current_function_candidate =
        [&](const std::uint32_t address) -> const FunctionCandidate* {
            ++staged->work.epoch_index_lookups;
            const auto local =
                staged->function_candidate_updates.find(address);
            if (local != staged->function_candidate_updates.end())
                return &local->second;
            if (!cold) {
                const auto current =
                    impl_->function_candidate_index.find(address);
                if (current != impl_->function_candidate_index.end())
                    return &current->second;
            }
            return static_cast<const FunctionCandidate*>(nullptr);
        };
    const auto current_function = [&](const std::uint32_t address)
        -> const FunctionCandidate* {
        ++staged->work.epoch_index_lookups;
        const auto local = staged->function_updates.find(address);
        if (local != staged->function_updates.end()) return &local->second;
        if (!cold) {
            const auto current = impl_->function_index.find(address);
            if (current != impl_->function_index.end())
                return &current->second;
        }
        return static_cast<const FunctionCandidate*>(nullptr);
    };
    const auto current_context = [&](const SessionContextKey& key)
        -> const ContextualInstruction* {
        ++staged->work.epoch_index_lookups;
        const auto local = staged->context_updates.find(key);
        if (local != staged->context_updates.end()) return &local->second;
        if (!cold) {
            const auto current = impl_->context_index.find(key);
            if (current != impl_->context_index.end()) return &current->second;
        }
        return static_cast<const ContextualInstruction*>(nullptr);
    };
    const auto current_diagnostic = [&](const SessionDiagnosticKey& key)
        -> const AnalysisDiagnostic* {
        ++staged->work.epoch_index_lookups;
        const auto local = staged->diagnostic_updates.find(key);
        if (local != staged->diagnostic_updates.end()) return &local->second;
        if (!cold) {
            const auto current = impl_->diagnostic_index.find(key);
            if (current != impl_->diagnostic_index.end())
                return &current->second;
        }
        return static_cast<const AnalysisDiagnostic*>(nullptr);
    };
    const auto is_scheduled = [&](const PendingAddress& work) {
        ++staged->work.epoch_index_lookups;
        return staged->scheduled_additions.contains(work) ||
               (!cold && impl_->scheduled_index.contains(work));
    };
    const auto is_proven = [&](const std::uint32_t address) {
        ++staged->work.epoch_index_lookups;
        return staged->proven_additions.contains(address) ||
               (!cold && impl_->proven_index.contains(address));
    };
    const auto candidate_member = [&](const std::uint32_t address) {
        ++staged->work.epoch_index_lookups;
        const auto local = staged->candidate_membership_updates.find(address);
        if (local != staged->candidate_membership_updates.end())
            return local->second;
        if (!cold) {
            const auto current =
                impl_->candidate_membership_index.find(address);
            if (current != impl_->candidate_membership_index.end())
                return current->second;
        }
        return false;
    };

    std::deque<PendingAddress> pending;
    const auto stage_enqueue = [&](const std::uint32_t address,
                                   const std::uint32_t incoming,
                                   const std::optional<std::uint32_t> owner,
                                   const ControlFlowEvidence evidence) {
        PendingAddress work{address, incoming, owner, evidence};
        if (is_scheduled(work)) return;
        staged->scheduled_additions.insert(work);
        pending.push_back(std::move(work));
    };
    const auto stage_enqueue_next = [&](const std::uint32_t address,
                                        const std::uint32_t distance,
                                        const ControlFlowEvidence evidence) {
        if (address > std::numeric_limits<std::uint32_t>::max() - distance)
            return;
        const auto next = address + distance;
        const auto* range = containing_function_range(
            *staged->exact_ranges, address);
        if (range == nullptr ||
            static_cast<std::uint64_t>(next) < range->end)
            stage_enqueue(next, address, std::nullopt, evidence);
    };
    std::function<void(std::uint32_t)> refresh_function;
    refresh_function = [&](const std::uint32_t address) {
        const auto* raw = current_function_candidate(address);
        if (raw == nullptr) return;
        auto value = *raw;
        if (control_flow_evidence_proven(value.evidence) &&
            !is_proven(address))
            value.evidence = ControlFlowEvidence::GuardedPartial;
        const auto* old = current_function(address);
        if (old != nullptr && same_session_function(*old, value)) return;
        if (old == nullptr) ++staged->function_count;
        staged->function_updates.insert_or_assign(address, value);
        ++staged->work.canonical_function_updates;
    };
    const auto update_function_candidate =
        [&](const std::uint32_t address,
            const FunctionOrigin origin,
            const AnalysisConfidence confidence,
            const ControlFlowEvidence evidence,
            const std::uint32_t size = 0u) {
            FunctionCandidate value;
            if (const auto* old = current_function_candidate(address))
                value = *old;
            std::unordered_map<std::uint32_t, FunctionCandidate> one;
            one.emplace(address, std::move(value));
            add_function_evidence(one, address, origin, confidence,
                                  evidence, size);
            staged->function_candidate_updates.insert_or_assign(
                address, std::move(one.at(address)));
            refresh_function(address);
        };
    const auto record_context = [&](ContextualInstruction contextual) {
        const SessionContextKey key{contextual.line.address,
                                    contextual.delay_slot_owner,
                                    contextual.incoming_address};
        const auto* old = current_context(key);
        if (old != nullptr &&
            !control_flow_evidence_preferred_for_static_decode(
                contextual.evidence, old->evidence))
            return;
        if (old == nullptr) ++staged->context_count;
        staged->context_updates.insert_or_assign(key, contextual);
        ++staged->work.canonical_context_updates;
        const auto address = contextual.line.address;
        if (control_flow_evidence_proven(contextual.evidence) &&
            contextual.line.instruction.is_known()) {
            if (!is_proven(address)) {
                staged->proven_additions.insert(address);
                staged->newly_proven_instruction_addresses.push_back(address);
                refresh_function(address);
            }
            if (candidate_member(address))
                staged->candidate_membership_updates[address] = false;
        } else if (!is_proven(address) && !candidate_member(address)) {
            staged->candidate_membership_updates[address] = true;
        }
    };
    const auto record_diagnostic = [&](AnalysisDiagnostic diagnostic) {
        const SessionDiagnosticKey key{diagnostic.address,
                                       diagnostic.opcode,
                                       diagnostic.reason};
        const auto* old = current_diagnostic(key);
        if (old != nullptr) {
            const auto old_blocks = analysis_diagnostic_blocks_codegen(*old);
            const auto new_blocks =
                analysis_diagnostic_blocks_codegen(diagnostic);
            if ((!new_blocks || old_blocks) &&
                (new_blocks != old_blocks ||
                 !control_flow_evidence_preferred_for_static_decode(
                     diagnostic.evidence, old->evidence)))
                return;
        }
        staged->diagnostic_updates.insert_or_assign(key,
                                                    std::move(diagnostic));
    };
    const auto apply_seed = [&](const AnalysisSeed& seed) {
        ++staged->work.seed_contract_items_visited;
        const auto validation = validate_committed_code_address(
            image, seed.address);
        if (!validation.valid()) return;
        const auto address = validation.resolved_address;
        const auto evidence = effective_seed_evidence(seed);
        RecursiveAnalysisSeedContract contract;
        ++staged->work.epoch_index_lookups;
        if (const auto local = staged->seed_updates.find(address);
            local != staged->seed_updates.end()) {
            contract = local->second;
        } else if (!cold) {
            const auto current = impl_->seed_index.find(address);
            if (current != impl_->seed_index.end()) contract = current->second;
        }
        if (contract.function_size != 0u && seed.function_size != 0u &&
            contract.function_size != seed.function_size)
            throw std::invalid_argument(
                "Explizite Funktionsgrenzen widersprechen sich.");
        contract.address = address;
        if (seed.function_size != 0u)
            contract.function_size = seed.function_size;
        contract.function_origins.insert(contract.function_origins.end(),
                                         seed.function_origins.begin(),
                                         seed.function_origins.end());
        std::sort(contract.function_origins.begin(),
                  contract.function_origins.end());
        contract.function_origins.erase(
            std::unique(contract.function_origins.begin(),
                        contract.function_origins.end()),
            contract.function_origins.end());
        contract.decode_evidences.push_back(evidence);
        std::sort(contract.decode_evidences.begin(),
                  contract.decode_evidences.end());
        contract.decode_evidences.erase(
            std::unique(contract.decode_evidences.begin(),
                        contract.decode_evidences.end()),
            contract.decode_evidences.end());
        staged->seed_updates.insert_or_assign(address,
                                              std::move(contract));
        stage_enqueue(address, address, std::nullopt, evidence);
        if (seed.function_size != 0u) {
            FunctionCandidate value;
            if (const auto* old = current_function_candidate(address))
                value = *old;
            value.address = address;
            if (value.size != 0u && value.size != seed.function_size)
                throw std::invalid_argument(
                    "Explizite Funktionsgrenzen widersprechen sich.");
            value.size = seed.function_size;
            staged->function_candidate_updates.insert_or_assign(address,
                                                                value);
        }
        for (const auto origin : seed.function_origins) {
            const auto confidence =
                origin == FunctionOrigin::UserOverride
                    ? AnalysisConfidence::Certain
                : origin == FunctionOrigin::GuardedSnapshot ||
                          origin == FunctionOrigin::RuntimeCopy ||
                          origin == FunctionOrigin::StoredCodeAddress
                    ? AnalysisConfidence::Medium
                    : AnalysisConfidence::High;
            update_function_candidate(address, origin, confidence,
                                      evidence, seed.function_size);
        }
        refresh_function(address);
    };

    if (cold) {
        for (const auto entry : image.entry_points()) {
            const auto validation = validate_committed_code_address(image,
                                                                    entry);
            if (!validation.valid())
                throw std::invalid_argument(
                    "Analyse-Einstiegspunkt ist ungueltig: " +
                    std::string(code_address_status_name(validation.status)) +
                    ".");
            const auto resolved = validation.resolved_address;
            update_function_candidate(
                resolved, FunctionOrigin::EntryPoint,
                AnalysisConfidence::Certain,
                ControlFlowEvidence::ProvenComplete);
            stage_enqueue(resolved, resolved, std::nullopt,
                          ControlFlowEvidence::ProvenComplete);
        }
        for (const auto& symbol : image.symbols()) {
            if (symbol.kind != katana::io::SymbolKind::Function ||
                (symbol.address & 1u) != 0u)
                continue;
            const auto validation = validate_committed_code_address(
                image, symbol.address);
            if (!validation.valid()) continue;
            const auto resolved = validation.resolved_address;
            update_function_candidate(
                resolved, FunctionOrigin::Symbol,
                AnalysisConfidence::High,
                ControlFlowEvidence::ProvenComplete);
            stage_enqueue(resolved, resolved, std::nullopt,
                          ControlFlowEvidence::ProvenComplete);
        }
    }
    const auto seeds = cold ? complete_seeds : delta.changed_seeds;
    for (const auto& seed : seeds) apply_seed(seed);

    while (!pending.empty() && staged->limit == RecursiveAnalysisLimit::None) {
        const auto work = pending.front();
        pending.pop_front();
        ++staged->processed_work_items;
        ++staged->work.decoded_work_items;
        const auto validation = validate_decode_candidate(image,
                                                          work.address);
        if (!validation.valid()) continue;
        const auto address = validation.resolved_address;
        const auto* segment = validation.segment;
        const auto offset = *segment->byte_offset(address);
        const auto opcode = katana::io::read_u16_le(segment->bytes, offset);
        katana::sh4::DisassemblyLine line;
        line.address = address;
        line.opcode = opcode;
        line.instruction = katana::sh4::decode(opcode);
        line.is_delay_slot = work.delay_slot_owner.has_value();
        line.target_address = katana::sh4::calculate_direct_branch_target(
            line.instruction, address);
        const auto* old_line = current_line(address);
        const bool new_line = old_line == nullptr;
        if (new_line && staged->instruction_count >= maximum_instructions) {
            staged->limit = RecursiveAnalysisLimit::InstructionBudgetExceeded;
            break;
        }
        const SessionContextKey context_key{
            address, work.delay_slot_owner, work.incoming_address};
        if (current_context(context_key) == nullptr &&
            staged->context_count >= maximum_contexts) {
            staged->limit = RecursiveAnalysisLimit::ContextBudgetExceeded;
            break;
        }
        bool line_changed = new_line;
        if (new_line) {
            ++staged->instruction_count;
            staged->line_updates.emplace(address, line);
        } else if (!line.is_delay_slot &&
                   !same_session_line(*old_line, line)) {
            staged->line_updates.insert_or_assign(address, line);
            line_changed = true;
        }
        if (line_changed) {
            ++staged->work.canonical_instruction_updates;
        }
        record_context({line, work.incoming_address,
                        work.delay_slot_owner, work.evidence});
        if (!line.instruction.is_known()) {
            record_diagnostic(
                {address, opcode, AnalysisDiagnosticKind::UnknownOpcode,
                 "unknown-opcode", work.evidence});
            continue;
        }
        if (line.is_delay_slot) {
            staged->delay_slot_additions.insert(address);
            if (line.instruction.changes_control_flow())
                record_diagnostic(
                    {address, opcode,
                     AnalysisDiagnosticKind::ControlFlowInDelaySlot,
                     "control-flow-in-delay-slot", work.evidence});
            continue;
        }
        const auto fallthrough_distance =
            line.instruction.has_delay_slot ? 4u : 2u;
        if (line.instruction.has_delay_slot &&
            address <= std::numeric_limits<std::uint32_t>::max() - 2u) {
            const auto delay_address = address + 2u;
            if (const auto* range = containing_function_range(
                    *staged->exact_ranges, address);
                range != nullptr &&
                static_cast<std::uint64_t>(delay_address) >= range->end)
                throw std::invalid_argument(
                    delay_slot_boundary_error(
                        address, delay_address, *range));
            stage_enqueue(delay_address, address, address, work.evidence);
            const auto delay_validation = validate_committed_code_address(
                image, delay_address);
            if (!delay_validation.valid()) {
                record_diagnostic(
                    {address, opcode,
                     AnalysisDiagnosticKind::DelaySlotUnavailable,
                     "delay-slot-unavailable", work.evidence});
                continue;
            }
            const auto delay_offset =
                *delay_validation.segment->byte_offset(delay_address);
            const auto delay_opcode = katana::io::read_u16_le(
                delay_validation.segment->bytes, delay_offset);
            if (!katana::sh4::decode(delay_opcode).is_known()) {
                record_diagnostic(
                    {address, opcode,
                     AnalysisDiagnosticKind::DelaySlotUnknownOpcode,
                     "delay-slot-unknown-opcode", work.evidence});
                continue;
            }
        }
        switch (line.instruction.control_flow) {
        case katana::sh4::ControlFlowKind::None:
            stage_enqueue_next(address, 2u, work.evidence);
            break;
        case katana::sh4::ControlFlowKind::ConditionalBranch:
            if (line.target_address.has_value())
                stage_enqueue(*line.target_address, address, std::nullopt,
                              work.evidence);
            stage_enqueue_next(address, fallthrough_distance, work.evidence);
            break;
        case katana::sh4::ControlFlowKind::Call:
            if (line.target_address.has_value()) {
                stage_enqueue(*line.target_address, address, std::nullopt,
                              work.evidence);
                if (((*line.target_address & 1u) == 0u) &&
                    validate_committed_code_address(
                        image, *line.target_address).valid() &&
                    !exact_function_range_strictly_contains(
                        *staged->exact_ranges, *line.target_address))
                    update_function_candidate(
                        *line.target_address, FunctionOrigin::DirectCall,
                        AnalysisConfidence::High, work.evidence);
            }
            stage_enqueue_next(address, fallthrough_distance, work.evidence);
            break;
        case katana::sh4::ControlFlowKind::IndirectCall:
            stage_enqueue_next(address, fallthrough_distance, work.evidence);
            break;
        case katana::sh4::ControlFlowKind::UnconditionalBranch:
            if (line.target_address.has_value())
                stage_enqueue(*line.target_address, address, std::nullopt,
                              work.evidence);
            break;
        case katana::sh4::ControlFlowKind::IndirectBranch:
        case katana::sh4::ControlFlowKind::Return:
        case katana::sh4::ControlFlowKind::Trap:
        case katana::sh4::ControlFlowKind::ExceptionReturn:
        case katana::sh4::ControlFlowKind::Halt:
            break;
        }
    }

    for (const auto& [address, line] : staged->line_updates) {
        staged->changed_instruction_addresses.push_back(address);
        staged->changed_instructions.push_back(line);
    }
    for (const auto& [address, function] : staged->function_updates) {
        staged->changed_function_entries.push_back(address);
        staged->changed_functions.push_back(function);
    }
    staged->newly_proven_instruction_addresses.assign(
        staged->proven_additions.begin(), staged->proven_additions.end());
    if (staged->limit == RecursiveAnalysisLimit::None) {
        // Allocate/copy every commit node before touching the published
        // flattened indices.  The subsequent node transfers do not allocate,
        // so an exception cannot expose a half-published session view.
        auto commit_seeds = staged->seed_updates;
        auto commit_lines = staged->line_updates;
        auto commit_function_candidates =
            staged->function_candidate_updates;
        auto commit_functions = staged->function_updates;
        auto commit_contexts = staged->context_updates;
        auto commit_diagnostics = staged->diagnostic_updates;
        auto commit_scheduled = staged->scheduled_additions;
        auto commit_delay_slots = staged->delay_slot_additions;
        auto commit_proven = staged->proven_additions;
        auto commit_candidate_membership =
            staged->candidate_membership_updates;
        const auto committed_items =
            commit_seeds.size() + commit_lines.size() +
            commit_function_candidates.size() + commit_functions.size() +
            commit_contexts.size() + commit_diagnostics.size() +
            commit_scheduled.size() + commit_delay_slots.size() +
            commit_proven.size() + commit_candidate_membership.size();
        if (cold) {
            const auto retired_items =
                impl_->seed_index.size() + impl_->line_index.size() +
                impl_->function_candidate_index.size() +
                impl_->function_index.size() + impl_->context_index.size() +
                impl_->diagnostic_index.size() +
                impl_->scheduled_index.size() +
                impl_->delay_slot_index.size() +
                impl_->proven_index.size() +
                impl_->candidate_membership_index.size();
            impl_->seed_index.swap(commit_seeds);
            impl_->line_index.swap(commit_lines);
            impl_->function_candidate_index.swap(
                commit_function_candidates);
            impl_->function_index.swap(commit_functions);
            impl_->context_index.swap(commit_contexts);
            impl_->diagnostic_index.swap(commit_diagnostics);
            impl_->scheduled_index.swap(commit_scheduled);
            impl_->delay_slot_index.swap(commit_delay_slots);
            impl_->proven_index.swap(commit_proven);
            impl_->candidate_membership_index.swap(
                commit_candidate_membership);
            staged->work.epoch_index_updates +=
                retired_items + committed_items;
        } else {
            impl_->scheduled_index.reserve(
                impl_->scheduled_index.size() + commit_scheduled.size());
            commit_prepared_map_nodes(impl_->seed_index,
                                      std::move(commit_seeds));
            commit_prepared_map_nodes(impl_->line_index,
                                      std::move(commit_lines));
            commit_prepared_map_nodes(
                impl_->function_candidate_index,
                std::move(commit_function_candidates));
            commit_prepared_map_nodes(impl_->function_index,
                                      std::move(commit_functions));
            commit_prepared_map_nodes(impl_->context_index,
                                      std::move(commit_contexts));
            commit_prepared_map_nodes(impl_->diagnostic_index,
                                      std::move(commit_diagnostics));
            impl_->scheduled_index.merge(commit_scheduled);
            impl_->delay_slot_index.merge(commit_delay_slots);
            impl_->proven_index.merge(commit_proven);
            commit_prepared_map_nodes(
                impl_->candidate_membership_index,
                std::move(commit_candidate_membership));
            staged->work.epoch_index_updates += committed_items;
        }
    }
    impl_->aggregate_work.add(staged->work);
    if (staged->limit == RecursiveAnalysisLimit::None)
        impl_->published = staged;

    RecursiveAnalysisSnapshot snapshot;
    snapshot.owner_identity_ = impl_->owner_identity;
    snapshot.epoch_data_ = staged.get();
    snapshot.epoch_keepalive_ = staged;
    snapshot.epoch_version_ = staged->version;
    snapshot.instruction_count_ = staged->instruction_count;
    snapshot.function_count_ = staged->function_count;
    snapshot.contextual_instruction_count_ = staged->context_count;
    snapshot.limit_ = staged->limit;
    snapshot.baseline_status_ = staged->baseline_status;
    snapshot.processed_work_items_ = staged->processed_work_items;
    snapshot.reused_contexts_ = staged->reused_contexts;
    snapshot.changed_instruction_addresses_ =
        staged->changed_instruction_addresses;
    snapshot.changed_instructions_ = staged->changed_instructions;
    snapshot.changed_function_entries_ = staged->changed_function_entries;
    snapshot.changed_functions_ = staged->changed_functions;
    snapshot.newly_proven_instruction_addresses_ =
        staged->newly_proven_instruction_addresses;
    snapshot.physical_work_ = staged->work;
    return snapshot;
}

RecursiveAnalysisResult RecursiveAnalysisSession::materialize(
    const katana::io::ExecutableImage& image,
    const RecursiveAnalysisSnapshot& snapshot) {
    if (snapshot.owner_identity_ != impl_->owner_identity ||
        snapshot.epoch_data_ == nullptr || snapshot.cold_retry_required_)
        throw std::invalid_argument(
            "Recursive-Analyse-Snapshot gehoert nicht zu dieser Session.");
    const auto* epoch = static_cast<const SessionEpoch*>(
        snapshot.epoch_data_);
    if (epoch->version != snapshot.epoch_version_)
        throw std::invalid_argument(
            "Recursive-Analyse-Snapshot besitzt eine veraltete Epoch.");
    if (epoch->image_identity != image.analysis_instance_identity())
        throw std::invalid_argument(
            "Recursive-Analyse-Snapshot gehoert zu einem anderen Image.");
    if (epoch->image_revision != image.analysis_revision())
        throw std::invalid_argument(
            "Recursive-Analyse-Snapshot gehoert zu einer veralteten Image-Revision.");
    std::vector<const SessionEpoch*> chain;
    for (auto current = epoch; current != nullptr;
         current = current->base.get())
        chain.push_back(current);
    std::reverse(chain.begin(), chain.end());
    RecursiveAnalysisPhysicalWork materialize_work;
    materialize_work.public_materializations = 1u;
    materialize_work.public_sort_items += chain.size();
    std::map<std::uint32_t, RecursiveAnalysisSeedContract> seeds;
    std::map<std::uint32_t, katana::sh4::DisassemblyLine> lines;
    std::map<std::uint32_t, FunctionCandidate> functions;
    std::map<SessionContextKey, ContextualInstruction> contexts;
    std::map<SessionDiagnosticKey, AnalysisDiagnostic> diagnostics;
    std::set<std::uint32_t> delay_slots;
    std::set<std::uint32_t> proven;
    std::map<std::uint32_t, bool> candidate_membership;
    for (const auto* layer : chain) {
        const auto copied_items =
            layer->seed_updates.size() + layer->line_updates.size() +
            layer->function_updates.size() + layer->context_updates.size() +
            layer->diagnostic_updates.size() +
            layer->delay_slot_additions.size() +
            layer->proven_additions.size() +
            layer->candidate_membership_updates.size();
        materialize_work.public_baseline_copy_items += copied_items;
        materialize_work.public_sort_items += copied_items;
        materialize_work.terminal_epoch_fold_items += copied_items;
        for (const auto& [key, value] : layer->seed_updates)
            seeds.insert_or_assign(key, value);
        for (const auto& [key, value] : layer->line_updates)
            lines.insert_or_assign(key, value);
        for (const auto& [key, value] : layer->function_updates)
            functions.insert_or_assign(key, value);
        for (const auto& [key, value] : layer->context_updates)
            contexts.insert_or_assign(key, value);
        for (const auto& [key, value] : layer->diagnostic_updates)
            diagnostics.insert_or_assign(key, value);
        delay_slots.insert(layer->delay_slot_additions.begin(),
                           layer->delay_slot_additions.end());
        proven.insert(layer->proven_additions.begin(),
                      layer->proven_additions.end());
        for (const auto& [key, value] :
             layer->candidate_membership_updates)
            candidate_membership[key] = value;
    }
    RecursiveAnalysisResult result;
    result.retained_baseline_contract_version =
        RecursiveAnalysisResult::baseline_contract_version;
    result.source_image_identity = epoch->image_identity;
    result.source_image_revision = epoch->image_revision;
    result.limit = epoch->limit;
    result.baseline_status = epoch->baseline_status;
    result.processed_work_items = epoch->processed_work_items;
    result.reused_contexts = epoch->reused_contexts;
    for (auto& [address, value] : seeds) {
        static_cast<void>(address);
        result.seed_contract.push_back(std::move(value));
    }
    for (auto& [address, value] : lines) {
        static_cast<void>(address);
        result.instructions.push_back(std::move(value));
    }
    result.proven_instruction_addresses.assign(proven.begin(), proven.end());
    for (const auto& [address, member] : candidate_membership)
        if (member && !proven.contains(address))
            result.guarded_candidate_instruction_addresses.push_back(address);
    for (auto& [key, value] : contexts) {
        static_cast<void>(key);
        result.contextual_instructions.push_back(std::move(value));
    }
    for (auto& [address, value] : functions) {
        static_cast<void>(address);
        result.functions.push_back(std::move(value));
    }
    for (auto& [key, value] : diagnostics) {
        static_cast<void>(key);
        result.diagnostics.push_back(std::move(value));
    }
    for (const auto& candidate : result.functions) {
        if (!delay_slots.contains(candidate.address)) continue;
        result.conflicts.push_back(
            {candidate.address, 2u,
             AnalysisConflictKind::FunctionEntryInDelaySlot});
        if (candidate.size != 0u)
            throw std::invalid_argument(
                "Explizite Funktionsgrenze beginnt in einem Delay Slot.");
    }
    classify_image(image, result.instructions, result);
    materialize_work.public_materialized_items +=
        result.seed_contract.size() + result.instructions.size() +
        result.contextual_instructions.size() + result.functions.size() +
        result.diagnostics.size() + result.conflicts.size() +
        result.ranges.size() + result.unreachable_code.size();
    materialize_work.public_sort_items +=
        result.seed_contract.size() + result.instructions.size() +
        result.contextual_instructions.size() + result.functions.size() +
        result.diagnostics.size() + result.conflicts.size() +
        result.ranges.size() + result.unreachable_code.size();
    result.retained_baseline_payload_sha256_ = baseline_payload_digest(
        result, &materialize_work.public_baseline_hash_bytes);
    impl_->aggregate_work.add(materialize_work);
    result.physical_work = impl_->aggregate_work;
    return result;
}

} // namespace detail

const char* discovered_byte_kind_name(const DiscoveredByteKind kind) noexcept {
    switch (kind) {
    case DiscoveredByteKind::Unknown:
        return "unknown";
    case DiscoveredByteKind::Code:
        return "code";
    case DiscoveredByteKind::Data:
        return "data";
    }
    return "unknown";
}

const char* function_origin_name(const FunctionOrigin origin) noexcept {
    switch (origin) {
    case FunctionOrigin::EntryPoint:
        return "entry-point";
    case FunctionOrigin::DirectCall:
        return "direct-call";
    case FunctionOrigin::IndirectCall:
        return "indirect-call";
    case FunctionOrigin::GuardedSnapshot:
        return "guarded-snapshot";
    case FunctionOrigin::RuntimeCopy:
        return "runtime-copy";
    case FunctionOrigin::JumpTableCall:
        return "jump-table-call";
    case FunctionOrigin::UserOverride:
        return "user-override";
    case FunctionOrigin::UserHint:
        return "user-hint";
    case FunctionOrigin::Symbol:
        return "symbol";
    case FunctionOrigin::StoredCodeAddress:
        return "stored-code-address";
    }
    return "unknown";
}

const char* analysis_confidence_name(const AnalysisConfidence confidence) noexcept {
    switch (confidence) {
    case AnalysisConfidence::Low:
        return "low";
    case AnalysisConfidence::Medium:
        return "medium";
    case AnalysisConfidence::High:
        return "high";
    case AnalysisConfidence::Certain:
        return "certain";
    }
    return "unknown";
}

const char* analysis_conflict_kind_name(const AnalysisConflictKind kind) noexcept {
    switch (kind) {
    case AnalysisConflictKind::FunctionEntryInDelaySlot:
        return "function-entry-in-delay-slot";
    }
    return "unknown";
}

std::string format_recursive_analysis_report(const RecursiveAnalysisResult& result,
                                             const std::span<const SymbolicAddress> symbols) {
    std::ostringstream output;
    output << "Katana rekursive Analyse\n"
           << "Instruktionen: " << result.instructions.size() << '\n'
           << "Instruktionskontexte: " << result.contextual_instructions.size() << '\n'
           << "Funktionen: " << result.functions.size() << '\n'
           << "Bereiche: " << result.ranges.size() << '\n'
           << "Unerreichbar: " << result.unreachable_code.size() << '\n'
           << "Konflikte: " << result.conflicts.size() << '\n'
           << "Diagnosen: " << result.diagnostics.size() << '\n'
           << "Limit: " << recursive_analysis_limit_name(result.limit) << '\n'
           << "Baseline: "
           << recursive_analysis_baseline_status_name(
                  result.baseline_status)
           << "\n\n";
    const auto address = [&output](const std::uint32_t value) {
        output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value
               << std::dec << std::setfill(' ');
    };

    for (const auto& function : result.functions) {
        output << "Funktion ";
        address(function.address);
        if (const auto* symbol = find_symbolic_address(symbols, function.address)) {
            output << " Symbol=" << format_symbolic_address(*symbol);
        }
        output << " Konfidenz=" << analysis_confidence_name(function.confidence)
               << " Evidenz=" << control_flow_evidence_name(function.evidence) << " Herkunft=";
        for (std::size_t index = 0; index < function.origins.size(); ++index) {
            if (index != 0u) {
                output << ',';
            }
            output << function_origin_name(function.origins[index]);
        }
        output << '\n';
    }
    for (const auto& range : result.ranges) {
        output << "Bereich ";
        address(range.start_address);
        output << " Groesse=" << range.size << " Art=" << discovered_byte_kind_name(range.kind)
               << '\n';
    }
    for (const auto& range : result.unreachable_code) {
        output << "Unerreichbar ";
        address(range.start_address);
        output << " Groesse=" << range.size << '\n';
    }
    for (const auto& conflict : result.conflicts) {
        output << "Konflikt ";
        address(conflict.address);
        output << " Groesse=" << conflict.size
               << " Grund=" << analysis_conflict_kind_name(conflict.kind) << '\n';
    }
    for (const auto& diagnostic : result.diagnostics) {
        output << "Diagnose ";
        address(diagnostic.address);
        if (const auto* symbol = find_symbolic_address(symbols, diagnostic.address)) {
            output << " Symbol=" << format_symbolic_address(*symbol);
        }
        output << " Opcode=0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
               << diagnostic.opcode << std::dec << std::setfill(' ')
               << " Grund=" << diagnostic.reason
               << " Evidenz=" << control_flow_evidence_name(diagnostic.evidence)
               << " Blockiert-Codegen="
               << (analysis_diagnostic_blocks_codegen(diagnostic) ? "ja" : "nein") << '\n';
    }
    return output.str();
}

} // namespace katana::analysis
