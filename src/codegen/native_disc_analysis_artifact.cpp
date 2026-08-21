#include "katana/codegen/native_disc_analysis_artifact.hpp"

#include "katana/codegen/latent_aot_analysis_cache.hpp"
#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace katana::codegen {
namespace {

constexpr std::string_view artifact_magic{"katana-native-disc-analysis-v1"};
constexpr std::size_t maximum_artifact_string_bytes = 64u * 1024u;
constexpr std::size_t maximum_artifact_collection_items = 4u * 1024u * 1024u;
constexpr std::size_t maximum_artifact_modules = 4096u;
constexpr std::size_t maximum_artifact_allocation_bytes =
    2u * maximum_native_disc_analysis_artifact_bytes;

class CodecError final : public std::runtime_error {
  public:
    CodecError() : std::runtime_error("native-disc-analysis-artifact-codec") {}
};

class Writer final {
  public:
    explicit Writer(const std::size_t maximum) : maximum_(maximum) {}

    void u8(const std::uint8_t value) { append(&value, sizeof(value)); }
    void u16(const std::uint16_t value) {
        const std::array<std::uint8_t, 2u> bytes{
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8u)};
        append(bytes.data(), bytes.size());
    }
    void u32(const std::uint32_t value) {
        std::array<std::uint8_t, 4u> bytes{};
        for (std::size_t index = 0u; index < bytes.size(); ++index)
            bytes[index] = static_cast<std::uint8_t>(value >> (index * 8u));
        append(bytes.data(), bytes.size());
    }
    void u64(const std::uint64_t value) {
        std::array<std::uint8_t, 8u> bytes{};
        for (std::size_t index = 0u; index < bytes.size(); ++index)
            bytes[index] = static_cast<std::uint8_t>(value >> (index * 8u));
        append(bytes.data(), bytes.size());
    }
    void boolean(const bool value) { u8(value ? 1u : 0u); }
    void text(const std::string_view value) {
        if (value.size() > maximum_artifact_string_bytes ||
            value.size() > std::numeric_limits<std::uint32_t>::max())
            throw CodecError();
        u32(static_cast<std::uint32_t>(value.size()));
        append(value.data(), value.size());
    }
    void blob(const std::span<const std::uint8_t> value) {
        if (value.size() > std::numeric_limits<std::uint64_t>::max())
            throw CodecError();
        u64(static_cast<std::uint64_t>(value.size()));
        append(value.data(), value.size());
    }
    template <typename Enum>
        requires std::is_enum_v<Enum>
    void enumeration(const Enum value) {
        using Underlying = std::underlying_type_t<Enum>;
        static_assert(sizeof(Underlying) == 1u);
        u8(static_cast<std::uint8_t>(value));
    }
    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }

  private:
    void append(const void* const data, const std::size_t size) {
        if (size > maximum_ - bytes_.size()) throw CodecError();
        const auto* first = static_cast<const std::uint8_t*>(data);
        bytes_.insert(bytes_.end(), first, first + size);
    }

    std::size_t maximum_ = 0u;
    std::vector<std::uint8_t> bytes_;
};

