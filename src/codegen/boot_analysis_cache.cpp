#include "katana/codegen/boot_analysis_cache.hpp"

#include "katana/analysis/abi.hpp"
#include "katana/analysis/analysis_index.hpp"
#include "katana/analysis/basic_blocks.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/graph_export.hpp"
#include "katana/analysis/jump_table_analysis.hpp"
#include "katana/analysis/runtime_code_copy_analysis.hpp"
#include "katana/analysis/symbol_names.hpp"
#include "katana/codegen/latent_aot_analysis_cache.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace katana::codegen {
namespace {

constexpr std::array<std::uint8_t, 8u> cache_magic{
    'K', 'B', 'O', 'O', 'T', 'A', 'C', '1'};
constexpr std::string_view key_magic{"katana-boot-analysis-key-v1"};
constexpr std::size_t sha256_bytes = 32u;
constexpr std::size_t cache_header_bytes =
    cache_magic.size() + sizeof(std::uint32_t) + sha256_bytes +
    sizeof(std::uint64_t) + sha256_bytes;
constexpr std::size_t maximum_identity_bytes = 64u * 1024u;
constexpr std::size_t maximum_text_bytes = 1024u * 1024u;
constexpr std::size_t maximum_container_entries = 2'000'000u;
constexpr std::size_t maximum_decode_allocation_bytes =
    512u * 1024u * 1024u;
constexpr IrProgramCacheLimits boot_ir_limits{
    192u * 1024u * 1024u,
    16'384u,
    262'144u,
    2'000'000u,
    4'000'000u,
    4'000'000u,
    2'000'000u,
    12u,
    768u * 1024u * 1024u};

class CodecError final : public std::runtime_error {
public:
    CodecError() : std::runtime_error("invalid boot analysis cache") {}
};

[[nodiscard]] bool lowercase_sha256(
    const std::string_view value) noexcept {
    return value.size() == sha256_bytes * 2u &&
           std::all_of(value.begin(), value.end(), [](const char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] std::uint8_t hex_nibble(const char value) {
    if (value >= '0' && value <= '9')
        return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
        return static_cast<std::uint8_t>(value - 'a' + 10);
    throw CodecError();
}

[[nodiscard]] std::array<std::uint8_t, sha256_bytes>
decode_sha256(const std::string_view value) {
    if (!lowercase_sha256(value)) throw CodecError();
    std::array<std::uint8_t, sha256_bytes> result{};
    for (std::size_t index = 0u; index < result.size(); ++index)
        result[index] = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(hex_nibble(value[index * 2u]) << 4u) |
            hex_nibble(value[index * 2u + 1u]));
    return result;
}

class Writer final {
public:
    explicit Writer(const std::size_t maximum_bytes)
        : maximum_bytes_(maximum_bytes) {
        if (maximum_bytes_ == 0u) throw CodecError();
    }

    void u8(const std::uint8_t value) {
        require(1u);
        bytes_.push_back(value);
    }

    void u16(const std::uint16_t value) {
        require(sizeof(value));
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void u32(const std::uint32_t value) {
        require(sizeof(value));
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void u64(const std::uint64_t value) {
        require(sizeof(value));
        for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void boolean(const bool value) {
        u8(value ? 1u : 0u);
    }

    void size(const std::size_t value) {
        u64(static_cast<std::uint64_t>(value));
    }

    template <typename Enum>
    void enumeration(const Enum value) {
        static_assert(std::is_enum_v<Enum>);
        u32(static_cast<std::uint32_t>(
            static_cast<std::underlying_type_t<Enum>>(value)));
    }

    void raw(const std::span<const std::uint8_t> bytes) {
        require(bytes.size());
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    void text(const std::string_view value) {
        if (value.size() > maximum_text_bytes) throw CodecError();
        u32(static_cast<std::uint32_t>(value.size()));
        raw(std::span(
            reinterpret_cast<const std::uint8_t*>(value.data()),
            value.size()));
    }

    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }

private:
    void require(const std::size_t count) const {
        if (count > maximum_bytes_ ||
            bytes_.size() > maximum_bytes_ - count)
            throw CodecError();
    }

    std::size_t maximum_bytes_ = 0u;
    std::vector<std::uint8_t> bytes_;
};

class Reader final {
public:
    explicit Reader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes),
          allocation_bytes_remaining_(maximum_decode_allocation_bytes) {}

    [[nodiscard]] std::uint8_t u8() {
        require(1u);
        return bytes_[offset_++];
    }

    [[nodiscard]] std::uint16_t u16() {
        require(sizeof(std::uint16_t));
        std::uint16_t result = 0u;
        for (std::size_t byte = 0u; byte < sizeof(result); ++byte)
            result |= static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(bytes_[offset_++]) <<
                (byte * 8u));
        return result;
    }

    [[nodiscard]] std::uint32_t u32() {
        require(sizeof(std::uint32_t));
        std::uint32_t result = 0u;
        for (std::size_t byte = 0u; byte < sizeof(result); ++byte)
            result |= static_cast<std::uint32_t>(bytes_[offset_++])
                      << (byte * 8u);
        return result;
    }

    [[nodiscard]] std::uint64_t u64() {
        require(sizeof(std::uint64_t));
        std::uint64_t result = 0u;
        for (std::size_t byte = 0u; byte < sizeof(result); ++byte)
            result |= static_cast<std::uint64_t>(bytes_[offset_++])
                      << (byte * 8u);
        return result;
    }

    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1u) throw CodecError();
        return value != 0u;
    }

    [[nodiscard]] std::size_t size() {
        const auto value = u64();
        if (value > std::numeric_limits<std::size_t>::max())
            throw CodecError();
        return static_cast<std::size_t>(value);
    }

    [[nodiscard]] std::size_t count(
        const std::size_t maximum = maximum_container_entries) {
        const auto value = static_cast<std::size_t>(u32());
        if (value > maximum) throw CodecError();
        return value;
    }

    template <typename Enum>
    [[nodiscard]] Enum enumeration(const Enum first, const Enum last) {
        static_assert(std::is_enum_v<Enum>);
        const auto value = u32();
        const auto minimum = static_cast<std::uint32_t>(
            static_cast<std::underlying_type_t<Enum>>(first));
        const auto maximum = static_cast<std::uint32_t>(
            static_cast<std::underlying_type_t<Enum>>(last));
        if (value < minimum || value > maximum) throw CodecError();
        return static_cast<Enum>(value);
    }

    [[nodiscard]] std::span<const std::uint8_t> raw(
        const std::size_t count) {
        require(count);
        const auto result = bytes_.subspan(offset_, count);
        offset_ += count;
        return result;
    }

    [[nodiscard]] std::string text() {
        const auto count = this->count(maximum_text_bytes);
        reserve_allocation(count, 1u);
        const auto bytes = raw(count);
        return std::string(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return offset_ == bytes_.size();
    }

    void reserve_allocation(const std::size_t count,
                            const std::size_t element_bytes,
                            const std::size_t minimum_encoded_bytes = 1u) {
        if (minimum_encoded_bytes == 0u ||
            count > remaining() / minimum_encoded_bytes ||
            (element_bytes != 0u &&
             count > std::numeric_limits<std::size_t>::max() /
                         element_bytes))
            throw CodecError();
        const auto bytes = count * element_bytes;
        if (bytes > allocation_bytes_remaining_) throw CodecError();
        allocation_bytes_remaining_ -= bytes;
    }

private:
    void require(const std::size_t count) const {
        if (count > remaining()) throw CodecError();
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0u;
    std::size_t allocation_bytes_remaining_ = 0u;
};

template <typename Value, typename Write>
void write_vector(Writer& output,
                  const std::vector<Value>& values,
                  Write&& write) {
    if (values.size() > maximum_container_entries) throw CodecError();
    output.u32(static_cast<std::uint32_t>(values.size()));
    for (const auto& value : values) write(output, value);
}

template <typename Value, typename Read>
[[nodiscard]] std::vector<Value> read_vector(
    Reader& input,
    Read&& read,
    const std::size_t maximum = maximum_container_entries,
    const std::size_t minimum_encoded_bytes = 1u) {
    const auto count = input.count(maximum);
    input.reserve_allocation(
        count, sizeof(Value), minimum_encoded_bytes);
    std::vector<Value> values;
    values.reserve(count);
    for (std::size_t index = 0u; index < count; ++index)
        values.push_back(read(input));
    return values;
}

void write_u32_vector(
    Writer& output,
    const std::vector<std::uint32_t>& values) {
    write_vector(
        output, values, [](Writer& writer, const std::uint32_t value) {
            writer.u32(value);
        });
}

[[nodiscard]] std::vector<std::uint32_t> read_u32_vector(
    Reader& input) {
    return read_vector<std::uint32_t>(
        input,
        [](Reader& reader) { return reader.u32(); },
        maximum_container_entries,
        sizeof(std::uint32_t));
}

template <typename Enum>
void write_enum_vector(Writer& output,
                       const std::vector<Enum>& values) {
    write_vector(output, values, [](Writer& writer, const Enum value) {
        writer.enumeration(value);
    });
}

template <typename Enum>
[[nodiscard]] std::vector<Enum> read_enum_vector(
    Reader& input,
    const Enum first,
    const Enum last) {
    return read_vector<Enum>(
        input,
        [first, last](Reader& reader) {
            return reader.enumeration(first, last);
        },
        maximum_container_entries,
        sizeof(std::uint32_t));
}

void write_optional_u32(
    Writer& output,
    const std::optional<std::uint32_t> value) {
    output.boolean(value.has_value());
    if (value.has_value()) output.u32(*value);
}

[[nodiscard]] std::optional<std::uint32_t> read_optional_u32(
    Reader& input) {
    if (!input.boolean()) return std::nullopt;
    return input.u32();
}

void write_function_candidate(
    Writer& output,
    const katana::analysis::FunctionCandidate& value) {
    output.u32(value.address);
    output.enumeration(value.confidence);
    output.enumeration(value.evidence);
    write_enum_vector(output, value.origins);
    output.u32(value.size);
}

[[nodiscard]] katana::analysis::FunctionCandidate
read_function_candidate(Reader& input) {
    katana::analysis::FunctionCandidate value;
    value.address = input.u32();
    value.confidence = input.enumeration(
        katana::analysis::AnalysisConfidence::Low,
        katana::analysis::AnalysisConfidence::Certain);
    value.evidence = input.enumeration(
        katana::analysis::ControlFlowEvidence::ProvenComplete,
        katana::analysis::ControlFlowEvidence::Unresolved);
    value.origins = read_enum_vector(
        input,
        katana::analysis::FunctionOrigin::EntryPoint,
        katana::analysis::FunctionOrigin::StoredCodeAddress);
    value.size = input.u32();
    return value;
}

void write_runtime_code_copy(
    Writer& output,
    const katana::analysis::RuntimeCodeCopy& value) {
    output.u32(value.setup_address);
    output.u32(value.loop_address);
    output.u32(value.source_begin);
    output.u32(value.source_end_inclusive);
    output.u32(value.source_byte_count);
    output.u32(std::bit_cast<std::uint32_t>(
        value.destination_vbr_delta));
    write_vector(
        output,
        value.patch_candidates,
        [](Writer& writer,
           const katana::analysis::RuntimeCodePatchCandidate& patch) {
            writer.u32(patch.store_instruction_address);
            writer.u32(patch.slot_address);
            writer.u32(patch.live_value);
            writer.u32(patch.target_address);
        });
    write_vector(
        output,
        value.mutable_ranges,
        [](Writer& writer,
           const katana::analysis::RuntimeCodeMutableRangeCandidate& range) {
            writer.u32(range.store_instruction_address);
            writer.u32(range.load_instruction_address);
            writer.u32(range.slot_address);
            writer.u32(range.size);
        });
    output.boolean(value.mutable_range_analysis_complete);
    output.enumeration(value.evidence);
    output.boolean(value.aot_candidates_only);
    output.text(value.reason);
}

[[nodiscard]] katana::analysis::RuntimeCodeCopy
read_runtime_code_copy(Reader& input) {
    katana::analysis::RuntimeCodeCopy value;
    value.setup_address = input.u32();
    value.loop_address = input.u32();
    value.source_begin = input.u32();
    value.source_end_inclusive = input.u32();
    value.source_byte_count = input.u32();
    value.destination_vbr_delta =
        std::bit_cast<std::int32_t>(input.u32());
    value.patch_candidates =
        read_vector<katana::analysis::RuntimeCodePatchCandidate>(
            input,
            [](Reader& reader) {
                return katana::analysis::RuntimeCodePatchCandidate{
                    reader.u32(),
                    reader.u32(),
                    reader.u32(),
                    reader.u32()};
            });
    value.mutable_ranges =
        read_vector<katana::analysis::RuntimeCodeMutableRangeCandidate>(
            input,
            [](Reader& reader) {
                return katana::analysis::
                    RuntimeCodeMutableRangeCandidate{
                        reader.u32(),
                        reader.u32(),
                        reader.u32(),
                        reader.u32()};
            });
    value.mutable_range_analysis_complete = input.boolean();
    value.evidence = input.enumeration(
        katana::analysis::ControlFlowEvidence::ProvenComplete,
        katana::analysis::ControlFlowEvidence::Unresolved);
    value.aot_candidates_only = input.boolean();
    value.reason = input.text();
    return value;
}

void write_indirect_resolution(
    Writer& output,
    const katana::analysis::IndirectControlFlowResolution& value) {
    output.u32(value.instruction_address);
    output.enumeration(value.kind);
    output.u8(value.register_index);
    output.enumeration(value.status);
    output.enumeration(value.evidence);
    output.enumeration(value.origin_class);
    write_enum_vector(output, value.evidence_origins);
    write_optional_u32(output, value.target);
    output.text(value.reason);
    write_u32_vector(output, value.targets);
    write_u32_vector(output, value.evidence_call_sites);
    write_u32_vector(output, value.evidence_callees);
    output.text(value.value_source);
    write_u32_vector(output, value.definition_sites);
    output.boolean(value.definition_complete);
    output.boolean(value.preceding_call);
    output.enumeration(value.instruction_kind);
    write_u32_vector(output, value.analysis_candidates);
}

[[nodiscard]] katana::analysis::IndirectControlFlowResolution
read_indirect_resolution(Reader& input) {
    katana::analysis::IndirectControlFlowResolution value;
    value.instruction_address = input.u32();
    value.kind = input.enumeration(
        katana::analysis::IndirectControlFlowKind::Jump,
        katana::analysis::IndirectControlFlowKind::Call);
    value.register_index = input.u8();
    if (value.register_index > 15u) throw CodecError();
    value.status = input.enumeration(
        katana::analysis::ResolutionStatus::Resolved,
        katana::analysis::ResolutionStatus::Unresolved);
    value.evidence = input.enumeration(
        katana::analysis::ControlFlowEvidence::ProvenComplete,
        katana::analysis::ControlFlowEvidence::Unresolved);
    value.origin_class = input.enumeration(
        katana::analysis::IndirectControlFlowOriginClass::NotApplicable,
        katana::analysis::IndirectControlFlowOriginClass::RuntimePointer);
    value.evidence_origins = read_enum_vector(
        input,
        katana::analysis::AnalysisEvidenceOrigin::LocalValue,
        katana::analysis::AnalysisEvidenceOrigin::RuntimeClassification);
    value.target = read_optional_u32(input);
    value.reason = input.text();
    value.targets = read_u32_vector(input);
    value.evidence_call_sites = read_u32_vector(input);
    value.evidence_callees = read_u32_vector(input);
    value.value_source = input.text();
    value.definition_sites = read_u32_vector(input);
    value.definition_complete = input.boolean();
    value.preceding_call = input.boolean();
    value.instruction_kind = input.enumeration(
        katana::sh4::InstructionKind::Unknown,
        katana::sh4::InstructionKind::Fschg);
    value.analysis_candidates = read_u32_vector(input);
    return value;
}

void write_jump_table(
    Writer& output,
    const katana::analysis::JumpTableAnalysis& value) {
    output.u32(value.dispatch_address);
    output.u32(value.table_address);
    output.u32(value.target_base);
    output.size(value.requested_entries);
    output.enumeration(value.dispatch_kind);
    output.enumeration(value.encoding);
    output.boolean(value.resolved);
    output.boolean(value.aot_candidates_only);
    output.boolean(value.candidate_scan_truncated);
    output.enumeration(value.evidence);
    write_vector(
        output,
        value.entries,
        [](Writer& writer,
           const katana::analysis::JumpTableEntry& entry) {
            writer.size(entry.index);
            writer.u32(entry.entry_address);
            writer.u32(entry.target);
            writer.boolean(entry.accepted);
            writer.text(entry.reason);
        });
    output.text(value.reason);
}

[[nodiscard]] katana::analysis::JumpTableAnalysis
read_jump_table(Reader& input) {
    katana::analysis::JumpTableAnalysis value;
    value.dispatch_address = input.u32();
    value.table_address = input.u32();
    value.target_base = input.u32();
    value.requested_entries = input.size();
    if (value.requested_entries > maximum_container_entries)
        throw CodecError();
    value.dispatch_kind = input.enumeration(
        katana::analysis::JumpTableDispatchKind::Jump,
        katana::analysis::JumpTableDispatchKind::Call);
    value.encoding = input.enumeration(
        katana::analysis::JumpTableEncoding::Absolute32,
        katana::analysis::JumpTableEncoding::SignedRelative32);
    value.resolved = input.boolean();
    value.aot_candidates_only = input.boolean();
    value.candidate_scan_truncated = input.boolean();
    value.evidence = input.enumeration(
        katana::analysis::ControlFlowEvidence::ProvenComplete,
        katana::analysis::ControlFlowEvidence::Unresolved);
    value.entries = read_vector<katana::analysis::JumpTableEntry>(
        input,
        [](Reader& reader) {
            katana::analysis::JumpTableEntry entry;
            entry.index = reader.size();
            entry.entry_address = reader.u32();
            entry.target = reader.u32();
            entry.accepted = reader.boolean();
            entry.reason = reader.text();
            return entry;
        });
    value.reason = input.text();
    return value;
}

void write_guarded_entry(
    Writer& output,
    const katana::analysis::GuardedAotEntry& value) {
    output.u32(value.guest_address);
    output.u32(value.shared_body_address);
    output.enumeration(value.evidence);
    write_enum_vector(output, value.origins);
    write_u32_vector(output, value.source_sites);
    write_u32_vector(output, value.source_objects);
    output.text(value.source_identity);
    output.u64(value.source_byte_offset);
    output.u32(value.entry_byte_extent);
    output.text(value.entry_byte_identity);
}

[[nodiscard]] katana::analysis::GuardedAotEntry
read_guarded_entry(Reader& input) {
    katana::analysis::GuardedAotEntry value;
    value.guest_address = input.u32();
    value.shared_body_address = input.u32();
    value.evidence = input.enumeration(
        katana::analysis::ControlFlowEvidence::ProvenComplete,
        katana::analysis::ControlFlowEvidence::Unresolved);
    value.origins = read_enum_vector(
        input,
        katana::analysis::GuardedAotEntryOrigin::IndirectCall,
        katana::analysis::GuardedAotEntryOrigin::
            ReturnedCodeAddressTable);
    value.source_sites = read_u32_vector(input);
    value.source_objects = read_u32_vector(input);
    value.source_identity = input.text();
    value.source_byte_offset = input.u64();
    value.entry_byte_extent = input.u32();
    value.entry_byte_identity = input.text();
    return value;
}

void write_guarded_walk(
    Writer& output,
    const katana::analysis::GuardedCodeInventoryWalkDiagnostics& value) {
    output.size(value.inventory_region_count);
    output.size(value.inventory_region_budget);
    output.size(value.pending_inventory_region_count);
    output.size(value.inventory_region_block_budget);
    output.size(value.inventory_region_block_limited_regions);
    output.size(value.forwarded_store_context_budget);
    output.size(value.forwarded_store_context_limited_functions);
    write_vector(
        output,
        value.forwarded_store_context_limit_diagnostics,
        [](Writer& writer,
           const katana::analysis::
               ForwardedStoreContextLimitDiagnostic& diagnostic) {
            writer.u32(diagnostic.owner_entry);
            writer.u32(diagnostic.target);
            writer.u32(diagnostic.exemplar_root_call_site);
            writer.size(diagnostic.context_count);
            writer.size(diagnostic.root_call_site_count);
            writer.size(diagnostic.evaluation_count);
            writer.boolean(diagnostic.tail);
            writer.boolean(diagnostic.isolated);
            writer.enumeration(diagnostic.reason);
        });
    output.size(value.contextual_return_context_budget);
    output.size(value.contextual_return_context_limited_functions);
    output.size(value.contextual_return_evaluation_budget);
    output.size(value.contextual_return_evaluation_limited_functions);
    output.size(value.contextual_provenance_replay_capsule_budget);
    output.size(value.contextual_provenance_replay_capsule_limited_functions);
    output.size(value.contextual_provenance_replay_key_byte_budget);
    output.size(value.contextual_provenance_replay_key_byte_limited_functions);
    output.size(value.abi_stack_argument_slot_budget);
    output.size(
        value.abi_stack_argument_projection_truncated_functions);
    output.size(value.local_fixpoint_iteration_budget);
    output.size(value.local_fixpoint_limited_evaluations);
    output.size(value.maximum_local_fixpoint_iterations);
    output.boolean(value.resolution_root_logical_budget_exhausted);
    output.boolean(value.inventory_candidate_values_truncated);
    output.boolean(value.abi_stack_base_unresolved);
    output.boolean(value.inventory_tail_target_unresolved);
}

[[nodiscard]] katana::analysis::GuardedCodeInventoryWalkDiagnostics
read_guarded_walk(Reader& input) {
    katana::analysis::GuardedCodeInventoryWalkDiagnostics value;
    value.inventory_region_count = input.size();
    value.inventory_region_budget = input.size();
    value.pending_inventory_region_count = input.size();
    value.inventory_region_block_budget = input.size();
    value.inventory_region_block_limited_regions = input.size();
    value.forwarded_store_context_budget = input.size();
    value.forwarded_store_context_limited_functions = input.size();
    // Scheduling-local cache telemetry is deliberately absent from the
    // canonical artifact. Cache hits reconstruct it as zero so one semantic
    // key always has one stable payload.
    value.forwarded_store_evaluation_cache_hits = 0u;
    value.forwarded_store_evaluation_cache_misses = 0u;
    value.forwarded_store_context_limit_diagnostics =
        read_vector<katana::analysis::
                        ForwardedStoreContextLimitDiagnostic>(
            input,
            [](Reader& reader) {
                katana::analysis::ForwardedStoreContextLimitDiagnostic
                    diagnostic;
                diagnostic.owner_entry = reader.u32();
                diagnostic.target = reader.u32();
                diagnostic.exemplar_root_call_site = reader.u32();
                diagnostic.context_count = reader.size();
                diagnostic.root_call_site_count = reader.size();
                diagnostic.evaluation_count = reader.size();
                diagnostic.tail = reader.boolean();
                diagnostic.isolated = reader.boolean();
                diagnostic.reason = reader.enumeration(
                    katana::analysis::
                        ForwardedStoreContextLimitReason::RootCallSites,
                    katana::analysis::
                        ForwardedStoreContextLimitReason::
                            ReevaluationCount);
                return diagnostic;
            });
    value.contextual_return_context_budget = input.size();
    value.contextual_return_context_limited_functions = input.size();
    value.contextual_return_evaluation_budget = input.size();
    value.contextual_return_evaluation_limited_functions = input.size();
    value.contextual_provenance_replay_capsule_budget = input.size();
    value.contextual_provenance_replay_capsule_limited_functions =
        input.size();
    value.contextual_provenance_replay_key_byte_budget = input.size();
    value.contextual_provenance_replay_key_byte_limited_functions =
        input.size();
    value.abi_stack_argument_slot_budget = input.size();
    value.abi_stack_argument_projection_truncated_functions =
        input.size();
    value.local_fixpoint_iteration_budget = input.size();
    value.local_fixpoint_limited_evaluations = input.size();
    value.maximum_local_fixpoint_iterations = input.size();
    value.resolution_root_logical_budget_exhausted = input.boolean();
    value.inventory_candidate_values_truncated = input.boolean();
    value.abi_stack_base_unresolved = input.boolean();
    value.inventory_tail_target_unresolved = input.boolean();
    return value;
}

void write_analysis_scalars(
    Writer& output,
    const katana::analysis::ControlFlowAnalysisResult& value) {
    output.u64(value.jump_table_cache.hits);
    output.u64(value.jump_table_cache.misses);
    output.u64(value.jump_table_cache.evictions);
    output.size(value.fixpoint_iterations);
    output.size(value.function_summary_iterations);
    output.size(value.function_scc_count);
    output.size(value.unchanged_ingress_skips);
    output.size(value.function_iteration_budget);
    output.boolean(value.function_budget_exhausted);
    output.size(value.raw_stored_code_inventory_candidates);
    output.size(value.raw_stored_code_inventory_budget);
    output.boolean(value.raw_stored_code_inventory_truncated);
    output.size(value.guarded_code_inventory_candidates);
    output.size(value.guarded_code_inventory_budget);
    output.boolean(
        value.guarded_code_inventory_candidate_budget_exhausted);
    write_guarded_walk(output, value.guarded_code_inventory_walk);
    output.size(value.guarded_code_shape_validation_work);
    output.size(value.guarded_code_shape_validation_work_budget);
    output.size(
        value.guarded_code_shape_budget_exceeded_candidates);
    output.boolean(value.candidate_inventory_truncated);
    output.boolean(value.returned_table_scan_truncated);
}

void read_analysis_scalars(
    Reader& input,
    katana::analysis::ControlFlowAnalysisResult& value) {
    value.jump_table_cache.hits = input.u64();
    value.jump_table_cache.misses = input.u64();
    value.jump_table_cache.evictions = input.u64();
    value.fixpoint_iterations = input.size();
    value.function_summary_iterations = input.size();
    value.function_scc_count = input.size();
    value.unchanged_ingress_skips = input.size();
    value.function_iteration_budget = input.size();
    value.function_budget_exhausted = input.boolean();
    value.raw_stored_code_inventory_candidates = input.size();
    value.raw_stored_code_inventory_budget = input.size();
    value.raw_stored_code_inventory_truncated = input.boolean();
    value.guarded_code_inventory_candidates = input.size();
    value.guarded_code_inventory_budget = input.size();
    value.guarded_code_inventory_candidate_budget_exhausted =
        input.boolean();
    value.guarded_code_inventory_walk = read_guarded_walk(input);
    value.guarded_code_shape_validation_work = input.size();
    value.guarded_code_shape_validation_work_budget = input.size();
    value.guarded_code_shape_budget_exceeded_candidates = input.size();
    value.candidate_inventory_truncated = input.boolean();
    value.returned_table_scan_truncated = input.boolean();
}

void write_hardware_loop(
    Writer& output,
    const katana::analysis::HardwareNaturalLoop& value) {
    output.u32(value.header_address);
    output.u32(value.latch_address);
    output.u32(value.backedge_instruction_address);
    output.enumeration(value.classification);
    output.boolean(value.unresolved_guard_access);
    write_u32_vector(
        output, value.unresolved_guard_read_instruction_addresses);
    write_u32_vector(output, value.block_addresses);
    write_u32_vector(output, value.counter_instruction_addresses);
    write_vector(
        output,
        value.accesses,
        [](Writer& writer,
           const katana::analysis::HardwareLoopAccessEvidence& access) {
            writer.u32(access.instruction_address);
            writer.u32(access.guest_address);
            writer.u32(access.canonical_address);
            writer.enumeration(access.region);
            writer.enumeration(access.kind);
            writer.u8(access.width);
            writer.boolean(access.linear_memory);
            writer.boolean(access.aperture_mapped);
            writer.enumeration(access.runtime_support);
            writer.boolean(access.guards_loop);
        });
}

[[nodiscard]] katana::analysis::HardwareNaturalLoop
read_hardware_loop(Reader& input) {
    katana::analysis::HardwareNaturalLoop value;
    value.header_address = input.u32();
    value.latch_address = input.u32();
    value.backedge_instruction_address = input.u32();
    value.classification = input.enumeration(
        katana::analysis::HardwareLoopClassification::Counter,
        katana::analysis::HardwareLoopClassification::Unknown);
    value.unresolved_guard_access = input.boolean();
    value.unresolved_guard_read_instruction_addresses =
        read_u32_vector(input);
    value.block_addresses = read_u32_vector(input);
    value.counter_instruction_addresses = read_u32_vector(input);
    value.accesses =
        read_vector<katana::analysis::HardwareLoopAccessEvidence>(
            input,
            [](Reader& reader) {
                katana::analysis::HardwareLoopAccessEvidence access;
                access.instruction_address = reader.u32();
                access.guest_address = reader.u32();
                access.canonical_address = reader.u32();
                access.region = reader.enumeration(
                    katana::analysis::DreamcastHardwareRegion::
                        SystemBus,
                    katana::analysis::DreamcastHardwareRegion::Unknown);
                access.kind = reader.enumeration(
                    katana::analysis::HardwareAccessKind::Read,
                    katana::analysis::HardwareAccessKind::Prefetch);
                access.width = reader.u8();
                access.linear_memory = reader.boolean();
                access.aperture_mapped = reader.boolean();
                access.runtime_support = reader.enumeration(
                    katana::analysis::HardwareRuntimeSupport::
                        Implemented,
                    katana::analysis::HardwareRuntimeSupport::Unmapped);
                access.guards_loop = reader.boolean();
                return access;
            });
    return value;
}

void write_graph(
    Writer& output,
    const katana::analysis::AnalysisGraph& value) {
    output.enumeration(value.kind);
    write_vector(
        output,
        value.nodes,
        [](Writer& writer,
           const katana::analysis::AnalysisGraphNode& node) {
            writer.u32(node.address);
            writer.u32(node.end_address);
            writer.text(node.symbol);
        });
    write_vector(
        output,
        value.edges,
        [](Writer& writer,
           const katana::analysis::AnalysisGraphEdge& edge) {
            writer.u32(edge.source);
            write_optional_u32(writer, edge.target);
            writer.u32(edge.callsite);
            writer.enumeration(edge.kind);
        });
}

[[nodiscard]] katana::analysis::AnalysisGraph read_graph(
    Reader& input) {
    katana::analysis::AnalysisGraph value;
    value.kind = input.enumeration(
        katana::analysis::AnalysisGraphKind::ControlFlow,
        katana::analysis::AnalysisGraphKind::CallGraph);
    value.nodes = read_vector<katana::analysis::AnalysisGraphNode>(
        input,
        [](Reader& reader) {
            return katana::analysis::AnalysisGraphNode{
                reader.u32(), reader.u32(), reader.text()};
        });
    value.edges = read_vector<katana::analysis::AnalysisGraphEdge>(
        input,
        [](Reader& reader) {
            katana::analysis::AnalysisGraphEdge edge;
            edge.source = reader.u32();
            edge.target = read_optional_u32(reader);
            edge.callsite = reader.u32();
            edge.kind = reader.enumeration(
                katana::analysis::AnalysisGraphEdgeKind::Fallthrough,
                katana::analysis::AnalysisGraphEdgeKind::UnresolvedCall);
            return edge;
        });
    return value;
}

[[nodiscard]] std::vector<std::uint8_t> serialize_payload(
    const PreparedBootAnalysisArtifact& artifact) {
    Writer output(
        maximum_boot_analysis_cache_artifact_bytes -
        cache_header_bytes);
    const auto ir_payload = serialize_ir_program_cache_payload(
        artifact.lowered_program, boot_ir_limits);
    output.u32(boot_analysis_cache_ir_schema_version);
    output.u64(static_cast<std::uint64_t>(ir_payload.size()));
    output.raw(ir_payload);
    write_vector(
        output,
        artifact.analysis.recursive.functions,
        write_function_candidate);
    write_vector(
        output,
        artifact.analysis.runtime_code_copies.copies,
        write_runtime_code_copy);
    write_vector(
        output,
        artifact.analysis.indirect_control_flow,
        write_indirect_resolution);
    write_vector(
        output,
        artifact.analysis.jump_tables,
        write_jump_table);
    write_vector(
        output,
        artifact.analysis.guarded_aot_entries,
        write_guarded_entry);
    write_analysis_scalars(output, artifact.analysis);
    write_vector(
        output, artifact.hardware_loops, write_hardware_loop);
    write_graph(output, artifact.control_flow_graph);
    write_graph(output, artifact.call_graph);
    return std::move(output).finish();
}

[[nodiscard]] PreparedBootAnalysisArtifact parse_payload(
    Reader& input) {
    if (input.u32() != boot_analysis_cache_ir_schema_version)
        throw CodecError();
    const auto ir_bytes_u64 = input.u64();
    if (ir_bytes_u64 > boot_ir_limits.maximum_payload_bytes ||
        ir_bytes_u64 > input.remaining())
        throw CodecError();
    PreparedBootAnalysisArtifact artifact;
    artifact.lowered_program = parse_ir_program_cache_payload(
        input.raw(static_cast<std::size_t>(ir_bytes_u64)),
        boot_ir_limits);
    artifact.analysis.recursive.functions =
        read_vector<katana::analysis::FunctionCandidate>(
            input, read_function_candidate, boot_ir_limits.maximum_functions);
    artifact.analysis.runtime_code_copies.copies =
        read_vector<katana::analysis::RuntimeCodeCopy>(
            input, read_runtime_code_copy);
    artifact.analysis.indirect_control_flow =
        read_vector<katana::analysis::IndirectControlFlowResolution>(
            input, read_indirect_resolution);
    artifact.analysis.jump_tables =
        read_vector<katana::analysis::JumpTableAnalysis>(
            input, read_jump_table);
    artifact.analysis.guarded_aot_entries =
        read_vector<katana::analysis::GuardedAotEntry>(
            input, read_guarded_entry);
    read_analysis_scalars(input, artifact.analysis);
    artifact.hardware_loops =
        read_vector<katana::analysis::HardwareNaturalLoop>(
            input, read_hardware_loop);
    artifact.control_flow_graph = read_graph(input);
    artifact.call_graph = read_graph(input);
    return artifact;
}

void append_key_field(
    std::ostringstream& output,
    const std::string_view value) {
    output << value.size() << ':' << value << ';';
}

template <typename Value>
void append_key_value(
    std::ostringstream& output,
    const Value& value) {
    std::ostringstream field;
    field << value;
    append_key_field(output, field.str());
}

[[nodiscard]] bool valid_analysis_identity(
    const std::string_view value) noexcept {
    return lowercase_sha256(value);
}

[[nodiscard]] std::optional<std::uint16_t> current_opcode(
    const katana::io::ExecutableImage& image,
    const std::uint32_t address) {
    const auto* segment = image.find_segment(address, 2u);
    if (segment == nullptr) return std::nullopt;
    const auto offset = segment->byte_offset(address);
    if (!offset.has_value() ||
        *offset > segment->bytes.size() ||
        2u > segment->bytes.size() - *offset)
        return std::nullopt;
    return static_cast<std::uint16_t>(
        segment->bytes[*offset] |
        (static_cast<std::uint16_t>(segment->bytes[*offset + 1u])
         << 8u));
}

} // namespace

std::string make_boot_analysis_cache_key(
    const katana::io::ExecutableImage& image,
    const katana::analysis::AnalysisOverrides* const overrides,
    const std::string_view semantic_contract_identity,
    const std::string_view analysis_implementation_identity) {
    if (!valid_analysis_identity(analysis_implementation_identity) ||
        semantic_contract_identity.size() > maximum_identity_bytes)
        throw std::invalid_argument(
            "Boot-Analysecache-Key ist unvollstaendig.");

    std::ostringstream canonical;
    append_key_field(canonical, key_magic);
    append_key_value(canonical, boot_analysis_cache_schema_version);
    append_key_value(
        canonical, boot_analysis_cache_ir_schema_version);
    append_key_value(canonical, katana::analysis::abi_version);
    append_key_field(canonical, analysis_implementation_identity);
    append_key_field(canonical, semantic_contract_identity);

    append_key_value(canonical, image.segments().size());
    for (const auto& segment : image.segments()) {
        append_key_field(canonical, segment.name);
        append_key_value(canonical, segment.virtual_address);
        append_key_value(canonical, segment.file_offset);
        append_key_value(canonical, segment.memory_size);
        append_key_value(
            canonical,
            static_cast<std::underlying_type_t<katana::io::SegmentKind>>(
                segment.kind));
        append_key_value(canonical, segment.permissions.readable);
        append_key_value(canonical, segment.permissions.writable);
        append_key_value(canonical, segment.permissions.executable);
        append_key_value(
            canonical,
            static_cast<std::underlying_type_t<
                katana::io::ImageSourceKind>>(segment.source_kind));
        append_key_value(
            canonical,
            static_cast<std::underlying_type_t<
                katana::io::ImageLoadPhase>>(segment.load_phase));
        append_key_field(canonical, segment.local_source_name);
        append_key_value(canonical, segment.latent_source_size);
        append_key_value(canonical, segment.bytes.size());
        append_key_field(
            canonical,
            katana::io::sha256_bytes(std::string_view(
                reinterpret_cast<const char*>(segment.bytes.data()),
                segment.bytes.size())));
    }
    append_key_value(canonical, image.entry_points().size());
    for (const auto entry : image.entry_points())
        append_key_value(canonical, entry);
    append_key_value(canonical, image.symbols().size());
    for (const auto& symbol : image.symbols()) {
        append_key_field(canonical, symbol.name);
        append_key_value(canonical, symbol.address);
        append_key_value(canonical, symbol.size);
        append_key_value(
            canonical,
            static_cast<std::underlying_type_t<katana::io::SymbolKind>>(
                symbol.kind));
        append_key_value(
            canonical,
            static_cast<std::underlying_type_t<
                katana::io::SymbolBinding>>(symbol.binding));
    }
    append_key_value(canonical, image.relocations().size());
    for (const auto& relocation : image.relocations()) {
        append_key_value(canonical, relocation.address);
        append_key_value(canonical, relocation.raw_type);
        append_key_value(
            canonical,
            static_cast<std::underlying_type_t<
                katana::io::RelocationKind>>(relocation.kind));
        append_key_field(canonical, relocation.symbol_name);
        append_key_value(canonical, relocation.symbol_address);
        append_key_value(canonical, relocation.addend);
        append_key_value(
            canonical, relocation.applied_value.has_value());
        if (relocation.applied_value.has_value())
            append_key_value(canonical, *relocation.applied_value);
    }
    append_key_value(canonical, image.address_aliases().size());
    for (const auto& alias : image.address_aliases()) {
        append_key_value(canonical, alias.source_start);
        append_key_value(canonical, alias.runtime_start);
        append_key_value(canonical, alias.size);
    }
    append_key_value(
        canonical,
        static_cast<std::underlying_type_t<katana::io::GuestCallAbi>>(
            image.guest_call_abi()));
    append_key_value(
        canonical,
        static_cast<std::underlying_type_t<
            katana::io::InitialSnapshotPolicy>>(
            image.initial_snapshot_policy()));
    append_key_value(
        canonical, image.initial_snapshot_entry().has_value());
    if (image.initial_snapshot_entry().has_value())
        append_key_value(canonical, *image.initial_snapshot_entry());
    append_key_value(
        canonical,
        static_cast<std::underlying_type_t<
            katana::io::ImageAddressModel>>(image.address_model()));

    append_key_value(canonical, overrides != nullptr);
    if (overrides != nullptr) {
        append_key_value(canonical, overrides->version);
        append_key_value(
            canonical,
            static_cast<std::underlying_type_t<
                katana::analysis::AnalysisDirectiveMode>>(
                overrides->mode));
        append_key_value(canonical, overrides->functions.size());
        for (const auto& function : overrides->functions) {
            append_key_value(canonical, function.address);
            append_key_value(canonical, function.line);
            append_key_value(canonical, function.size);
        }
        append_key_value(
            canonical, overrides->function_boundaries.size());
        for (const auto& boundary : overrides->function_boundaries) {
            append_key_value(canonical, boundary.address);
            append_key_value(canonical, boundary.line);
            append_key_value(canonical, boundary.size);
        }
        append_key_value(canonical, overrides->jumps.size());
        for (const auto& jump : overrides->jumps) {
            append_key_value(canonical, jump.instruction_address);
            append_key_value(canonical, jump.target);
            append_key_value(canonical, jump.line);
        }
        append_key_value(canonical, overrides->jump_tables.size());
        for (const auto& table : overrides->jump_tables) {
            append_key_value(canonical, table.dispatch_address);
            append_key_value(canonical, table.table_address);
            append_key_value(canonical, table.entry_count);
            append_key_value(canonical, table.line);
            append_key_value(canonical, table.entry_stride);
            append_key_value(canonical, table.relative_base);
            append_key_value(
                canonical,
                static_cast<std::underlying_type_t<
                    katana::analysis::JumpTableOverrideEncoding>>(
                    table.encoding));
            append_key_value(
                canonical,
                static_cast<std::underlying_type_t<
                    katana::analysis::JumpTableOverrideTransfer>>(
                    table.transfer));
            append_key_value(canonical, table.require_dispatch);
        }
    }
    return katana::io::sha256_bytes(canonical.str());
}

bool boot_analysis_artifact_cacheable(
    const PreparedBootAnalysisArtifact& artifact) noexcept {
    if (artifact.lowered_program.empty() ||
        artifact.control_flow_graph.kind !=
            katana::analysis::AnalysisGraphKind::ControlFlow ||
        artifact.call_graph.kind !=
            katana::analysis::AnalysisGraphKind::CallGraph ||
        !artifact.analysis.guarded_aot_entry_rejections.empty() ||
        !katana::analysis::guarded_aot_inventory_complete(
            artifact.analysis) ||
        std::any_of(
            artifact.analysis.recursive.diagnostics.begin(),
            artifact.analysis.recursive.diagnostics.end(),
            katana::analysis::analysis_diagnostic_blocks_codegen))
        return false;
    return std::none_of(
        artifact.analysis.indirect_control_flow.begin(),
        artifact.analysis.indirect_control_flow.end(),
        [](const auto& resolution) {
            const auto status =
                katana::analysis::control_flow_report_status(
                    resolution);
            return status ==
                       katana::analysis::ControlFlowReportStatus::
                           GuardedPartial ||
                   status ==
                       katana::analysis::ControlFlowReportStatus::
                           Unresolved;
        });
}

std::vector<std::uint8_t> serialize_boot_analysis_cache(
    const std::string_view key,
    const PreparedBootAnalysisArtifact& artifact) {
    if (!lowercase_sha256(key) ||
        !boot_analysis_artifact_cacheable(artifact))
        throw std::invalid_argument(
            "Boot-Analysecache-Artefakt ist nicht cachebar.");
    const auto payload = serialize_payload(artifact);
    const auto payload_sha = decode_sha256(
        katana::io::sha256_bytes(std::string_view(
            reinterpret_cast<const char*>(payload.data()),
            payload.size())));
    Writer output(maximum_boot_analysis_cache_artifact_bytes);
    output.raw(cache_magic);
    output.u32(boot_analysis_cache_schema_version);
    output.raw(decode_sha256(key));
    output.u64(static_cast<std::uint64_t>(payload.size()));
    output.raw(payload_sha);
    output.raw(payload);
    return std::move(output).finish();
}

BootAnalysisCacheParseResult parse_boot_analysis_cache(
    const std::string_view expected_key,
    const std::span<const std::uint8_t> artifact) {
    if (!lowercase_sha256(expected_key) ||
        artifact.size() < cache_header_bytes ||
        artifact.size() >
            maximum_boot_analysis_cache_artifact_bytes)
        return {BootAnalysisCacheState::Corrupt, {}};
    try {
        Reader input(artifact);
        if (!std::ranges::equal(
                input.raw(cache_magic.size()), cache_magic))
            throw CodecError();
        const auto schema = input.u32();
        if (schema != boot_analysis_cache_schema_version)
            return {BootAnalysisCacheState::Miss, {}};
        if (!std::ranges::equal(
                input.raw(sha256_bytes),
                decode_sha256(expected_key)))
            return {BootAnalysisCacheState::Miss, {}};
        const auto payload_size_u64 = input.u64();
        if (payload_size_u64 >
                maximum_boot_analysis_cache_artifact_bytes -
                    cache_header_bytes ||
            payload_size_u64 != input.remaining() - sha256_bytes)
            throw CodecError();
        const auto expected_payload_sha = input.raw(sha256_bytes);
        const auto payload =
            input.raw(static_cast<std::size_t>(payload_size_u64));
        if (!input.empty()) throw CodecError();
        const auto actual_payload_sha = decode_sha256(
            katana::io::sha256_bytes(std::string_view(
                reinterpret_cast<const char*>(payload.data()),
                payload.size())));
        if (!std::ranges::equal(
                expected_payload_sha, actual_payload_sha))
            throw CodecError();
        Reader payload_input(payload);
        auto parsed = parse_payload(payload_input);
        if (!payload_input.empty() ||
            !boot_analysis_artifact_cacheable(parsed))
            throw CodecError();
        return {
            BootAnalysisCacheState::Hit, std::move(parsed)};
    } catch (const CodecError&) {
        return {BootAnalysisCacheState::Corrupt, {}};
    } catch (const std::runtime_error&) {
        return {BootAnalysisCacheState::Corrupt, {}};
    } catch (const std::bad_alloc&) {
        return {BootAnalysisCacheState::Corrupt, {}};
    } catch (const std::length_error&) {
        return {BootAnalysisCacheState::Corrupt, {}};
    }
}

bool validate_boot_analysis_cache_source_binding(
    PreparedBootAnalysisArtifact& artifact,
    const katana::io::ExecutableImage& image,
    const katana::analysis::AnalysisOverrides* const overrides,
    katana::analysis::DreamcastHardwareAudit*
        const rebuilt_hardware_audit) noexcept {
    if (!boot_analysis_positive_product_cache_enabled)
        return false;
    try {
        if (!boot_analysis_artifact_cacheable(artifact))
            return false;
        // The parsed graph is never an authority. Requiring it to be
        // internally well-formed limits the hint surface; everything used by
        // native emission is reconstructed below from current image bytes.
        katana::ir::require_valid_program(artifact.lowered_program);

        std::set<std::uint32_t> cached_instruction_addresses;
        std::set<std::uint32_t> cached_block_leaders;
        std::set<std::uint32_t> cached_function_entries;
        for (const auto& function : artifact.lowered_program) {
            cached_function_entries.insert(function.entry_address);
            for (const auto& block : function.blocks) {
                if (block.instructions.empty() ||
                    block.start_address !=
                        block.instructions.front().source_address)
                    return false;
                cached_block_leaders.insert(block.start_address);
                for (const auto& instruction : block.instructions)
                    cached_instruction_addresses.insert(
                        instruction.source_address);
            }
        }
        if (cached_instruction_addresses.empty() ||
            cached_block_leaders.empty() ||
            cached_function_entries.empty())
            return false;

        std::vector<katana::sh4::DisassemblyLine> current_lines;
        current_lines.reserve(cached_instruction_addresses.size());
        for (const auto address : cached_instruction_addresses) {
            const auto* segment = image.find_segment(address, 2u);
            const auto opcode = current_opcode(image, address);
            if (segment == nullptr || !segment->permissions.executable ||
                !opcode.has_value())
                return false;
            katana::sh4::DisassemblyLine line;
            line.address = address;
            line.opcode = *opcode;
            line.instruction = katana::sh4::decode(*opcode);
            if (!line.instruction.is_known()) return false;
            line.target_address =
                katana::sh4::calculate_direct_branch_target(
                    line.instruction, line.address);
            current_lines.push_back(std::move(line));
        }

        std::unordered_map<std::uint32_t, std::size_t> line_by_address;
        line_by_address.reserve(current_lines.size());
        for (std::size_t index = 0u; index < current_lines.size(); ++index)
            line_by_address.emplace(current_lines[index].address, index);
        for (std::size_t index = 0u; index < current_lines.size(); ++index) {
            const auto& owner = current_lines[index];
            if (!owner.instruction.has_delay_slot) continue;
            if (owner.address >
                std::numeric_limits<std::uint32_t>::max() - 2u)
                return false;
            const auto slot_address = owner.address + 2u;
            const auto slot = line_by_address.find(slot_address);
            if (slot == line_by_address.end() ||
                current_lines[slot->second].is_delay_slot)
                return false;
            current_lines[slot->second].is_delay_slot = true;
        }
        for (const auto leader : cached_block_leaders) {
            const auto line = line_by_address.find(leader);
            if (line == line_by_address.end() ||
                current_lines[line->second].is_delay_slot)
                return false;
        }

        auto rebuilt_analysis = std::move(artifact.analysis);
        const auto cached_guarded_entries =
            std::move(rebuilt_analysis.guarded_aot_entries);
        const auto cached_function_candidates =
            std::move(rebuilt_analysis.recursive.functions);
        rebuilt_analysis.recursive = {};
        rebuilt_analysis.runtime_code_copies = {};
        rebuilt_analysis.indirect_control_flow.clear();
        rebuilt_analysis.static_return_continuations.clear();
        rebuilt_analysis.jump_tables.clear();
        rebuilt_analysis.function_value_summaries.clear();
        rebuilt_analysis.resolved_edges.clear();
        rebuilt_analysis.sites.clear();
        rebuilt_analysis.guarded_aot_entries.clear();
        rebuilt_analysis.guarded_aot_entry_rejections.clear();
        rebuilt_analysis.instruction_arena.reset();
        rebuilt_analysis.block_spans.clear();
        rebuilt_analysis.evidence_ids = {};
        rebuilt_analysis.jump_table_cache = {};
        rebuilt_analysis.directive_diagnostics.clear();
        rebuilt_analysis.symbolic_addresses.clear();

        std::set<std::uint32_t> function_roots =
            cached_function_entries;
        for (const auto& candidate : cached_function_candidates) {
            // Candidate-only decode failures are deliberately absent from the
            // native program. They cannot become cache-hit function seeds.
            if (cached_block_leaders.contains(candidate.address))
                function_roots.insert(candidate.address);
        }
        function_roots.insert(
            image.entry_points().begin(), image.entry_points().end());
        if (overrides != nullptr) {
            for (const auto& function : overrides->functions)
                function_roots.insert(function.address);
        }
        if (std::any_of(
                function_roots.begin(),
                function_roots.end(),
                [&](const auto address) {
                    return !cached_block_leaders.contains(address);
                }))
            return false;

        std::map<std::uint32_t, std::uint32_t> exact_function_sizes;
        for (const auto& symbol : image.symbols()) {
            if (symbol.kind != katana::io::SymbolKind::Function ||
                symbol.size == 0u ||
                (symbol.size & 1u) != 0u ||
                !function_roots.contains(symbol.address) ||
                image.find_segment(symbol.address, symbol.size) == nullptr)
                continue;
            exact_function_sizes.try_emplace(
                symbol.address, symbol.size);
        }
        if (overrides != nullptr) {
            for (const auto& function : overrides->functions) {
                if (function.size == 0u) continue;
                if ((function.size & 1u) != 0u) return false;
                const auto* segment =
                    image.find_segment(function.address, function.size);
                if (segment == nullptr ||
                    !segment->permissions.executable)
                    return false;
                const auto [existing, inserted] =
                    exact_function_sizes.emplace(
                        function.address, function.size);
                if (!inserted && existing->second != function.size)
                    return false;
            }
            for (const auto& boundary : overrides->function_boundaries) {
                if (boundary.size == 0u ||
                    (boundary.size & 1u) != 0u)
                    return false;
                const auto* segment = image.find_segment(
                    boundary.address, boundary.size);
                if (segment == nullptr ||
                    !segment->permissions.executable)
                    return false;
                const auto [existing, inserted] =
                    exact_function_sizes.emplace(
                        boundary.address, boundary.size);
                if (!inserted && existing->second != boundary.size)
                    return false;
            }
        }

        for (const auto address : function_roots) {
            katana::analysis::FunctionCandidate candidate;
            candidate.address = address;
            candidate.confidence =
                katana::analysis::AnalysisConfidence::High;
            candidate.evidence =
                katana::analysis::ControlFlowEvidence::ProvenComplete;
            if (std::find(
                    image.entry_points().begin(),
                    image.entry_points().end(),
                    address) != image.entry_points().end()) {
                candidate.origins.push_back(
                    katana::analysis::FunctionOrigin::EntryPoint);
                candidate.confidence =
                    katana::analysis::AnalysisConfidence::Certain;
            }
            if (overrides != nullptr) {
                const auto found = std::find_if(
                    overrides->functions.begin(),
                    overrides->functions.end(),
                    [address](const auto& value) {
                        return value.address == address;
                    });
                if (found != overrides->functions.end()) {
                    candidate.origins.push_back(
                        overrides->mode ==
                                katana::analysis::
                                    AnalysisDirectiveMode::Override
                            ? katana::analysis::FunctionOrigin::UserOverride
                            : katana::analysis::FunctionOrigin::UserHint);
                    candidate.confidence =
                        katana::analysis::AnalysisConfidence::Certain;
                }
            }
            if (std::any_of(
                    image.symbols().begin(),
                    image.symbols().end(),
                    [address](const auto& symbol) {
                        return symbol.address == address &&
                               symbol.kind ==
                                   katana::io::SymbolKind::Function;
                    })) {
                candidate.origins.push_back(
                    katana::analysis::FunctionOrigin::Symbol);
            }
            if (candidate.origins.empty())
                candidate.origins.push_back(
                    katana::analysis::FunctionOrigin::DirectCall);
            if (const auto exact =
                    exact_function_sizes.find(address);
                exact != exact_function_sizes.end())
                candidate.size = exact->second;
            rebuilt_analysis.recursive.functions.push_back(
                std::move(candidate));
        }

        std::set<std::uint32_t> guarded_addresses;
        for (const auto& cached : cached_guarded_entries) {
            if (!guarded_addresses.insert(
                    cached.guest_address).second ||
                !cached_block_leaders.contains(cached.guest_address))
                return false;
            katana::analysis::GuardedAotEntry entry;
            entry.guest_address = cached.guest_address;
            // Shared-body ownership is diagnostic only and cannot be proven
            // from bytes. Keep every guarded entry independently dispatchable.
            entry.shared_body_address = cached.guest_address;
            entry.evidence =
                katana::analysis::ControlFlowEvidence::GuardedPartial;
            entry.origins = {
                katana::analysis::GuardedAotEntryOrigin::
                    StoredCodeAddress};
            entry.source_sites = {cached.guest_address};
            rebuilt_analysis.guarded_aot_entries.push_back(
                std::move(entry));
        }

        for (const auto& line : current_lines) {
            const auto lowered = katana::ir::lower_instruction(line);
            if (lowered.operation !=
                    katana::ir::Operation::JumpRegister &&
                lowered.operation !=
                    katana::ir::Operation::CallRegister)
                continue;
            katana::analysis::IndirectControlFlowResolution resolution;
            resolution.instruction_address = line.address;
            resolution.kind =
                lowered.operation ==
                        katana::ir::Operation::CallRegister
                    ? katana::analysis::IndirectControlFlowKind::Call
                    : katana::analysis::IndirectControlFlowKind::Jump;
            resolution.register_index = lowered.branch_register;
            resolution.status =
                katana::analysis::ResolutionStatus::Unresolved;
            resolution.evidence =
                katana::analysis::ControlFlowEvidence::RuntimeOnly;
            resolution.origin_class =
                katana::analysis::
                    IndirectControlFlowOriginClass::RuntimePointer;
            resolution.evidence_origins = {
                katana::analysis::AnalysisEvidenceOrigin::
                    RuntimeClassification};
            resolution.reason =
                "cache-hit-runtime-authoritative-indirect-target";
            resolution.value_source =
                "current-runtime-register";
            resolution.instruction_kind = line.instruction.kind;
            rebuilt_analysis.indirect_control_flow.push_back(
                std::move(resolution));
        }

        if (overrides != nullptr) {
            for (const auto& declaration : overrides->jump_tables) {
                const auto dispatch =
                    line_by_address.find(
                        declaration.dispatch_address);
                if (dispatch == line_by_address.end()) {
                    if (declaration.require_dispatch) return false;
                    continue;
                }
                const auto lowered = katana::ir::lower_instruction(
                    current_lines[dispatch->second]);
                const auto is_call =
                    lowered.operation ==
                    katana::ir::Operation::CallRegister;
                const auto is_jump =
                    lowered.operation ==
                    katana::ir::Operation::JumpRegister;
                if (!is_call && !is_jump) return false;
                if ((declaration.transfer ==
                         katana::analysis::
                             JumpTableOverrideTransfer::Call &&
                     !is_call) ||
                    (declaration.transfer ==
                         katana::analysis::
                             JumpTableOverrideTransfer::Jump &&
                     !is_jump))
                    return false;
                const auto encoding = [&] {
                    switch (declaration.encoding) {
                    case katana::analysis::
                        JumpTableOverrideEncoding::Absolute32:
                        return katana::analysis::
                            JumpTableEncoding::Absolute32;
                    case katana::analysis::
                        JumpTableOverrideEncoding::SignedRelative16:
                        return katana::analysis::
                            JumpTableEncoding::SignedRelative16;
                    case katana::analysis::
                        JumpTableOverrideEncoding::SignedRelative32:
                        return katana::analysis::
                            JumpTableEncoding::SignedRelative32;
                    }
                    return katana::analysis::
                        JumpTableEncoding::Absolute32;
                }();
                auto table =
                    encoding ==
                            katana::analysis::
                                JumpTableEncoding::Absolute32 &&
                        declaration.entry_stride ==
                            sizeof(std::uint32_t) &&
                        declaration.relative_base == 0u
                        ? katana::analysis::analyze_jump_table(
                              image,
                              declaration.dispatch_address,
                              declaration.table_address,
                              declaration.entry_count)
                        : katana::analysis::
                              analyze_declared_jump_table(
                                  image,
                                  declaration.dispatch_address,
                                  declaration.table_address,
                                  declaration.relative_base,
                                  declaration.entry_count,
                                  declaration.entry_stride,
                                  encoding);
                table.dispatch_kind =
                    is_call
                        ? katana::analysis::
                              JumpTableDispatchKind::Call
                        : katana::analysis::
                              JumpTableDispatchKind::Jump;
                table.evidence =
                    overrides->mode ==
                            katana::analysis::
                                AnalysisDirectiveMode::Override
                        ? katana::analysis::
                              ControlFlowEvidence::ForcedOverride
                        : katana::analysis::
                              ControlFlowEvidence::HintCandidate;
                rebuilt_analysis.jump_tables.push_back(
                    std::move(table));
            }
        }

        rebuilt_analysis.recursive.instructions = current_lines;
        rebuilt_analysis.recursive
            .guarded_candidate_instruction_addresses.assign(
                cached_instruction_addresses.begin(),
                cached_instruction_addresses.end());
        rebuilt_analysis.recursive.contextual_instructions.reserve(
            current_lines.size());
        for (const auto& line : current_lines) {
            katana::analysis::ContextualInstruction contextual;
            contextual.line = line;
            contextual.incoming_address = line.address;
            if (line.is_delay_slot)
                contextual.delay_slot_owner = line.address - 2u;
            contextual.evidence =
                katana::analysis::ControlFlowEvidence::RuntimeOnly;
            rebuilt_analysis.recursive.contextual_instructions.push_back(
                std::move(contextual));
        }

        const auto safepoints =
            katana::ir::architectural_safepoint_block_leaders(
                rebuilt_analysis);
        std::vector<std::uint32_t> all_block_leaders(
            cached_block_leaders.begin(),
            cached_block_leaders.end());
        all_block_leaders.insert(
            all_block_leaders.end(),
            safepoints.begin(),
            safepoints.end());
        std::sort(
            all_block_leaders.begin(), all_block_leaders.end());
        all_block_leaders.erase(
            std::unique(
                all_block_leaders.begin(),
                all_block_leaders.end()),
            all_block_leaders.end());

        auto rebuilt_program = katana::ir::lower_program(
            rebuilt_analysis, all_block_leaders);
        katana::ir::require_valid_program(rebuilt_program);

        std::set<std::uint32_t> rebuilt_instruction_addresses;
        std::set<std::uint32_t> rebuilt_block_leaders;
        std::set<std::uint32_t> rebuilt_function_entries;
        for (const auto& function : rebuilt_program) {
            rebuilt_function_entries.insert(function.entry_address);
            for (const auto& block : function.blocks) {
                rebuilt_block_leaders.insert(block.start_address);
                for (const auto& instruction : block.instructions)
                    rebuilt_instruction_addresses.insert(
                        instruction.source_address);
            }
        }
        if (rebuilt_instruction_addresses !=
                cached_instruction_addresses ||
            std::any_of(
                cached_block_leaders.begin(),
                cached_block_leaders.end(),
                [&](const auto leader) {
                    return !rebuilt_block_leaders.contains(leader);
                }))
            return false;

        for (const auto entry : rebuilt_function_entries) {
            const auto exists = std::any_of(
                rebuilt_analysis.recursive.functions.begin(),
                rebuilt_analysis.recursive.functions.end(),
                [entry](const auto& candidate) {
                    return candidate.address == entry;
                });
            if (exists) continue;
            rebuilt_analysis.recursive.functions.push_back(
                {entry,
                 katana::analysis::AnalysisConfidence::High,
                 katana::analysis::
                     ControlFlowEvidence::ProvenComplete,
                 {katana::analysis::FunctionOrigin::DirectCall},
                 0u});
        }
        std::sort(
            rebuilt_analysis.recursive.functions.begin(),
            rebuilt_analysis.recursive.functions.end(),
            [](const auto& left, const auto& right) {
                return left.address < right.address;
            });

        std::unordered_map<
            const katana::io::ImageSegment*,
            std::string>
            segment_identities;
        for (auto& entry : rebuilt_analysis.guarded_aot_entries) {
            const auto line = line_by_address.find(entry.guest_address);
            if (line == line_by_address.end() ||
                !rebuilt_block_leaders.contains(entry.guest_address))
                return false;
            entry.entry_byte_extent =
                current_lines[line->second].instruction.has_delay_slot
                    ? 4u
                    : 2u;
            const auto* segment = image.find_segment(
                entry.guest_address, entry.entry_byte_extent);
            const auto byte_offset =
                segment != nullptr
                    ? segment->byte_offset(entry.guest_address)
                    : std::optional<std::size_t>{};
            const auto source_offset =
                segment != nullptr
                    ? segment->source_byte_offset(
                          entry.guest_address)
                    : std::optional<std::uint64_t>{};
            if (segment == nullptr || !byte_offset.has_value() ||
                !source_offset.has_value() ||
                *byte_offset > segment->bytes.size() ||
                entry.entry_byte_extent >
                    segment->bytes.size() - *byte_offset)
                return false;
            auto& source_identity = segment_identities[segment];
            if (source_identity.empty()) {
                source_identity =
                    "sha256:" +
                    katana::io::sha256_bytes(std::string_view(
                        reinterpret_cast<const char*>(
                            segment->bytes.data()),
                        segment->bytes.size()));
            }
            entry.source_identity = source_identity;
            entry.source_byte_offset = *source_offset;
            entry.entry_byte_identity =
                "sha256:" +
                katana::io::sha256_bytes(std::string_view(
                    reinterpret_cast<const char*>(
                        segment->bytes.data() + *byte_offset),
                    entry.entry_byte_extent));
        }

        for (const auto& table : rebuilt_analysis.jump_tables) {
            if (!table.resolved) continue;
            for (const auto& entry : table.entries) {
                if (entry.accepted &&
                    !rebuilt_block_leaders.contains(entry.target))
                    return false;
            }
        }

        rebuilt_analysis.runtime_code_copies =
            katana::analysis::analyze_runtime_code_copies(
                image, current_lines);

        const auto source_blocks =
            katana::analysis::build_basic_blocks(
                current_lines,
                rebuilt_analysis.resolved_edges,
                all_block_leaders);
        auto instruction_arena =
            std::make_shared<katana::analysis::InstructionArena>(
                current_lines);
        rebuilt_analysis.block_spans =
            katana::analysis::build_block_spans(
                *instruction_arena, source_blocks);
        rebuilt_analysis.instruction_arena =
            std::move(instruction_arena);

        katana::analysis::SymbolNameIndex symbol_names(image);
        std::set<std::uint32_t> symbolic_addresses;
        for (const auto& line : current_lines)
            symbolic_addresses.insert(line.address);
        symbolic_addresses.insert(
            rebuilt_function_entries.begin(),
            rebuilt_function_entries.end());
        for (const auto address : symbolic_addresses) {
            if (auto symbolic = symbol_names.resolve(address);
                symbolic.has_value())
                rebuilt_analysis.symbolic_addresses.push_back(
                    std::move(*symbolic));
        }

        auto hardware_audit =
            katana::analysis::audit_dreamcast_hardware(
                image, rebuilt_analysis);
        auto control_flow_graph =
            katana::analysis::build_control_flow_graph(
                rebuilt_analysis);
        auto call_graph =
            katana::analysis::build_call_graph(rebuilt_analysis);

        artifact.analysis = std::move(rebuilt_analysis);
        artifact.lowered_program = std::move(rebuilt_program);
        artifact.hardware_loops = hardware_audit.loops;
        artifact.control_flow_graph =
            std::move(control_flow_graph);
        artifact.call_graph = std::move(call_graph);
        if (!boot_analysis_artifact_cacheable(artifact)) return false;
        if (rebuilt_hardware_audit != nullptr)
            *rebuilt_hardware_audit = std::move(hardware_audit);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace katana::codegen