class Reader final {
  public:
    explicit Reader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::uint8_t u8() { return take(1u)[0u]; }
    [[nodiscard]] std::uint16_t u16() {
        const auto bytes = take(2u);
        return static_cast<std::uint16_t>(bytes[0u]) |
               static_cast<std::uint16_t>(bytes[1u] << 8u);
    }
    [[nodiscard]] std::uint32_t u32() {
        const auto bytes = take(4u);
        std::uint32_t value = 0u;
        for (std::size_t index = 0u; index < bytes.size(); ++index)
            value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8u);
        return value;
    }
    [[nodiscard]] std::uint64_t u64() {
        const auto bytes = take(8u);
        std::uint64_t value = 0u;
        for (std::size_t index = 0u; index < bytes.size(); ++index)
            value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8u);
        return value;
    }
    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1u) throw CodecError();
        return value != 0u;
    }
    [[nodiscard]] std::string text() {
        const auto size = u32();
        if (size > maximum_artifact_string_bytes) throw CodecError();
        charge(size);
        const auto bytes = take(size);
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    [[nodiscard]] std::span<const std::uint8_t> blob() {
        const auto size = u64();
        if (size > std::numeric_limits<std::size_t>::max()) throw CodecError();
        return take(static_cast<std::size_t>(size));
    }
    template <typename Enum>
        requires std::is_enum_v<Enum>
    [[nodiscard]] Enum enumeration(const Enum maximum) {
        const auto value = u8();
        if (value > static_cast<std::uint8_t>(maximum)) throw CodecError();
        return static_cast<Enum>(value);
    }
    template <typename T>
    [[nodiscard]] std::size_t count(const std::size_t maximum =
                                        maximum_artifact_collection_items) {
        const auto value = u64();
        if (value > maximum || value > std::numeric_limits<std::size_t>::max())
            throw CodecError();
        const auto result = static_cast<std::size_t>(value);
        if (result > maximum_artifact_allocation_bytes / sizeof(T))
            throw CodecError();
        charge(result * sizeof(T));
        return result;
    }
    [[nodiscard]] bool empty() const noexcept { return cursor_ == bytes_.size(); }

  private:
    [[nodiscard]] std::span<const std::uint8_t> take(const std::size_t size) {
        if (cursor_ > bytes_.size() || size > bytes_.size() - cursor_)
            throw CodecError();
        const auto result = bytes_.subspan(cursor_, size);
        cursor_ += size;
        return result;
    }
    void charge(const std::size_t bytes) {
        if (bytes > maximum_artifact_allocation_bytes - allocation_bytes_)
            throw CodecError();
        allocation_bytes_ += bytes;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_ = 0u;
    std::size_t allocation_bytes_ = 0u;
};

template <typename T, typename Emit>
void write_vector(Writer& output, const std::vector<T>& values, Emit&& emit) {
    output.u64(values.size());
    for (const auto& value : values) emit(value);
}

template <typename T, typename Parse>
std::vector<T> read_vector(Reader& input, Parse&& parse,
                           const std::size_t maximum =
                               maximum_artifact_collection_items) {
    const auto count = input.count<T>(maximum);
    std::vector<T> result;
    result.reserve(count);
    for (std::size_t index = 0u; index < count; ++index)
        result.push_back(parse());
    return result;
}

void write_u32_vector(Writer& output, const std::vector<std::uint32_t>& values) {
    write_vector(output, values, [&](const auto value) { output.u32(value); });
}

std::vector<std::uint32_t> read_u32_vector(Reader& input) {
    return read_vector<std::uint32_t>(input, [&] { return input.u32(); });
}

void write_u8_vector(Writer& output, const std::vector<std::uint8_t>& values) {
    write_vector(output, values, [&](const auto value) { output.u8(value); });
}

std::vector<std::uint8_t> read_u8_vector(Reader& input) {
    return read_vector<std::uint8_t>(input, [&] { return input.u8(); });
}

void write_hardware_loop(Writer& output,
                         const katana::analysis::HardwareNaturalLoop& loop) {
    using namespace katana::analysis;
    output.u32(loop.header_address);
    output.u32(loop.latch_address);
    output.u32(loop.backedge_instruction_address);
    output.enumeration(loop.classification);
    output.boolean(loop.unresolved_guard_access);
    write_u32_vector(output, loop.unresolved_guard_read_instruction_addresses);
    write_u32_vector(output, loop.block_addresses);
    write_u32_vector(output, loop.counter_instruction_addresses);
    write_vector(output, loop.local_progress_evidence, [&](const auto& value) {
        output.u32(value.condition_instruction_address);
        output.u32(value.progress_instruction_address);
        output.u8(value.register_index);
        output.enumeration(value.kind);
    });
    write_vector(output, loop.accesses, [&](const auto& value) {
        output.u32(value.instruction_address);
        output.u32(value.guest_address);
        output.u32(value.canonical_address);
        output.enumeration(value.region);
        output.enumeration(value.kind);
        output.u8(value.width);
        output.boolean(value.linear_memory);
        output.boolean(value.aperture_mapped);
        output.enumeration(value.runtime_support);
        output.boolean(value.guards_loop);
    });
    write_vector(output, loop.matching_write_candidates, [&](const auto& value) {
        output.u32(value.instruction_address);
        output.u32(value.guest_address);
        output.u32(value.canonical_address);
        output.u8(value.width);
    });
    output.boolean(loop.matching_write_candidates_truncated);
}

katana::analysis::HardwareNaturalLoop read_hardware_loop(Reader& input) {
    using namespace katana::analysis;
    HardwareNaturalLoop loop;
    loop.header_address = input.u32();
    loop.latch_address = input.u32();
    loop.backedge_instruction_address = input.u32();
    loop.classification = input.enumeration(HardwareLoopClassification::Unknown);
    loop.unresolved_guard_access = input.boolean();
    loop.unresolved_guard_read_instruction_addresses = read_u32_vector(input);
    loop.block_addresses = read_u32_vector(input);
    loop.counter_instruction_addresses = read_u32_vector(input);
    loop.local_progress_evidence = read_vector<HardwareLoopLocalProgressEvidence>(
        input, [&] {
            HardwareLoopLocalProgressEvidence value;
            value.condition_instruction_address = input.u32();
            value.progress_instruction_address = input.u32();
            value.register_index = input.u8();
            value.kind = input.enumeration(HardwareLoopLocalProgressKind::PointerTraversal);
            return value;
        });
    loop.accesses = read_vector<HardwareLoopAccessEvidence>(input, [&] {
        HardwareLoopAccessEvidence value;
        value.instruction_address = input.u32();
        value.guest_address = input.u32();
        value.canonical_address = input.u32();
        value.region = input.enumeration(DreamcastHardwareRegion::Unknown);
        value.kind = input.enumeration(HardwareAccessKind::Prefetch);
        value.width = input.u8();
        value.linear_memory = input.boolean();
        value.aperture_mapped = input.boolean();
        value.runtime_support = input.enumeration(HardwareRuntimeSupport::Unmapped);
        value.guards_loop = input.boolean();
        return value;
    });
    loop.matching_write_candidates = read_vector<HardwareLoopWriteCandidate>(
        input, [&] {
            HardwareLoopWriteCandidate value;
            value.instruction_address = input.u32();
            value.guest_address = input.u32();
            value.canonical_address = input.u32();
            value.width = input.u8();
            return value;
        });
    loop.matching_write_candidates_truncated = input.boolean();
    return loop;
}

void write_hardware_audit(Writer& output,
                          const katana::analysis::DreamcastHardwareAudit& audit) {
    output.text(audit.scope);
    output.u64(audit.image_bytes);
    output.u64(audit.reachable_instructions);
    output.u64(audit.reachable_functions);
    output.u64(audit.unknown_instructions);
    output.u64(audit.memory_access_sites);
    output.u64(audit.resolved_memory_access_sites);
    output.u64(audit.unresolved_memory_access_sites);
    write_vector(output, audit.unresolved_memory_instruction_sites, [&](const auto& value) {
        output.u32(value.instruction_address);
        output.u8(value.access_mask);
        output.u8(value.width_mask);
    });
    output.u64(audit.implemented_addresses);
    output.u64(audit.partial_addresses);
    output.u64(audit.known_gap_addresses);
    output.u64(audit.rejected_addresses);
    output.u64(audit.unmapped_addresses);
    output.u64(audit.unresolved_poll_guard_loops);
    write_vector(output, audit.instruction_diagnostics, [&](const auto& value) {
        output.u32(value.address);
        output.u16(value.opcode);
        output.text(value.reason);
        output.enumeration(value.evidence);
        write_u32_vector(output, value.incoming_addresses);
        write_u32_vector(output, value.delay_slot_owners);
    });
    write_vector(output, audit.references, [&](const auto& value) {
        output.u32(value.instruction_address);
        output.u32(value.guest_address);
        output.u32(value.canonical_address);
        output.enumeration(value.region);
        output.enumeration(value.kind);
        output.u8(value.width);
        output.boolean(value.aperture_mapped);
        output.enumeration(value.runtime_support);
        output.text(value.support_reason);
        output.text(value.register_name);
    });
    write_vector(output, audit.addresses, [&](const auto& value) {
        output.u32(value.guest_address);
        output.u32(value.canonical_address);
        output.enumeration(value.region);
        output.boolean(value.aperture_mapped);
        output.enumeration(value.runtime_support);
        output.text(value.support_reason);
        output.text(value.register_name);
        output.u64(value.reads);
        output.u64(value.writes);
        output.u64(value.prefetches);
        write_u8_vector(output, value.widths);
        write_u32_vector(output, value.instruction_addresses);
    });
    write_vector(output, audit.loops,
                 [&](const auto& loop) { write_hardware_loop(output, loop); });
}

katana::analysis::DreamcastHardwareAudit read_hardware_audit(Reader& input) {
    using namespace katana::analysis;
    DreamcastHardwareAudit audit;
    audit.scope = input.text();
    audit.image_bytes = input.u64();
    audit.reachable_instructions = input.u64();
    audit.reachable_functions = input.u64();
    audit.unknown_instructions = input.u64();
    audit.memory_access_sites = input.u64();
    audit.resolved_memory_access_sites = input.u64();
    audit.unresolved_memory_access_sites = input.u64();
    audit.unresolved_memory_instruction_sites =
        read_vector<UnresolvedMemoryInstructionSite>(input, [&] {
            UnresolvedMemoryInstructionSite value;
            value.instruction_address = input.u32();
            value.access_mask = input.u8();
            value.width_mask = input.u8();
            return value;
        });
    audit.implemented_addresses = input.u64();
    audit.partial_addresses = input.u64();
    audit.known_gap_addresses = input.u64();
    audit.rejected_addresses = input.u64();
    audit.unmapped_addresses = input.u64();
    audit.unresolved_poll_guard_loops = input.u64();
    audit.instruction_diagnostics = read_vector<HardwareInstructionDiagnostic>(
        input, [&] {
            HardwareInstructionDiagnostic value;
            value.address = input.u32();
            value.opcode = input.u16();
            value.reason = input.text();
            value.evidence = input.enumeration(ControlFlowEvidence::Unresolved);
            value.incoming_addresses = read_u32_vector(input);
            value.delay_slot_owners = read_u32_vector(input);
            return value;
        });
    audit.references = read_vector<HardwareAccessReference>(input, [&] {
        HardwareAccessReference value;
        value.instruction_address = input.u32();
        value.guest_address = input.u32();
        value.canonical_address = input.u32();
        value.region = input.enumeration(DreamcastHardwareRegion::Unknown);
        value.kind = input.enumeration(HardwareAccessKind::Prefetch);
        value.width = input.u8();
        value.aperture_mapped = input.boolean();
        value.runtime_support = input.enumeration(HardwareRuntimeSupport::Unmapped);
        value.support_reason = input.text();
        value.register_name = input.text();
        return value;
    });
    audit.addresses = read_vector<HardwareAddressSummary>(input, [&] {
        HardwareAddressSummary value;
        value.guest_address = input.u32();
        value.canonical_address = input.u32();
        value.region = input.enumeration(DreamcastHardwareRegion::Unknown);
        value.aperture_mapped = input.boolean();
        value.runtime_support = input.enumeration(HardwareRuntimeSupport::Unmapped);
        value.support_reason = input.text();
        value.register_name = input.text();
        value.reads = input.u64();
        value.writes = input.u64();
        value.prefetches = input.u64();
        value.widths = read_u8_vector(input);
        value.instruction_addresses = read_u32_vector(input);
        return value;
    });
    audit.loops = read_vector<HardwareNaturalLoop>(
        input, [&] { return read_hardware_loop(input); });
    // The loop records are the bounded source of truth. Keep the redundant
    // aggregate canonical when older producers observed the same loop through
    // more than one audit projection; the envelope hash still protects every
    // serialized byte and product admission is replayed after decode.
    audit.unresolved_poll_guard_loops =
        count_unresolved_poll_guard_loops(audit.loops);
    return audit;
}

IrProgramCacheLimits latent_ir_limits() {
    return {maximum_latent_aot_analysis_cache_artifact_bytes,
            maximum_latent_aot_analysis_cache_functions,
            maximum_latent_aot_analysis_cache_blocks,
            maximum_latent_aot_analysis_cache_instructions,
            maximum_latent_aot_analysis_cache_successors,
            maximum_latent_aot_analysis_cache_targets,
            maximum_latent_aot_analysis_cache_callsites,
            maximum_latent_aot_analysis_cache_parser_depth,
            256u * 1024u * 1024u};
}

void write_identity(Writer& output,
                    const NativeDiscAnalysisArtifactIdentity& identity) {
    output.text(identity.key);
    output.text(identity.content_identity);
    output.text(identity.boot_byte_identity);
    output.text(identity.project_identity);
    output.text(identity.analysis_contract_identity);
    output.text(identity.image_analysis_key);
    output.text(identity.game_project_identity);
    output.text(identity.native_port_identity);
    output.text(identity.native_port_artifact_identity);
    output.text(identity.analysis_implementation_identity);
    output.text(identity.analysis_cache_implementation_identity);
    output.text(identity.codegen_implementation_identity);
    output.u32(identity.analyzer_abi);
    output.u32(identity.backend_abi);
    output.u32(identity.analysis_mode);
    output.u32(identity.disc_volume_start_lba);
    output.u32(identity.disc_extent_lba_bias);
}

NativeDiscAnalysisArtifactIdentity read_identity(Reader& input) {
    NativeDiscAnalysisArtifactIdentity identity;
    identity.key = input.text();
    identity.content_identity = input.text();
    identity.boot_byte_identity = input.text();
    identity.project_identity = input.text();
    identity.analysis_contract_identity = input.text();
    identity.image_analysis_key = input.text();
    identity.game_project_identity = input.text();
    identity.native_port_identity = input.text();
    identity.native_port_artifact_identity = input.text();
    identity.analysis_implementation_identity = input.text();
    identity.analysis_cache_implementation_identity = input.text();
    identity.codegen_implementation_identity = input.text();
    identity.analyzer_abi = input.u32();
    identity.backend_abi = input.u32();
    identity.analysis_mode = input.u32();
    identity.disc_volume_start_lba = input.u32();
    identity.disc_extent_lba_bias = input.u32();
    return identity;
}

void write_latent_module(Writer& output, const PreparedLatentAotModule& module) {
    output.text(module.id);
    output.text(module.byte_identity);
    output.u32(module.byte_size);
    output.u32(module.source_address);
    write_vector(output, module.source_bindings, [&](const auto& value) {
        output.text(value.id);
        output.enumeration(value.transform);
        output.text(value.byte_identity);
        output.u64(value.disc_byte_offset);
        output.u32(value.byte_size);
    });
    write_u32_vector(output, module.entry_offsets);
    write_u32_vector(output, module.external_code_pointer_candidates);
    write_vector(output, module.external_code_pointer_evidence, [&](const auto& value) {
        output.u32(value.source_offset);
        output.u32(value.target_address);
        output.enumeration(value.kind);
        output.u32(value.sink_address);
        output.u8(value.argument_index);
    });
    write_vector(output, module.external_transfers, [&](const auto& value) {
        output.u32(value.source_offset);
        output.u32(value.target_address);
        output.enumeration(value.kind);
    });
    write_vector(output, module.block_identities, [&](const auto& value) {
        output.u32(value.source_offset);
        output.u32(value.size);
        output.text(value.sha256);
    });
    write_vector(output, module.function_identities, [&](const auto& value) {
        output.u32(value.source_offset);
        output.u32(value.size);
        output.text(value.sha256);
    });
    const auto ir = serialize_ir_program_cache_payload(module.program, latent_ir_limits());
    output.blob(ir);
}

PreparedLatentAotModule read_latent_module(Reader& input) {
    PreparedLatentAotModule module;
    module.id = input.text();
    module.byte_identity = input.text();
    module.byte_size = input.u32();
    module.source_address = input.u32();
    module.source_bindings = read_vector<PreparedLatentAotSourceBinding>(
        input, [&] {
            PreparedLatentAotSourceBinding value;
            value.id = input.text();
            value.transform = input.enumeration(LatentAotSourceTransform::SegaPrs);
            value.byte_identity = input.text();
            value.disc_byte_offset = input.u64();
            value.byte_size = input.u32();
            return value;
        }, 1024u);
    module.entry_offsets = read_u32_vector(input);
    module.external_code_pointer_candidates = read_u32_vector(input);
    module.external_code_pointer_evidence =
        read_vector<PreparedLatentAotCodePointerEvidence>(input, [&] {
            PreparedLatentAotCodePointerEvidence value;
            value.source_offset = input.u32();
            value.target_address = input.u32();
            value.kind = input.enumeration(
                PreparedLatentAotCodePointerEvidenceKind::CallbackArgument);
            value.sink_address = input.u32();
            value.argument_index = input.u8();
            return value;
        });
    module.external_transfers = read_vector<PreparedLatentAotExternalTransfer>(
        input, [&] {
            PreparedLatentAotExternalTransfer value;
            value.source_offset = input.u32();
            value.target_address = input.u32();
            value.kind = input.enumeration(PreparedLatentAotExternalTransferKind::Jump);
            return value;
        });
    module.block_identities = read_vector<PreparedLatentAotBlockIdentity>(input, [&] {
        PreparedLatentAotBlockIdentity value;
        value.source_offset = input.u32();
        value.size = input.u32();
        value.sha256 = input.text();
        return value;
    });
    module.function_identities = read_vector<PreparedLatentAotFunctionIdentity>(
        input, [&] {
            PreparedLatentAotFunctionIdentity value;
            value.source_offset = input.u32();
            value.size = input.u32();
            value.sha256 = input.text();
            return value;
        });
    module.program = parse_ir_program_cache_payload(input.blob(), latent_ir_limits());
    return module;
}

void write_latent(Writer& output, const LatentAotDiscovery& latent) {
    write_vector(output, latent.modules,
                 [&](const auto& module) { write_latent_module(output, module); });
    write_vector(output, latent.analysis_candidate_duration_ms,
                 [&](const auto value) { output.u64(value); });
    write_vector(output, latent.analysis_candidate_diagnostics, [&](const auto& value) {
        output.u32(value.transformed_byte_size);
        output.u32(value.source_byte_size);
        output.u32(value.entry_count);
        output.boolean(value.transformed_source);
        output.boolean(value.admitted);
        output.text(value.rejection);
        output.text(value.rejection_detail);
    });
    output.u64(latent.examined_files);
    output.u64(latent.rejected_files);
    output.u64(latent.duplicate_files);
    output.u64(latent.examined_bytes);
    output.u64(latent.prs_files_examined);
    output.u64(latent.prs_files_decoded);
    output.u64(latent.prs_files_rejected);
    output.u64(latent.prs_candidates_admitted);
    output.u64(latent.prs_decoded_bytes);
    output.boolean(latent.prs_decoded_budget_exhausted);
    output.u64(latent.analysis_cache_positive_hits);
    output.u64(latent.analysis_cache_negative_hits);
    output.u64(latent.analysis_cache_misses);
    output.u64(latent.analysis_cache_corrupt_entries);
    output.u64(latent.analysis_cache_stores);
    output.u64(latent.analysis_full_pipeline_runs);
}

LatentAotDiscovery read_latent(Reader& input) {
    LatentAotDiscovery latent;
    latent.modules = read_vector<PreparedLatentAotModule>(
        input, [&] { return read_latent_module(input); }, maximum_artifact_modules);
    latent.analysis_candidate_duration_ms = read_vector<std::uint64_t>(
        input, [&] { return input.u64(); });
    latent.analysis_candidate_diagnostics =
        read_vector<LatentAotDiscovery::CandidateDiagnostic>(input, [&] {
            LatentAotDiscovery::CandidateDiagnostic value;
            value.transformed_byte_size = input.u32();
            value.source_byte_size = input.u32();
            value.entry_count = input.u32();
            value.transformed_source = input.boolean();
            value.admitted = input.boolean();
            value.rejection = input.text();
            value.rejection_detail = input.text();
            return value;
        });
    latent.examined_files = input.u64();
    latent.rejected_files = input.u64();
    latent.duplicate_files = input.u64();
    latent.examined_bytes = input.u64();
    latent.prs_files_examined = input.u64();
    latent.prs_files_decoded = input.u64();
    latent.prs_files_rejected = input.u64();
    latent.prs_candidates_admitted = input.u64();
    latent.prs_decoded_bytes = input.u64();
    latent.prs_decoded_budget_exhausted = input.boolean();
    latent.analysis_cache_positive_hits = input.u64();
    latent.analysis_cache_negative_hits = input.u64();
    latent.analysis_cache_misses = input.u64();
    latent.analysis_cache_corrupt_entries = input.u64();
    latent.analysis_cache_stores = input.u64();
    latent.analysis_full_pipeline_runs = input.u64();
    return latent;
}

bool sorted_unique(const std::vector<std::uint32_t>& values) {
    return std::is_sorted(values.begin(), values.end()) &&
           std::adjacent_find(values.begin(), values.end()) == values.end();
}

void append_identity_key_field(std::string& material,
                               const std::string_view value) {
    material.push_back('s');
    material += std::to_string(value.size());
    material.push_back(':');
    material.append(value);
    material.push_back(';');
}

void append_identity_key_value(std::string& material,
                               const std::uint64_t value) {
    material.push_back('i');
    material += std::to_string(value);
    material.push_back(';');
}

} // namespace

std::string native_disc_analysis_artifact_identity_key(
    const NativeDiscAnalysisArtifactIdentity& identity) {
    std::string material;
    material.reserve(1024u);
    append_identity_key_field(
        material, "katana-native-disc-analysis-artifact-identity-v1");
    append_identity_key_value(
        material, native_disc_analysis_artifact_schema_version);
    append_identity_key_value(
        material, native_disc_analysis_artifact_codec_version);
    append_identity_key_field(material, identity.content_identity);
    append_identity_key_field(material, identity.boot_byte_identity);
    append_identity_key_field(material, identity.project_identity);
    append_identity_key_field(material, identity.analysis_contract_identity);
    append_identity_key_field(material, identity.game_project_identity);
    append_identity_key_field(material, identity.native_port_identity);
    append_identity_key_field(
        material, identity.native_port_artifact_identity);
    append_identity_key_field(
        material, identity.analysis_implementation_identity);
    append_identity_key_field(
        material, identity.analysis_cache_implementation_identity);
    append_identity_key_field(
        material, identity.codegen_implementation_identity);
    append_identity_key_value(material, identity.analyzer_abi);
    append_identity_key_value(material, identity.backend_abi);
    append_identity_key_value(material, identity.analysis_mode);
    append_identity_key_value(material, identity.disc_volume_start_lba);
    append_identity_key_value(material, identity.disc_extent_lba_bias);
    return katana::io::sha256_bytes(material);
}

bool native_disc_analysis_artifact_checkpointable(
    const NativeDiscAnalysisArtifact& artifact) noexcept {
    try {
        return !artifact.identity.key.empty() &&
               artifact.identity.key ==
                   native_disc_analysis_artifact_identity_key(
                       artifact.identity) &&
               !artifact.identity.content_identity.empty() &&
               !artifact.identity.boot_byte_identity.empty() &&
               !artifact.identity.project_identity.empty() &&
               !artifact.identity.analysis_contract_identity.empty() &&
               !artifact.identity.image_analysis_key.empty() &&
               !artifact.identity.native_port_identity.empty() &&
               !artifact.identity.native_port_artifact_identity.empty() &&
               !artifact.identity.analysis_implementation_identity.empty() &&
               !artifact.identity.analysis_cache_implementation_identity.empty() &&
               !artifact.identity.codegen_implementation_identity.empty() &&
               artifact.identity.analyzer_abi != 0u &&
               artifact.identity.backend_abi != 0u &&
               artifact.entry_address != 0u && artifact.boot_address != 0u &&
               artifact.boot_size != 0u &&
               artifact.backend_admitted &&
               boot_analysis_artifact_cacheable(artifact.primary) &&
               sorted_unique(artifact.external_primary_roots) &&
               sorted_unique(artifact.native_resume_entries) &&
               std::all_of(artifact.latent.modules.begin(),
                           artifact.latent.modules.end(), [](const auto& module) {
                               return !module.id.empty() &&
                                      !module.byte_identity.empty() &&
                                      module.byte_size != 0u &&
                                      !module.program.empty() &&
                                      sorted_unique(module.entry_offsets);
                           });
    } catch (...) {
        return false;
    }
}

bool native_disc_analysis_artifact_product_admissible(
    const NativeDiscAnalysisArtifact& artifact) noexcept {
    return native_disc_analysis_artifact_checkpointable(artifact) &&
           artifact.guarded_inventory_complete &&
           artifact.native_hardware_closure_complete &&
           artifact.replacement_reachability_proven &&
           artifact.native_hardware_gaps == 0u;
}

bool native_disc_analysis_artifact_publishable(
    const NativeDiscAnalysisArtifact& artifact) noexcept {
    return native_disc_analysis_artifact_product_admissible(artifact);
}

std::vector<std::uint8_t> serialize_native_disc_analysis_artifact(
    const NativeDiscAnalysisArtifact& artifact) {
    if (!native_disc_analysis_artifact_checkpointable(artifact))
        throw std::invalid_argument(
            "NativeDisc-Analysecheckpoint ist nicht publizierbar.");

    Writer payload(maximum_native_disc_analysis_artifact_bytes);
    write_identity(payload, artifact.identity);
    const auto primary = serialize_boot_analysis_cache(
        artifact.identity.image_analysis_key, artifact.primary);
    payload.blob(primary);
    write_latent(payload, artifact.latent);
    write_hardware_audit(payload, artifact.primary_hardware_audit);
    write_hardware_audit(payload, artifact.native_hardware_audit);
    write_u32_vector(payload, artifact.external_primary_roots);
    write_u32_vector(payload, artifact.native_resume_entries);
    payload.u32(artifact.entry_address);
    payload.u32(artifact.boot_address);
    payload.u64(artifact.boot_size);
    payload.u32(artifact.product_entry_address);
    payload.u32(artifact.product_entry_owner);
    payload.u64(artifact.known_hardware_sites);
    payload.u64(artifact.native_hardware_gaps);
    payload.u64(artifact.sdk_provider_candidates);
    payload.boolean(artifact.guarded_inventory_complete);
    payload.boolean(artifact.native_hardware_closure_complete);
    payload.boolean(artifact.replacement_reachability_proven);
    payload.boolean(artifact.backend_admitted);
    auto payload_bytes = std::move(payload).finish();

    const auto payload_sha = katana::io::sha256_bytes(std::string_view(
        reinterpret_cast<const char*>(payload_bytes.data()), payload_bytes.size()));
    Writer output(maximum_native_disc_analysis_artifact_bytes);
    output.text(artifact_magic);
    output.u32(native_disc_analysis_artifact_schema_version);
    output.u32(native_disc_analysis_artifact_codec_version);
    output.text(artifact.identity.key);
    output.text(payload_sha);
    output.blob(payload_bytes);
    return std::move(output).finish();
}

NativeDiscAnalysisArtifactParseResult parse_native_disc_analysis_artifact(
    const std::string_view expected_key,
    const std::span<const std::uint8_t> bytes) {
    if (expected_key.empty())
        return {NativeDiscAnalysisArtifactState::Miss, {}, "expected-key-empty"};
    if (bytes.empty() || bytes.size() > maximum_native_disc_analysis_artifact_bytes)
        return {NativeDiscAnalysisArtifactState::Corrupt, {}, "artifact-size"};
    std::string_view decode_stage = "envelope";
    try {
        Reader envelope(bytes);
        if (envelope.text() != artifact_magic ||
            envelope.u32() != native_disc_analysis_artifact_schema_version ||
            envelope.u32() != native_disc_analysis_artifact_codec_version)
            return {NativeDiscAnalysisArtifactState::Miss, {}, "schema"};
        const auto stored_key = envelope.text();
        if (stored_key != expected_key)
            return {NativeDiscAnalysisArtifactState::Miss, {}, "identity-key"};
        const auto expected_sha = envelope.text();
        const auto payload_bytes = envelope.blob();
        if (!envelope.empty()) throw CodecError();
        const auto actual_sha = katana::io::sha256_bytes(std::string_view(
            reinterpret_cast<const char*>(payload_bytes.data()), payload_bytes.size()));
        if (expected_sha != actual_sha) throw CodecError();

        decode_stage = "identity";
        Reader payload(payload_bytes);
        NativeDiscAnalysisArtifact artifact;
        artifact.identity = read_identity(payload);
        if (artifact.identity.key != expected_key ||
            artifact.identity.key !=
                native_disc_analysis_artifact_identity_key(
                    artifact.identity))
            throw CodecError();
        decode_stage = "primary";
        const auto primary = parse_boot_analysis_cache(
            artifact.identity.image_analysis_key, payload.blob());
        if (primary.state != BootAnalysisCacheState::Hit) throw CodecError();
        artifact.primary = std::move(primary.artifact);
        decode_stage = "latent";
        artifact.latent = read_latent(payload);
        decode_stage = "hardware-audits";
        artifact.primary_hardware_audit = read_hardware_audit(payload);
        artifact.native_hardware_audit = read_hardware_audit(payload);
        decode_stage = "roots";
        artifact.external_primary_roots = read_u32_vector(payload);
        artifact.native_resume_entries = read_u32_vector(payload);
        decode_stage = "summary";
        artifact.entry_address = payload.u32();
        artifact.boot_address = payload.u32();
        artifact.boot_size = payload.u64();
        artifact.product_entry_address = payload.u32();
        artifact.product_entry_owner = payload.u32();
        artifact.known_hardware_sites = payload.u64();
        artifact.native_hardware_gaps = payload.u64();
        artifact.sdk_provider_candidates = payload.u64();
        artifact.guarded_inventory_complete = payload.boolean();
        artifact.native_hardware_closure_complete = payload.boolean();
        artifact.replacement_reachability_proven = payload.boolean();
        artifact.backend_admitted = payload.boolean();
        decode_stage = "checkpoint-contract";
        if (!payload.empty() ||
            !native_disc_analysis_artifact_checkpointable(artifact))
            throw CodecError();
        return {NativeDiscAnalysisArtifactState::Hit, std::move(artifact), "hit"};
    } catch (const std::bad_alloc&) {
        return {NativeDiscAnalysisArtifactState::Corrupt, {}, "allocation"};
    } catch (const std::exception&) {
        return {
            NativeDiscAnalysisArtifactState::Corrupt,
            {},
            "codec-" + std::string(decode_stage)};
    }
}

} // namespace katana::codegen
