#include "katana/codegen/latent_aot_registry.hpp"

#include "native_aot_resume.hpp"
#include "structured_control_flow_progress.hpp"

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/hardware_audit.hpp"
#include "katana/analysis/abi.hpp"
#include "katana/analysis/code_address.hpp"
#include "katana/analysis/parallel_work.hpp"
#include "katana/build_contract.hpp"
#include "katana/codegen/cache.hpp"
#include "katana/codegen/latent_aot_analysis_cache.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/ir/register_liveness.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/runtime/iso9660.hpp"
#include "katana/runtime/block_table.hpp"
#include "katana/sh4/decoder.hpp"

#include "../runtime/prs_decode.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace katana::codegen {
namespace {

constexpr std::uint32_t iso_sector_size = 2048u;
constexpr std::size_t maximum_latent_aot_entry_hints =
    maximum_prepared_latent_aot_entry_hints;
constexpr std::size_t maximum_latent_aot_runtime_alias_entry_passes = 16u;
// The in-process replay cache is an optimization only.  Keep its ownership
// explicit and bounded so a retained source catalog cannot turn a long
// cross-image session into an unbounded second IR heap.  A budget miss keeps
// the complete existing entries and makes the next wave cold.
constexpr std::size_t maximum_latent_aot_session_static_cache_entries =
    256u;
constexpr std::size_t maximum_latent_aot_session_static_cache_bytes =
    static_cast<std::size_t>(512ull * 1024ull * 1024ull);
constexpr std::size_t maximum_latent_aot_source_bindings =
    maximum_prepared_latent_aot_source_bindings;
constexpr std::uint64_t maximum_validated_latent_aot_total_module_bytes =
    maximum_prepared_latent_aot_total_module_bytes;
constexpr std::uint64_t maximum_validated_latent_aot_total_source_bytes =
    maximum_prepared_latent_aot_total_source_bytes;
constexpr std::uint32_t latent_aot_runtime_page_size = 4096u;
constexpr std::uint32_t latent_aot_main_ram_begin = 0x8C000000u;
constexpr std::uint32_t latent_aot_main_ram_end = 0x8D000000u;
constexpr std::size_t minimum_latent_aot_runtime_cluster_targets = 16u;
constexpr std::size_t minimum_latent_aot_prefix_entry_table_targets = 3u;
constexpr std::size_t maximum_latent_aot_prefix_entry_table_targets = 64u;
constexpr std::size_t latent_aot_ff_padded_prefix_pointer_count = 2u;
constexpr std::size_t minimum_latent_aot_inferred_authoritative_entries = 1u;
constexpr std::size_t maximum_latent_aot_inferred_authoritative_candidates =
    maximum_latent_aot_source_bindings;
constexpr std::size_t maximum_latent_aot_indexed_call_table_targets = 64u;
constexpr std::size_t minimum_latent_aot_indexed_call_table_targets = 2u;
// A table-dispatch producer may be split at an independently reachable
// normal-entry leader even though the physical instructions remain a single
// straight-line slice.  Keep the cross-block proof deliberately small: this
// is discovery evidence, not a general value-propagation pass.
constexpr std::size_t
    maximum_latent_indexed_call_table_producer_instructions = 12u;
constexpr std::size_t maximum_latent_aot_descriptor_table_records = 256u;
constexpr std::size_t minimum_latent_aot_descriptor_table_targets = 2u;
constexpr std::uint32_t maximum_latent_aot_descriptor_table_stride = 256u;
constexpr std::uint32_t
    maximum_latent_aot_descriptor_object_field_displacement = 4096u;
constexpr std::size_t maximum_latent_aot_file_references = 4096u;
constexpr std::size_t maximum_analysis_implementation_identity_bytes = 4096u;
// The persistent FunctionValue epoch is live for an entire candidate pass.
// Its 512 MiB cap therefore remains part of the task admission reservation.
// The two 1 GiB exact-replay caches and the 1 GiB resolution-ready arena are
// different: they retain only finalized, pure artifacts and are now bounded
// child arenas charged to the parent executor at their actual retained size.
// A full parent evicts such aliases for exact recomputation instead of
// reducing any logical limit or candidate/root inventory.
constexpr std::uint64_t latent_aot_module_analysis_structural_reserve_bytes =
    512ull * 1024ull * 1024ull;
constexpr std::uint64_t latent_aot_function_value_cache_budget_bytes =
    2'048ull * 1024ull * 1024ull;
constexpr std::uint64_t latent_aot_function_value_ready_budget_bytes =
    1'024ull * 1024ull * 1024ull;
constexpr std::uint64_t latent_aot_module_analysis_reserve_bytes =
    latent_aot_module_analysis_structural_reserve_bytes;
constexpr std::string_view latent_aot_analysis_cache_artifact{
    "module-analysis.bin"};
constexpr std::string_view latent_aot_module_static_cache_artifact{
    "module-static.bin"};
constexpr std::uint32_t latent_aot_module_static_cache_schema_version = 2u;
constexpr std::array<std::uint8_t, 8u> latent_aot_module_static_cache_magic{
    'K', 'L', 'A', 'T', 'S', 'T', 'A', '1'};

[[nodiscard]] constexpr bool
latent_aot_module_transient_bytes_overflow(
    const std::size_t source_bytes) noexcept {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if constexpr (maximum < latent_aot_module_analysis_reserve_bytes)
        return true;
    constexpr auto reserve = static_cast<std::size_t>(
        latent_aot_module_analysis_reserve_bytes);
    return source_bytes > maximum - reserve;
}

[[nodiscard]] constexpr std::size_t
latent_aot_module_transient_bytes(const std::size_t source_bytes) noexcept {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (latent_aot_module_transient_bytes_overflow(source_bytes))
        return maximum;
    constexpr auto reserve = static_cast<std::size_t>(
        latent_aot_module_analysis_reserve_bytes);
    return source_bytes > maximum - reserve
               ? maximum
               : source_bytes + reserve;
}

void append_executor_snapshot(
    katana::ProgressCounterSnapshot& counters,
    const katana::analysis::ParallelWorkExecutorSnapshot& snapshot) {
    counters.active_workers = snapshot.running;
    counters.executor_running_workers = snapshot.running;
    counters.executor_waiting_workers = snapshot.waiting;
    counters.executor_idle_workers = snapshot.idle;
    counters.executor_queued_work = snapshot.queued;
    counters.executor_memory_blocked_work = snapshot.memory_blocked;
    counters.executor_continuations = snapshot.continuations;
    counters.analysis_memory_capacity_bytes = snapshot.memory_capacity;
    counters.analysis_memory_used_bytes = snapshot.memory_used;
    counters.analysis_memory_peak_bytes = snapshot.memory_peak;
}

struct DiscFileCandidate {
    std::uint32_t size = 0u;
    std::uint32_t source_address = 0u;
    std::vector<std::uint8_t> bytes;
    std::string byte_identity;
    std::vector<PreparedLatentAotSourceBinding> source_bindings;
    std::vector<std::uint32_t> entry_offsets;
    std::vector<std::uint32_t> explicit_entry_offsets;
    // Optional single-module audit evidence supplied by an independently
    // proven loader contract. Whole-disc discovery never populates it.
    std::optional<std::uint32_t> proven_runtime_base;
    // A transformed module may carry its own bounded, direct-mapped entry
    // table at offset zero.  Once the complete table shape,
    // module extent and every target have been validated against the exact
    // transformed byte identity, those offsets are authoritative in the same
    // stop-on-miss sense as external exact hints.  Keep the provenance
    // distinct: a derived table must never become a user-supplied hint or make
    // a rejected heuristic candidate fatal.
    bool inferred_authoritative_entry_table = false;
    // Exact complete-disassembly entries, retained separately from the
    // ordinary ingress offsets so block-only authority never becomes a
    // public callable root by accident.
    std::vector<CompleteDisassemblyEntryAuthority> authority_entries;
    CompleteDisassemblyModuleClass module_class =
        CompleteDisassemblyModuleClass::LatentLoaded;
};

bool candidate_has_authoritative_entries(
    const DiscFileCandidate& candidate) noexcept {
    return !candidate.explicit_entry_offsets.empty() ||
           candidate.inferred_authoritative_entry_table;
}

bool valid_sha256_identity(const std::string_view identity) noexcept {
    constexpr std::string_view prefix{"sha256:"};
    if (identity.size() != prefix.size() + 64u || !identity.starts_with(prefix))
        return false;
    for (const auto character : identity.substr(prefix.size())) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f')))
            return false;
    }
    return true;
}

bool valid_entry_hint(const LatentAotEntryHint& hint) noexcept {
    // byte_size binds the encoded disc extent.  It is deliberately not an
    // executable-size bound: a transformed source such as Sega PRS may have
    // an odd encoded size and an entry beyond that compressed byte count.
    // The identity-specific raw/decoded candidate path validates the aligned
    // entry against the resolved executable bytes before accepting the hint.
    const bool valid_kind =
        hint.entry_kind == CompleteDisassemblyEntryKind::DeclaredEntry ||
        hint.entry_kind == CompleteDisassemblyEntryKind::FunctionEntry ||
        hint.entry_kind == CompleteDisassemblyEntryKind::ControlFlowTarget ||
        hint.entry_kind == CompleteDisassemblyEntryKind::CodePointerTarget;
    const bool valid_module_class =
        hint.module_class == CompleteDisassemblyModuleClass::LatentLoaded ||
        hint.module_class == CompleteDisassemblyModuleClass::PrimaryStatic ||
        hint.module_class == CompleteDisassemblyModuleClass::FixedRuntimeImage;
    const bool valid_probe =
        hint.entry_byte_size == 0u ||
        (hint.entry_byte_size >= 2u && hint.entry_byte_size <= 256u &&
         (hint.entry_byte_size & 1u) == 0u &&
         valid_sha256_identity(hint.entry_byte_identity));
    return valid_sha256_identity(hint.byte_identity) && hint.byte_size != 0u &&
           valid_kind && valid_module_class && valid_probe &&
           (hint.module_relative_offset & 1u) == 0u &&
           (hint.source_address == 0u ||
            (hint.source_address & 0xFFFu) == 0u) &&
           (hint.proven_runtime_base == 0u ||
            (hint.proven_runtime_base &
             (latent_aot_runtime_page_size - 1u)) == 0u);
}

bool entry_hint_less(const LatentAotEntryHint& left,
                     const LatentAotEntryHint& right) noexcept {
    if (left.byte_identity != right.byte_identity)
        return left.byte_identity < right.byte_identity;
    if (left.disc_byte_offset != right.disc_byte_offset)
        return left.disc_byte_offset < right.disc_byte_offset;
    if (left.byte_size != right.byte_size) return left.byte_size < right.byte_size;
    if (left.module_relative_offset != right.module_relative_offset)
        return left.module_relative_offset < right.module_relative_offset;
    if (left.source_address != right.source_address)
        return left.source_address < right.source_address;
    if (left.proven_runtime_base != right.proven_runtime_base)
        return left.proven_runtime_base < right.proven_runtime_base;
    if (left.entry_kind != right.entry_kind)
        return static_cast<unsigned>(left.entry_kind) <
               static_cast<unsigned>(right.entry_kind);
    if (left.module_class != right.module_class)
        return static_cast<unsigned>(left.module_class) <
               static_cast<unsigned>(right.module_class);
    if (left.entry_byte_size != right.entry_byte_size)
        return left.entry_byte_size < right.entry_byte_size;
    return left.entry_byte_identity < right.entry_byte_identity;
}

std::vector<LatentAotEntryHint>
normalize_entry_hints(const std::span<const LatentAotEntryHint> entry_hints) {
    if (entry_hints.size() > maximum_latent_aot_entry_hints)
        throw std::invalid_argument("latent-aot-entry-hint-budget");
    std::vector<LatentAotEntryHint> normalized(entry_hints.begin(), entry_hints.end());
    if (std::any_of(normalized.begin(), normalized.end(),
                    [](const auto& hint) { return !valid_entry_hint(hint); }))
        throw std::invalid_argument("latent-aot-entry-hint-invalid");
    std::sort(normalized.begin(), normalized.end(), entry_hint_less);
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    return normalized;
}

std::string normalize_disc_reference(const std::string_view value) {
    std::string result;
    result.reserve(value.size() + 1u);
    if (value.empty() || value.front() != '/') result.push_back('/');
    for (const auto character : value) {
        auto normalized = character;
        if (normalized == '\\') normalized = '/';
        if (normalized >= 'a' && normalized <= 'z')
            normalized = static_cast<char>(normalized - 'a' + 'A');
        result.push_back(normalized);
    }
    const auto version = result.find(';');
    if (version != std::string::npos) result.resize(version);
    while (result.size() > 1u && result.back() == '/') result.pop_back();
    return result;
}

std::vector<std::string> normalize_file_references(
    const std::span<const std::string> references) {
    if (references.size() > maximum_latent_aot_file_references)
        throw std::invalid_argument("latent-aot-file-reference-budget");
    std::vector<std::string> result;
    result.reserve(references.size());
    for (const auto& reference : references) {
        if (reference.empty() || reference.size() > 256u)
            throw std::invalid_argument("latent-aot-file-reference-invalid");
        result.push_back(normalize_disc_reference(reference));
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::string_view disc_basename(const std::string_view value) noexcept {
    const auto slash = value.find_last_of('/');
    return value.substr(slash == std::string_view::npos ? 0u : slash + 1u);
}

bool disc_file_uses_sega_prs(const std::string_view value) {
    return normalize_disc_reference(value).ends_with(".PRS");
}

bool candidate_has_transformed_source(
    const DiscFileCandidate& candidate) noexcept {
    return std::any_of(
        candidate.source_bindings.begin(), candidate.source_bindings.end(),
        [](const auto& binding) {
            return binding.transform != LatentAotSourceTransform::Identity;
        });
}

bool source_binding_less(const PreparedLatentAotSourceBinding& left,
                         const PreparedLatentAotSourceBinding& right) noexcept {
    if (left.disc_byte_offset != right.disc_byte_offset)
        return left.disc_byte_offset < right.disc_byte_offset;
    if (left.byte_size != right.byte_size)
        return left.byte_size < right.byte_size;
    if (left.transform != right.transform)
        return left.transform < right.transform;
    if (left.byte_identity != right.byte_identity)
        return left.byte_identity < right.byte_identity;
    return left.id < right.id;
}

PreparedLatentAotSourceBinding make_source_binding(
    const LatentAotSourceTransform transform,
    const std::string_view source_byte_identity,
    const std::uint64_t disc_byte_offset,
    const std::uint32_t byte_size) {
    const auto transform_name =
        transform == LatentAotSourceTransform::Identity ? "" : "prs-";
    return {"latent-aot-source-" + std::string(transform_name) +
                std::string(source_byte_identity.substr(7u)) + "-" +
                std::to_string(disc_byte_offset) + "-" +
                std::to_string(byte_size),
            transform,
            std::string(source_byte_identity),
            disc_byte_offset,
            byte_size};
}

bool valid_source_transform(const LatentAotSourceTransform transform) noexcept {
    switch (transform) {
    case LatentAotSourceTransform::Identity:
    case LatentAotSourceTransform::SegaPrs:
        return true;
    }
    return false;
}

bool insert_source_binding(
    DiscFileCandidate& candidate,
    PreparedLatentAotSourceBinding binding) {
    const auto position = std::lower_bound(candidate.source_bindings.begin(),
                                           candidate.source_bindings.end(),
                                           binding,
                                           source_binding_less);
    if (position != candidate.source_bindings.end() &&
        *position == binding)
        return false;
    candidate.source_bindings.insert(position, std::move(binding));
    return true;
}

void merge_entry_offsets(std::vector<std::uint32_t>& destination,
                         const std::vector<std::uint32_t>& source) {
    destination.insert(destination.end(), source.begin(), source.end());
    std::sort(destination.begin(), destination.end());
    destination.erase(std::unique(destination.begin(), destination.end()),
                      destination.end());
}

void merge_authority_entries(
    std::vector<CompleteDisassemblyEntryAuthority>& destination,
    std::vector<CompleteDisassemblyEntryAuthority> source) {
    destination.insert(destination.end(),
                       std::make_move_iterator(source.begin()),
                       std::make_move_iterator(source.end()));
    std::sort(destination.begin(), destination.end(),
              [](const auto& left, const auto& right) {
                  if (left.module_relative_offset !=
                      right.module_relative_offset)
                      return left.module_relative_offset <
                             right.module_relative_offset;
                  if (left.byte_size != right.byte_size)
                      return left.byte_size < right.byte_size;
                  if (left.byte_identity != right.byte_identity)
                      return left.byte_identity < right.byte_identity;
                  return static_cast<unsigned>(left.kind) <
                         static_cast<unsigned>(right.kind);
              });
    destination.erase(std::unique(destination.begin(), destination.end()),
                      destination.end());
}

std::vector<std::uint32_t> candidate_analysis_roots(
    const DiscFileCandidate& candidate) {
    if (candidate.authority_entries.empty())
        return candidate.entry_offsets;
    std::vector<std::uint32_t> roots;
    for (const auto offset : candidate.entry_offsets) {
        const bool authority_offset = std::any_of(
            candidate.authority_entries.begin(), candidate.authority_entries.end(),
            [&](const auto& entry) {
                return entry.module_relative_offset == offset;
            });
        if (!authority_offset) roots.push_back(offset);
    }
    for (const auto& entry : candidate.authority_entries) {
        if (entry.kind != CompleteDisassemblyEntryKind::ControlFlowTarget)
            roots.push_back(entry.module_relative_offset);
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

std::vector<std::uint32_t> candidate_public_roots(
    const DiscFileCandidate& candidate,
    const std::span<const katana::ir::Function> program) {
    if (candidate.authority_entries.empty())
        return candidate.entry_offsets;
    std::vector<std::uint32_t> roots;
    for (const auto offset : candidate.entry_offsets) {
        const bool authority_offset = std::any_of(
            candidate.authority_entries.begin(), candidate.authority_entries.end(),
            [&](const auto& entry) {
                return entry.module_relative_offset == offset;
            });
        if (!authority_offset) roots.push_back(offset);
    }
    for (const auto& entry : candidate.authority_entries) {
        const auto address = candidate.source_address +
                             entry.module_relative_offset;
        const bool function_entry = std::any_of(
            program.begin(), program.end(),
            [&](const auto& function) {
                return function.entry_address == address;
            });
        if (entry.kind == CompleteDisassemblyEntryKind::DeclaredEntry ||
            entry.kind == CompleteDisassemblyEntryKind::FunctionEntry ||
            (entry.kind == CompleteDisassemblyEntryKind::CodePointerTarget &&
             function_entry))
            roots.push_back(entry.module_relative_offset);
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

void remove_authority_block_only_roots(
    std::vector<std::uint32_t>& roots,
    const std::span<const CompleteDisassemblyEntryAuthority> entries) {
    if (roots.empty() || entries.empty()) return;
    roots.erase(
        std::remove_if(
            roots.begin(), roots.end(), [&](const auto offset) {
                const auto entry = std::lower_bound(
                    entries.begin(), entries.end(), offset,
                    [](const auto& candidate, const auto value) {
                        return candidate.module_relative_offset < value;
                    });
                return entry != entries.end() &&
                       entry->module_relative_offset == offset &&
                       entry->kind ==
                           CompleteDisassemblyEntryKind::ControlFlowTarget;
            }),
        roots.end());
}

std::vector<std::uint32_t> latent_program_strict_interior_addresses(
    const std::span<const katana::ir::Function> program) {
    std::vector<std::uint32_t> result;
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.source_address != function.entry_address)
                    result.push_back(instruction.source_address);
            }
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool latent_function_root_reaches_before_entry(
    const std::span<const katana::ir::Function> program,
    const std::uint32_t entry_address) noexcept {
    const auto function = std::find_if(
        program.begin(), program.end(), [&](const auto& candidate) {
            return candidate.entry_address == entry_address;
        });
    if (function == program.end()) return false;
    return std::any_of(
        function->blocks.begin(), function->blocks.end(),
        [&](const auto& block) {
            return block.start_address < entry_address ||
                   std::any_of(
                       block.instructions.begin(), block.instructions.end(),
                       [&](const auto& instruction) {
                           return instruction.source_address < entry_address;
                       });
        });
}

std::optional<katana::sh4::DecodedInstruction>
latent_candidate_instruction_at(
    const DiscFileCandidate& candidate,
    const std::uint32_t address) noexcept {
    const auto module_end =
        static_cast<std::uint64_t>(candidate.source_address) +
        candidate.bytes.size();
    if ((address & 1u) != 0u || address < candidate.source_address ||
        static_cast<std::uint64_t>(address) + 2u > module_end)
        return std::nullopt;
    const auto offset = static_cast<std::size_t>(
        address - candidate.source_address);
    const auto opcode = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(candidate.bytes[offset]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(candidate.bytes[offset + 1u])
            << 8u));
    auto decoded = katana::sh4::decode(opcode);
    if (!decoded.is_known()) return std::nullopt;
    return decoded;
}

bool latent_candidate_entry_is_physical_delay_slot(
    const DiscFileCandidate& candidate,
    const std::uint32_t offset) noexcept {
    const auto address = static_cast<std::uint64_t>(
                             candidate.source_address) + offset;
    if (address > std::numeric_limits<std::uint32_t>::max())
        return false;
    return katana::analysis::prove_sh4_physical_delay_slot(
               candidate.bytes,
               candidate.source_address,
               static_cast<std::uint32_t>(address))
        .has_value();
}

bool latent_validated_reverse_prologue_start(
    const DiscFileCandidate& candidate,
    const std::uint32_t address) noexcept {
    if (address < candidate.source_address + 4u) return false;
    const auto previous_return =
        latent_candidate_instruction_at(candidate, address - 4u);
    const auto previous_delay =
        latent_candidate_instruction_at(candidate, address - 2u);
    if (!previous_return || !previous_delay ||
        previous_return->kind != katana::sh4::InstructionKind::Rts ||
        !previous_return->has_delay_slot)
        return false;

    std::array<bool, 16u> saved_registers{};
    bool saw_callee_saved_push = false;
    for (std::size_t instruction_index = 0u;
         instruction_index < 8u; ++instruction_index) {
        const auto instruction_address64 =
            static_cast<std::uint64_t>(address) +
            instruction_index * 2u;
        if (instruction_address64 >
            std::numeric_limits<std::uint32_t>::max())
            return false;
        const auto instruction = latent_candidate_instruction_at(
            candidate,
            static_cast<std::uint32_t>(instruction_address64));
        if (!instruction) return false;
        if (instruction->kind ==
                katana::sh4::InstructionKind::
                    MovLongStorePreDecrement &&
            instruction->destination_register == 15u &&
            instruction->source_register >= 8u &&
            instruction->source_register <= 14u) {
            saved_registers[instruction->source_register] = true;
            saw_callee_saved_push = true;
            continue;
        }
        if (instruction->kind ==
                katana::sh4::InstructionKind::MovRegister &&
            instruction->destination_register >= 8u &&
            instruction->destination_register <= 14u &&
            saved_registers[instruction->destination_register])
            continue;
        if (instruction->kind ==
                katana::sh4::InstructionKind::
                    StoreSpecialRegisterPreDecrement &&
            instruction->destination_register == 15u &&
            instruction->special_register ==
                katana::sh4::SpecialRegister::Pr)
            return saw_callee_saved_push;
        return false;
    }
    return false;
}

bool latent_validated_fallthrough_save_prefix(
    const DiscFileCandidate& candidate,
    const std::uint32_t entry_address,
    const std::uint32_t body_address) noexcept {
    if (body_address <= entry_address ||
        body_address - entry_address > 8u * sizeof(std::uint16_t) ||
        ((body_address - entry_address) & 1u) != 0u)
        return false;

    std::array<bool, 16u> saved_registers{};
    bool saw_callee_saved_push = false;
    bool saw_pr_push = false;
    for (auto address = entry_address; address < body_address;
         address += sizeof(std::uint16_t)) {
        const auto instruction =
            latent_candidate_instruction_at(candidate, address);
        if (!instruction.has_value()) return false;
        if (instruction->kind ==
                katana::sh4::InstructionKind::MovLongStorePreDecrement &&
            instruction->destination_register == 15u &&
            instruction->source_register >= 8u &&
            instruction->source_register <= 14u) {
            if (saved_registers[instruction->source_register]) return false;
            saved_registers[instruction->source_register] = true;
            saw_callee_saved_push = true;
            continue;
        }
        if (instruction->kind ==
                katana::sh4::InstructionKind::
                    StoreSpecialRegisterPreDecrement &&
            instruction->destination_register == 15u &&
            instruction->special_register ==
                katana::sh4::SpecialRegister::Pr &&
            !saw_pr_push) {
            saw_pr_push = true;
            continue;
        }
        return false;
    }
    return saw_callee_saved_push && saw_pr_push;
}

std::vector<std::uint32_t> latent_explicit_tail_prologue_entry_offsets(
    const DiscFileCandidate& candidate,
    const std::span<const katana::ir::Function> program) {
    std::vector<std::uint32_t> result;
    for (const auto explicit_offset : candidate.explicit_entry_offsets) {
        const auto entry = candidate.source_address + explicit_offset;
        const auto function = std::find_if(
            program.begin(), program.end(), [&](const auto& candidate_function) {
                return candidate_function.entry_address == entry;
            });
        if (function == program.end()) continue;
        for (const auto& block : function->blocks) {
            if (block.instructions.size() < 2u) continue;
            std::size_t control_index = block.instructions.size() - 1u;
            if (block.instructions.back().delay_slot.role ==
                katana::ir::DelaySlotRole::Slot) {
                if (control_index == 0u) continue;
                --control_index;
            }
            const auto& control = block.instructions[control_index];
            if (control.operation != katana::ir::Operation::Branch ||
                !control.target_address ||
                *control.target_address >= entry ||
                !latent_validated_reverse_prologue_start(
                    candidate, *control.target_address))
                continue;

            auto restore_pr = std::find_if(
                block.instructions.begin(),
                block.instructions.begin() +
                    static_cast<std::ptrdiff_t>(control_index),
                [](const auto& instruction) {
                    return instruction.operation ==
                               katana::ir::Operation::
                                   LoadSpecialRegisterPostIncrement &&
                           instruction.special_register ==
                               katana::ir::SpecialRegister::Pr &&
                           instruction.source_register == 15u;
                });
            if (restore_pr ==
                block.instructions.begin() +
                    static_cast<std::ptrdiff_t>(control_index))
                continue;
            const auto epilogue_is_restore_only = std::all_of(
                std::next(restore_pr), block.instructions.end(),
                [&](const auto& instruction) {
                    if (&instruction == &control)
                        return true;
                    return instruction.operation ==
                               katana::ir::Operation::LoadLongPostIncrement ||
                           instruction.operation ==
                               katana::ir::Operation::FmovLoadPostIncrement;
                });
            if (!epilogue_is_restore_only) continue;
            result.push_back(
                *control.target_address - candidate.source_address);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool valid_candidate_entry_offsets(const DiscFileCandidate& candidate) noexcept {
    if (candidate.source_bindings.empty() ||
        !std::is_sorted(candidate.source_bindings.begin(),
                        candidate.source_bindings.end(),
                        source_binding_less) ||
        std::adjacent_find(candidate.source_bindings.begin(),
                           candidate.source_bindings.end()) !=
            candidate.source_bindings.end() ||
        std::any_of(candidate.source_bindings.begin(),
                    candidate.source_bindings.end(),
                    [&](const auto& binding) {
                        return binding.id.empty() ||
                               !valid_source_transform(binding.transform) ||
                               !valid_sha256_identity(binding.byte_identity) ||
                               binding.byte_size == 0u ||
                               (binding.transform ==
                                    LatentAotSourceTransform::Identity &&
                                (binding.byte_size != candidate.size ||
                                 binding.byte_identity !=
                                     candidate.byte_identity)) ||
                               binding.disc_byte_offset >
                                   std::numeric_limits<std::uint64_t>::max() -
                                       binding.byte_size;
                    }) ||
        candidate.entry_offsets.empty() ||
        !std::is_sorted(candidate.entry_offsets.begin(), candidate.entry_offsets.end()) ||
        std::adjacent_find(candidate.entry_offsets.begin(), candidate.entry_offsets.end()) !=
            candidate.entry_offsets.end() ||
        !std::is_sorted(candidate.explicit_entry_offsets.begin(),
                        candidate.explicit_entry_offsets.end()) ||
        std::adjacent_find(candidate.explicit_entry_offsets.begin(),
                           candidate.explicit_entry_offsets.end()) !=
            candidate.explicit_entry_offsets.end())
        return false;
    if (candidate.inferred_authoritative_entry_table &&
        (!candidate.explicit_entry_offsets.empty() ||
         !candidate_has_transformed_source(candidate)))
        return false;
    if (candidate.explicit_entry_offsets.empty()) {
        const bool conventional_heuristic_entry =
            candidate.entry_offsets.size() == 1u &&
            candidate.entry_offsets.front() == 0u;
        const bool transformed_prefix_entry_table =
            candidate_has_transformed_source(candidate) &&
            candidate.entry_offsets.size() >=
                minimum_latent_aot_inferred_authoritative_entries &&
            candidate.entry_offsets.size() <=
                maximum_latent_aot_prefix_entry_table_targets;
        if (!conventional_heuristic_entry &&
            !transformed_prefix_entry_table)
            return false;
        if (candidate.inferred_authoritative_entry_table !=
            transformed_prefix_entry_table)
            return false;
    } else if (candidate.entry_offsets != candidate.explicit_entry_offsets) {
        return false;
    } else if (candidate.inferred_authoritative_entry_table) {
        return false;
    }
    const auto valid_offset = [&candidate](const std::uint32_t offset) {
        return (offset & 1u) == 0u &&
               static_cast<std::uint64_t>(offset) + 2u <=
                   candidate.bytes.size() &&
               !latent_candidate_entry_is_physical_delay_slot(
                   candidate, offset);
    };
    return std::all_of(candidate.entry_offsets.begin(), candidate.entry_offsets.end(),
                       valid_offset) &&
           std::all_of(candidate.explicit_entry_offsets.begin(),
                       candidate.explicit_entry_offsets.end(),
                       [&](const auto offset) {
                           return valid_offset(offset) &&
                                  std::binary_search(candidate.entry_offsets.begin(),
                                                     candidate.entry_offsets.end(), offset);
                       });
}

bool safe_component(const std::string_view component) noexcept {
    return !component.empty() && component != "." && component != ".." &&
           component.find('/') == std::string_view::npos &&
           component.find('\\') == std::string_view::npos &&
           component.find(':') == std::string_view::npos;
}

std::uint32_t align_up(const std::uint32_t value, const std::uint32_t alignment) {
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u)
        throw std::invalid_argument("Latente AOT-Ausrichtung ist ungueltig.");
    const auto aligned = (static_cast<std::uint64_t>(value) + alignment - 1u) &
                         ~static_cast<std::uint64_t>(alignment - 1u);
    if (aligned > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("Latente AOT-Quelladresse laeuft ueber.");
    return static_cast<std::uint32_t>(aligned);
}

bool runtime_only_candidate_stack_loss_is_bounded(
    const katana::analysis::ControlFlowAnalysisResult& analysis,
    const katana::analysis::ResolutionRetentionLimitReason
        resolution_retention_limit_reason) {
    const auto& walk = analysis.guarded_code_inventory_walk;
    const bool candidate_stack_resolution_loss =
        walk.inventory_candidate_values_truncated ||
        walk.abi_stack_base_unresolved ||
        walk.detached_stack_callback_loss ||
        walk.memory_callback_loss;
    if ((analysis.function_budget_exhausted &&
         (resolution_retention_limit_reason !=
              katana::analysis::ResolutionRetentionLimitReason::
                  IncompleteRoot ||
          !candidate_stack_resolution_loss ||
          analysis.function_summary_iterations >=
              analysis.function_iteration_budget)) ||
        analysis.persistent_analysis_bypass_reason !=
            katana::analysis::PersistentAnalysisBypassReason::None ||
        analysis.termination_reason !=
            katana::analysis::ControlFlowAnalysisTerminationReason::None ||
        analysis.raw_stored_code_inventory_truncated ||
        analysis.guarded_code_inventory_candidate_budget_exhausted ||
        analysis.candidate_inventory_truncated ||
        analysis.returned_table_scan_truncated ||
        analysis.guarded_code_shape_budget_exceeded_candidates != 0u ||
        !analysis.guarded_aot_entry_rejections.empty())
        return false;
    return !walk.truncated_except_candidate_stack_resolution_loss();
}

bool complete_native_graph(
    const katana::analysis::ControlFlowAnalysisResult& analysis,
    const bool exact_runtime_only_stop_on_miss,
    const katana::analysis::ResolutionRetentionLimitReason
        resolution_retention_limit_reason) {
    if (std::any_of(analysis.recursive.diagnostics.begin(),
                    analysis.recursive.diagnostics.end(),
                    katana::analysis::analysis_diagnostic_blocks_codegen))
        return false;
    if (!katana::analysis::guarded_aot_inventory_complete(analysis) &&
        !(exact_runtime_only_stop_on_miss &&
          runtime_only_candidate_stack_loss_is_bounded(
              analysis, resolution_retention_limit_reason)))
        return false;
    return std::none_of(
        analysis.indirect_control_flow.begin(),
        analysis.indirect_control_flow.end(),
        [](const auto& resolution) {
            const auto status = katana::analysis::control_flow_report_status(resolution);
            return status == katana::analysis::ControlFlowReportStatus::GuardedPartial ||
                   status == katana::analysis::ControlFlowReportStatus::Unresolved;
        });
}

std::string incomplete_native_graph_summary(
    const katana::analysis::ControlFlowAnalysisResult& analysis) {
    std::ostringstream result;
    std::size_t reported = 0u;
    constexpr std::size_t maximum_reported_sites = 8u;
    for (const auto& resolution : analysis.indirect_control_flow) {
        const auto status =
            katana::analysis::control_flow_report_status(resolution);
        if (status !=
                katana::analysis::ControlFlowReportStatus::GuardedPartial &&
            status !=
                katana::analysis::ControlFlowReportStatus::Unresolved)
            continue;
        if (reported != 0u) result << '+';
        result << "indirect-0x" << std::hex << std::uppercase
               << resolution.instruction_address << '-'
               << katana::analysis::control_flow_report_status_name(status);
        if (++reported == maximum_reported_sites) break;
    }
    if (reported == 0u) {
        for (const auto& diagnostic : analysis.recursive.diagnostics) {
            if (!katana::analysis::analysis_diagnostic_blocks_codegen(
                    diagnostic))
                continue;
            if (reported != 0u) result << '+';
            result << "diagnostic-0x" << std::hex << std::uppercase
                   << diagnostic.address;
            if (++reported == maximum_reported_sites) break;
        }
    }
    return reported == 0u ? "unknown-control-flow-loss" : result.str();
}

std::string guarded_aot_inventory_loss_summary(
    const katana::analysis::ControlFlowAnalysisResult& analysis) {
    std::string result;
    const auto append = [&](const std::string_view name,
                            const bool lost) {
        if (!lost) return;
        if (!result.empty()) result += '+';
        result += name;
    };
    const auto& walk = analysis.guarded_code_inventory_walk;
    append("function-budget", analysis.function_budget_exhausted);
    append("analysis-termination",
           analysis.termination_reason !=
               katana::analysis::ControlFlowAnalysisTerminationReason::None);
    append("raw-stored", analysis.raw_stored_code_inventory_truncated);
    append("candidate-budget",
           analysis.guarded_code_inventory_candidate_budget_exhausted);
    append("pending-regions", walk.pending_inventory_region_count != 0u);
    append("region-blocks",
           walk.inventory_region_block_limited_regions != 0u);
    append("forwarded-contexts",
           walk.forwarded_store_context_limited_functions != 0u);
    append("contextual-contexts",
           walk.contextual_return_context_limited_functions != 0u);
    append("contextual-evaluations",
           walk.contextual_return_evaluation_limited_functions != 0u);
    append("provenance-capsules",
           walk.contextual_provenance_replay_capsule_limited_functions != 0u);
    append("provenance-key-bytes",
           walk.contextual_provenance_replay_key_byte_limited_functions != 0u);
    append("abi-stack-projection",
           walk.abi_stack_argument_projection_truncated_functions != 0u);
    append("local-fixpoint", walk.local_fixpoint_limited_evaluations != 0u);
    append("root-logical-budget",
           walk.resolution_root_logical_budget_exhausted);
    append("candidate-values", walk.inventory_candidate_values_truncated);
    append("abi-stack-base", walk.abi_stack_base_unresolved);
    append("detached-stack-callback", walk.detached_stack_callback_loss);
    append("memory-callback", walk.memory_callback_loss);
    append("tail-target", walk.inventory_tail_target_unresolved);
    append("candidate-inventory", analysis.candidate_inventory_truncated);
    append("returned-table", analysis.returned_table_scan_truncated);
    append("shape-budget",
           analysis.guarded_code_shape_budget_exceeded_candidates != 0u);
    append("entry-rejections", !analysis.guarded_aot_entry_rejections.empty());
    return result.empty() ? "none" : result;
}

bool contains_extent(const std::uint32_t start,
                     const std::uint32_t extent,
                     const std::uint32_t address,
                     const std::uint32_t width = 2u) noexcept {
    return width != 0u && address >= start &&
           static_cast<std::uint64_t>(address) + width <=
               static_cast<std::uint64_t>(start) + extent;
}

bool relocation_closed_impl(const std::span<const katana::ir::Function> program,
                            const std::uint32_t start,
                            const std::uint32_t extent) noexcept {
    using Operation = katana::ir::Operation;
    const auto code_address = [&](const std::uint32_t address) {
        return contains_extent(start, extent, address);
    };
    for (const auto& function : program) {
        if (!code_address(function.entry_address)) return false;
        for (const auto address : function.direct_callees)
            if (!code_address(address)) return false;
        for (const auto address : function.indirect_call_sites)
            if (!code_address(address)) return false;
        for (const auto& block : function.blocks) {
            if (!code_address(block.start_address)) return false;
            for (const auto successor : block.successors)
                if (!code_address(successor)) return false;
            for (const auto target :
                 block.guarded_case_ownership_targets)
                if (!code_address(target)) return false;
            for (const auto& instruction : block.instructions) {
                if (!code_address(instruction.source_address)) return false;
                if (instruction.delay_slot.counterpart_address &&
                    !code_address(*instruction.delay_slot.counterpart_address))
                    return false;
                if (instruction.target_address && !code_address(*instruction.target_address))
                    return false;
                for (const auto target : instruction.resolved_targets)
                    if (!code_address(target)) return false;
                if (instruction.effective_address) {
                    std::uint32_t width = 1u;
                    if (instruction.operation == Operation::LoadWordSignedPcRelative)
                        width = 2u;
                    else if (instruction.operation == Operation::LoadLongPcRelative)
                        width = 4u;
                    if (!contains_extent(
                            start, extent, *instruction.effective_address, width))
                        return false;
                }
                const bool relocates_source_plus_four =
                    instruction.operation == Operation::Call ||
                    instruction.operation == Operation::CallRegister ||
                    ((instruction.operation == Operation::JumpRegister ||
                      instruction.operation == Operation::CallRegister) &&
                     instruction.branch_register_relative);
                if (relocates_source_plus_four &&
                    !code_address(instruction.source_address + 4u))
                    return false;
                if ((instruction.operation == Operation::BranchIfTrue ||
                     instruction.operation == Operation::BranchIfFalse) &&
                    !code_address(instruction.source_address +
                                  (instruction.delay_slot.role ==
                                           katana::ir::DelaySlotRole::Owner
                                       ? 4u
                                       : 2u)))
                    return false;
                if (instruction.operation == Operation::Sleep &&
                    !code_address(instruction.source_address + 2u))
                    return false;
            }
            const auto is_terminal = [](const Operation operation) {
                return operation == Operation::Branch || operation == Operation::Call ||
                       operation == Operation::BranchIfTrue ||
                       operation == Operation::BranchIfFalse ||
                       operation == Operation::JumpRegister ||
                       operation == Operation::CallRegister ||
                       operation == Operation::Return ||
                       operation == Operation::TrapAlways ||
                       operation == Operation::ReturnFromException ||
                       operation == Operation::Sleep;
            };
            const auto terminal =
                std::find_if(block.instructions.begin(),
                             block.instructions.end(),
                             [&](const auto& instruction) {
                                 return instruction.delay_slot.role !=
                                            katana::ir::DelaySlotRole::Slot &&
                                        is_terminal(instruction.operation);
                             });
            if (block.successors.empty() && !block.instructions.empty() &&
                terminal == block.instructions.end()) {
                const auto& final = block.instructions.back();
                if (!code_address(final.source_address +
                                  (final.delay_slot.role ==
                                           katana::ir::DelaySlotRole::Owner
                                       ? 4u
                                       : 2u)))
                    return false;
            }
        }
    }
    return true;
}

std::optional<std::uint32_t> latent_direct_code_address(
    const std::uint32_t value) noexcept {
    const auto segment = value >> 29u;
    if (segment == 4u || segment == 5u)
        return (value & 0x1fffffffu) | 0x80000000u;
    // No-MMU P0 main-RAM pointers are the physical spelling of the same code
    // address. Other P0 values are not promoted into executable candidates.
    if (segment == 0u && value >= 0x08000000u && value < 0x10000000u)
        return value | 0x80000000u;
    return std::nullopt;
}

bool valid_latent_runtime_base(std::uint32_t base,
                               std::uint32_t byte_size) noexcept;

std::vector<std::uint32_t> latent_prefix_entry_table_offsets(
    const std::span<const std::uint8_t> bytes) {
    constexpr auto cell_size = sizeof(std::uint32_t);
    if (bytes.size() <
        (latent_aot_ff_padded_prefix_pointer_count + 1u) * cell_size)
        return {};
    const auto cell_at = [&bytes](const std::size_t cell)
        -> std::optional<std::uint32_t> {
        const auto offset = cell * cell_size;
        if (offset > bytes.size() || cell_size > bytes.size() - offset)
            return std::nullopt;
        return static_cast<std::uint32_t>(bytes[offset]) |
               (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
               (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
               (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
    };

    std::vector<std::uint32_t> targets;
    targets.reserve(maximum_latent_aot_prefix_entry_table_targets);
    bool terminated = false;
    bool null_table_invalid = false;
    // The cardinality cap applies to non-null targets, not to the terminating
    // cell.  A valid 64-target table therefore needs one additional bounded
    // read for cell 64.  Reject a 65th target instead of silently narrowing
    // the documented 3..64 contract to 3..63.
    for (std::size_t cell = 0u;
         cell <= maximum_latent_aot_prefix_entry_table_targets &&
         (cell + 1u) * cell_size <= bytes.size();
         ++cell) {
        const auto raw = *cell_at(cell);
        if (raw == 0u) {
            terminated = true;
            break;
        }
        if (targets.size() ==
            maximum_latent_aot_prefix_entry_table_targets) {
            null_table_invalid = true;
            break;
        }
        const auto normalized = latent_direct_code_address(raw);
        if (!normalized.has_value() ||
            *normalized < latent_aot_main_ram_begin ||
            *normalized >= latent_aot_main_ram_end) {
            null_table_invalid = true;
            break;
        }
        targets.push_back(*normalized);
    }
    if (null_table_invalid || !terminated ||
        targets.size() < minimum_latent_aot_prefix_entry_table_targets) {
        targets.clear();

        // Some statically linked modules use a distinct compact header: one
        // direct entry pointer followed by one same-module anchor pointer and
        // 0xFFFFFFFF cells up to the first executable byte.  This is not a
        // relaxed two-element form of the null-terminated table above.  The
        // second pointer can identify module data, so it must constrain the
        // bounded relative layout without becoming an executable root.  The
        // sole returned entry still has to pass decode, early-control-flow,
        // complete CFG, relocation-closure and emitted-block validation.
        constexpr auto ff_pointer_count =
            latent_aot_ff_padded_prefix_pointer_count;
        constexpr auto ff_header_bytes =
            static_cast<std::uint32_t>(ff_pointer_count * cell_size);
        if (bytes.size() < ff_header_bytes + cell_size) return {};
        for (std::size_t cell = 0u; cell < ff_pointer_count; ++cell) {
            const auto raw = *cell_at(cell);
            const auto normalized = latent_direct_code_address(raw);
            if (!normalized.has_value() ||
                *normalized < latent_aot_main_ram_begin ||
                *normalized >= latent_aot_main_ram_end)
                return {};
            targets.push_back(*normalized);
        }
        if (targets.front() >= targets.back()) return {};

        const auto runtime_base =
            targets.front() & ~(latent_aot_runtime_page_size - 1u);
        if (!valid_latent_runtime_base(
                runtime_base, static_cast<std::uint32_t>(bytes.size())))
            return {};
        const auto first_entry_offset = targets.front() - runtime_base;
        if (first_entry_offset < ff_header_bytes + cell_size ||
            (first_entry_offset & (cell_size - 1u)) != 0u ||
            first_entry_offset > latent_aot_runtime_page_size ||
            first_entry_offset > bytes.size())
            return {};
        for (std::uint32_t offset = ff_header_bytes;
             offset < first_entry_offset; offset += cell_size) {
            const auto raw = *cell_at(offset / cell_size);
            if (raw != std::numeric_limits<std::uint32_t>::max()) return {};
        }

        std::vector<std::uint32_t> pointer_offsets;
        pointer_offsets.reserve(targets.size());
        for (const auto target : targets) {
            if (target < runtime_base) return {};
            const auto offset = target - runtime_base;
            if ((offset & 1u) != 0u || offset < first_entry_offset ||
                 static_cast<std::uint64_t>(offset) + 2u > bytes.size())
                return {};
            pointer_offsets.push_back(offset);
        }
        if (pointer_offsets.front() != first_entry_offset ||
            std::adjacent_find(pointer_offsets.begin(), pointer_offsets.end()) !=
                pointer_offsets.end())
            return {};
        return {first_entry_offset};
    }

    std::set<std::uint32_t> unique_targets(targets.begin(), targets.end());
    if (unique_targets.size() != targets.size()) return {};

    // The table proves a self-consistent relative layout, not an authoritative
    // runtime placement.  Derive no address from a file name or mutable
    // observation: use the lowest encoded entry page only to recover bounded
    // module-relative offsets.  Product activation independently derives the
    // actual runtime start from a reached block, validates the exact
    // materialized module/code identities and installs that mapping before
    // any recovered entry can dispatch.  The complete CFA and relocation-
    // closure gates below still have to prove every recovered entry.
    const auto first_target = *unique_targets.begin();
    const auto runtime_base =
        first_target & ~(latent_aot_runtime_page_size - 1u);
    if (!valid_latent_runtime_base(
            runtime_base, static_cast<std::uint32_t>(bytes.size())))
        return {};

    const auto table_bytes =
        static_cast<std::uint32_t>((targets.size() + 1u) * cell_size);
    std::vector<std::uint32_t> entry_offsets;
    entry_offsets.reserve(targets.size());
    for (const auto target : targets) {
        if (target < runtime_base) return {};
        const auto offset = target - runtime_base;
        if ((offset & 1u) != 0u || offset < table_bytes ||
            static_cast<std::uint64_t>(offset) + 2u > bytes.size())
            return {};
        entry_offsets.push_back(offset);
    }
    std::sort(entry_offsets.begin(), entry_offsets.end());
    if (std::adjacent_find(entry_offsets.begin(), entry_offsets.end()) !=
        entry_offsets.end())
        return {};
    return entry_offsets;
}

std::optional<std::uint32_t> latent_read_u32(
    const DiscFileCandidate& candidate,
    const std::uint32_t address) noexcept {
    if (address < candidate.source_address) return std::nullopt;
    const auto offset = address - candidate.source_address;
    if (offset > candidate.bytes.size() ||
        sizeof(std::uint32_t) > candidate.bytes.size() - offset)
        return std::nullopt;
    return static_cast<std::uint32_t>(candidate.bytes[offset]) |
           (static_cast<std::uint32_t>(candidate.bytes[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(candidate.bytes[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(candidate.bytes[offset + 3u]) << 24u);
}

std::optional<std::uint32_t> read_latent_pc_literal(
    const std::span<const std::uint8_t> bytes,
    const std::uint32_t source_address,
    const std::uint32_t address,
    const std::uint8_t width) noexcept {
    if ((width != 2u && width != 4u) || address < source_address)
        return std::nullopt;
    const auto offset = address - source_address;
    if (offset > bytes.size() || width > bytes.size() - offset)
        return std::nullopt;
    std::uint32_t value = 0u;
    for (std::uint8_t index = 0u; index < width; ++index)
        value |= static_cast<std::uint32_t>(bytes[offset + index])
                 << (index * 8u);
    return value;
}

bool pc_literal_evidence_less(
    const PreparedLatentAotPcLiteralEvidence& left,
    const PreparedLatentAotPcLiteralEvidence& right) noexcept {
    return std::tie(left.instruction_offset, left.literal_offset, left.bits,
                    left.width_bytes, left.signed_value) <
           std::tie(right.instruction_offset, right.literal_offset, right.bits,
                    right.width_bytes, right.signed_value);
}

std::optional<std::vector<PreparedLatentAotPcLiteralEvidence>>
collect_latent_pc_literal_evidence(
    const std::uint32_t source_address,
    const std::span<const std::uint8_t> bytes,
    const std::span<const katana::ir::Function> program) {
    std::vector<PreparedLatentAotPcLiteralEvidence> result;
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                const auto width =
                    instruction.operation ==
                            katana::ir::Operation::LoadWordSignedPcRelative
                        ? static_cast<std::uint8_t>(2u)
                        : instruction.operation ==
                                  katana::ir::Operation::LoadLongPcRelative
                              ? static_cast<std::uint8_t>(4u)
                              : static_cast<std::uint8_t>(0u);
                if (width == 0u) continue;
                if ((instruction.source_address & 1u) != 0u ||
                    instruction.source_address < source_address ||
                    instruction.effective_address == std::nullopt)
                    return std::nullopt;
                const auto instruction_offset =
                    instruction.source_address - source_address;
                const auto literal_address = *instruction.effective_address;
                if ((literal_address & (width - 1u)) != 0u ||
                    literal_address < source_address ||
                    literal_address - source_address >= bytes.size())
                    return std::nullopt;
                const auto instruction_opcode = read_latent_pc_literal(
                    bytes, source_address, instruction.source_address, 2u);
                const auto literal_bits = read_latent_pc_literal(
                    bytes, source_address, literal_address, width);
                if (!instruction_opcode || !literal_bits ||
                    *instruction_opcode != instruction.original_opcode)
                    return std::nullopt;
                const auto decoded = katana::sh4::decode(
                    static_cast<std::uint16_t>(*instruction_opcode));
                const auto decoded_kind = decoded.kind;
                const auto expected_kind =
                    width == 2u
                        ? katana::sh4::InstructionKind::MovWordLoadPcRelative
                        : katana::sh4::InstructionKind::MovLongLoadPcRelative;
                if (!decoded.is_known() || decoded_kind != expected_kind)
                    return std::nullopt;
                const auto pc_base =
                    width == 2u
                        ? static_cast<std::uint64_t>(
                              instruction.source_address) +
                              4u
                        : (static_cast<std::uint64_t>(
                               instruction.source_address) +
                           4u) &
                              ~std::uint64_t{3u};
                const auto decoded_literal_address =
                    pc_base + static_cast<std::uint32_t>(
                                  decoded.displacement);
                if (decoded_literal_address >
                        std::numeric_limits<std::uint32_t>::max() ||
                    decoded_literal_address != literal_address)
                    return std::nullopt;
                result.push_back(
                    {instruction_offset,
                     literal_address - source_address,
                     *literal_bits,
                     width,
                     width == 2u});
            }
        }
    }
    std::sort(result.begin(), result.end(), pc_literal_evidence_less);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    if (result.size() > maximum_prepared_latent_aot_pc_literal_evidence)
        return std::nullopt;
    return result;
}

bool validate_latent_pc_literal_evidence(
    const PreparedLatentAotModule& module,
    const std::span<const std::uint8_t> decoded) {
    if (module.pc_literal_evidence.size() >
            maximum_prepared_latent_aot_pc_literal_evidence ||
        !std::is_sorted(module.pc_literal_evidence.begin(),
                        module.pc_literal_evidence.end(),
                        pc_literal_evidence_less) ||
        std::adjacent_find(module.pc_literal_evidence.begin(),
                           module.pc_literal_evidence.end(),
                           [](const auto& left, const auto& right) {
                               return left.instruction_offset ==
                                      right.instruction_offset;
                           }) != module.pc_literal_evidence.end())
        return false;
    for (const auto& evidence : module.pc_literal_evidence) {
        if ((evidence.instruction_offset & 1u) != 0u ||
            (evidence.literal_offset &
             (static_cast<std::uint32_t>(evidence.width_bytes) - 1u)) != 0u ||
            (evidence.width_bytes != 2u && evidence.width_bytes != 4u) ||
            evidence.instruction_offset > decoded.size() ||
            decoded.size() - evidence.instruction_offset < 2u ||
            evidence.literal_offset > decoded.size() ||
            evidence.width_bytes > decoded.size() - evidence.literal_offset)
            return false;
        const auto opcode = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(decoded[evidence.instruction_offset]) |
            static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(
                    decoded[evidence.instruction_offset + 1u])
                << 8u));
        const auto decoded_instruction = katana::sh4::decode(opcode);
        const auto expected_kind = evidence.width_bytes == 2u
                                        ? katana::sh4::InstructionKind::
                                              MovWordLoadPcRelative
                                        : katana::sh4::InstructionKind::
                                              MovLongLoadPcRelative;
        if (!decoded_instruction.is_known() ||
            decoded_instruction.kind != expected_kind)
            return false;
        const auto instruction_address =
            static_cast<std::uint64_t>(module.source_address) +
            evidence.instruction_offset;
        const auto pc_base = evidence.width_bytes == 2u
                                 ? instruction_address + 4u
                                 : (instruction_address + 4u) &
                                       ~std::uint64_t{3u};
        const auto literal_address =
            pc_base + static_cast<std::uint32_t>(
                          decoded_instruction.displacement);
        if (literal_address > std::numeric_limits<std::uint32_t>::max() ||
            literal_address < module.source_address ||
            literal_address - module.source_address != evidence.literal_offset)
            return false;
        std::uint32_t bits = 0u;
        for (std::uint8_t index = 0u; index < evidence.width_bytes; ++index)
            bits |= static_cast<std::uint32_t>(
                        decoded[evidence.literal_offset + index])
                    << (index * 8u);
        if (bits != evidence.bits || evidence.signed_value !=
                                         (evidence.width_bytes == 2u))
            return false;
    }
    return true;
}

bool valid_latent_runtime_base(const std::uint32_t base,
                               const std::uint32_t byte_size) noexcept {
    return (base & (latent_aot_runtime_page_size - 1u)) == 0u &&
           base >= latent_aot_main_ram_begin &&
           static_cast<std::uint64_t>(base) + byte_size <=
               latent_aot_main_ram_end;
}

std::optional<std::uint32_t> infer_latent_runtime_pointer_cluster_base(
    const std::set<std::uint32_t>& targets,
    const std::uint32_t byte_size) {
    if (byte_size < sizeof(std::uint16_t) ||
        byte_size > latent_aot_main_ram_end - latent_aot_main_ram_begin)
        return std::nullopt;
    const auto maximum_base = static_cast<std::uint32_t>(
        latent_aot_main_ram_end - byte_size);
    const auto maximum_base_page =
        maximum_base & ~(latent_aot_runtime_page_size - 1u);
    std::map<std::uint32_t, std::int64_t> events;
    for (const auto target : targets) {
        if (target < latent_aot_main_ram_begin ||
            target >= latent_aot_main_ram_end)
            continue;
        const auto lowest_unaligned =
            static_cast<std::uint64_t>(target) + 1u >= byte_size
                ? static_cast<std::uint64_t>(target) + 1u - byte_size
                : 0u;
        const auto lowest = static_cast<std::uint32_t>(
            (std::max<std::uint64_t>(lowest_unaligned,
                                     latent_aot_main_ram_begin) +
             latent_aot_runtime_page_size - 1u) &
            ~(static_cast<std::uint64_t>(latent_aot_runtime_page_size) - 1u));
        const auto highest = std::min(
            target & ~(latent_aot_runtime_page_size - 1u),
            maximum_base_page);
        if (lowest > highest) continue;
        ++events[lowest];
        const auto after = static_cast<std::uint64_t>(highest) +
                           latent_aot_runtime_page_size;
        if (after <= latent_aot_main_ram_end)
            --events[static_cast<std::uint32_t>(after)];
    }
    const auto sentinel = static_cast<std::uint64_t>(maximum_base_page) +
                          latent_aot_runtime_page_size;
    if (sentinel <= latent_aot_main_ram_end)
        events.try_emplace(static_cast<std::uint32_t>(sentinel), 0);
    if (events.size() < 2u) return std::nullopt;

    std::int64_t active = 0;
    std::size_t best_coverage = 0u;
    std::optional<std::uint32_t> best_base;
    bool best_is_ambiguous = false;
    for (auto current = events.begin(); std::next(current) != events.end();
         ++current) {
        active += current->second;
        if (active <= 0 || current->first > maximum_base_page) continue;
        const auto next = std::next(current)->first;
        if (next <= current->first) continue;
        const auto page_count =
            (static_cast<std::uint64_t>(next) - current->first) /
            latent_aot_runtime_page_size;
        const auto coverage = static_cast<std::size_t>(active);
        if (coverage > best_coverage) {
            best_coverage = coverage;
            best_base = page_count == 1u
                            ? std::optional<std::uint32_t>{current->first}
                            : std::nullopt;
            best_is_ambiguous = page_count != 1u;
        } else if (coverage == best_coverage && coverage != 0u) {
            best_is_ambiguous = true;
        }
    }
    if (best_is_ambiguous || !best_base.has_value() ||
        best_coverage < minimum_latent_aot_runtime_cluster_targets)
        return std::nullopt;

    std::optional<std::uint32_t> minimum_offset;
    std::optional<std::uint32_t> maximum_offset;
    const auto limit = static_cast<std::uint64_t>(*best_base) + byte_size;
    for (const auto target : targets) {
        if (target < *best_base || static_cast<std::uint64_t>(target) >= limit)
            continue;
        const auto offset = target - *best_base;
        minimum_offset = !minimum_offset.has_value()
                             ? offset
                             : std::min(*minimum_offset, offset);
        maximum_offset = !maximum_offset.has_value()
                             ? offset
                             : std::max(*maximum_offset, offset);
    }
    const auto edge_window = std::max<std::uint32_t>(
        latent_aot_runtime_page_size, byte_size / 8u);
    if (!minimum_offset.has_value() || !maximum_offset.has_value() ||
        *minimum_offset > edge_window ||
        static_cast<std::uint64_t>(*maximum_offset) + edge_window < byte_size)
        return std::nullopt;
    return best_base;
}

struct LatentCodeAddressResolver final {
    struct LocalResolution final {
        std::uint32_t target = 0u;
        // A source-range address is an exact module identity. A runtime-base
        // projection inferred from pointer-shaped module words is only a
        // positive candidate and must retain runtime dispatch.
        bool exact = false;
    };

    std::uint32_t source_address = 0u;
    std::uint32_t byte_size = 0u;
    std::optional<std::uint32_t> preferred_runtime_base;
    // True when the bounded whole-image pointer cluster selects one unique
    // runtime page and any available function-offset vote agrees with it.
    // General runtime aliases remain candidates; the authoritative
    // loader-tail proof may additionally require this stronger placement.
    bool preferred_runtime_base_identity_consistent = false;
    std::set<std::uint32_t> function_entries;
    std::set<std::uint32_t> block_entries;
    std::span<const std::uint32_t> external_code_targets;

    [[nodiscard]] std::optional<LocalResolution> resolve_local_with_evidence(
        const std::uint32_t raw) const noexcept {
        const auto normalized = latent_direct_code_address(raw);
        if (!normalized.has_value()) return std::nullopt;
        const auto source_end =
            static_cast<std::uint64_t>(source_address) + byte_size;
        if (*normalized >= source_address &&
            static_cast<std::uint64_t>(*normalized) < source_end)
            return LocalResolution{*normalized, true};
        if (!preferred_runtime_base.has_value() ||
            *normalized < *preferred_runtime_base)
            return std::nullopt;
        const auto offset = *normalized - *preferred_runtime_base;
        if (offset >= byte_size) return std::nullopt;
        return LocalResolution{source_address + offset, false};
    }

    [[nodiscard]] std::optional<std::uint32_t> resolve_local(
        const std::uint32_t raw) const noexcept {
        const auto result = resolve_local_with_evidence(raw);
        return result.has_value()
                   ? std::optional<std::uint32_t>{result->target}
                   : std::nullopt;
    }

    [[nodiscard]] bool is_external(
        const std::uint32_t raw) const noexcept {
        const auto normalized = latent_direct_code_address(raw);
        if (!normalized.has_value()) return false;
        const auto source_end =
            static_cast<std::uint64_t>(source_address) + byte_size;
        // Exact ownership by the current transformed module wins over an
        // overlapping primary-image entry. Shared guest windows otherwise
        // turn a valid local edge into a contradictory cross-image transfer.
        // Runtime-base projections remain external-first because they are
        // positive placement evidence rather than exact source ownership.
        if (*normalized >= source_address &&
            static_cast<std::uint64_t>(*normalized) < source_end)
            return false;
        return std::binary_search(external_code_targets.begin(),
                                  external_code_targets.end(), *normalized);
    }

    [[nodiscard]] std::optional<std::uint32_t> resolve(
        const std::uint32_t raw,
        const bool require_function) const noexcept {
        const auto normalized = latent_direct_code_address(raw);
        if (!normalized.has_value()) return std::nullopt;
        if (is_external(raw))
            return *normalized;

        const auto admit_local = [&](const std::uint32_t candidate) {
            return require_function ? function_entries.contains(candidate)
                                    : block_entries.contains(candidate);
        };
        const auto local = resolve_local(raw);
        return local.has_value() && admit_local(*local) ? local
                                                        : std::nullopt;
    }
};

std::optional<std::uint32_t>
infer_latent_configured_root_dispatch_base(
    const DiscFileCandidate& candidate,
    std::span<const katana::ir::Function> program,
    std::span<const std::uint32_t> root_offsets,
    std::size_t maximum_entry_scan_instructions);

LatentCodeAddressResolver make_latent_code_address_resolver(
    const DiscFileCandidate& candidate,
    const std::span<const katana::ir::Function> program,
    const std::span<const std::uint32_t> external_code_targets) {
    LatentCodeAddressResolver resolver;
    resolver.source_address = candidate.source_address;
    resolver.byte_size = candidate.size;
    resolver.external_code_targets = external_code_targets;
    std::map<std::uint32_t, std::vector<std::uint32_t>> function_offsets_by_low;
    std::set<std::uint32_t> direct_main_ram_targets;
    for (const auto& function : program) {
        resolver.function_entries.insert(function.entry_address);
        if (function.entry_address >= candidate.source_address) {
            const auto offset = function.entry_address - candidate.source_address;
            if (offset < candidate.size)
                function_offsets_by_low[offset & 0xfffu].push_back(offset);
        }
        for (const auto& block : function.blocks)
            resolver.block_entries.insert(block.start_address);
    }
    resolver.block_entries.insert(resolver.function_entries.begin(),
                                  resolver.function_entries.end());
    if (candidate.proven_runtime_base.has_value()) {
        resolver.preferred_runtime_base = candidate.proven_runtime_base;
        resolver.preferred_runtime_base_identity_consistent = true;
        return resolver;
    }

    // Infer a preferred runtime load base only as positive discovery evidence.
    // These cells are not relocation records, so even multiple independent
    // pointers cannot make control flow complete. A tie or a single
    // observation remains unresolved and fail-closed; accepted projections
    // retain runtime dispatch at every transfer site.
    std::map<std::uint32_t, std::set<std::uint32_t>> base_targets;
    for (std::size_t cell = 0u;
         cell + sizeof(std::uint32_t) <= candidate.bytes.size();
         cell += alignof(std::uint32_t)) {
        const auto raw = static_cast<std::uint32_t>(candidate.bytes[cell]) |
                         (static_cast<std::uint32_t>(candidate.bytes[cell + 1u]) << 8u) |
                         (static_cast<std::uint32_t>(candidate.bytes[cell + 2u]) << 16u) |
                         (static_cast<std::uint32_t>(candidate.bytes[cell + 3u]) << 24u);
        const auto normalized = latent_direct_code_address(raw);
        if (!normalized.has_value()) continue;
        if (*normalized >= latent_aot_main_ram_begin &&
            *normalized < latent_aot_main_ram_end)
            direct_main_ram_targets.insert(*normalized);
        const auto offsets =
            function_offsets_by_low.find(*normalized & 0xfffu);
        if (offsets == function_offsets_by_low.end()) continue;
        std::set<std::uint32_t> cell_bases;
        for (const auto offset : offsets->second) {
            if (*normalized < offset) continue;
            const auto base = *normalized - offset;
            if (!valid_latent_runtime_base(base, candidate.size) ||
                base == candidate.source_address)
                continue;
            cell_bases.insert(base);
        }
        // Repeated copies of one pointer are one observation, not independent
        // evidence for a load base.  Count distinct absolute targets so a
        // repeated table slot cannot promote a guessed alias by itself.
        for (const auto base : cell_bases)
            base_targets[base].insert(*normalized);
    }
    std::size_t best_votes = 0u;
    std::size_t second_votes = 0u;
    std::optional<std::uint32_t> best_base;
    for (const auto& [base, targets] : base_targets) {
        const auto votes = targets.size();
        if (votes > best_votes) {
            second_votes = best_votes;
            best_votes = votes;
            best_base = base;
        } else if (votes > second_votes) {
            second_votes = votes;
        }
    }
    const auto function_evidence_base =
        best_votes >= 2u && best_votes > second_votes ? best_base
                                                      : std::nullopt;
    const auto cluster_base = infer_latent_runtime_pointer_cluster_base(
        direct_main_ram_targets, candidate.size);
    if (function_evidence_base.has_value() && cluster_base.has_value() &&
        function_evidence_base != cluster_base)
        return resolver;
    // A unique bounded whole-image cluster is already an independent module
    // placement proof.  Function-offset votes strengthen it when available,
    // but the absence of such a vote is not contradictory evidence: stripped
    // loader modules commonly expose only their configured entry wrappers in
    // the first pass.  A real disagreement remains fail-closed above.
    resolver.preferred_runtime_base_identity_consistent =
        cluster_base.has_value() &&
        (!function_evidence_base.has_value() ||
         function_evidence_base == cluster_base);
    resolver.preferred_runtime_base = function_evidence_base.has_value()
                                          ? function_evidence_base
                                          : cluster_base;
    return resolver;
}

std::optional<std::uint32_t> latent_block_register_literal(
    const DiscFileCandidate& candidate,
    const katana::ir::BasicBlock& block,
    const std::size_t before_index,
    const std::uint8_t register_index,
    const std::size_t depth = 0u) noexcept {
    using katana::ir::Operation;
    if (register_index >= 16u || depth > 4u) return std::nullopt;
    for (auto index = before_index; index-- > 0u;) {
        const auto& instruction = block.instructions[index];
        const auto use_def =
            katana::ir::instruction_register_use_def(instruction);
        if ((use_def.defs & katana::ir::gpr_register_bit(register_index)) == 0u)
            continue;
        if (instruction.operation == Operation::LoadLongPcRelative &&
            instruction.destination_register == register_index &&
            instruction.effective_address.has_value())
            return latent_read_u32(candidate, *instruction.effective_address);
        if (instruction.operation == Operation::MovRegister &&
            instruction.destination_register == register_index)
            return latent_block_register_literal(
                candidate, block, index, instruction.source_register,
                depth + 1u);
        return std::nullopt;
    }
    return std::nullopt;
}

// Stronger source-root dispatch evidence than the general register-literal
// lane: the branch register must be defined directly by one immutable
// PC-relative literal in the same block and no call may sit between that
// writer and the terminal transfer.  This is intentionally narrower than
// callee-saved literal propagation; it is suitable for entry dispatchers
// without turning a value retained across an unknown call into a root.
std::optional<std::uint32_t>
latent_block_direct_pc_literal_without_call(
    const DiscFileCandidate& candidate,
    const katana::ir::BasicBlock& block,
    const std::size_t before_index,
    const std::uint8_t register_index) noexcept {
    using katana::ir::Operation;
    if (register_index >= 16u) return std::nullopt;
    for (auto index = before_index; index-- > 0u;) {
        const auto& instruction = block.instructions[index];
        if (instruction.operation == Operation::Call ||
            instruction.operation == Operation::CallRegister)
            return std::nullopt;
        const auto use_def =
            katana::ir::instruction_register_use_def(instruction);
        if ((use_def.defs & katana::ir::gpr_register_bit(register_index)) ==
            0u)
            continue;
        if (instruction.operation != Operation::LoadLongPcRelative ||
            instruction.destination_register != register_index ||
            !instruction.effective_address.has_value())
            return std::nullopt;
        return latent_read_u32(candidate, *instruction.effective_address);
    }
    return std::nullopt;
}

// Lowering may split the literal load and the terminal register jump into
// adjacent IR blocks.  Preserve the same bounded proof in that representation:
// the immediately preceding SH-4 instruction in the same analysed function
// must be the PC-relative writer of the exact branch register.  No wider
// backwards search, call crossing, or value propagation is admitted here.
std::optional<std::uint32_t> latent_function_adjacent_pc_literal(
    const DiscFileCandidate& candidate,
    const katana::ir::Function& function,
    const std::uint32_t control_address,
    const std::uint8_t register_index) noexcept {
    using katana::ir::Operation;
    if (register_index >= 16u || control_address < 2u)
        return std::nullopt;
    const auto writer_address = control_address - 2u;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.source_address != writer_address) continue;
            if (instruction.operation != Operation::LoadLongPcRelative ||
                instruction.destination_register != register_index ||
                !instruction.effective_address.has_value())
                return std::nullopt;
            return latent_read_u32(candidate,
                                   *instruction.effective_address);
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool latent_entry_has_early_control_flow(
    const DiscFileCandidate& candidate,
    const std::uint32_t source_address,
    const std::size_t maximum_instructions) noexcept {
    if ((source_address & 1u) != 0u ||
        source_address < candidate.source_address)
        return false;
    const auto offset = source_address - candidate.source_address;
    if (offset > candidate.bytes.size() ||
        candidate.bytes.size() - offset < sizeof(std::uint16_t))
        return false;
    const auto available = (candidate.bytes.size() - offset) / 2u;
    const auto count = std::min(maximum_instructions, available);
    for (std::size_t index = 0u; index < count; ++index) {
        const auto byte = offset + index * 2u;
        const auto opcode = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(candidate.bytes[byte]) |
            static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(candidate.bytes[byte + 1u])
                << 8u));
        const auto decoded = katana::sh4::decode(opcode);
        if (!decoded.is_known()) return false;
        if (decoded.changes_control_flow()) {
            if (!decoded.has_delay_slot) return true;
            // The slot is part of the transfer's architectural extent. A
            // data word can otherwise mimic BSR/BRA as the first halfword
            // and satisfy the early-control-flow probe before its invalid
            // second halfword is inspected.
            if (byte > candidate.bytes.size() ||
                candidate.bytes.size() - byte < 2u * sizeof(std::uint16_t))
                return false;
            const auto slot_opcode = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(candidate.bytes[byte + 2u]) |
                static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(candidate.bytes[byte + 3u])
                    << 8u));
            const auto slot = katana::sh4::decode(slot_opcode);
            return slot.is_known() && !slot.changes_control_flow();
        }
    }
    return false;
}

// Cross-image callback contracts are positive root evidence, not proof that
// an arbitrary candidate word is executable. In particular, a data pointer
// stored through a factory-return receiver can share the same field
// displacement as an unrelated callback record. Before such a heuristic root
// is allowed into the module fixpoint, require every path owned by the proposed
// wrapper to decode to a bounded terminal, closed loop, or an already analysed
// function entry. Direct calls and tail transfers cross function ownership:
// validate their targets as separate plausible code entries instead of
// charging potentially large callee bodies to the wrapper's bounded proof.
// Calls retain their continuation; indirect transfers remain
// runtime-dispatched and are terminal only when the architecture makes them so.
[[nodiscard]] bool latent_entry_has_bounded_complete_local_control_flow(
    const DiscFileCandidate& candidate,
    const std::uint32_t source_address,
    const std::size_t maximum_instructions,
    const std::set<std::uint32_t>& known_function_entries) noexcept {
    using katana::sh4::ControlFlowKind;

    if (maximum_instructions == 0u || (source_address & 1u) != 0u ||
        source_address < candidate.source_address)
        return false;
    const auto candidate_end = static_cast<std::uint64_t>(
                                   candidate.source_address) +
                               candidate.bytes.size();
    if (static_cast<std::uint64_t>(source_address) + 2u > candidate_end)
        return false;

    std::deque<std::uint32_t> pending{source_address};
    std::set<std::uint32_t> queued{source_address};
    std::set<std::uint32_t> visited;
    std::set<std::uint32_t> physical_delay_slots;
    std::size_t decoded_instructions = 0u;
    bool saw_control_flow = false;
    bool reached_known_function = false;

    const auto local_address = [&](const std::int64_t address) {
        return address >= candidate.source_address &&
               static_cast<std::uint64_t>(address) + 2u <= candidate_end &&
               (address & 1) == 0;
    };
    const auto enqueue = [&](const std::int64_t address) {
        if (!local_address(address)) return false;
        const auto local = static_cast<std::uint32_t>(address);
        if (physical_delay_slots.contains(local)) return false;
        if (!visited.contains(local) && queued.insert(local).second)
            pending.push_back(local);
        return true;
    };
    const auto decode_at = [&](const std::uint32_t address) {
        const auto offset = address - candidate.source_address;
        const auto opcode = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(candidate.bytes[offset]) |
            static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(candidate.bytes[offset + 1u])
                << 8u));
        return katana::sh4::decode(opcode);
    };
    while (!pending.empty()) {
        auto address = pending.front();
        pending.pop_front();
        queued.erase(address);
        if (physical_delay_slots.contains(address)) return false;
        if (address != source_address &&
            known_function_entries.contains(address)) {
            if (!saw_control_flow &&
                !latent_validated_fallthrough_save_prefix(
                    candidate, source_address, address))
                return false;
            reached_known_function = true;
            continue;
        }

        while (true) {
            if (!local_address(address) ||
                physical_delay_slots.contains(address))
                return false;
            if (visited.contains(address)) break;
            if (++decoded_instructions > maximum_instructions) return false;
            const auto decoded = decode_at(address);
            if (!decoded.is_known()) return false;
            visited.insert(address);

            if (!decoded.changes_control_flow()) {
                address += 2u;
                if (address != source_address &&
                    known_function_entries.contains(address)) {
                    if (!saw_control_flow &&
                        !latent_validated_fallthrough_save_prefix(
                            candidate, source_address, address))
                        return false;
                    reached_known_function = true;
                    break;
                }
                continue;
            }
            saw_control_flow = true;

            const auto continuation64 =
                static_cast<std::uint64_t>(address) +
                (decoded.has_delay_slot ? 4u : 2u);
            if (continuation64 >
                std::numeric_limits<std::uint32_t>::max())
                return false;
            const auto continuation =
                static_cast<std::uint32_t>(continuation64);
            if (decoded.has_delay_slot) {
                const auto slot64 =
                    static_cast<std::uint64_t>(address) + 2u;
                if (slot64 >
                    std::numeric_limits<std::uint32_t>::max())
                    return false;
                const auto slot = static_cast<std::uint32_t>(slot64);
                if (!local_address(slot) || visited.contains(slot) ||
                    physical_delay_slots.contains(slot) ||
                    ++decoded_instructions > maximum_instructions)
                    return false;
                const auto slot_decoded = decode_at(slot);
                if (!slot_decoded.is_known() ||
                    slot_decoded.changes_control_flow())
                    return false;
                physical_delay_slots.insert(slot);
                visited.insert(slot);
            }

            const auto direct_target = [&]() -> std::optional<std::int64_t> {
                if (decoded.control_flow !=
                        ControlFlowKind::ConditionalBranch &&
                    decoded.control_flow !=
                        ControlFlowKind::UnconditionalBranch &&
                    decoded.control_flow != ControlFlowKind::Call)
                    return std::nullopt;
                return static_cast<std::int64_t>(address) + 4 +
                       decoded.displacement;
            }();

            switch (decoded.control_flow) {
            case ControlFlowKind::ConditionalBranch:
                if (!direct_target.has_value() ||
                    !enqueue(*direct_target))
                    return false;
                address = continuation;
                continue;
            case ControlFlowKind::Call:
                if (!direct_target.has_value()) return false;
                if (!known_function_entries.contains(
                        static_cast<std::uint32_t>(*direct_target))) {
                    if (!local_address(*direct_target)) return false;
                    const auto callee =
                        static_cast<std::uint32_t>(*direct_target);
                    const auto callee_offset =
                        callee - candidate.source_address;
                    if (physical_delay_slots.contains(callee) ||
                        latent_candidate_entry_is_physical_delay_slot(
                            candidate, callee_offset) ||
                        !latent_entry_has_early_control_flow(
                            candidate, callee, maximum_instructions))
                        return false;
                }
                address = continuation;
                continue;
            case ControlFlowKind::IndirectCall:
                address = continuation;
                continue;
            case ControlFlowKind::UnconditionalBranch:
                if (!direct_target.has_value() ||
                    !local_address(*direct_target) ||
                    physical_delay_slots.contains(
                        static_cast<std::uint32_t>(*direct_target)) ||
                    !latent_entry_has_early_control_flow(
                        candidate,
                        static_cast<std::uint32_t>(*direct_target),
                        maximum_instructions))
                    return false;
                break;
            case ControlFlowKind::IndirectBranch:
            case ControlFlowKind::Return:
            case ControlFlowKind::Trap:
            case ControlFlowKind::ExceptionReturn:
            case ControlFlowKind::Halt:
                break;
            case ControlFlowKind::None:
                return false;
            }
            break;
        }
    }
    return saw_control_flow || reached_known_function;
}

std::optional<std::uint32_t>
infer_latent_configured_root_dispatch_base(
    const DiscFileCandidate& candidate,
    const std::span<const katana::ir::Function> program,
    const std::span<const std::uint32_t> root_offsets,
    const std::size_t maximum_entry_scan_instructions) {
    using katana::ir::Operation;
    if (root_offsets.empty() || maximum_entry_scan_instructions == 0u)
        return std::nullopt;

    std::set<std::uint32_t> literal_targets;
    for (const auto& function : program) {
        if (function.entry_address < candidate.source_address) continue;
        const auto function_offset =
            function.entry_address - candidate.source_address;
        if (!std::binary_search(root_offsets.begin(), root_offsets.end(),
                                function_offset))
            continue;
        const bool contains_call = std::any_of(
            function.blocks.begin(), function.blocks.end(),
            [](const auto& block) {
                return std::any_of(
                    block.instructions.begin(), block.instructions.end(),
                    [](const auto& instruction) {
                        return instruction.operation == Operation::Call ||
                               instruction.operation ==
                                   Operation::CallRegister;
                    });
            });
        if (contains_call) continue;
        for (const auto& block : function.blocks) {
            for (std::size_t index = 0u; index < block.instructions.size();
                 ++index) {
                const auto& instruction = block.instructions[index];
                if (instruction.operation != Operation::JumpRegister ||
                    instruction.branch_register_relative)
                    continue;
                const auto raw =
                    latent_block_direct_pc_literal_without_call(
                        candidate, block, index,
                        instruction.branch_register);
                if (!raw.has_value()) continue;
                const auto normalized = latent_direct_code_address(*raw);
                if (normalized.has_value() &&
                    *normalized >= latent_aot_main_ram_begin &&
                    *normalized < latent_aot_main_ram_end)
                    literal_targets.insert(*normalized);
            }
        }
    }
    if (literal_targets.size() < 2u) return std::nullopt;

    std::set<std::uint32_t> candidate_bases;
    const auto maximum_base = static_cast<std::uint32_t>(
        latent_aot_main_ram_end - candidate.size);
    for (const auto target : literal_targets) {
        const auto lowest_unaligned =
            static_cast<std::uint64_t>(target) + 1u >= candidate.size
                ? static_cast<std::uint64_t>(target) + 1u - candidate.size
                : 0u;
        const auto lowest = static_cast<std::uint32_t>(
            (std::max<std::uint64_t>(lowest_unaligned,
                                     latent_aot_main_ram_begin) +
             latent_aot_runtime_page_size - 1u) &
            ~(static_cast<std::uint64_t>(latent_aot_runtime_page_size) -
              1u));
        const auto highest = std::min(
            target & ~(latent_aot_runtime_page_size - 1u),
            maximum_base & ~(latent_aot_runtime_page_size - 1u));
        for (auto base = lowest; base <= highest;) {
            candidate_bases.insert(base);
            if (highest - base < latent_aot_runtime_page_size) break;
            base += latent_aot_runtime_page_size;
        }
    }

    std::size_t best_coverage = 0u;
    std::optional<std::uint32_t> best_base;
    bool best_ambiguous = false;
    for (const auto base : candidate_bases) {
        std::size_t coverage = 0u;
        const auto end = static_cast<std::uint64_t>(base) + candidate.size;
        for (const auto target : literal_targets) {
            if (target < base || static_cast<std::uint64_t>(target) >= end)
                continue;
            const auto offset = target - base;
            if ((offset & 1u) == 0u &&
                latent_entry_has_early_control_flow(
                    candidate, candidate.source_address + offset,
                    maximum_entry_scan_instructions))
                ++coverage;
        }
        if (coverage > best_coverage) {
            best_coverage = coverage;
            best_base = base;
            best_ambiguous = false;
        } else if (coverage == best_coverage && coverage >= 2u) {
            best_ambiguous = true;
        }
    }
    return best_coverage >= 2u && !best_ambiguous ? best_base
                                                  : std::nullopt;
}

struct LatentIndexedCallTableProducer final {
    const katana::ir::Instruction* load = nullptr;
    const katana::ir::Instruction* table_literal = nullptr;
};

[[nodiscard]] bool latent_indexed_table_slice_barrier(
    const katana::ir::Instruction& instruction) noexcept {
    using katana::ir::DelaySlotRole;
    using katana::ir::Operation;
    if (instruction.delay_slot.role != DelaySlotRole::None ||
        instruction.is_privileged)
        return true;
    switch (instruction.operation) {
    case Operation::Unknown:
    case Operation::Branch:
    case Operation::BranchIfTrue:
    case Operation::BranchIfFalse:
    case Operation::JumpRegister:
    case Operation::Call:
    case Operation::CallRegister:
    case Operation::Return:
    case Operation::TrapAlways:
    case Operation::ReturnFromException:
    case Operation::Sleep:
        return true;
    default:
        return false;
    }
}

// Return the exact instruction at an address only when the function contains
// one such instruction.  Normal-entry aliases and malformed IR are both
// ambiguous here and must not be converted into table-root evidence.
const katana::ir::Instruction* latent_unique_function_instruction(
    const katana::ir::Function& function,
    const std::uint32_t address,
    bool& duplicate) noexcept {
    const katana::ir::Instruction* result = nullptr;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.source_address != address) continue;
            if (result != nullptr) {
                duplicate = true;
                return nullptr;
            }
            result = &instruction;
        }
    }
    return result;
}

// Prove an indexed absolute CallRegister from a bounded, physically
// contiguous first-writer slice.  The old implementation searched only the
// current BasicBlock, which loses a valid producer when a normal-entry leader
// splits otherwise linear instructions.  Deliberately do not follow CFG
// predecessors or dataflow facts from outside this slice: that would allow a
// call or an ambiguous register value to manufacture executable roots.
std::optional<LatentIndexedCallTableProducer>
latent_indexed_call_table_producer(
    const katana::ir::Function& function,
    const katana::ir::Instruction& call) noexcept {
    using katana::ir::Operation;
    if (call.operation != Operation::CallRegister ||
        call.branch_register_relative || call.branch_register >= 16u ||
        call.source_address < 2u)
        return std::nullopt;

    bool duplicate = false;
    if (latent_unique_function_instruction(
            function, call.source_address, duplicate) != &call || duplicate)
        return std::nullopt;

    const auto branch_bit =
        katana::ir::gpr_register_bit(call.branch_register);
    bool found_load = false;
    bool found_scale = false;
    bool found_table_literal = false;
    std::uint8_t table_register = 0u;
    const katana::ir::Instruction* table_literal = nullptr;
    const katana::ir::Instruction* load = nullptr;
    std::array<const katana::ir::Instruction*,
               maximum_latent_indexed_call_table_producer_instructions>
        scanned{};
    std::size_t scanned_count = 0u;

    // The call itself is not part of the producer slice.  Every preceding
    // halfword must be represented exactly once in this same Function.
    for (std::size_t distance = 1u;
         distance <= maximum_latent_indexed_call_table_producer_instructions;
         ++distance) {
        const auto delta = static_cast<std::uint32_t>(distance * 2u);
        if (call.source_address < delta) return std::nullopt;
        const auto address = call.source_address - delta;
        duplicate = false;
        const auto* instruction = latent_unique_function_instruction(
            function, address, duplicate);
        if (instruction == nullptr || duplicate) return std::nullopt;
        if (latent_indexed_table_slice_barrier(*instruction))
            return std::nullopt;
        scanned[scanned_count++] = instruction;

        const auto use_def =
            katana::ir::instruction_register_use_def(*instruction);
        if (!found_load) {
            // Any write to the eventual branch register between the indexed
            // load and the call invalidates the first-writer proof.  A write
            // to r0 in this suffix is likewise a post-load clobber and cannot
            // be the scale producer we are looking for.
            if ((use_def.defs & branch_bit) != 0u) {
                if (instruction->operation != Operation::LoadLongR0Indexed ||
                    instruction->destination_register != call.branch_register)
                    return std::nullopt;
                table_register = instruction->source_register;
                if (table_register >= 16u) return std::nullopt;
                for (std::size_t index = 0u; index + 1u < scanned_count;
                     ++index) {
                    const auto suffix_use_def =
                        katana::ir::instruction_register_use_def(
                            *scanned[index]);
                    if ((suffix_use_def.defs &
                         katana::ir::gpr_register_bit(table_register)) != 0u)
                        return std::nullopt;
                }
                load = instruction;
                found_load = true;
                continue;
            }
            if ((use_def.defs & katana::ir::gpr_register_bit(0u)) != 0u)
                return std::nullopt;
            continue;
        }

        if (!found_scale &&
            (use_def.defs & katana::ir::gpr_register_bit(0u)) != 0u) {
            if (instruction->operation != Operation::ShiftLogicalLeftTwo ||
                instruction->destination_register != 0u)
                return std::nullopt;
            found_scale = true;
        }
        if (!found_table_literal &&
            (use_def.defs & katana::ir::gpr_register_bit(table_register)) !=
                0u) {
            if ((instruction->operation != Operation::LoadLongPcRelative &&
                 instruction->operation != Operation::MoveAddressPcRelative) ||
                instruction->destination_register != table_register ||
                !instruction->effective_address.has_value())
                return std::nullopt;
            table_literal = instruction;
            found_table_literal = true;
        }

        if (found_scale && found_table_literal)
            return LatentIndexedCallTableProducer{load,
                                                   table_literal};

        continue;
    }
    return std::nullopt;
}

using LatentRegisterLiteralState =
    std::array<std::optional<std::uint32_t>, 16u>;

struct LatentBlockLiteralState final {
    bool reachable = false;
    LatentRegisterLiteralState registers{};
};

void apply_latent_register_literal_data_instruction(
    const DiscFileCandidate& candidate,
    const katana::ir::Instruction& instruction,
    LatentRegisterLiteralState& registers) noexcept {
    using katana::ir::Operation;
    const auto previous = registers;
    const auto use_def =
        katana::ir::instruction_register_use_def(instruction);
    for (std::uint8_t index = 0u; index < registers.size(); ++index) {
        if ((use_def.defs & katana::ir::gpr_register_bit(index)) != 0u)
            registers[index].reset();
    }

    switch (instruction.operation) {
    case Operation::MovImmediate:
    case Operation::Constant32:
        registers[instruction.destination_register] =
            static_cast<std::uint32_t>(instruction.immediate);
        break;
    case Operation::MovRegister:
        registers[instruction.destination_register] =
            previous[instruction.source_register];
        break;
    case Operation::LoadLongPcRelative:
        if (instruction.effective_address.has_value())
            registers[instruction.destination_register] =
                latent_read_u32(candidate, *instruction.effective_address);
        break;
    case Operation::MoveAddressPcRelative:
        if (instruction.effective_address.has_value())
            registers[instruction.destination_register] =
                *instruction.effective_address;
        break;
    default:
        break;
    }
}

void clear_latent_call_volatile_registers(
    LatentRegisterLiteralState& registers) noexcept {
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        registers[index].reset();
}

struct LatentBlockLiteralTrace final {
    std::vector<LatentRegisterLiteralState> before;
    std::vector<std::optional<LatentRegisterLiteralState>>
        call_arguments_after_delay;
    LatentRegisterLiteralState continuation{};
    bool delay_slots_complete = true;
};

LatentBlockLiteralTrace trace_latent_block_literals(
    const DiscFileCandidate& candidate,
    const katana::ir::BasicBlock& block,
    const LatentRegisterLiteralState& input) {
    using katana::ir::DelaySlotRole;
    using katana::ir::Operation;

    LatentBlockLiteralTrace trace;
    trace.before.reserve(block.instructions.size());
    trace.call_arguments_after_delay.resize(block.instructions.size());
    auto state = input;
    std::optional<std::size_t> delayed_call_index;
    std::optional<std::uint32_t> delayed_call_slot;

    for (std::size_t index = 0u; index < block.instructions.size(); ++index) {
        const auto& instruction = block.instructions[index];
        const bool is_expected_delay_slot =
            delayed_call_index.has_value() &&
            delayed_call_slot.has_value() &&
            instruction.delay_slot.role == DelaySlotRole::Slot &&
            instruction.delay_slot.counterpart_address ==
                block.instructions[*delayed_call_index].source_address &&
            instruction.source_address == *delayed_call_slot;

        // A malformed or split call/delay pair cannot preserve caller-saved
        // facts into the next instruction.  Degrade them before continuing;
        // the missing post-delay argument state remains explicitly absent.
        if (delayed_call_index.has_value() && !is_expected_delay_slot) {
            clear_latent_call_volatile_registers(state);
            delayed_call_index.reset();
            delayed_call_slot.reset();
            trace.delay_slots_complete = false;
        }

        trace.before.push_back(state);
        apply_latent_register_literal_data_instruction(
            candidate, instruction, state);

        if (is_expected_delay_slot) {
            trace.call_arguments_after_delay[*delayed_call_index] = state;
            clear_latent_call_volatile_registers(state);
            delayed_call_index.reset();
            delayed_call_slot.reset();
        }

        const bool call = instruction.operation == Operation::Call ||
                          instruction.operation == Operation::CallRegister;
        if (!call) continue;
        if (instruction.delay_slot.role == DelaySlotRole::Owner &&
            instruction.delay_slot.counterpart_address.has_value()) {
            delayed_call_index = index;
            delayed_call_slot =
                instruction.delay_slot.counterpart_address;
        } else {
            // A call without a physical delay slot publishes its outgoing
            // arguments immediately and clobbers caller-saved registers only
            // for the continuation state.
            trace.call_arguments_after_delay[index] = state;
            clear_latent_call_volatile_registers(state);
        }
    }

    if (delayed_call_index.has_value()) {
        clear_latent_call_volatile_registers(state);
        trace.delay_slots_complete = false;
    }
    trace.continuation = state;
    return trace;
}

std::vector<LatentBlockLiteralState> latent_block_literal_inputs(
    const DiscFileCandidate& candidate,
    const katana::ir::Function& function) {
    std::vector<LatentBlockLiteralState> inputs(function.blocks.size());
    std::map<std::uint32_t, std::size_t> block_indexes;
    for (std::size_t index = 0u; index < function.blocks.size(); ++index)
        block_indexes.emplace(function.blocks[index].start_address, index);
    const auto entry = block_indexes.find(function.entry_address);
    if (entry == block_indexes.end()) return inputs;
    inputs[entry->second].reachable = true;

    // Each reachable block is evaluated once initially and then at most once
    // for every register fact that degrades from one exact literal to Top.
    // A monotone worklist reaches the real fixed point independent of CFG
    // depth; the budget is a checked lattice bound, not a silent pass cap.
    std::deque<std::size_t> pending{entry->second};
    std::vector<bool> queued(function.blocks.size(), false);
    queued[entry->second] = true;
    const auto evaluation_budget =
        std::max<std::size_t>(1u, function.blocks.size() * 17u);
    std::size_t evaluations = 0u;
    while (!pending.empty()) {
        const auto index = pending.front();
        pending.pop_front();
        queued[index] = false;
        if (++evaluations > evaluation_budget) {
            std::ostringstream reason;
            reason << "latent-literal-worklist-budget-entry-0x"
                   << std::hex << std::uppercase
                   << function.entry_address << "-pending";
            std::size_t reported = 0u;
            for (const auto block_index : pending) {
                reason << "-0x"
                       << function.blocks[block_index].start_address;
                if (++reported == 8u) break;
            }
            throw std::runtime_error(reason.str());
        }
        if (!inputs[index].reachable) continue;
        const auto trace = trace_latent_block_literals(
            candidate, function.blocks[index], inputs[index].registers);
        const auto& output = trace.continuation;
        for (const auto successor : function.blocks[index].successors) {
            const auto found = block_indexes.find(successor);
            if (found == block_indexes.end()) continue;
            auto& destination = inputs[found->second];
            bool changed = false;
            if (!destination.reachable) {
                destination.reachable = true;
                destination.registers = output;
                changed = true;
            } else {
                for (std::size_t reg = 0u; reg < output.size(); ++reg) {
                    if (destination.registers[reg].has_value() &&
                        destination.registers[reg] != output[reg]) {
                        destination.registers[reg].reset();
                        changed = true;
                    }
                }
            }
            if (changed && !queued[found->second]) {
                pending.push_back(found->second);
                queued[found->second] = true;
            }
        }
    }
    return inputs;
}

struct LatentAffineValue final {
    bool known = false;
    std::uint32_t constant = 0u;
    std::uint32_t scale = 0u;
    // At most one of the incoming r4..r7 bits may be set.
    std::uint8_t input_mask = 0u;

    bool operator==(const LatentAffineValue&) const = default;
};

[[nodiscard]] LatentAffineValue latent_affine_exact(
    const std::uint32_t value) noexcept {
    return {true, value, 0u, 0u};
}

[[nodiscard]] LatentAffineValue latent_affine_input(
    const std::uint8_t bit) noexcept {
    return {true, 0u, 1u, bit};
}

[[nodiscard]] bool latent_affine_single_input(
    const LatentAffineValue& value) noexcept {
    return value.known && value.input_mask != 0u &&
           (value.input_mask & static_cast<std::uint8_t>(
                                   value.input_mask - 1u)) == 0u;
}

[[nodiscard]] LatentAffineValue latent_affine_add(
    const LatentAffineValue& left,
    const LatentAffineValue& right) noexcept {
    if (!left.known || !right.known) return {};
    if (left.input_mask != 0u && right.input_mask != 0u &&
        left.input_mask != right.input_mask)
        return {};
    const auto mask = static_cast<std::uint8_t>(left.input_mask |
                                                right.input_mask);
    const auto scale64 = static_cast<std::uint64_t>(left.scale) +
                         right.scale;
    if (scale64 > maximum_latent_aot_descriptor_table_stride)
        return {};
    return {true,
            static_cast<std::uint32_t>(left.constant + right.constant),
            static_cast<std::uint32_t>(scale64), mask};
}

[[nodiscard]] LatentAffineValue latent_affine_multiply(
    const LatentAffineValue& left,
    const LatentAffineValue& right) noexcept {
    if (!left.known || !right.known) return {};
    if (left.input_mask != 0u && right.input_mask != 0u) return {};
    const auto& affine = left.input_mask != 0u ? left : right;
    const auto& factor = left.input_mask != 0u ? right : left;
    if (factor.input_mask != 0u) return {};
    const auto scale64 = static_cast<std::uint64_t>(affine.scale) *
                         factor.constant;
    if (scale64 > maximum_latent_aot_descriptor_table_stride)
        return {};
    return {true,
            static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(affine.constant) *
                factor.constant),
            static_cast<std::uint32_t>(scale64),
            affine.input_mask};
}

struct LatentAffineState final {
    std::array<LatentAffineValue, 16u> registers{};
    LatentAffineValue macl;
    std::optional<std::int32_t> stack_offset{0};
    std::map<std::int32_t, LatentAffineValue> stack_values;

    bool operator==(const LatentAffineState&) const = default;
};

[[nodiscard]] std::optional<std::int32_t> latent_stack_address(
    const LatentAffineState& state,
    const std::int32_t displacement = 0) noexcept {
    if (!state.stack_offset.has_value()) return std::nullopt;
    const auto address = static_cast<std::int64_t>(*state.stack_offset) +
                         displacement;
    if (address < std::numeric_limits<std::int32_t>::min() ||
        address > std::numeric_limits<std::int32_t>::max())
        return std::nullopt;
    return static_cast<std::int32_t>(address);
}

void apply_latent_affine_data_instruction(
    const DiscFileCandidate& candidate,
    const katana::ir::Instruction& instruction,
    LatentAffineState& state) noexcept {
    using katana::ir::Operation;
    using katana::ir::SpecialRegister;
    const auto previous = state;
    const auto use_def =
        katana::ir::instruction_register_use_def(instruction);
    for (std::uint8_t index = 0u; index < state.registers.size(); ++index) {
        if ((use_def.defs & katana::ir::gpr_register_bit(index)) != 0u)
            state.registers[index] = {};
    }

    const auto load_stack = [&](const std::int32_t displacement) {
        const auto address = latent_stack_address(previous, displacement);
        if (!address.has_value()) return LatentAffineValue{};
        const auto found = previous.stack_values.find(*address);
        return found != previous.stack_values.end()
                   ? found->second
                   : LatentAffineValue{};
    };
    const auto store_stack = [&](const std::int32_t displacement,
                                 const LatentAffineValue value) {
        const auto address = latent_stack_address(state, displacement);
        if (!address.has_value()) {
            state.stack_values.clear();
            return;
        }
        state.stack_values.insert_or_assign(*address, value);
    };

    switch (instruction.operation) {
    case Operation::MovImmediate:
    case Operation::Constant32:
        state.registers[instruction.destination_register] =
            latent_affine_exact(
                static_cast<std::uint32_t>(instruction.immediate));
        break;
    case Operation::MovRegister:
        state.registers[instruction.destination_register] =
            previous.registers[instruction.source_register];
        break;
    case Operation::LoadLongPcRelative:
        if (instruction.effective_address.has_value()) {
            const auto value = latent_read_u32(
                candidate, *instruction.effective_address);
            if (value.has_value())
                state.registers[instruction.destination_register] =
                    latent_affine_exact(*value);
        }
        break;
    case Operation::MoveAddressPcRelative:
        if (instruction.effective_address.has_value())
            state.registers[instruction.destination_register] =
                latent_affine_exact(*instruction.effective_address);
        break;
    case Operation::AddRegister:
        state.registers[instruction.destination_register] =
            latent_affine_add(
                previous.registers[instruction.destination_register],
                previous.registers[instruction.source_register]);
        break;
    case Operation::AddImmediate:
        state.registers[instruction.destination_register] =
            latent_affine_add(
                previous.registers[instruction.destination_register],
                latent_affine_exact(static_cast<std::uint32_t>(
                    instruction.immediate)));
        if (instruction.destination_register == 15u) {
            const auto next = latent_stack_address(
                previous, instruction.immediate);
            state.stack_offset = next;
            if (!next.has_value()) state.stack_values.clear();
        }
        break;
    case Operation::MultiplyLong:
        state.macl = latent_affine_multiply(
            previous.registers[instruction.destination_register],
            previous.registers[instruction.source_register]);
        break;
    case Operation::StoreSpecialRegister:
        if (instruction.special_register == SpecialRegister::Macl)
            state.registers[instruction.destination_register] =
                previous.macl;
        break;
    case Operation::ShiftLogicalLeftOne:
    case Operation::ShiftArithmeticLeftOne:
    case Operation::ShiftLogicalLeftTwo:
    case Operation::ShiftLogicalLeftEight:
    case Operation::ShiftLogicalLeftSixteen: {
        const std::uint32_t shift =
            instruction.operation == Operation::ShiftLogicalLeftTwo
                ? 2u
                : instruction.operation == Operation::ShiftLogicalLeftEight
                      ? 8u
                      : instruction.operation ==
                                Operation::ShiftLogicalLeftSixteen
                            ? 16u
                            : 1u;
        state.registers[instruction.destination_register] =
            latent_affine_multiply(
                previous.registers[instruction.destination_register],
                latent_affine_exact(1u << shift));
        break;
    }
    case Operation::StoreLong:
        if (instruction.destination_register == 15u)
            store_stack(0, previous.registers[instruction.source_register]);
        break;
    case Operation::StoreLongDisplacement:
        if (instruction.destination_register == 15u)
            store_stack(instruction.displacement,
                        previous.registers[instruction.source_register]);
        break;
    case Operation::StoreLongPreDecrement:
        if (instruction.destination_register == 15u) {
            state.stack_offset = latent_stack_address(previous, -4);
            if (!state.stack_offset.has_value())
                state.stack_values.clear();
            else
                store_stack(0,
                            previous.registers[instruction.source_register]);
        }
        break;
    case Operation::LoadLong:
        if (instruction.source_register == 15u)
            state.registers[instruction.destination_register] =
                load_stack(0);
        break;
    case Operation::LoadLongDisplacement:
        if (instruction.source_register == 15u)
            state.registers[instruction.destination_register] =
                load_stack(instruction.displacement);
        break;
    case Operation::LoadLongPostIncrement:
        if (instruction.source_register == 15u) {
            state.registers[instruction.destination_register] =
                load_stack(0);
            state.stack_offset = latent_stack_address(previous, 4);
            if (!state.stack_offset.has_value())
                state.stack_values.clear();
        }
        break;
    default:
        if (katana::ir::contains_accumulator_register(
                instruction.accumulator_effects.writes_if_s_clear,
                katana::ir::AccumulatorRegister::Macl) ||
            katana::ir::contains_accumulator_register(
                instruction.accumulator_effects.writes_if_s_set,
                katana::ir::AccumulatorRegister::Macl))
            state.macl = {};
        break;
    }
}

void clear_latent_affine_call_volatile_registers(
    LatentAffineState& state) noexcept {
    for (std::uint8_t index = 0u; index <= 7u; ++index)
        state.registers[index] = {};
    state.macl = {};
}

struct LatentAffineTrace final {
    std::vector<LatentAffineState> before;
    std::vector<std::optional<LatentAffineState>>
        transfer_arguments_after_delay;
    LatentAffineState continuation;
    bool delay_slots_complete = true;
};

LatentAffineTrace trace_latent_affine_values(
    const DiscFileCandidate& candidate,
    const katana::ir::BasicBlock& block,
    const LatentAffineState& input) {
    using katana::ir::DelaySlotRole;
    using katana::ir::Operation;
    LatentAffineTrace trace;
    trace.before.reserve(block.instructions.size());
    trace.transfer_arguments_after_delay.resize(block.instructions.size());
    auto state = input;
    std::optional<std::size_t> delayed_transfer_index;
    std::optional<std::uint32_t> delayed_transfer_slot;
    bool delayed_transfer_is_call = false;

    for (std::size_t index = 0u; index < block.instructions.size(); ++index) {
        const auto& instruction = block.instructions[index];
        const bool expected_slot = delayed_transfer_index.has_value() &&
            delayed_transfer_slot.has_value() &&
            instruction.delay_slot.role == DelaySlotRole::Slot &&
            instruction.delay_slot.counterpart_address ==
                block.instructions[*delayed_transfer_index].source_address &&
            instruction.source_address == *delayed_transfer_slot;
        if (delayed_transfer_index.has_value() && !expected_slot) {
            if (delayed_transfer_is_call)
                clear_latent_affine_call_volatile_registers(state);
            delayed_transfer_index.reset();
            delayed_transfer_slot.reset();
            delayed_transfer_is_call = false;
            trace.delay_slots_complete = false;
        }

        trace.before.push_back(state);
        apply_latent_affine_data_instruction(candidate, instruction, state);

        if (expected_slot) {
            trace.transfer_arguments_after_delay[*delayed_transfer_index] =
                state;
            if (delayed_transfer_is_call)
                clear_latent_affine_call_volatile_registers(state);
            delayed_transfer_index.reset();
            delayed_transfer_slot.reset();
            delayed_transfer_is_call = false;
        }

        const bool is_call = instruction.operation == Operation::Call ||
                             instruction.operation ==
                                 Operation::CallRegister;
        const bool is_jump = instruction.operation == Operation::Branch ||
                             instruction.operation ==
                                 Operation::JumpRegister;
        if (!is_call && !is_jump) continue;
        if (instruction.delay_slot.role == DelaySlotRole::Owner &&
            instruction.delay_slot.counterpart_address.has_value()) {
            delayed_transfer_index = index;
            delayed_transfer_slot =
                instruction.delay_slot.counterpart_address;
            delayed_transfer_is_call = is_call;
        } else {
            trace.transfer_arguments_after_delay[index] = state;
            if (is_call)
                clear_latent_affine_call_volatile_registers(state);
        }
    }
    if (delayed_transfer_index.has_value()) {
        if (delayed_transfer_is_call)
            clear_latent_affine_call_volatile_registers(state);
        trace.delay_slots_complete = false;
    }
    trace.continuation = std::move(state);
    return trace;
}

struct LatentAffineBlockInput final {
    bool reachable = false;
    LatentAffineState state;
};

[[nodiscard]] bool join_latent_affine_state(
    LatentAffineState& destination,
    const LatentAffineState& source) {
    bool changed = false;
    for (std::size_t index = 0u; index < destination.registers.size();
         ++index) {
        if (destination.registers[index].known &&
            destination.registers[index] != source.registers[index]) {
            destination.registers[index] = {};
            changed = true;
        }
    }
    if (destination.macl.known && destination.macl != source.macl) {
        destination.macl = {};
        changed = true;
    }
    if (destination.stack_offset != source.stack_offset) {
        if (destination.stack_offset.has_value() ||
            !destination.stack_values.empty()) {
            destination.stack_offset.reset();
            destination.stack_values.clear();
            changed = true;
        }
        return changed;
    }
    for (auto iterator = destination.stack_values.begin();
         iterator != destination.stack_values.end();) {
        const auto found = source.stack_values.find(iterator->first);
        if (found == source.stack_values.end() ||
            found->second != iterator->second) {
            iterator = destination.stack_values.erase(iterator);
            changed = true;
        } else {
            ++iterator;
        }
    }
    return changed;
}

std::vector<LatentAffineBlockInput> latent_affine_block_inputs(
    const DiscFileCandidate& candidate,
    const katana::ir::Function& function) {
    std::vector<LatentAffineBlockInput> inputs(function.blocks.size());
    std::map<std::uint32_t, std::size_t> block_indexes;
    for (std::size_t index = 0u; index < function.blocks.size(); ++index)
        block_indexes.emplace(function.blocks[index].start_address, index);
    const auto entry = block_indexes.find(function.entry_address);
    if (entry == block_indexes.end()) return inputs;
    auto& initial = inputs[entry->second];
    initial.reachable = true;
    for (std::uint8_t argument = 0u; argument < 4u; ++argument)
        initial.state.registers[4u + argument] =
            latent_affine_input(static_cast<std::uint8_t>(1u << argument));

    std::deque<std::size_t> pending{entry->second};
    std::vector<bool> queued(function.blocks.size(), false);
    queued[entry->second] = true;
    const auto budget =
        std::max<std::size_t>(1u, function.blocks.size() * 33u);
    std::size_t evaluations = 0u;
    while (!pending.empty()) {
        const auto index = pending.front();
        pending.pop_front();
        queued[index] = false;
        if (++evaluations > budget)
            throw std::runtime_error(
                "latent-affine-descriptor-worklist-budget");
        if (!inputs[index].reachable) continue;
        const auto trace = trace_latent_affine_values(
            candidate, function.blocks[index], inputs[index].state);
        for (const auto successor : function.blocks[index].successors) {
            const auto found = block_indexes.find(successor);
            if (found == block_indexes.end()) continue;
            auto& destination = inputs[found->second];
            bool changed = false;
            if (!destination.reachable) {
                destination.reachable = true;
                destination.state = trace.continuation;
                changed = true;
            } else {
                changed = join_latent_affine_state(
                    destination.state, trace.continuation);
            }
            if (changed && !queued[found->second]) {
                pending.push_back(found->second);
                queued[found->second] = true;
            }
        }
    }
    return inputs;
}

struct LatentRecordReceiverState final {
    std::array<std::optional<std::uint32_t>, 16u> receivers{};
    // Exact copies of the four incoming SH-4 ABI arguments.  These aliases
    // are not record evidence by themselves.  A bit becomes record evidence
    // only after the matching value is passed to an independently proven
    // persistent-pointer sink.
    std::array<std::uint8_t, 16u> input_aliases{};
    std::uint8_t published_input_aliases = 0u;
};

// Zero is not a valid normalized code address.  It is used only as an
// address-less provenance marker for the canonical incoming r4 record
// receiver; external factory returns always carry a normalized non-zero code
// address.
constexpr std::uint32_t latent_incoming_record_receiver_marker = 0u;

struct LatentBlockRecordReceiverState final {
    bool reachable = false;
    LatentRecordReceiverState state{};
};

[[nodiscard]] bool latent_record_receiver_is_proven(
    const LatentRecordReceiverState& state,
    const std::uint8_t reg) noexcept {
    return reg < state.receivers.size() &&
           (state.receivers[reg].has_value() ||
            (state.input_aliases[reg] &
             state.published_input_aliases) != 0u);
}

void apply_latent_record_receiver_data_instruction(
    const katana::ir::Instruction& instruction,
    LatentRecordReceiverState& state) noexcept {
    using katana::ir::Operation;
    const auto previous = state;
    const auto use_def =
        katana::ir::instruction_register_use_def(instruction);
    for (std::uint8_t index = 0u; index < state.receivers.size(); ++index) {
        if ((use_def.defs & katana::ir::gpr_register_bit(index)) != 0u) {
            state.receivers[index].reset();
            state.input_aliases[index] = 0u;
        }
    }
    if (instruction.operation == Operation::MovRegister) {
        state.receivers[instruction.destination_register] =
            previous.receivers[instruction.source_register];
        state.input_aliases[instruction.destination_register] =
            previous.input_aliases[instruction.source_register];
    }
}

void clear_latent_record_call_volatile_registers(
    LatentRecordReceiverState& state) noexcept {
    for (std::uint8_t index = 0u; index <= 7u; ++index) {
        state.receivers[index].reset();
        state.input_aliases[index] = 0u;
    }
}

std::optional<std::uint32_t> latent_external_call_target(
    const katana::ir::Instruction& instruction,
    const LatentRegisterLiteralState& literals,
    const LatentCodeAddressResolver& resolver) noexcept {
    using katana::ir::Operation;
    std::optional<std::uint32_t> raw;
    if (instruction.operation == Operation::Call &&
        instruction.target_address.has_value())
        raw = instruction.target_address;
    else if (instruction.operation == Operation::CallRegister &&
             !instruction.branch_register_relative &&
             instruction.branch_register < literals.size())
        raw = literals[instruction.branch_register];
    if (!raw.has_value() || !resolver.is_external(*raw))
        return std::nullopt;
    return latent_direct_code_address(*raw);
}

[[nodiscard]] std::uint8_t latent_external_persistent_pointer_sink_mask(
    const katana::ir::Instruction& instruction,
    const LatentRegisterLiteralState& literals,
    const LatentCodeAddressResolver& resolver,
    const std::span<const LatentAotExternalPersistentPointerSink>
        persistent_pointer_sinks) noexcept {
    const auto target =
        latent_external_call_target(instruction, literals, resolver);
    if (!target.has_value()) return 0u;
    const auto sink = std::lower_bound(
        persistent_pointer_sinks.begin(), persistent_pointer_sinks.end(),
        *target,
        [](const auto& candidate_sink, const std::uint32_t address) {
            return candidate_sink.function_address < address;
        });
    if (sink == persistent_pointer_sinks.end() ||
        sink->function_address != *target)
        return 0u;
    return sink->argument_mask;
}

void publish_latent_record_input_aliases(
    LatentRecordReceiverState& state,
    const std::uint8_t argument_mask) noexcept {
    for (std::uint8_t argument = 0u; argument < 4u; ++argument) {
        if ((argument_mask & static_cast<std::uint8_t>(1u << argument)) ==
            0u)
            continue;
        state.published_input_aliases = static_cast<std::uint8_t>(
            state.published_input_aliases |
            state.input_aliases[4u + argument]);
    }
}

std::optional<std::uint32_t> latent_local_call_target(
    const katana::ir::Instruction& instruction,
    const LatentRegisterLiteralState& literals,
    const LatentCodeAddressResolver& resolver) noexcept {
    using katana::ir::Operation;
    std::optional<std::uint32_t> raw;
    if (instruction.operation == Operation::Call &&
        instruction.target_address.has_value())
        raw = instruction.target_address;
    else if (instruction.operation == Operation::CallRegister &&
             !instruction.branch_register_relative &&
             instruction.branch_register < literals.size())
        raw = literals[instruction.branch_register];
    if (!raw.has_value()) return std::nullopt;
    const auto target = resolver.resolve_local(*raw);
    if (!target.has_value() ||
        !resolver.function_entries.contains(*target))
        return std::nullopt;
    return target;
}

std::optional<std::uint32_t> latent_external_transfer_target(
    const katana::ir::Instruction& instruction,
    const LatentRegisterLiteralState& literals,
    const LatentCodeAddressResolver& resolver) noexcept {
    using katana::ir::Operation;
    std::optional<std::uint32_t> raw;
    if ((instruction.operation == Operation::Call ||
         instruction.operation == Operation::Branch) &&
        instruction.target_address.has_value())
        raw = instruction.target_address;
    else if ((instruction.operation == Operation::CallRegister ||
              instruction.operation == Operation::JumpRegister) &&
             !instruction.branch_register_relative &&
             instruction.branch_register < literals.size())
        raw = literals[instruction.branch_register];
    if (!raw.has_value() || !resolver.is_external(*raw))
        return std::nullopt;
    return latent_direct_code_address(*raw);
}

struct LatentBlockRecordReceiverTrace final {
    std::vector<LatentRecordReceiverState> before;
    LatentRecordReceiverState continuation{};
    std::vector<std::uint32_t> local_record_callees;
};

LatentBlockRecordReceiverTrace trace_latent_record_receivers(
    const katana::ir::BasicBlock& block,
    const LatentBlockLiteralTrace& literal_trace,
    const LatentCodeAddressResolver& resolver,
    const LatentRecordReceiverState& input,
    const std::span<const LatentAotExternalPersistentPointerSink>
        persistent_pointer_sinks) {
    using katana::ir::DelaySlotRole;
    using katana::ir::Operation;

    LatentBlockRecordReceiverTrace trace;
    trace.before.reserve(block.instructions.size());
    auto state = input;
    std::optional<std::size_t> delayed_call_index;
    std::optional<std::uint32_t> delayed_call_slot;
    std::optional<std::uint32_t> delayed_factory;
    std::optional<std::uint32_t> delayed_local_call;
    std::uint8_t delayed_persistent_pointer_sink_mask = 0u;

    for (std::size_t index = 0u; index < block.instructions.size(); ++index) {
        const auto& instruction = block.instructions[index];
        const bool expected_delay_slot =
            delayed_call_index.has_value() &&
            delayed_call_slot.has_value() &&
            instruction.delay_slot.role == DelaySlotRole::Slot &&
            instruction.delay_slot.counterpart_address ==
                block.instructions[*delayed_call_index].source_address &&
            instruction.source_address == *delayed_call_slot;
        if (delayed_call_index.has_value() && !expected_delay_slot) {
            // A split or malformed call/delay pair cannot establish an exact
            // constructor-return receiver in the continuation.
            clear_latent_record_call_volatile_registers(state);
            delayed_call_index.reset();
            delayed_call_slot.reset();
            delayed_factory.reset();
            delayed_local_call.reset();
            delayed_persistent_pointer_sink_mask = 0u;
        }

        trace.before.push_back(state);
        apply_latent_record_receiver_data_instruction(instruction, state);

        if (expected_delay_slot) {
            publish_latent_record_input_aliases(
                state, delayed_persistent_pointer_sink_mask);
            if (delayed_local_call.has_value() &&
                latent_record_receiver_is_proven(state, 4u))
                trace.local_record_callees.push_back(*delayed_local_call);
            clear_latent_record_call_volatile_registers(state);
            if (delayed_factory.has_value())
                state.receivers[0u] = delayed_factory;
            delayed_call_index.reset();
            delayed_call_slot.reset();
            delayed_factory.reset();
            delayed_local_call.reset();
            delayed_persistent_pointer_sink_mask = 0u;
        }

        const bool call = instruction.operation == Operation::Call ||
                          instruction.operation == Operation::CallRegister;
        if (!call) continue;
        const auto& literals = index < literal_trace.before.size()
                                   ? literal_trace.before[index]
                                   : LatentRegisterLiteralState{};
        const auto factory =
            latent_external_call_target(instruction, literals, resolver);
        const auto local_call =
            latent_local_call_target(instruction, literals, resolver);
        const auto persistent_pointer_sink_mask =
            latent_external_persistent_pointer_sink_mask(
                instruction, literals, resolver,
                persistent_pointer_sinks);
        if (instruction.delay_slot.role == DelaySlotRole::Owner &&
            instruction.delay_slot.counterpart_address.has_value()) {
            delayed_call_index = index;
            delayed_call_slot = instruction.delay_slot.counterpart_address;
            delayed_factory = factory;
            delayed_local_call = local_call;
            delayed_persistent_pointer_sink_mask =
                persistent_pointer_sink_mask;
        } else {
            publish_latent_record_input_aliases(
                state, persistent_pointer_sink_mask);
            if (local_call.has_value() &&
                latent_record_receiver_is_proven(state, 4u))
                trace.local_record_callees.push_back(*local_call);
            clear_latent_record_call_volatile_registers(state);
            if (factory.has_value()) state.receivers[0u] = factory;
        }
    }
    if (delayed_call_index.has_value())
        clear_latent_record_call_volatile_registers(state);
    trace.continuation = state;
    return trace;
}

std::vector<LatentBlockRecordReceiverState>
latent_block_record_receiver_inputs(
    const DiscFileCandidate& candidate,
    const katana::ir::Function& function,
    const LatentCodeAddressResolver& resolver,
    const std::span<const LatentBlockLiteralState> literal_inputs,
    const bool incoming_record_receiver,
    const std::span<const LatentAotExternalPersistentPointerSink>
        persistent_pointer_sinks) {
    std::vector<LatentBlockRecordReceiverState> inputs(
        function.blocks.size());
    std::map<std::uint32_t, std::size_t> block_indexes;
    for (std::size_t index = 0u; index < function.blocks.size(); ++index)
        block_indexes.emplace(function.blocks[index].start_address, index);
    const auto entry = block_indexes.find(function.entry_address);
    if (entry == block_indexes.end()) return inputs;
    inputs[entry->second].reachable = true;
    for (std::uint8_t argument = 0u; argument < 4u; ++argument)
        inputs[entry->second].state.input_aliases[4u + argument] =
            static_cast<std::uint8_t>(1u << argument);
    // The canonical callback-record ABI passes the record receiver in the
    // first argument register r4.  Preserve that incoming receiver as a
    // provenance marker until a real register definition/call clobber kills
    // it.  This is deliberately only r4: r5..r7 remain unknown because a
    // field-shape contract alone must not turn arbitrary pointer arguments
    // into callback records.  The marker carries no address and is consumed
    // only by the field-sink receiver predicate below.
    if (incoming_record_receiver)
        inputs[entry->second].state.receivers[4u] =
            latent_incoming_record_receiver_marker;

    std::deque<std::size_t> pending{entry->second};
    std::vector<bool> queued(function.blocks.size(), false);
    queued[entry->second] = true;
    const auto evaluation_budget =
        std::max<std::size_t>(1u, function.blocks.size() * 17u);
    std::size_t evaluations = 0u;
    while (!pending.empty()) {
        const auto index = pending.front();
        pending.pop_front();
        queued[index] = false;
        if (++evaluations > evaluation_budget)
            throw std::runtime_error(
                "latent-record-receiver-worklist-budget");
        if (!inputs[index].reachable) continue;
        const auto literal_trace = trace_latent_block_literals(
            candidate, function.blocks[index],
            index < literal_inputs.size() && literal_inputs[index].reachable
                ? literal_inputs[index].registers
                : LatentRegisterLiteralState{});
        const auto receiver_trace = trace_latent_record_receivers(
            function.blocks[index], literal_trace, resolver,
            inputs[index].state, persistent_pointer_sinks);
        for (const auto successor : function.blocks[index].successors) {
            const auto found = block_indexes.find(successor);
            if (found == block_indexes.end()) continue;
            auto& destination = inputs[found->second];
            bool changed = false;
            if (!destination.reachable) {
                destination.reachable = true;
                destination.state = receiver_trace.continuation;
                changed = true;
            } else {
                for (std::size_t reg = 0u;
                     reg < receiver_trace.continuation.receivers.size();
                     ++reg) {
                    if (destination.state.receivers[reg].has_value() &&
                        destination.state.receivers[reg] !=
                            receiver_trace.continuation.receivers[reg]) {
                        destination.state.receivers[reg].reset();
                        changed = true;
                    }
                    if (destination.state.input_aliases[reg] !=
                        receiver_trace.continuation.input_aliases[reg]) {
                        destination.state.input_aliases[reg] = 0u;
                        changed = true;
                    }
                }
                const auto shared_published_aliases =
                    static_cast<std::uint8_t>(
                        destination.state.published_input_aliases &
                        receiver_trace.continuation
                            .published_input_aliases);
                if (shared_published_aliases !=
                    destination.state.published_input_aliases) {
                    destination.state.published_input_aliases =
                        shared_published_aliases;
                    changed = true;
                }
            }
            if (changed && !queued[found->second]) {
                pending.push_back(found->second);
                queued[found->second] = true;
            }
        }
    }
    return inputs;
}

std::vector<std::uint32_t> latent_record_callback_entry_offsets(
    const DiscFileCandidate& candidate,
    const std::span<const katana::ir::Function> program,
    const std::span<const std::uint32_t> external_code_targets,
    const std::span<const LatentAotExternalPersistentPointerSink>
        external_persistent_pointer_sinks,
    const std::span<const LatentAotExternalCallbackFieldSink>
        external_callback_field_sinks,
    const std::span<const std::uint32_t> additional_record_entry_offsets,
    const std::size_t maximum_entry_scan_instructions) {
    using katana::ir::Operation;
    if (external_callback_field_sinks.empty()) return {};
    const auto resolver = make_latent_code_address_resolver(
        candidate, program, external_code_targets);
    if (!resolver.preferred_runtime_base.has_value()) return {};

    using FieldShape = std::pair<std::int32_t, std::uint8_t>;
    std::set<FieldShape> sink_shapes;
    for (const auto& sink : external_callback_field_sinks)
        sink_shapes.emplace(sink.displacement, sink.width);

    // Only authoritative module entries receive the canonical record ABI by
    // construction. Propagate that provenance through exact local calls while
    // r4 remains live (including the delay slot); seeding every decoded
    // helper as an independent ABI entry both invents receivers and can turn
    // one field shape into an unbounded root family.
    std::map<std::uint32_t, const katana::ir::Function*> functions;
    for (const auto& function : program)
        functions.emplace(function.entry_address, &function);
    std::set<std::uint32_t> incoming_record_functions;
    std::deque<std::uint32_t> pending_record_functions;
    for (const auto offset : candidate.entry_offsets) {
        const auto entry = candidate.source_address + offset;
        if (!functions.contains(entry) ||
            !incoming_record_functions.insert(entry).second)
            continue;
        pending_record_functions.push_back(entry);
    }
    for (const auto offset : additional_record_entry_offsets) {
        if (offset > candidate.bytes.size()) continue;
        const auto entry64 = static_cast<std::uint64_t>(
                                 candidate.source_address) +
                             offset;
        if (entry64 > std::numeric_limits<std::uint32_t>::max()) continue;
        const auto entry = static_cast<std::uint32_t>(entry64);
        if (!functions.contains(entry) ||
            !incoming_record_functions.insert(entry).second)
            continue;
        pending_record_functions.push_back(entry);
    }
    while (!pending_record_functions.empty()) {
        const auto entry = pending_record_functions.front();
        pending_record_functions.pop_front();
        const auto function = functions.find(entry);
        if (function == functions.end()) continue;
        const auto literal_inputs =
            latent_block_literal_inputs(candidate, *function->second);
        const auto receiver_inputs = latent_block_record_receiver_inputs(
            candidate, *function->second, resolver, literal_inputs, true,
            external_persistent_pointer_sinks);
        for (std::size_t block_index = 0u;
             block_index < function->second->blocks.size(); ++block_index) {
            if (block_index >= receiver_inputs.size() ||
                !receiver_inputs[block_index].reachable)
                continue;
            const auto literal_trace = trace_latent_block_literals(
                candidate, function->second->blocks[block_index],
                block_index < literal_inputs.size() &&
                        literal_inputs[block_index].reachable
                    ? literal_inputs[block_index].registers
                    : LatentRegisterLiteralState{});
            const auto receiver_trace = trace_latent_record_receivers(
                function->second->blocks[block_index], literal_trace,
                resolver, receiver_inputs[block_index].state,
                external_persistent_pointer_sinks);
            for (const auto callee : receiver_trace.local_record_callees) {
                if (!functions.contains(callee) ||
                    !incoming_record_functions.insert(callee).second)
                    continue;
                pending_record_functions.push_back(callee);
            }
        }
    }

    std::vector<std::uint32_t> result;
    for (const auto& function : program) {
        const auto literal_inputs =
            latent_block_literal_inputs(candidate, function);
        const auto receiver_inputs = latent_block_record_receiver_inputs(
            candidate, function, resolver, literal_inputs,
            incoming_record_functions.contains(function.entry_address),
            external_persistent_pointer_sinks);
        for (std::size_t block_index = 0u;
             block_index < function.blocks.size(); ++block_index) {
            if (block_index >= receiver_inputs.size() ||
                !receiver_inputs[block_index].reachable)
                continue;
            const auto& block = function.blocks[block_index];
            const auto literal_trace = trace_latent_block_literals(
                candidate, block,
                block_index < literal_inputs.size() &&
                        literal_inputs[block_index].reachable
                    ? literal_inputs[block_index].registers
                    : LatentRegisterLiteralState{});
            const auto receiver_trace = trace_latent_record_receivers(
                block, literal_trace, resolver,
                receiver_inputs[block_index].state,
                external_persistent_pointer_sinks);
            for (std::size_t index = 0u; index < block.instructions.size();
                 ++index) {
                const auto& store = block.instructions[index];
                if (store.operation != Operation::StoreLongDisplacement ||
                    !sink_shapes.contains(
                        {store.displacement,
                         static_cast<std::uint8_t>(4u)}) ||
                    index >= literal_trace.before.size() ||
                    index >= receiver_trace.before.size() ||
                    store.destination_register >=
                        receiver_trace.before[index].receivers.size() ||
                    !latent_record_receiver_is_proven(
                        receiver_trace.before[index],
                        store.destination_register))
                    continue;
                auto raw = literal_trace.before[index]
                                                [store.source_register];
                if (!raw.has_value())
                    raw = latent_block_register_literal(
                        candidate, block, index, store.source_register);
                if (!raw.has_value()) continue;
                const auto target = resolver.resolve_local(*raw);
                if (!target.has_value() || (*target & 1u) != 0u ||
                    *target < candidate.source_address)
                    continue;
                const auto offset = *target - candidate.source_address;
                if (offset > candidate.bytes.size() ||
                    candidate.bytes.size() - offset <
                        sizeof(std::uint16_t) ||
                    !latent_entry_has_early_control_flow(
                        candidate, *target,
                        maximum_entry_scan_instructions))
                    continue;
                if (!resolver.function_entries.contains(*target)) {
                    result.push_back(offset);
                    if (result.size() >
                        maximum_prepared_latent_aot_code_pointer_evidence)
                        throw std::runtime_error(
                            "Latente Record-Callback-Analyse ueberschreitet "
                            "ihr Rootbudget.");
                }
            }
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

[[nodiscard]] bool latent_entry_references_record_table_base(
    const DiscFileCandidate& candidate,
    const LatentCodeAddressResolver& resolver,
    const std::uint32_t entry_address,
    const std::uint32_t record_table_base,
    const std::size_t maximum_entry_scan_instructions) noexcept {
    const auto candidate_end =
        static_cast<std::uint64_t>(candidate.source_address) +
        candidate.bytes.size();
    const auto instruction_limit = std::min<std::size_t>(
        maximum_entry_scan_instructions, 128u);
    for (std::size_t index = 0u; index < instruction_limit; ++index) {
        const auto address64 = static_cast<std::uint64_t>(entry_address) +
                               index * sizeof(std::uint16_t);
        if (address64 > std::numeric_limits<std::uint32_t>::max() ||
            address64 + sizeof(std::uint16_t) > candidate_end)
            return false;
        const auto address = static_cast<std::uint32_t>(address64);
        const auto decoded =
            latent_candidate_instruction_at(candidate, address);
        if (!decoded.has_value()) return false;
        if (decoded->kind ==
            katana::sh4::InstructionKind::MovLongLoadPcRelative) {
            const auto literal_address64 =
                ((address64 + 4u) & ~std::uint64_t{3u}) +
                static_cast<std::uint32_t>(decoded->displacement);
            if (literal_address64 <=
                std::numeric_limits<std::uint32_t>::max()) {
                const auto raw = latent_read_u32(
                    candidate,
                    static_cast<std::uint32_t>(literal_address64));
                if (raw.has_value()) {
                    const auto local = resolver.resolve_local(*raw);
                    if (local.has_value() &&
                        *local == record_table_base)
                        return true;
                }
            }
        }
        // The mutually identifying literal must belong to the bounded entry
        // prefix, not to bytes reached only after an unresolved transfer.
        if (decoded->changes_control_flow()) return false;
    }
    return false;
}

// Some loaded modules publish callback records as immutable data instead of
// constructing them through executable stores.  A whole-image pointer-shaped
// word is far too weak to become a function root.  Admit only a mutually
// identifying record family: an identity-bound primary consumer supplies the
// callback field shape; at least three local records repeat the same callback
// with one bounded aligned stride; and the callback's own early code loads a
// literal which resolves exactly back to the first record.  Every component
// remains candidate-local and runtime-base/generation bound.  A missing or
// ambiguous component leaves the entry unresolved.
std::vector<std::uint32_t>
latent_mutual_record_table_callback_entry_offsets(
    const DiscFileCandidate& candidate,
    const std::span<const katana::ir::Function> program,
    const std::span<const std::uint32_t> external_code_targets,
    const std::span<const LatentAotExternalCallbackFieldSink>
        external_callback_field_sinks,
    const std::size_t maximum_entry_scan_instructions) {
    if (external_callback_field_sinks.empty() ||
        maximum_entry_scan_instructions == 0u)
        return {};
    const auto resolver = make_latent_code_address_resolver(
        candidate, program, external_code_targets);
    if (!resolver.preferred_runtime_base.has_value() ||
        !resolver.preferred_runtime_base_identity_consistent)
        return {};

    std::set<std::uint32_t> callback_displacements;
    for (const auto& sink : external_callback_field_sinks) {
        if (!sink.call || sink.width != sizeof(std::uint32_t) ||
            sink.displacement < 0)
            continue;
        const auto displacement =
            static_cast<std::uint32_t>(sink.displacement);
        if ((displacement & (alignof(std::uint32_t) - 1u)) != 0u ||
            displacement >
                maximum_latent_aot_descriptor_object_field_displacement)
            continue;
        callback_displacements.insert(displacement);
    }
    if (callback_displacements.empty()) return {};

    std::map<std::uint32_t, std::vector<std::uint32_t>>
        callback_cells_by_target;
    std::size_t local_pointer_cells = 0u;
    for (std::size_t offset = 0u;
         offset + sizeof(std::uint32_t) <= candidate.bytes.size();
         offset += alignof(std::uint32_t)) {
        const auto raw = static_cast<std::uint32_t>(candidate.bytes[offset]) |
                         (static_cast<std::uint32_t>(candidate.bytes[offset + 1u])
                          << 8u) |
                         (static_cast<std::uint32_t>(candidate.bytes[offset + 2u])
                          << 16u) |
                         (static_cast<std::uint32_t>(candidate.bytes[offset + 3u])
                          << 24u);
        const auto target = resolver.resolve_local(raw);
        if (!target.has_value() || (*target & 1u) != 0u ||
            resolver.function_entries.contains(*target))
            continue;
        if (++local_pointer_cells >
            maximum_prepared_latent_aot_code_pointer_evidence)
            return {};
        const auto cell_address64 =
            static_cast<std::uint64_t>(candidate.source_address) + offset;
        if (cell_address64 > std::numeric_limits<std::uint32_t>::max())
            return {};
        callback_cells_by_target[*target].push_back(
            static_cast<std::uint32_t>(cell_address64));
    }

    std::vector<std::uint32_t> result;
    for (const auto& [target, cells] : callback_cells_by_target) {
        if (cells.size() < 3u ||
            !latent_entry_has_early_control_flow(
                candidate, target, maximum_entry_scan_instructions))
            continue;
        bool admitted = false;
        for (std::size_t first_index = 0u;
             first_index + 2u < cells.size() && !admitted;
             ++first_index) {
            const auto first = cells[first_index];
            for (std::size_t second_index = first_index + 1u;
                 second_index + 1u < cells.size(); ++second_index) {
                const auto stride = cells[second_index] - first;
                if (stride > maximum_latent_aot_descriptor_table_stride)
                    break;
                if (stride < alignof(std::uint32_t) ||
                    (stride & (alignof(std::uint32_t) - 1u)) != 0u)
                    continue;
                const auto third64 =
                    static_cast<std::uint64_t>(cells[second_index]) +
                    stride;
                if (third64 > std::numeric_limits<std::uint32_t>::max() ||
                    !std::binary_search(
                        cells.begin() +
                            static_cast<std::ptrdiff_t>(second_index + 1u),
                        cells.end(), static_cast<std::uint32_t>(third64)))
                    continue;
                for (const auto displacement : callback_displacements) {
                    if (displacement + sizeof(std::uint32_t) > stride ||
                        first < displacement)
                        continue;
                    const auto record_base = first - displacement;
                    if (record_base < candidate.source_address ||
                        (record_base &
                         (alignof(std::uint32_t) - 1u)) != 0u ||
                        !latent_entry_references_record_table_base(
                            candidate, resolver, target, record_base,
                            maximum_entry_scan_instructions))
                        continue;
                    result.push_back(target - candidate.source_address);
                    admitted = true;
                    break;
                }
                if (admitted) break;
            }
        }
        if (result.size() >
            maximum_prepared_latent_aot_code_pointer_evidence)
            throw std::runtime_error(
                "Latente Mutual-Record-Callback-Analyse ueberschreitet "
                "ihr Rootbudget.");
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

struct LatentLocalDescriptorField final {
    std::uint8_t receiver_input_mask = 0u;
    std::uint32_t displacement = 0u;

    bool operator==(const LatentLocalDescriptorField&) const = default;
};

struct LatentLocalDescriptorValue final {
    bool known = false;
    std::uint32_t constant = 0u;
    std::array<std::uint32_t, 16u> coefficients{};
    std::optional<LatentLocalDescriptorField> field;
};

using LatentLocalDescriptorState =
    std::array<LatentLocalDescriptorValue, 16u>;

struct LatentLocalDescriptorContract final {
    std::uint32_t pointer_field_displacement = 0u;
    std::uint32_t record_stride = 0u;
    std::uint32_t callback_field_displacement = 0u;

    bool operator<(const LatentLocalDescriptorContract& other) const noexcept {
        return std::tie(pointer_field_displacement, record_stride,
                        callback_field_displacement) <
               std::tie(other.pointer_field_displacement,
                        other.record_stride,
                        other.callback_field_displacement);
    }
};

[[nodiscard]] LatentLocalDescriptorValue latent_local_descriptor_exact(
    const std::uint32_t value) noexcept {
    LatentLocalDescriptorValue result;
    result.known = true;
    result.constant = value;
    return result;
}

[[nodiscard]] LatentLocalDescriptorValue latent_local_descriptor_symbol(
    const std::uint8_t index) noexcept {
    LatentLocalDescriptorValue result;
    result.known = true;
    result.coefficients[index] = 1u;
    return result;
}

[[nodiscard]] LatentLocalDescriptorValue
latent_local_descriptor_block_input(
    const LatentAffineValue& affine,
    const std::uint8_t fallback_symbol) noexcept {
    if (!affine.known)
        return latent_local_descriptor_symbol(fallback_symbol);
    if (affine.input_mask == 0u)
        return affine.scale == 0u
                   ? latent_local_descriptor_exact(affine.constant)
                   : LatentLocalDescriptorValue{};
    if (!latent_affine_single_input(affine) || affine.scale == 0u ||
        affine.scale > maximum_latent_aot_descriptor_table_stride)
        return {};

    std::uint8_t argument = 0u;
    while ((affine.input_mask &
            static_cast<std::uint8_t>(1u << argument)) == 0u)
        ++argument;
    if (argument >= 4u) return {};
    LatentLocalDescriptorValue result;
    result.known = true;
    result.constant = affine.constant;
    result.coefficients[4u + argument] = affine.scale;
    return result;
}

[[nodiscard]] LatentLocalDescriptorValue latent_local_descriptor_add(
    const LatentLocalDescriptorValue& left,
    const LatentLocalDescriptorValue& right) noexcept {
    if (!left.known || !right.known ||
        (left.field.has_value() && right.field.has_value()))
        return {};
    const auto constant = static_cast<std::uint64_t>(left.constant) +
                          right.constant;
    if (constant > std::numeric_limits<std::uint32_t>::max()) return {};
    LatentLocalDescriptorValue result;
    result.known = true;
    result.constant = static_cast<std::uint32_t>(constant);
    result.field = left.field.has_value() ? left.field : right.field;
    for (std::size_t index = 0u; index < result.coefficients.size(); ++index) {
        const auto coefficient =
            static_cast<std::uint64_t>(left.coefficients[index]) +
            right.coefficients[index];
        if (coefficient > maximum_latent_aot_descriptor_table_stride)
            return {};
        result.coefficients[index] =
            static_cast<std::uint32_t>(coefficient);
    }
    return result;
}

[[nodiscard]] LatentLocalDescriptorValue latent_local_descriptor_scale(
    const LatentLocalDescriptorValue& value,
    const std::uint32_t factor) noexcept {
    if (!value.known || value.field.has_value()) return {};
    const auto constant = static_cast<std::uint64_t>(value.constant) * factor;
    if (constant > std::numeric_limits<std::uint32_t>::max()) return {};
    auto result = value;
    result.constant = static_cast<std::uint32_t>(constant);
    for (auto& coefficient : result.coefficients) {
        const auto scaled = static_cast<std::uint64_t>(coefficient) * factor;
        if (scaled > maximum_latent_aot_descriptor_table_stride) return {};
        coefficient = static_cast<std::uint32_t>(scaled);
    }
    return result;
}

[[nodiscard]] std::optional<std::uint32_t>
latent_local_descriptor_exact_constant(
    const LatentLocalDescriptorValue& value) noexcept {
    if (!value.known || value.field.has_value() ||
        std::any_of(value.coefficients.begin(), value.coefficients.end(),
                    [](const auto coefficient) { return coefficient != 0u; }))
        return std::nullopt;
    return value.constant;
}

[[nodiscard]] std::optional<LatentLocalDescriptorField>
latent_local_descriptor_field_load(
    const LatentLocalDescriptorValue& receiver,
    const std::uint32_t displacement) noexcept {
    if (!receiver.known || receiver.field.has_value())
        return std::nullopt;
    std::optional<std::uint8_t> receiver_argument;
    for (std::uint8_t index = 0u; index < receiver.coefficients.size();
         ++index) {
        const auto coefficient = receiver.coefficients[index];
        if (coefficient == 0u) continue;
        if (coefficient != 1u || index < 4u || index > 7u ||
            receiver_argument.has_value())
            return std::nullopt;
        receiver_argument = static_cast<std::uint8_t>(index - 4u);
    }
    if (!receiver_argument.has_value()) return std::nullopt;
    const auto total = static_cast<std::uint64_t>(receiver.constant) +
                       displacement;
    if (total > maximum_latent_aot_descriptor_object_field_displacement)
        return std::nullopt;
    return LatentLocalDescriptorField{
        static_cast<std::uint8_t>(1u << *receiver_argument),
        static_cast<std::uint32_t>(total)};
}

void apply_latent_local_descriptor_instruction(
    const DiscFileCandidate& candidate,
    const katana::ir::Instruction& instruction,
    LatentLocalDescriptorState& state) noexcept {
    using katana::ir::Operation;
    const auto previous = state;
    const auto use_def =
        katana::ir::instruction_register_use_def(instruction);
    for (std::uint8_t index = 0u; index < state.size(); ++index) {
        if ((use_def.defs & katana::ir::gpr_register_bit(index)) != 0u)
            state[index] = {};
    }

    switch (instruction.operation) {
    case Operation::MovImmediate:
    case Operation::Constant32:
        state[instruction.destination_register] =
            latent_local_descriptor_exact(
                static_cast<std::uint32_t>(instruction.immediate));
        break;
    case Operation::MovRegister:
        state[instruction.destination_register] =
            previous[instruction.source_register];
        break;
    case Operation::LoadLongPcRelative:
        if (instruction.effective_address.has_value()) {
            const auto value = latent_read_u32(
                candidate, *instruction.effective_address);
            if (value.has_value())
                state[instruction.destination_register] =
                    latent_local_descriptor_exact(*value);
        }
        break;
    case Operation::MoveAddressPcRelative:
        if (instruction.effective_address.has_value())
            state[instruction.destination_register] =
                latent_local_descriptor_exact(
                    *instruction.effective_address);
        break;
    case Operation::AddRegister:
        state[instruction.destination_register] =
            latent_local_descriptor_add(
                previous[instruction.destination_register],
                previous[instruction.source_register]);
        break;
    case Operation::AddImmediate:
        state[instruction.destination_register] =
            latent_local_descriptor_add(
                previous[instruction.destination_register],
                latent_local_descriptor_exact(
                    static_cast<std::uint32_t>(instruction.immediate)));
        break;
    case Operation::ShiftLogicalLeftOne:
    case Operation::ShiftArithmeticLeftOne:
    case Operation::ShiftLogicalLeftTwo:
    case Operation::ShiftLogicalLeftEight:
    case Operation::ShiftLogicalLeftSixteen: {
        const std::uint32_t factor =
            instruction.operation == Operation::ShiftLogicalLeftTwo
                ? 4u
                : instruction.operation == Operation::ShiftLogicalLeftEight
                      ? 256u
                      : instruction.operation ==
                                Operation::ShiftLogicalLeftSixteen
                            ? 65'536u
                            : 2u;
        state[instruction.destination_register] =
            latent_local_descriptor_scale(
                previous[instruction.destination_register], factor);
        break;
    }
    case Operation::LoadLongDisplacement: {
        if (instruction.displacement < 0) break;
        const auto field = latent_local_descriptor_field_load(
            previous[instruction.source_register],
            static_cast<std::uint32_t>(instruction.displacement));
        if (!field.has_value()) break;
        auto value = latent_local_descriptor_exact(0u);
        value.field = *field;
        state[instruction.destination_register] = value;
        break;
    }
    case Operation::LoadLongR0Indexed: {
        const auto displacement =
            latent_local_descriptor_exact_constant(previous[0u]);
        if (!displacement.has_value()) break;
        const auto field = latent_local_descriptor_field_load(
            previous[instruction.source_register], *displacement);
        if (!field.has_value()) break;
        auto value = latent_local_descriptor_exact(0u);
        value.field = *field;
        state[instruction.destination_register] = value;
        break;
    }
    default:
        break;
    }
}

std::set<LatentLocalDescriptorContract>
latent_local_descriptor_contracts(
    const DiscFileCandidate& candidate,
    const std::span<const katana::ir::Function> program) {
    using katana::ir::Operation;
    std::set<LatentLocalDescriptorContract> result;
    for (const auto& function : program) {
        const auto affine_inputs =
            latent_affine_block_inputs(candidate, function);
        for (std::size_t block_index = 0u;
             block_index < function.blocks.size(); ++block_index) {
            if (block_index >= affine_inputs.size() ||
                !affine_inputs[block_index].reachable)
                continue;
            const auto& block = function.blocks[block_index];
            LatentLocalDescriptorState descriptor_state;
            for (std::uint8_t index = 0u; index < descriptor_state.size();
                 ++index) {
                const auto& affine = affine_inputs[block_index].state
                                         .registers[index];
                descriptor_state[index] = latent_local_descriptor_block_input(
                    affine, index);
            }
            std::vector<LatentLocalDescriptorState> before;
            before.reserve(block.instructions.size());
            for (std::size_t index = 0u; index < block.instructions.size();
                 ++index) {
                before.push_back(descriptor_state);
                apply_latent_local_descriptor_instruction(
                    candidate, block.instructions[index],
                    descriptor_state);
            }

            for (std::size_t call_index = 0u;
                 call_index < block.instructions.size(); ++call_index) {
                const auto& call = block.instructions[call_index];
                if (call.operation != Operation::CallRegister ||
                    call.branch_register_relative)
                    continue;
                std::optional<std::size_t> load_index;
                for (auto index = call_index; index-- > 0u;) {
                    const auto& definition = block.instructions[index];
                    const auto use_def =
                        katana::ir::instruction_register_use_def(definition);
                    if ((use_def.defs & katana::ir::gpr_register_bit(
                                             call.branch_register)) == 0u)
                        continue;
                    if ((definition.operation ==
                             Operation::LoadLongDisplacement ||
                         definition.operation ==
                             Operation::LoadLongR0Indexed) &&
                        definition.destination_register ==
                            call.branch_register)
                        load_index = index;
                    break;
                }
                if (!load_index.has_value() ||
                    *load_index >= before.size())
                    continue;
                const auto& load = block.instructions[*load_index];
                const auto& address =
                    before[*load_index][load.source_register];
                if (!address.known || !address.field.has_value()) continue;

                std::optional<std::uint32_t> callback_displacement;
                if (load.operation == Operation::LoadLongDisplacement &&
                    load.displacement >= 0) {
                    callback_displacement =
                        static_cast<std::uint32_t>(load.displacement);
                } else if (load.operation == Operation::LoadLongR0Indexed) {
                    callback_displacement =
                        latent_local_descriptor_exact_constant(
                            before[*load_index][0u]);
                }
                if (!callback_displacement.has_value()) continue;

                std::optional<std::uint32_t> stride;
                bool multiple_variables = false;
                for (const auto coefficient : address.coefficients) {
                    if (coefficient == 0u) continue;
                    if (stride.has_value()) {
                        multiple_variables = true;
                        break;
                    }
                    stride = coefficient;
                }
                if (multiple_variables || !stride.has_value() ||
                    *stride < sizeof(std::uint32_t) ||
                    *stride > maximum_latent_aot_descriptor_table_stride ||
                    (*stride & 3u) != 0u)
                    continue;
                const auto callback64 =
                    static_cast<std::uint64_t>(address.constant) +
                    *callback_displacement;
                if (callback64 + sizeof(std::uint32_t) > *stride)
                    continue;
                result.insert(
                    {address.field->displacement, *stride,
                     static_cast<std::uint32_t>(callback64)});
            }
        }
    }
    return result;
}

std::vector<std::uint32_t>
latent_local_persisted_descriptor_callback_entry_offsets(
    const DiscFileCandidate& candidate,
    const std::span<const katana::ir::Function> program,
    const std::span<const std::uint32_t> external_code_targets,
    const std::size_t maximum_entry_scan_instructions) {
    using katana::ir::Operation;
    const auto resolver = make_latent_code_address_resolver(
        candidate, program, external_code_targets);
    if (!resolver.preferred_runtime_base.has_value()) return {};
    const auto contracts =
        latent_local_descriptor_contracts(candidate, program);
    if (contracts.empty()) return {};

    std::vector<std::uint32_t> result;
    std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>
        inspected_tables;
    for (const auto& function : program) {
        const auto affine_inputs =
            latent_affine_block_inputs(candidate, function);
        for (std::size_t block_index = 0u;
             block_index < function.blocks.size(); ++block_index) {
            if (block_index >= affine_inputs.size() ||
                !affine_inputs[block_index].reachable)
                continue;
            const auto& block = function.blocks[block_index];
            const auto trace = trace_latent_affine_values(
                candidate, block, affine_inputs[block_index].state);
            for (std::size_t index = 0u;
                 index < block.instructions.size(); ++index) {
                const auto& store = block.instructions[index];
                if (store.operation != Operation::StoreLongDisplacement &&
                    store.operation != Operation::StoreLongR0Indexed)
                    continue;
                if (index >= trace.before.size()) continue;
                const auto& state = trace.before[index];
                const auto& receiver =
                    state.registers[store.destination_register];
                if (!latent_affine_single_input(receiver) ||
                    receiver.scale != 1u)
                    continue;
                std::optional<std::uint32_t> field_displacement;
                if (store.operation == Operation::StoreLongDisplacement &&
                    store.displacement >= 0) {
                    const auto displacement =
                        static_cast<std::uint64_t>(receiver.constant) +
                        static_cast<std::uint32_t>(store.displacement);
                    if (displacement <=
                        maximum_latent_aot_descriptor_object_field_displacement)
                        field_displacement =
                            static_cast<std::uint32_t>(displacement);
                } else if (store.operation == Operation::StoreLongR0Indexed) {
                    const auto& index_value = state.registers[0u];
                    if (index_value.known && index_value.input_mask == 0u &&
                        index_value.scale == 0u) {
                        const auto displacement =
                            static_cast<std::uint64_t>(receiver.constant) +
                            index_value.constant;
                        if (displacement <=
                            maximum_latent_aot_descriptor_object_field_displacement)
                            field_displacement =
                                static_cast<std::uint32_t>(displacement);
                    }
                }
                if (!field_displacement.has_value()) continue;
                const auto& source = state.registers[store.source_register];
                if (!source.known || source.input_mask != 0u ||
                    source.scale != 0u)
                    continue;
                const auto table = resolver.resolve_local(source.constant);
                if (!table.has_value() || (*table & 3u) != 0u) continue;

                for (const auto& contract : contracts) {
                    if (contract.pointer_field_displacement !=
                        *field_displacement)
                        continue;
                    if (!inspected_tables.emplace(
                            *table, contract.record_stride,
                            contract.callback_field_displacement)
                             .second)
                        continue;
                    std::vector<std::uint32_t> targets;
                    bool terminated = false;
                    for (std::size_t record = 0u;
                         record <=
                             maximum_latent_aot_descriptor_table_records;
                         ++record) {
                        const auto address64 =
                            static_cast<std::uint64_t>(*table) +
                            static_cast<std::uint64_t>(record) *
                                contract.record_stride +
                            contract.callback_field_displacement;
                        if (address64 >
                            std::numeric_limits<std::uint32_t>::max())
                            break;
                        const auto raw = latent_read_u32(
                            candidate,
                            static_cast<std::uint32_t>(address64));
                        if (!raw.has_value()) break;
                        if (*raw == 0u) continue;
                        const auto target = resolver.resolve_local(*raw);
                        if (!target.has_value() || (*target & 1u) != 0u ||
                            !latent_entry_has_early_control_flow(
                                candidate, *target,
                                maximum_entry_scan_instructions)) {
                            terminated = true;
                            break;
                        }
                        if (record ==
                            maximum_latent_aot_descriptor_table_records) {
                            targets.clear();
                            break;
                        }
                        targets.push_back(*target);
                    }
                    std::sort(targets.begin(), targets.end());
                    targets.erase(
                        std::unique(targets.begin(), targets.end()),
                        targets.end());
                    if (!terminated ||
                        targets.size() <
                            minimum_latent_aot_descriptor_table_targets)
                        continue;
                    for (const auto target : targets) {
                        result.push_back(
                            target - candidate.source_address);
                        if (result.size() >
                            maximum_prepared_latent_aot_code_pointer_evidence)
                            throw std::runtime_error(
                                "Latente lokale Descriptor-Callback-Analyse "
                                "ueberschreitet ihr Rootbudget.");
                    }
                }
            }
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<std::uint32_t>
latent_persisted_descriptor_callback_entry_offsets(
    const DiscFileCandidate& candidate,
    const std::span<const katana::ir::Function> program,
    const std::span<const std::uint32_t> external_code_targets,
    const std::span<const LatentAotExternalPersistentPointerSink>
        external_persistent_pointer_sinks,
    const std::span<const LatentAotExternalCallbackFieldSink>
        external_callback_field_sinks,
    const std::size_t maximum_entry_scan_instructions) {
    using katana::ir::Operation;
    if (external_persistent_pointer_sinks.empty() ||
        external_callback_field_sinks.empty())
        return {};
    const auto resolver = make_latent_code_address_resolver(
        candidate, program, external_code_targets);
    if (!resolver.preferred_runtime_base.has_value()) return {};

    using FieldShape = std::pair<std::int32_t, std::uint8_t>;
    std::set<FieldShape> field_shapes;
    for (const auto& sink : external_callback_field_sinks) {
        if (sink.width == 4u && sink.displacement >= 0)
            field_shapes.emplace(sink.displacement, sink.width);
    }

    std::vector<std::uint32_t> result;
    std::set<std::tuple<std::uint32_t, std::uint32_t, std::int32_t>>
        inspected_tables;
    for (const auto& function : program) {
        const auto literal_inputs =
            latent_block_literal_inputs(candidate, function);
        const auto affine_inputs =
            latent_affine_block_inputs(candidate, function);
        for (std::size_t block_index = 0u;
             block_index < function.blocks.size(); ++block_index) {
            if (block_index >= affine_inputs.size() ||
                !affine_inputs[block_index].reachable)
                continue;
            const auto& block = function.blocks[block_index];
            const auto literal_trace = trace_latent_block_literals(
                candidate, block,
                block_index < literal_inputs.size() &&
                        literal_inputs[block_index].reachable
                    ? literal_inputs[block_index].registers
                    : LatentRegisterLiteralState{});
            const auto affine_trace = trace_latent_affine_values(
                candidate, block, affine_inputs[block_index].state);
            for (std::size_t index = 0u;
                 index < block.instructions.size(); ++index) {
                const auto& instruction = block.instructions[index];
                if (instruction.operation != Operation::Call &&
                    instruction.operation != Operation::CallRegister &&
                    instruction.operation != Operation::Branch &&
                    instruction.operation != Operation::JumpRegister)
                    continue;
                if (index >= literal_trace.before.size() ||
                    index >= affine_trace.transfer_arguments_after_delay.size() ||
                    !affine_trace.transfer_arguments_after_delay[index]
                         .has_value())
                    continue;
                const auto callee = latent_external_transfer_target(
                    instruction, literal_trace.before[index], resolver);
                if (!callee.has_value()) continue;
                const auto sink = std::lower_bound(
                    external_persistent_pointer_sinks.begin(),
                    external_persistent_pointer_sinks.end(), *callee,
                    [](const auto& candidate_sink,
                       const std::uint32_t address) {
                        return candidate_sink.function_address < address;
                    });
                if (sink == external_persistent_pointer_sinks.end() ||
                    sink->function_address != *callee)
                    continue;
                const auto& outgoing =
                    *affine_trace.transfer_arguments_after_delay[index];
                for (std::uint8_t argument = 0u; argument < 4u;
                     ++argument) {
                    if ((sink->argument_mask & static_cast<std::uint8_t>(
                                                  1u << argument)) == 0u)
                        continue;
                    const auto& descriptor =
                        outgoing.registers[4u + argument];
                    if (!latent_affine_single_input(descriptor) ||
                        descriptor.scale < 4u ||
                        descriptor.scale >
                            maximum_latent_aot_descriptor_table_stride ||
                        (descriptor.scale & 3u) != 0u)
                        continue;
                    const auto table =
                        resolver.resolve_local(descriptor.constant);
                    if (!table.has_value() || (*table & 3u) != 0u)
                        continue;
                    for (const auto [displacement, width] : field_shapes) {
                        static_cast<void>(width);
                        if (static_cast<std::uint64_t>(displacement) + 4u >
                            descriptor.scale)
                            continue;
                        if (!inspected_tables.emplace(
                                *table, descriptor.scale, displacement)
                                 .second)
                            continue;
                        std::vector<std::uint32_t> targets;
                        bool terminated = false;
                        for (std::size_t record = 0u;
                             record <=
                                 maximum_latent_aot_descriptor_table_records;
                             ++record) {
                            const auto address64 =
                                static_cast<std::uint64_t>(*table) +
                                static_cast<std::uint64_t>(record) *
                                    descriptor.scale +
                                static_cast<std::uint32_t>(displacement);
                            if (address64 >
                                std::numeric_limits<std::uint32_t>::max())
                                break;
                            const auto raw = latent_read_u32(
                                candidate,
                                static_cast<std::uint32_t>(address64));
                            if (!raw.has_value()) break;
                            if (*raw == 0u) continue;
                            const auto target =
                                resolver.resolve_local(*raw);
                            if (!target.has_value() ||
                                (*target & 1u) != 0u ||
                                !latent_entry_has_early_control_flow(
                                    candidate, *target,
                                    maximum_entry_scan_instructions)) {
                                terminated = true;
                                break;
                            }
                            if (record ==
                                maximum_latent_aot_descriptor_table_records) {
                                targets.clear();
                                break;
                            }
                            targets.push_back(*target);
                        }
                        std::sort(targets.begin(), targets.end());
                        targets.erase(
                            std::unique(targets.begin(), targets.end()),
                            targets.end());
                        if (!terminated ||
                            targets.size() <
                                minimum_latent_aot_descriptor_table_targets)
                            continue;
                        for (const auto target : targets) {
                            result.push_back(
                                target - candidate.source_address);
                            if (result.size() >
                                maximum_prepared_latent_aot_code_pointer_evidence)
                                throw std::runtime_error(
                                    "Latente Descriptor-Callback-Analyse "
                                    "ueberschreitet ihr Rootbudget.");
                        }
                    }
                }
            }
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<std::uint32_t> latent_indexed_call_table_entry_offsets(
    const DiscFileCandidate& candidate,
    const std::span<const katana::ir::Function> program,
    const std::span<const std::uint32_t> external_code_targets,
    const std::size_t maximum_entry_scan_instructions) {
    using katana::ir::Operation;
    const auto resolver = make_latent_code_address_resolver(
        candidate, program, external_code_targets);
    if (!resolver.preferred_runtime_base.has_value()) return {};

    std::vector<std::uint32_t> result;
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            for (const auto& call : block.instructions) {
                if (call.operation != Operation::CallRegister ||
                    call.branch_register_relative)
                    continue;

                const auto producer =
                    latent_indexed_call_table_producer(function, call);
                if (!producer.has_value() || producer->load == nullptr ||
                    producer->table_literal == nullptr ||
                    !producer->table_literal->effective_address.has_value())
                    continue;
                const auto raw_table =
                    producer->table_literal->operation ==
                            Operation::LoadLongPcRelative
                        ? latent_read_u32(
                              candidate,
                              *producer->table_literal->effective_address)
                        : std::optional<std::uint32_t>{
                              *producer->table_literal->effective_address};
                if (!raw_table.has_value()) continue;
                const auto table = resolver.resolve_local(*raw_table);
                if (!table.has_value() || (*table & 3u) != 0u)
                    continue;

                std::vector<std::uint32_t> table_targets;
                table_targets.reserve(
                    maximum_latent_aot_indexed_call_table_targets);
                bool terminated = false;
                for (std::size_t slot = 0u;
                     slot <=
                     maximum_latent_aot_indexed_call_table_targets;
                     ++slot) {
                    const auto address64 =
                        static_cast<std::uint64_t>(*table) + slot * 4u;
                    if (address64 >
                        std::numeric_limits<std::uint32_t>::max())
                        break;
                    const auto raw = latent_read_u32(
                        candidate,
                        static_cast<std::uint32_t>(address64));
                    if (!raw.has_value()) break;
                    if (*raw == 0u && slot == 0u) {
                        continue;
                    }
                    if (*raw == 0u) {
                        terminated = true;
                        break;
                    }
                    if (slot ==
                        maximum_latent_aot_indexed_call_table_targets) {
                        table_targets.clear();
                        break;
                    }
                    const auto target = resolver.resolve_local(*raw);
                    const bool physical_delay_slot =
                        target.has_value() && *target >= candidate.source_address &&
                        latent_candidate_entry_is_physical_delay_slot(
                            candidate, *target - candidate.source_address);
                    if (!target.has_value() || physical_delay_slot ||
                        !latent_entry_has_early_control_flow(
                            candidate, *target,
                            maximum_entry_scan_instructions)) {
                        table_targets.clear();
                        break;
                    }
                    table_targets.push_back(*target);
                }
                if (table_targets.size() <
                        minimum_latent_aot_indexed_call_table_targets ||
                    !terminated)
                    continue;
                for (const auto target : table_targets)
                    result.push_back(target - candidate.source_address);
            }
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<std::uint32_t> latent_runtime_alias_call_entry_offsets(
    const DiscFileCandidate& candidate,
    const std::span<const katana::ir::Function> program,
    const std::span<const std::uint32_t> external_code_targets,
    const std::size_t maximum_entry_scan_instructions,
    const std::span<const std::uint32_t> authoritative_tail_roots,
    const bool authoritative_tails_only = false,
    const std::size_t analysis_pass = 0u,
    std::vector<LatentAotLoaderTailAuditDiagnostic>* const
        loader_tail_diagnostics = nullptr) {
    using katana::ir::Operation;
    using katana::ir::DelaySlotRole;
    // Loader tails are admitted only from configured immutable roots (or
    // tails already admitted by this same family).  This deliberately does
    // not use the full analysis root set: callbacks, tables and interior
    // labels are not valid tail sources merely because they were discovered.
    std::vector<std::uint32_t> tail_roots =
        candidate.inferred_authoritative_entry_table
            ? candidate.entry_offsets
            : candidate.explicit_entry_offsets;
    tail_roots.insert(tail_roots.end(), authoritative_tail_roots.begin(),
                      authoritative_tail_roots.end());
    std::sort(tail_roots.begin(), tail_roots.end());
    tail_roots.erase(std::unique(tail_roots.begin(), tail_roots.end()),
                     tail_roots.end());
    auto resolver = make_latent_code_address_resolver(
        candidate, program, external_code_targets);
    const auto configured_root_base =
        infer_latent_configured_root_dispatch_base(
            candidate, program, tail_roots,
            maximum_entry_scan_instructions);
    if (configured_root_base.has_value()) {
        // Exact literal-fed transfers from configured immutable loader roots
        // are stronger placement evidence than pointer-shaped words found by
        // a whole-image scan.  The inferred base is still only positive
        // discovery evidence: generated register dispatch retains its runtime
        // target and loaded-module identity guard.
        resolver.preferred_runtime_base = configured_root_base;
        resolver.preferred_runtime_base_identity_consistent = true;
    }
    if (!resolver.preferred_runtime_base.has_value()) {
        if (loader_tail_diagnostics != nullptr) {
            for (const auto root_offset : tail_roots) {
                loader_tail_diagnostics->push_back(
                    {analysis_pass,
                     root_offset,
                     0u,
                     0u,
                     0u,
                     0u,
                     LatentAotLoaderTailAuditStatus::RuntimeBaseMissing});
            }
        }
        return {};
    }
    std::vector<std::uint32_t> authoritative_tail_offsets;
    for (const auto root_offset : tail_roots) {
        LatentAotLoaderTailAuditDiagnostic best_diagnostic{
            analysis_pass,
            root_offset,
            0u,
            0u,
            0u,
            0u,
            LatentAotLoaderTailAuditStatus::RootOutOfRange};
        const auto consider_diagnostic =
            [&](const LatentAotLoaderTailAuditStatus status,
                const std::uint32_t block_address = 0u,
                const std::uint32_t literal_address = 0u,
                const std::uint32_t raw_target = 0u,
                const std::uint32_t target_address = 0u) {
                if (static_cast<std::uint8_t>(status) <
                    static_cast<std::uint8_t>(best_diagnostic.status))
                    return;
                best_diagnostic.status = status;
                best_diagnostic.block_offset =
                    block_address >= candidate.source_address
                        ? block_address - candidate.source_address
                        : 0u;
                best_diagnostic.literal_offset =
                    literal_address >= candidate.source_address
                        ? literal_address - candidate.source_address
                        : 0u;
                best_diagnostic.raw_target = raw_target;
                best_diagnostic.target_offset =
                    target_address >= candidate.source_address
                        ? target_address - candidate.source_address
                        : 0u;
            };
        if (root_offset > candidate.bytes.size() ||
            candidate.bytes.size() - root_offset < sizeof(std::uint16_t)) {
            if (loader_tail_diagnostics != nullptr)
                loader_tail_diagnostics->push_back(best_diagnostic);
            continue;
        }
        const auto root_address = candidate.source_address + root_offset;
        const auto function = std::find_if(
            program.begin(), program.end(), [&](const auto& value) {
                return value.entry_address == root_address;
            });
        if (function == program.end()) {
            best_diagnostic.status =
                LatentAotLoaderTailAuditStatus::RootFunctionMissing;
            if (loader_tail_diagnostics != nullptr)
                loader_tail_diagnostics->push_back(best_diagnostic);
            continue;
        }
        for (const auto& block : function->blocks) {
            if (block.instructions.size() < 3u ||
                block.instructions.back().delay_slot.role !=
                    DelaySlotRole::Slot) {
                consider_diagnostic(
                    LatentAotLoaderTailAuditStatus::
                        TerminalDelaySlotMissing,
                    block.start_address);
                continue;
            }
            const auto control_index = block.instructions.size() - 2u;
            const auto& control = block.instructions[control_index];
            const auto& delay = block.instructions.back();
            if (control.operation != Operation::JumpRegister ||
                control.branch_register_relative ||
                control.delay_slot.role != DelaySlotRole::Owner ||
                !delay.delay_slot.counterpart_address.has_value() ||
                *delay.delay_slot.counterpart_address !=
                    control.source_address) {
                consider_diagnostic(
                    LatentAotLoaderTailAuditStatus::
                        TerminalRegisterJumpMissing,
                    block.start_address);
                continue;
            }
            std::optional<std::size_t> restore_pr;
            for (std::size_t index = 0u; index < control_index; ++index) {
                const auto& instruction = block.instructions[index];
                if (instruction.operation ==
                        Operation::LoadSpecialRegisterPostIncrement &&
                    instruction.special_register ==
                        katana::ir::SpecialRegister::Pr &&
                    instruction.source_register == 15u)
                    restore_pr = index;
            }
            if (!restore_pr.has_value()) {
                consider_diagnostic(
                    LatentAotLoaderTailAuditStatus::PrRestoreMissing,
                    block.start_address);
                continue;
            }
            std::optional<std::uint32_t> raw_target;
            std::optional<std::uint32_t> literal_address;
            bool stack_restore = false;
            bool valid_epilogue = true;
            const auto is_stack_restore = [](const auto& instruction) {
                if (instruction.source_register != 15u) return false;
                if (instruction.operation == Operation::LoadLongPostIncrement)
                    return instruction.destination_register >= 8u &&
                           instruction.destination_register <= 14u;
                if (instruction.operation ==
                    Operation::FmovLoadPostIncrement)
                    return instruction.destination_register >= 12u &&
                           instruction.destination_register <= 15u;
                return false;
            };
            for (std::size_t index = *restore_pr + 1u;
                 index < control_index; ++index) {
                const auto& instruction = block.instructions[index];
                if (instruction.operation == Operation::LoadLongPcRelative) {
                    if (instruction.destination_register !=
                            control.branch_register ||
                        !instruction.effective_address.has_value() ||
                        raw_target.has_value()) {
                        valid_epilogue = false;
                        break;
                    }
                    raw_target = latent_read_u32(
                        candidate, *instruction.effective_address);
                    if (!raw_target.has_value()) {
                        valid_epilogue = false;
                        break;
                    }
                    literal_address = instruction.effective_address;
                    continue;
                }
                if (is_stack_restore(instruction)) {
                    stack_restore = true;
                    continue;
                }
                if (instruction.operation == Operation::MovRegister &&
                    instruction.destination_register !=
                        control.branch_register)
                    continue;
                valid_epilogue = false;
                break;
            }
            if (!valid_epilogue || !stack_restore ||
                !is_stack_restore(delay)) {
                consider_diagnostic(
                    LatentAotLoaderTailAuditStatus::EpilogueInvalid,
                    block.start_address,
                    literal_address.value_or(0u),
                    raw_target.value_or(0u));
                continue;
            }
            if (!raw_target.has_value() || !literal_address.has_value()) {
                consider_diagnostic(
                    LatentAotLoaderTailAuditStatus::TargetLiteralMissing,
                    block.start_address);
                continue;
            }
            const auto target = resolver.resolve_local_with_evidence(*raw_target);
            if (!target.has_value()) {
                consider_diagnostic(
                    LatentAotLoaderTailAuditStatus::TargetUnresolved,
                    block.start_address, *literal_address, *raw_target);
                continue;
            }
            if (!target->exact &&
                !resolver.preferred_runtime_base_identity_consistent) {
                consider_diagnostic(
                    LatentAotLoaderTailAuditStatus::
                        RuntimeBaseIdentityMismatch,
                    block.start_address, *literal_address, *raw_target,
                    target->target);
                continue;
            }
            if ((target->target & 1u) != 0u ||
                target->target < candidate.source_address ||
                static_cast<std::uint64_t>(target->target) + 2u >
                    static_cast<std::uint64_t>(candidate.source_address) +
                        candidate.bytes.size()) {
                consider_diagnostic(
                    LatentAotLoaderTailAuditStatus::TargetOutOfRange,
                    block.start_address, *literal_address, *raw_target,
                    target->target);
                continue;
            }
            if (!latent_entry_has_early_control_flow(
                    candidate, target->target,
                    maximum_entry_scan_instructions)) {
                consider_diagnostic(
                    LatentAotLoaderTailAuditStatus::
                        TargetControlFlowMissing,
                    block.start_address, *literal_address, *raw_target,
                    target->target);
                continue;
            }
            const auto offset = target->target - candidate.source_address;
            if (std::find(authoritative_tail_offsets.begin(),
                          authoritative_tail_offsets.end(), offset) ==
                authoritative_tail_offsets.end())
                authoritative_tail_offsets.push_back(offset);
            consider_diagnostic(
                LatentAotLoaderTailAuditStatus::Accepted,
                block.start_address, *literal_address, *raw_target,
                target->target);
        }
        if (loader_tail_diagnostics != nullptr)
            loader_tail_diagnostics->push_back(best_diagnostic);
    }
    std::sort(authoritative_tail_offsets.begin(),
              authoritative_tail_offsets.end());
    authoritative_tail_offsets.erase(
        std::unique(authoritative_tail_offsets.begin(),
                    authoritative_tail_offsets.end()),
        authoritative_tail_offsets.end());
    if (authoritative_tails_only) return authoritative_tail_offsets;

    // A direct local BSR is independent evidence that its destination is a
    // callable function entry even when stripped boundaries caused the first
    // analysis pass to retain it only as an interior block. This lets a
    // literal-fed JMP to the same destination prove a tail call without
    // admitting ordinary computed case transfers.
    std::set<std::uint32_t> direct_local_call_entries;
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.operation != Operation::Call ||
                    !instruction.target_address.has_value())
                    continue;
                const auto target = resolver.resolve_local_with_evidence(
                    *instruction.target_address);
                if (!target.has_value() || !target->exact ||
                    (target->target & 1u) != 0u ||
                    target->target < candidate.source_address)
                    continue;
                const auto offset =
                    target->target - candidate.source_address;
                if (offset <= candidate.bytes.size() &&
                    candidate.bytes.size() - offset >=
                        sizeof(std::uint16_t))
                    direct_local_call_entries.insert(target->target);
            }
        }
    }

    std::vector<std::uint32_t> result = authoritative_tail_offsets;
    for (const auto& function : program) {
        const bool configured_root_function =
            function.entry_address >= candidate.source_address &&
            (std::binary_search(
                 candidate.explicit_entry_offsets.begin(),
                 candidate.explicit_entry_offsets.end(),
                 function.entry_address - candidate.source_address) ||
             std::binary_search(
                 authoritative_tail_roots.begin(),
                 authoritative_tail_roots.end(),
                 function.entry_address - candidate.source_address));
        const auto block_inputs =
            latent_block_literal_inputs(candidate, function);
        for (std::size_t block_index = 0u;
             block_index < function.blocks.size(); ++block_index) {
            const auto& block = function.blocks[block_index];
            const auto trace = trace_latent_block_literals(
                candidate, block,
                block_index < block_inputs.size() &&
                        block_inputs[block_index].reachable
                    ? block_inputs[block_index].registers
                    : LatentRegisterLiteralState{});
            for (std::size_t index = 0u; index < block.instructions.size();
                 ++index) {
                const auto& instruction = block.instructions[index];
                const bool register_call =
                    instruction.operation == Operation::CallRegister;
                const bool register_jump =
                    instruction.operation == Operation::JumpRegister;
                if ((!register_call && !register_jump) ||
                    instruction.branch_register_relative)
                    continue;
                const auto& literals = trace.before[index];
                auto raw = instruction.branch_register < literals.size()
                               ? literals[instruction.branch_register]
                               : std::nullopt;
                if (!raw.has_value())
                    raw = latent_block_register_literal(
                        candidate, block, index,
                        instruction.branch_register);
                if (!raw.has_value() && register_jump &&
                    configured_root_function)
                    raw = latent_function_adjacent_pc_literal(
                        candidate, function, instruction.source_address,
                        instruction.branch_register);
                if (!raw.has_value()) continue;
                const auto target = resolver.resolve_local(*raw);
                if (!target.has_value() || (*target & 1u) != 0u ||
                    *target < candidate.source_address)
                    continue;
                if (register_call &&
                    resolver.function_entries.contains(*target))
                    continue;
                if (register_jump) {
                    if (resolver.function_entries.contains(*target))
                        continue;
                    bool bounded_configured_root_dispatch = false;
                    if (configured_root_function) {
                        auto direct_raw =
                            latent_block_direct_pc_literal_without_call(
                                candidate, block, index,
                                instruction.branch_register);
                        if (!direct_raw.has_value())
                            direct_raw = latent_function_adjacent_pc_literal(
                                candidate, function,
                                instruction.source_address,
                                instruction.branch_register);
                        if (direct_raw.has_value()) {
                            const auto direct_target =
                                resolver.resolve_local_with_evidence(
                                    *direct_raw);
                            // A projected runtime base remains positive root
                            // evidence, not an exact edge.  The emitted
                            // register jump keeps its runtime dispatch guard;
                            // a mismatching loaded-module generation therefore
                            // fails closed instead of calling this candidate.
                            bounded_configured_root_dispatch =
                                direct_target.has_value() &&
                                direct_target->target == *target;
                        }
                    }
                    if (!direct_local_call_entries.contains(*target) &&
                        !bounded_configured_root_dispatch)
                        continue;
                }
                const auto offset = *target - candidate.source_address;
                if (offset > candidate.bytes.size() ||
                    candidate.bytes.size() - offset < sizeof(std::uint16_t) ||
                    !latent_entry_has_early_control_flow(
                        candidate, *target,
                        maximum_entry_scan_instructions))
                    continue;
                result.push_back(offset);
            }
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

struct LatentExternalCallbackResolution final {
    std::vector<std::uint32_t> local_entry_offsets;
    std::vector<std::uint32_t> local_record_entry_offsets;
    std::vector<PreparedLatentAotCodePointerEvidence> external_evidence;
};

LatentExternalCallbackResolution resolve_latent_external_callbacks(
    const DiscFileCandidate& candidate,
    const std::span<const katana::ir::Function> program,
    const std::span<const std::uint32_t> external_code_targets,
    const std::span<const LatentAotExternalCallbackSink>
        external_callback_sinks,
    const std::size_t maximum_entry_scan_instructions) {
    using katana::ir::Operation;
    LatentExternalCallbackResolution result;
    if (external_callback_sinks.empty()) return result;

    const auto resolver = make_latent_code_address_resolver(
        candidate, program, external_code_targets);

    for (const auto& function : program) {
        const auto block_inputs =
            latent_block_literal_inputs(candidate, function);
        for (std::size_t block_index = 0u;
             block_index < function.blocks.size(); ++block_index) {
            const auto& block = function.blocks[block_index];
            const auto trace = trace_latent_block_literals(
                candidate, block,
                block_index < block_inputs.size() &&
                        block_inputs[block_index].reachable
                    ? block_inputs[block_index].registers
                    : LatentRegisterLiteralState{});
            for (std::size_t index = 0u; index < block.instructions.size();
                 ++index) {
                const auto& instruction = block.instructions[index];
                if (instruction.operation == Operation::Call ||
                    (instruction.operation == Operation::CallRegister &&
                     !instruction.branch_register_relative)) {
                    const auto inspect_callback_call = [&]() {
                        if (index >= trace.before.size() ||
                            index >=
                                trace.call_arguments_after_delay.size() ||
                            !trace.call_arguments_after_delay[index]
                                 .has_value())
                            return;
                        const auto& literals = trace.before[index];
                        const auto& outgoing =
                            *trace.call_arguments_after_delay[index];
                        std::optional<std::uint32_t> raw_callee;
                        if (instruction.operation == Operation::Call) {
                            raw_callee = instruction.target_address;
                        } else {
                            raw_callee =
                                instruction.branch_register < literals.size()
                                    ? literals[instruction.branch_register]
                                    : std::nullopt;
                            if (!raw_callee.has_value())
                                raw_callee = latent_block_register_literal(
                                    candidate, block, index,
                                    instruction.branch_register);
                        }
                        if (!raw_callee.has_value()) return;
                        const auto callee =
                            resolver.resolve(*raw_callee, true);
                        if (!callee.has_value()) return;
                        const auto sink = std::lower_bound(
                            external_callback_sinks.begin(),
                            external_callback_sinks.end(), *callee,
                            [](const auto& candidate_sink,
                               const std::uint32_t address) {
                                return candidate_sink.function_address <
                                       address;
                            });
                        if (sink == external_callback_sinks.end() ||
                            sink->function_address != *callee)
                            return;

                        for (std::uint8_t argument = 0u; argument < 4u;
                             ++argument) {
                            if ((sink->argument_mask &
                                 static_cast<std::uint8_t>(1u << argument)) ==
                                0u)
                                continue;
                            const auto argument_register =
                                static_cast<std::uint8_t>(4u + argument);
                            const auto raw_callback =
                                outgoing[argument_register];
                            if (!raw_callback.has_value()) continue;
                            const auto target =
                                resolver.resolve_local(*raw_callback);
                            if (!target.has_value()) {
                                // A transformed module may register a
                                // resident title callback with a resident
                                // higher-order API.  This is the callback
                                // counterpart to a literal cross-image
                                // Call/Jump: retain the direct main-RAM value
                                // as a candidate, but leave its admission to
                                // the exporter-side executable-byte and entry
                                // shape proof.  Pointer-shaped module data on
                                // its own never reaches this path.
                                const auto external =
                                    latent_direct_code_address(
                                        *raw_callback);
                                if (external.has_value())
                                    result.external_evidence.push_back(
                                        {instruction.source_address -
                                             candidate.source_address,
                                         *external,
                                         PreparedLatentAotCodePointerEvidenceKind::
                                             CallbackArgument,
                                         *callee,
                                         argument});
                                continue;
                            }
                            if ((*target & 1u) != 0u ||
                                *target < candidate.source_address)
                                continue;
                            const auto offset =
                                *target - candidate.source_address;
                            if (offset > candidate.bytes.size() ||
                                candidate.bytes.size() - offset <
                                    sizeof(std::uint16_t) ||
                                !latent_entry_has_early_control_flow(
                                    candidate, *target,
                                    maximum_entry_scan_instructions))
                                continue;
                            if ((sink->record_argument_mask &
                                 static_cast<std::uint8_t>(1u << argument)) !=
                                0u)
                                result.local_record_entry_offsets.push_back(
                                    offset);
                            if (!resolver.function_entries.contains(*target))
                                result.local_entry_offsets.push_back(offset);
                        }
                    };
                    inspect_callback_call();
                }
            }
        }
    }
    const auto normalize = [](auto& values) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    normalize(result.local_entry_offsets);
    normalize(result.local_record_entry_offsets);
    std::sort(result.external_evidence.begin(),
              result.external_evidence.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.source_offset,
                                  left.target_address,
                                  left.kind,
                                  left.sink_address,
                                  left.argument_index) <
                         std::tie(right.source_offset,
                                  right.target_address,
                                  right.kind,
                                  right.sink_address,
                                  right.argument_index);
              });
    result.external_evidence.erase(
        std::unique(result.external_evidence.begin(),
                    result.external_evidence.end()),
        result.external_evidence.end());
    if (result.external_evidence.size() >
        maximum_prepared_latent_aot_code_pointer_evidence)
        throw std::runtime_error(
            "Latentes AOT-Modul ueberschreitet das externe "
            "Callback-Evidence-Budget.");
    return result;
}

LatentExternalCallbackResolution
resolve_latent_external_callback_record_tables(
    const DiscFileCandidate& candidate,
    const std::span<const katana::ir::Function> program,
    const std::span<const std::uint32_t> external_code_targets,
    const std::span<const std::uint32_t> external_data_targets,
    const std::span<const LatentAotExternalCallbackRecordTable>
        external_callback_record_tables,
    const std::size_t maximum_entry_scan_instructions) {
    LatentExternalCallbackResolution result;
    if (external_data_targets.empty() ||
        external_callback_record_tables.empty())
        return result;

    const auto resolver = make_latent_code_address_resolver(
        candidate, program, external_code_targets);
    if (!resolver.preferred_runtime_base.has_value()) return result;

    std::set<std::tuple<std::uint32_t, std::int32_t, std::uint32_t,
                        std::int32_t, std::uint32_t, std::uint8_t>>
        inspected_tables;
    for (const auto raw_header : external_data_targets) {
        const auto header = resolver.resolve_local(raw_header);
        if (!header.has_value() || (*header & 3u) != 0u)
            continue;
        for (const auto& contract : external_callback_record_tables) {
            if (contract.header_table_pointer_displacement < 4)
                continue;
            const auto count_address =
                static_cast<std::uint64_t>(*header) +
                static_cast<std::uint32_t>(
                    contract.header_table_pointer_displacement - 4);
            const auto table_pointer_address =
                static_cast<std::uint64_t>(*header) +
                static_cast<std::uint32_t>(
                    contract.header_table_pointer_displacement);
            if (count_address >
                    std::numeric_limits<std::uint32_t>::max() ||
                table_pointer_address >
                std::numeric_limits<std::uint32_t>::max())
                continue;
            const auto record_count = latent_read_u32(
                candidate, static_cast<std::uint32_t>(count_address));
            const auto raw_table = latent_read_u32(
                candidate,
                static_cast<std::uint32_t>(table_pointer_address));
            if (!record_count.has_value() || !raw_table.has_value() ||
                *record_count < minimum_latent_aot_descriptor_table_targets ||
                *record_count > maximum_latent_aot_descriptor_table_records)
                continue;
            const auto table = resolver.resolve_local(*raw_table);
            if (!table.has_value() || (*table & 3u) != 0u ||
                static_cast<std::uint64_t>(*table) +
                        static_cast<std::uint64_t>(*record_count) *
                            contract.record_stride !=
                    *header ||
                !inspected_tables.emplace(
                    *header,
                    contract.header_table_pointer_displacement,
                    contract.record_stride,
                    contract.callback_displacement,
                    contract.callback_sink_address,
                    contract.callback_argument)
                     .second)
                continue;

            std::vector<std::uint32_t> local_targets;
            std::vector<PreparedLatentAotCodePointerEvidence>
                external_evidence;
            bool valid_table = true;
            for (std::size_t record = 0u; record < *record_count; ++record) {
                const auto callback_address64 =
                    static_cast<std::uint64_t>(*table) +
                    static_cast<std::uint64_t>(record) *
                        contract.record_stride +
                    static_cast<std::uint32_t>(
                        contract.callback_displacement);
                if (callback_address64 >
                    std::numeric_limits<std::uint32_t>::max()) {
                    valid_table = false;
                    break;
                }
                const auto callback_address =
                    static_cast<std::uint32_t>(callback_address64);
                const auto raw_callback =
                    latent_read_u32(candidate, callback_address);
                if (!raw_callback.has_value()) {
                    valid_table = false;
                    break;
                }
                if (*raw_callback == 0u) continue;

                const auto local = resolver.resolve_local(*raw_callback);
                if (local.has_value()) {
                    if ((*local & 1u) != 0u ||
                        !latent_entry_has_early_control_flow(
                            candidate, *local,
                            maximum_entry_scan_instructions)) {
                        valid_table = false;
                        break;
                    }
                    local_targets.push_back(*local);
                    continue;
                }

                const auto external =
                    latent_direct_code_address(*raw_callback);
                if (external.has_value()) {
                    external_evidence.push_back(
                        {callback_address - candidate.source_address,
                         *external,
                         PreparedLatentAotCodePointerEvidenceKind::
                             CallbackArgument,
                         contract.callback_sink_address,
                         contract.callback_argument});
                    continue;
                }

                valid_table = false;
                break;
            }

            std::set<std::uint32_t> distinct_targets(
                local_targets.begin(), local_targets.end());
            for (const auto& evidence : external_evidence)
                distinct_targets.insert(evidence.target_address);
            if (!valid_table ||
                distinct_targets.size() <
                    minimum_latent_aot_descriptor_table_targets)
                continue;
            result.local_entry_offsets.reserve(
                result.local_entry_offsets.size() + local_targets.size());
            for (const auto target : local_targets)
                result.local_entry_offsets.push_back(
                    target - candidate.source_address);
            result.external_evidence.insert(
                result.external_evidence.end(),
                external_evidence.begin(), external_evidence.end());
            if (result.local_entry_offsets.size() +
                    result.external_evidence.size() >
                maximum_prepared_latent_aot_code_pointer_evidence)
                throw std::runtime_error(
                    "Latente Record-Callback-Analyse ueberschreitet ihr "
                    "Rootbudget.");
        }
    }

    std::sort(result.local_entry_offsets.begin(),
              result.local_entry_offsets.end());
    result.local_entry_offsets.erase(
        std::unique(result.local_entry_offsets.begin(),
                    result.local_entry_offsets.end()),
        result.local_entry_offsets.end());
    std::sort(result.external_evidence.begin(),
              result.external_evidence.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.source_offset,
                                  left.target_address,
                                  left.kind,
                                  left.sink_address,
                                  left.argument_index) <
                         std::tie(right.source_offset,
                                  right.target_address,
                                  right.kind,
                                  right.sink_address,
                                  right.argument_index);
              });
    result.external_evidence.erase(
        std::unique(result.external_evidence.begin(),
                    result.external_evidence.end()),
        result.external_evidence.end());
    return result;
}

struct LatentLiteralTransferResolution final {
    std::vector<PreparedLatentAotExternalTransfer> admitted;
    std::vector<PreparedLatentAotExternalTransfer> candidates;
};

LatentLiteralTransferResolution
resolve_latent_literal_transfers(
    const DiscFileCandidate& candidate,
    std::span<katana::ir::Function> program,
    const std::span<const std::uint32_t> external_code_targets) {
    using katana::ir::DynamicTargetClass;
    using katana::ir::Operation;
    const auto resolver = make_latent_code_address_resolver(
        candidate, program, external_code_targets);
    LatentLiteralTransferResolution result;
    using TransferKey =
        std::pair<std::uint32_t, PreparedLatentAotExternalTransferKind>;
    std::map<TransferKey, std::size_t> transfer_occurrences;
    std::vector<PreparedLatentAotExternalTransfer> external_observations;
    for (auto& function : program) {
        const auto block_inputs =
            latent_block_literal_inputs(candidate, function);
        for (std::size_t block_index = 0u;
             block_index < function.blocks.size(); ++block_index) {
            auto& block = function.blocks[block_index];
            const auto trace = trace_latent_block_literals(
                candidate, block,
                block_index < block_inputs.size() &&
                        block_inputs[block_index].reachable
                    ? block_inputs[block_index].registers
                    : LatentRegisterLiteralState{});
            for (std::size_t index = 0u; index < block.instructions.size();
                 ++index) {
                auto& instruction = block.instructions[index];
                if ((instruction.operation == Operation::CallRegister ||
                     instruction.operation == Operation::JumpRegister) &&
                    !instruction.branch_register_relative) {
                    const auto transfer_kind =
                        instruction.operation == Operation::CallRegister
                            ? PreparedLatentAotExternalTransferKind::Call
                            : PreparedLatentAotExternalTransferKind::Jump;
                    const auto source_offset =
                        instruction.source_address - candidate.source_address;
                    ++transfer_occurrences[{source_offset, transfer_kind}];
                    const auto& literals = trace.before[index];
                    auto raw = instruction.branch_register < literals.size()
                                   ? literals[instruction.branch_register]
                                   : std::nullopt;
                    if (!raw.has_value())
                        raw = latent_block_register_literal(
                            candidate, block, index,
                            instruction.branch_register);
                    const auto append_transfer =
                        [&](auto& transfers, const std::uint32_t target) {
                            transfers.push_back(
                                {source_offset, target, transfer_kind});
                        };
                    if (raw.has_value() && resolver.is_external(*raw)) {
                        const auto target =
                            latent_direct_code_address(*raw);
                        if (target.has_value()) {
                            if (instruction.resolved_targets.empty())
                                append_transfer(external_observations,
                                                *target);
                            else
                                append_transfer(result.candidates, *target);
                        }
                    } else if (raw.has_value()) {
                        const auto local =
                            resolver.resolve_local_with_evidence(*raw);
                        if (!local.has_value()) {
                            // A literal-loaded register transfer is stronger
                            // evidence than a pointer-shaped data cell.
                            // Preserve an otherwise unknown direct-main-RAM
                            // target so the product-level cross-image fixpoint
                            // can validate it against independent primary
                            // image bytes. No IR edge is closed here.
                            const auto target =
                                latent_direct_code_address(*raw);
                            if (target.has_value())
                                append_transfer(result.candidates, *target);
                        } else {
                            const auto admitted =
                                instruction.operation ==
                                        Operation::CallRegister
                                    ? resolver.function_entries.contains(
                                          local->target)
                                    : resolver.block_entries.contains(
                                          local->target);
                            if (admitted) {
                                instruction.resolved_targets = {
                                    local->target};
                                instruction.dynamic_target_class =
                                    local->exact
                                        ? DynamicTargetClass::GuardedComplete
                                        : DynamicTargetClass::GuardedPartial;
                                // Only an exact source-range target closes the
                                // indirect edge. A projected runtime alias
                                // remains a guarded positive edge.
                                block.has_indirect_successor = !local->exact;
                                if (instruction.operation ==
                                    Operation::CallRegister) {
                                    const auto insertion = std::lower_bound(
                                        function.direct_callees.begin(),
                                        function.direct_callees.end(),
                                        local->target);
                                    if (insertion ==
                                            function.direct_callees.end() ||
                                        *insertion != local->target)
                                        function.direct_callees.insert(
                                            insertion, local->target);
                                } else if (std::any_of(
                                               function.blocks.begin(),
                                               function.blocks.end(),
                                               [&](const auto& candidate_block) {
                                                   return candidate_block
                                                              .start_address ==
                                                          local->target;
                                               })) {
                                    const auto insertion = std::lower_bound(
                                        block.successors.begin(),
                                        block.successors.end(), local->target);
                                    if (insertion == block.successors.end() ||
                                        *insertion != local->target)
                                        block.successors.insert(insertion,
                                                                local->target);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    // Resume entries can produce multiple IR views of one physical transfer.
    // Publish a cross-image edge only when every view independently observes
    // the same external target and none already owns a local resolved edge.
    // Conflicting or incomplete views remain bounded candidate evidence.
    std::sort(external_observations.begin(), external_observations.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.source_offset,
                                  left.target_address,
                                  left.kind) <
                         std::tie(right.source_offset,
                                  right.target_address,
                                  right.kind);
              });
    for (auto first = external_observations.begin();
         first != external_observations.end();) {
        const auto last = std::find_if(
            first, external_observations.end(),
            [&](const auto& candidate_transfer) {
                return candidate_transfer != *first;
            });
        const auto occurrence = transfer_occurrences.find(
            {first->source_offset, first->kind});
        const auto observation_count =
            static_cast<std::size_t>(std::distance(first, last));
        if (occurrence != transfer_occurrences.end() &&
            observation_count == occurrence->second)
            result.admitted.push_back(*first);
        else
            result.candidates.push_back(*first);
        first = last;
    }
    const auto normalize = [](auto& transfers) {
        std::sort(transfers.begin(), transfers.end(),
                  [](const auto& left, const auto& right) {
                      return std::tie(left.source_offset,
                                      left.target_address,
                                      left.kind) <
                             std::tie(right.source_offset,
                                      right.target_address,
                                      right.kind);
                  });
        transfers.erase(std::unique(transfers.begin(), transfers.end()),
                        transfers.end());
        if (transfers.size() > maximum_prepared_latent_aot_external_transfers)
            throw std::runtime_error(
                "Latentes AOT-Modul ueberschreitet das externe "
                "Transferbudget.");
    };
    normalize(result.admitted);
    normalize(result.candidates);
    return result;
}

bool valid_linear_physical_range(const LatentAotOccupiedRange range) noexcept {
    if (range.size == 0u ||
        range.size >
            0x1'0000'0000ull - static_cast<std::uint64_t>(range.start))
        return false;
    const auto physical_start = katana::runtime::canonical_physical_address(range.start);
    const auto last = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(range.start) + range.size - 1u);
    return static_cast<std::uint64_t>(physical_start) + range.size <= 0x1'0000'0000ull &&
           katana::runtime::canonical_physical_address(last) ==
               physical_start + range.size - 1u;
}

bool physical_overlap(const LatentAotOccupiedRange left,
                      const LatentAotOccupiedRange right) noexcept {
    const auto left_begin = static_cast<std::uint64_t>(
        katana::runtime::canonical_physical_address(left.start));
    const auto right_begin = static_cast<std::uint64_t>(
        katana::runtime::canonical_physical_address(right.start));
    return left_begin < right_begin + right.size &&
           right_begin < left_begin + left.size;
}

struct CandidateAnalysisOutcome {
    std::optional<PreparedLatentAotModule> module;
    LatentAotAnalysisRejection rejection =
        LatentAotAnalysisRejection::ProgramInvalid;
    bool deterministic = true;
    std::string rejection_detail{"none"};
    bool terminal_inventory_candidate_values = false;
};

// The static candidate graph is independent of the cross-image declaration
// wave, but it is not independent of the exact analyzer admission contract.
// Keep this key deliberately explicit: a session replay is allowed only for
// the same decoded bytes, roots, ABI, limits, implementation identities and
// persistent FVA epoch.  Resolver/sink declarations are intentionally kept
// outside this key and are tracked by LatentAotResolverContract below.
struct LatentAotStaticCandidateKey final {
    std::string byte_identity;
    std::uint32_t byte_size = 0u;
    std::uint32_t source_address = 0u;
    std::vector<std::uint32_t> entry_offsets;
    std::vector<std::uint32_t> explicit_entry_offsets;
    std::vector<CompleteDisassemblyEntryAuthority> authority_entries;
    CompleteDisassemblyModuleClass module_class =
        CompleteDisassemblyModuleClass::LatentLoaded;
    bool exact_candidate = false;
    bool inferred_authoritative_entry_table = false;
    std::optional<std::uint32_t> proven_runtime_base;
    std::uint8_t mode = 0u;
    std::uint8_t completeness_policy = 0u;
    std::uint32_t analyzer_abi = 0u;
    std::size_t maximum_entry_scan_instructions = 0u;
    std::size_t maximum_native_instructions = 0u;
    std::size_t maximum_blocks = 0u;
    std::size_t maximum_functions = 0u;
    std::size_t maximum_analysis_iterations = 0u;
    std::size_t maximum_analysis_contexts = 0u;
    std::string analysis_implementation_identity;
    std::string analysis_cache_implementation_identity;
    std::string ir_product_implementation_identity;
    std::string persistent_epoch_identity;
    std::vector<PreparedLatentAotSourceBinding> source_bindings;

    [[nodiscard]] bool operator==(
        const LatentAotStaticCandidateKey&) const = default;
};

// Resolver declarations are monotonic across the native cross-image
// fixpoint.  They are not part of the static-program key because an additive
// declaration wave may be serviced by the bounded resolvers below.  Removal,
// replacement or reordering is never replayed.
struct LatentAotResolverContract final {
    std::vector<std::uint32_t> external_code_targets;
    std::vector<std::uint32_t> external_data_targets;
    std::vector<LatentAotExternalCallbackSink> external_callback_sinks;
    std::vector<LatentAotExternalPersistentPointerSink>
        external_persistent_pointer_sinks;
    std::vector<LatentAotExternalCallbackFieldSink>
        external_callback_field_sinks;
    std::vector<LatentAotExternalCallbackRecordTable>
        external_callback_record_tables;

    [[nodiscard]] bool operator==(
        const LatentAotResolverContract&) const = default;
};

struct LatentAotStaticCandidateState final {
    LatentAotStaticCandidateKey key;
    LatentAotResolverContract resolver_contract;
    // This is the post-discovery, pre-finalization IR.  Keeping the
    // unoptimized source-bound graph is essential: finalize_candidate_program
    // must rerun source validation and lowering for a changed resolver wave.
    std::vector<katana::ir::Function> program;
    std::vector<std::uint32_t> published_entry_offsets;
    std::vector<std::uint32_t> analysis_entry_offsets;
    std::vector<std::uint32_t> non_function_entry_offsets;
    std::vector<std::uint32_t> authoritative_tail_roots;
    std::vector<katana::ir::ExternalDispatchEntry>
        external_dispatch_entries;
    katana::analysis::DreamcastHardwareAudit hardware_audit;
    std::optional<std::uint32_t> preferred_runtime_base;
    bool preferred_runtime_base_identity_consistent = false;
    // A locally saturated callback-value inventory cannot become complete by
    // adding external resolver declarations. Retain only this exact terminal
    // negative class; every other rejection remains cold-analyzed.
    bool terminal_inventory_candidate_values = false;
    std::string terminal_rejection_detail;
    // Accounted retained allocation estimate used only by the bounded
    // in-process session cache.  It is deliberately stored with the entry so
    // replacement/complete-entry removal can update the aggregate without
    // walking another thread's graph.
    std::size_t retained_bytes = 0u;
    // Exact raw artifact observed while importing this state.  A changed
    // resolver wave may replace it only through CodegenCache's bounded
    // compare-and-replace operation; authority-gated runs retain it until the
    // outer publish transaction commits.
    std::string persistent_cache_key;
    std::string persistent_observed_payload;
    bool imported_from_persistent_cache = false;
};

using LatentAotStaticCandidateCache =
    std::vector<std::shared_ptr<LatentAotStaticCandidateState>>;

class StaticCandidateCodecError final : public std::runtime_error {
  public:
    StaticCandidateCodecError() : std::runtime_error(
        "invalid latent AOT module-static cache") {}
};

class StaticCandidateWriter final {
  public:
    explicit StaticCandidateWriter(const std::size_t limit) : limit_(limit) {}
    void u8(const std::uint8_t value) { append(value); }
    void u16(const std::uint16_t value) { append(value); }
    void u32(const std::uint32_t value) { append(value); }
    void u64(const std::uint64_t value) { append(value); }
    void boolean(const bool value) { u8(value ? 1u : 0u); }
    template <typename Enum> void enumeration(const Enum value) {
        using U = std::underlying_type_t<Enum>;
        static_assert(sizeof(U) <= sizeof(std::uint32_t));
        u32(static_cast<std::uint32_t>(static_cast<U>(value)));
    }
    void raw(const std::span<const std::uint8_t> value) {
        require(value.size());
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void text(const std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            throw StaticCandidateCodecError{};
        u32(static_cast<std::uint32_t>(value.size()));
        raw({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    }
    void blob(const std::span<const std::uint8_t> value) {
        u64(value.size());
        raw(value);
    }
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }

  private:
    template <typename T> void append(const T value) {
        require(sizeof(T));
        using U = std::make_unsigned_t<T>;
        const auto bits = static_cast<U>(value);
        for (std::size_t index = 0u; index < sizeof(T); ++index)
            bytes_.push_back(static_cast<std::uint8_t>(bits >> (index * 8u)));
    }
    void require(const std::size_t count) const {
        if (count > limit_ || bytes_.size() > limit_ - count)
            throw StaticCandidateCodecError{};
    }
    std::size_t limit_;
    std::vector<std::uint8_t> bytes_;
};

class StaticCandidateReader final {
  public:
    explicit StaticCandidateReader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {}
    std::uint8_t u8() { return read<std::uint8_t>(); }
    std::uint16_t u16() { return read<std::uint16_t>(); }
    std::uint32_t u32() { return read<std::uint32_t>(); }
    std::uint64_t u64() { return read<std::uint64_t>(); }
    bool boolean() {
        const auto value = u8();
        if (value > 1u) throw StaticCandidateCodecError{};
        return value != 0u;
    }
    template <typename Enum> Enum enumeration(const Enum maximum) {
        const auto value = u32();
        if (value > static_cast<std::uint32_t>(maximum))
            throw StaticCandidateCodecError{};
        return static_cast<Enum>(value);
    }
    std::string text(const std::size_t maximum = 16u * 1024u * 1024u) {
        const auto count = static_cast<std::size_t>(u32());
        if (count > maximum) throw StaticCandidateCodecError{};
        const auto value = take(count);
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }
    std::span<const std::uint8_t> blob(const std::size_t maximum) {
        const auto count64 = u64();
        if (count64 > maximum ||
            count64 > std::numeric_limits<std::size_t>::max())
            throw StaticCandidateCodecError{};
        return take(static_cast<std::size_t>(count64));
    }
    std::size_t size(const std::size_t maximum) {
        const auto value = u64();
        if (value > maximum ||
            value > std::numeric_limits<std::size_t>::max())
            throw StaticCandidateCodecError{};
        return static_cast<std::size_t>(value);
    }
    std::span<const std::uint8_t> raw(const std::size_t count) {
        return take(count);
    }
    [[nodiscard]] bool empty() const noexcept { return cursor_ == bytes_.size(); }

  private:
    template <typename T> T read() {
        const auto value = take(sizeof(T));
        std::make_unsigned_t<T> bits = 0u;
        for (std::size_t index = 0u; index < sizeof(T); ++index)
            bits |= static_cast<std::make_unsigned_t<T>>(value[index]) <<
                    (index * 8u);
        return static_cast<T>(bits);
    }
    std::span<const std::uint8_t> take(const std::size_t count) {
        if (count > bytes_.size() || cursor_ > bytes_.size() - count)
            throw StaticCandidateCodecError{};
        const auto value = bytes_.subspan(cursor_, count);
        cursor_ += count;
        return value;
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_ = 0u;
};

template <typename T, typename Write>
void write_static_vector(StaticCandidateWriter& output,
                         const std::vector<T>& values,
                         Write&& write) {
    if (values.size() > std::numeric_limits<std::uint32_t>::max())
        throw StaticCandidateCodecError{};
    output.u32(static_cast<std::uint32_t>(values.size()));
    for (const auto& value : values) write(value);
}

template <typename T, typename Read>
std::vector<T> read_static_vector(StaticCandidateReader& input,
                                  const std::size_t maximum,
                                  Read&& read) {
    const auto count = static_cast<std::size_t>(input.u32());
    if (count > maximum) throw StaticCandidateCodecError{};
    std::vector<T> values;
    values.reserve(count);
    for (std::size_t index = 0u; index < count; ++index)
        values.push_back(read());
    return values;
}

void write_static_u32s(StaticCandidateWriter& output,
                       const std::vector<std::uint32_t>& values) {
    write_static_vector(output, values,
                        [&](const auto value) { output.u32(value); });
}

std::vector<std::uint32_t> read_static_u32s(
    StaticCandidateReader& input,
    const std::size_t maximum = maximum_prepared_latent_aot_block_identities) {
    return read_static_vector<std::uint32_t>(
        input, maximum, [&] { return input.u32(); });
}

void write_static_key(StaticCandidateWriter& output,
                      const LatentAotStaticCandidateKey& key) {
    output.text(key.byte_identity);
    output.u32(key.byte_size);
    output.u32(key.source_address);
    write_static_u32s(output, key.entry_offsets);
    write_static_u32s(output, key.explicit_entry_offsets);
    output.u8(static_cast<std::uint8_t>(key.module_class));
    write_static_vector(output, key.authority_entries, [&](const auto& entry) {
        output.u32(entry.module_relative_offset);
        output.u32(entry.byte_size);
        output.text(entry.byte_identity);
        output.u8(static_cast<std::uint8_t>(entry.kind));
    });
    output.boolean(key.exact_candidate);
    output.boolean(key.inferred_authoritative_entry_table);
    output.boolean(key.proven_runtime_base.has_value());
    if (key.proven_runtime_base) output.u32(*key.proven_runtime_base);
    output.u8(key.mode);
    output.u8(key.completeness_policy);
    output.u32(key.analyzer_abi);
    output.u64(key.maximum_entry_scan_instructions);
    output.u64(key.maximum_native_instructions);
    output.u64(key.maximum_blocks);
    output.u64(key.maximum_functions);
    output.u64(key.maximum_analysis_iterations);
    output.u64(key.maximum_analysis_contexts);
    output.text(key.analysis_implementation_identity);
    output.text(key.analysis_cache_implementation_identity);
    output.text(key.ir_product_implementation_identity);
    output.text(key.persistent_epoch_identity);
    write_static_vector(output, key.source_bindings, [&](const auto& binding) {
        output.text(binding.id);
        output.enumeration(binding.transform);
        output.text(binding.byte_identity);
        output.u64(binding.disc_byte_offset);
        output.u32(binding.byte_size);
    });
}

LatentAotStaticCandidateKey read_static_key(StaticCandidateReader& input) {
    LatentAotStaticCandidateKey key;
    key.byte_identity = input.text(96u);
    key.byte_size = input.u32();
    key.source_address = input.u32();
    key.entry_offsets = read_static_u32s(input);
    key.explicit_entry_offsets = read_static_u32s(input);
    const auto module_class = input.u8();
    if (module_class > static_cast<std::uint8_t>(
                          CompleteDisassemblyModuleClass::FixedRuntimeImage))
        throw StaticCandidateCodecError{};
    key.module_class = static_cast<CompleteDisassemblyModuleClass>(module_class);
    key.authority_entries = read_static_vector<CompleteDisassemblyEntryAuthority>(
        input, maximum_complete_disassembly_entries, [&] {
            CompleteDisassemblyEntryAuthority entry;
            entry.module_relative_offset = input.u32();
            entry.byte_size = input.u32();
            entry.byte_identity = input.text(96u);
            const auto kind = input.u8();
            if (kind > static_cast<std::uint8_t>(
                           CompleteDisassemblyEntryKind::CodePointerTarget))
                throw StaticCandidateCodecError{};
            entry.kind = static_cast<CompleteDisassemblyEntryKind>(kind);
            return entry;
        });
    key.exact_candidate = input.boolean();
    key.inferred_authoritative_entry_table = input.boolean();
    if (input.boolean()) key.proven_runtime_base = input.u32();
    key.mode = input.u8();
    key.completeness_policy = input.u8();
    key.analyzer_abi = input.u32();
    key.maximum_entry_scan_instructions = input.size(
        maximum_prepared_latent_aot_instructions_per_module);
    key.maximum_native_instructions = input.size(
        maximum_prepared_latent_aot_instructions_per_module);
    key.maximum_blocks = input.size(
        maximum_prepared_latent_aot_blocks_per_module);
    key.maximum_functions = input.size(
        maximum_prepared_latent_aot_functions_per_module);
    key.maximum_analysis_iterations = input.size(
        std::numeric_limits<std::size_t>::max());
    key.maximum_analysis_contexts = input.size(
        std::numeric_limits<std::size_t>::max());
    key.analysis_implementation_identity = input.text(4096u);
    key.analysis_cache_implementation_identity = input.text(4096u);
    key.ir_product_implementation_identity = input.text(4096u);
    key.persistent_epoch_identity = input.text(96u);
    key.source_bindings = read_static_vector<PreparedLatentAotSourceBinding>(
        input, maximum_prepared_latent_aot_source_bindings, [&] {
            PreparedLatentAotSourceBinding binding;
            binding.id = input.text(4096u);
            binding.transform = input.enumeration(LatentAotSourceTransform::SegaPrs);
            binding.byte_identity = input.text(96u);
            binding.disc_byte_offset = input.u64();
            binding.byte_size = input.u32();
            return binding;
        });
    return key;
}

void write_static_resolver_contract(
    StaticCandidateWriter& output,
    const LatentAotResolverContract& contract) {
    write_static_u32s(output, contract.external_code_targets);
    write_static_u32s(output, contract.external_data_targets);
    write_static_vector(output, contract.external_callback_sinks,
                        [&](const auto& sink) {
                            output.u32(sink.function_address);
                            output.u8(sink.argument_mask);
                            output.u8(sink.record_argument_mask);
                        });
    write_static_vector(output, contract.external_persistent_pointer_sinks,
                        [&](const auto& sink) {
                            output.u32(sink.function_address);
                            output.u8(sink.argument_mask);
                        });
    write_static_vector(output, contract.external_callback_field_sinks,
                        [&](const auto& sink) {
                            output.u32(sink.function_address);
                            output.u32(sink.call_instruction_address);
                            output.u32(sink.load_instruction_address);
                            output.u32(std::bit_cast<std::uint32_t>(sink.displacement));
                            output.u8(sink.width);
                            output.boolean(sink.call);
                            output.u8(sink.receiver_argument_mask);
                        });
    write_static_vector(output, contract.external_callback_record_tables,
                        [&](const auto& table) {
                            output.u32(table.function_address);
                            output.u32(table.call_instruction_address);
                            output.u32(table.callback_load_instruction_address);
                            output.u32(table.callback_sink_address);
                            output.u32(std::bit_cast<std::uint32_t>(
                                table.header_table_pointer_displacement));
                            output.u32(table.record_stride);
                            output.u32(std::bit_cast<std::uint32_t>(
                                table.callback_displacement));
                            output.u8(table.callback_argument);
                            output.u8(table.width);
                        });
}

LatentAotResolverContract read_static_resolver_contract(
    StaticCandidateReader& input) {
    LatentAotResolverContract contract;
    contract.external_code_targets = read_static_u32s(input);
    contract.external_data_targets = read_static_u32s(input);
    contract.external_callback_sinks =
        read_static_vector<LatentAotExternalCallbackSink>(
            input, maximum_prepared_latent_aot_entry_hints, [&] {
                LatentAotExternalCallbackSink sink;
                sink.function_address = input.u32();
                sink.argument_mask = input.u8();
                sink.record_argument_mask = input.u8();
                return sink;
            });
    contract.external_persistent_pointer_sinks =
        read_static_vector<LatentAotExternalPersistentPointerSink>(
            input, maximum_prepared_latent_aot_entry_hints, [&] {
                LatentAotExternalPersistentPointerSink sink;
                sink.function_address = input.u32();
                sink.argument_mask = input.u8();
                return sink;
            });
    contract.external_callback_field_sinks =
        read_static_vector<LatentAotExternalCallbackFieldSink>(
            input, maximum_prepared_latent_aot_entry_hints, [&] {
                LatentAotExternalCallbackFieldSink sink;
                sink.function_address = input.u32();
                sink.call_instruction_address = input.u32();
                sink.load_instruction_address = input.u32();
                sink.displacement = std::bit_cast<std::int32_t>(input.u32());
                sink.width = input.u8();
                sink.call = input.boolean();
                sink.receiver_argument_mask = input.u8();
                return sink;
            });
    contract.external_callback_record_tables =
        read_static_vector<LatentAotExternalCallbackRecordTable>(
            input, maximum_prepared_latent_aot_entry_hints, [&] {
                LatentAotExternalCallbackRecordTable table;
                table.function_address = input.u32();
                table.call_instruction_address = input.u32();
                table.callback_load_instruction_address = input.u32();
                table.callback_sink_address = input.u32();
                table.header_table_pointer_displacement =
                    std::bit_cast<std::int32_t>(input.u32());
                table.record_stride = input.u32();
                table.callback_displacement =
                    std::bit_cast<std::int32_t>(input.u32());
                table.callback_argument = input.u8();
                table.width = input.u8();
                return table;
            });
    return contract;
}

void write_static_hardware_loop(
    StaticCandidateWriter& output,
    const katana::analysis::HardwareNaturalLoop& loop) {
    output.u32(loop.header_address);
    output.u32(loop.latch_address);
    output.u32(loop.backedge_instruction_address);
    output.enumeration(loop.classification);
    output.boolean(loop.unresolved_guard_access);
    write_static_u32s(output, loop.unresolved_guard_read_instruction_addresses);
    write_static_u32s(output, loop.block_addresses);
    write_static_u32s(output, loop.counter_instruction_addresses);
    write_static_vector(output, loop.local_progress_evidence, [&](const auto& value) {
        output.u32(value.condition_instruction_address);
        output.u32(value.progress_instruction_address);
        output.u8(value.register_index);
        output.enumeration(value.kind);
    });
    write_static_vector(output, loop.accesses, [&](const auto& value) {
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
    write_static_vector(output, loop.matching_write_candidates, [&](const auto& value) {
        output.u32(value.instruction_address);
        output.u32(value.guest_address);
        output.u32(value.canonical_address);
        output.u8(value.width);
    });
    output.boolean(loop.matching_write_candidates_truncated);
}

katana::analysis::HardwareNaturalLoop read_static_hardware_loop(
    StaticCandidateReader& input) {
    using namespace katana::analysis;
    HardwareNaturalLoop loop;
    loop.header_address = input.u32();
    loop.latch_address = input.u32();
    loop.backedge_instruction_address = input.u32();
    loop.classification = input.enumeration(HardwareLoopClassification::Unknown);
    loop.unresolved_guard_access = input.boolean();
    loop.unresolved_guard_read_instruction_addresses = read_static_u32s(input);
    loop.block_addresses = read_static_u32s(input);
    loop.counter_instruction_addresses = read_static_u32s(input);
    loop.local_progress_evidence =
        read_static_vector<HardwareLoopLocalProgressEvidence>(
            input, maximum_prepared_latent_aot_instructions_per_module, [&] {
                HardwareLoopLocalProgressEvidence value;
                value.condition_instruction_address = input.u32();
                value.progress_instruction_address = input.u32();
                value.register_index = input.u8();
                value.kind = input.enumeration(
                    HardwareLoopLocalProgressKind::PointerTraversal);
                return value;
            });
    loop.accesses = read_static_vector<HardwareLoopAccessEvidence>(
        input, maximum_prepared_latent_aot_instructions_per_module, [&] {
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
    loop.matching_write_candidates =
        read_static_vector<HardwareLoopWriteCandidate>(
            input, maximum_prepared_latent_aot_instructions_per_module, [&] {
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

void write_static_hardware_audit(
    StaticCandidateWriter& output,
    const katana::analysis::DreamcastHardwareAudit& audit) {
    output.text(audit.scope);
    output.u64(audit.image_bytes);
    output.u64(audit.reachable_instructions);
    output.u64(audit.reachable_functions);
    output.u64(audit.unknown_instructions);
    output.u64(audit.memory_access_sites);
    output.u64(audit.resolved_memory_access_sites);
    output.u64(audit.unresolved_memory_access_sites);
    write_static_vector(output, audit.unresolved_memory_instruction_sites,
                        [&](const auto& value) {
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
    write_static_vector(output, audit.instruction_diagnostics,
                        [&](const auto& value) {
                            output.u32(value.address);
                            output.u16(value.opcode);
                            output.text(value.reason);
                            output.enumeration(value.evidence);
                            write_static_u32s(output, value.incoming_addresses);
                            write_static_u32s(output, value.delay_slot_owners);
                        });
    write_static_vector(output, audit.references, [&](const auto& value) {
        output.u32(value.instruction_address);
        output.u32(value.guest_address);
        output.u32(value.canonical_address);
        output.boolean(value.canonical_address_known);
        output.text(value.address_expression);
        output.enumeration(value.region);
        output.enumeration(value.kind);
        output.u8(value.width);
        output.boolean(value.aperture_mapped);
        output.enumeration(value.runtime_support);
        output.text(value.support_reason);
        output.text(value.register_name);
    });
    write_static_vector(output, audit.addresses, [&](const auto& value) {
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
        write_static_vector(output, value.widths,
                            [&](const auto width) { output.u8(width); });
        write_static_u32s(output, value.instruction_addresses);
    });
    write_static_vector(output, audit.loops,
                        [&](const auto& loop) {
                            write_static_hardware_loop(output, loop);
                        });
}

katana::analysis::DreamcastHardwareAudit read_static_hardware_audit(
    StaticCandidateReader& input) {
    using namespace katana::analysis;
    DreamcastHardwareAudit audit;
    audit.scope = input.text(4096u);
    audit.image_bytes = input.u64();
    audit.reachable_instructions = input.u64();
    audit.reachable_functions = input.u64();
    audit.unknown_instructions = input.u64();
    audit.memory_access_sites = input.u64();
    audit.resolved_memory_access_sites = input.u64();
    audit.unresolved_memory_access_sites = input.u64();
    audit.unresolved_memory_instruction_sites =
        read_static_vector<UnresolvedMemoryInstructionSite>(
            input, maximum_prepared_latent_aot_instructions_per_module, [&] {
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
    audit.instruction_diagnostics =
        read_static_vector<HardwareInstructionDiagnostic>(
            input, maximum_prepared_latent_aot_instructions_per_module, [&] {
                HardwareInstructionDiagnostic value;
                value.address = input.u32();
                value.opcode = input.u16();
                value.reason = input.text(4096u);
                value.evidence = input.enumeration(ControlFlowEvidence::Unresolved);
                value.incoming_addresses = read_static_u32s(input);
                value.delay_slot_owners = read_static_u32s(input);
                return value;
            });
    audit.references = read_static_vector<HardwareAccessReference>(
        input, maximum_prepared_latent_aot_instructions_per_module, [&] {
            HardwareAccessReference value;
            value.instruction_address = input.u32();
            value.guest_address = input.u32();
            value.canonical_address = input.u32();
            value.canonical_address_known = input.boolean();
            value.address_expression = input.text(4096u);
            value.region = input.enumeration(DreamcastHardwareRegion::Unknown);
            value.kind = input.enumeration(HardwareAccessKind::Prefetch);
            value.width = input.u8();
            value.aperture_mapped = input.boolean();
            value.runtime_support = input.enumeration(HardwareRuntimeSupport::Unmapped);
            value.support_reason = input.text(4096u);
            value.register_name = input.text(4096u);
            return value;
        });
    audit.addresses = read_static_vector<HardwareAddressSummary>(
        input, maximum_prepared_latent_aot_instructions_per_module, [&] {
            HardwareAddressSummary value;
            value.guest_address = input.u32();
            value.canonical_address = input.u32();
            value.region = input.enumeration(DreamcastHardwareRegion::Unknown);
            value.aperture_mapped = input.boolean();
            value.runtime_support = input.enumeration(HardwareRuntimeSupport::Unmapped);
            value.support_reason = input.text(4096u);
            value.register_name = input.text(4096u);
            value.reads = input.u64();
            value.writes = input.u64();
            value.prefetches = input.u64();
            value.widths = read_static_vector<std::uint8_t>(
                input, 32u, [&] { return input.u8(); });
            value.instruction_addresses = read_static_u32s(input);
            return value;
        });
    audit.loops = read_static_vector<HardwareNaturalLoop>(
        input, maximum_prepared_latent_aot_blocks_per_module,
        [&] { return read_static_hardware_loop(input); });
    if (audit.unresolved_poll_guard_loops !=
        katana::analysis::count_unresolved_poll_guard_loops(audit.loops))
        throw StaticCandidateCodecError{};
    return audit;
}

constexpr IrProgramCacheLimits module_static_ir_limits{
    48u * 1024u * 1024u,
    maximum_prepared_latent_aot_functions_per_module,
    maximum_prepared_latent_aot_blocks_per_module,
    maximum_prepared_latent_aot_instructions_per_module,
    maximum_latent_aot_analysis_cache_successors,
    maximum_latent_aot_analysis_cache_targets,
    maximum_latent_aot_analysis_cache_callsites,
    maximum_latent_aot_analysis_cache_parser_depth,
    maximum_latent_aot_session_static_cache_bytes};

std::string static_candidate_cache_key(
    const LatentAotStaticCandidateKey& key) {
    StaticCandidateWriter material(4u * 1024u * 1024u);
    material.raw(latent_aot_module_static_cache_magic);
    material.u32(latent_aot_module_static_cache_schema_version);
    write_static_key(material, key);
    return katana::io::sha256_bytes(std::string_view(
        reinterpret_cast<const char*>(material.bytes().data()),
        material.bytes().size()));
}

bool sorted_unique_u32(const std::vector<std::uint32_t>& values) noexcept {
    return std::adjacent_find(values.begin(), values.end(),
                              std::greater_equal<>{}) == values.end();
}

template <typename T, typename Less>
bool sorted_unique_static_values(const std::vector<T>& values,
                                 Less less) noexcept {
    if (!std::is_sorted(values.begin(), values.end(), less)) return false;
    return std::adjacent_find(
               values.begin(), values.end(), [&](const auto& left,
                                                   const auto& right) {
                   return !less(left, right) && !less(right, left);
               }) == values.end();
}

bool canonical_static_resolver_contract(
    const LatentAotResolverContract& contract) noexcept {
    const auto sink_less = [](const auto& left, const auto& right) {
        return left.function_address < right.function_address;
    };
    const auto field_less = [](const auto& left, const auto& right) {
        return std::tie(left.function_address,
                        left.call_instruction_address,
                        left.load_instruction_address,
                        left.displacement,
                        left.width,
                        left.call) <
               std::tie(right.function_address,
                        right.call_instruction_address,
                        right.load_instruction_address,
                        right.displacement,
                        right.width,
                        right.call);
    };
    const auto table_less = [](const auto& left, const auto& right) {
        return std::tie(left.function_address,
                        left.call_instruction_address,
                        left.callback_load_instruction_address,
                        left.callback_sink_address,
                        left.header_table_pointer_displacement,
                        left.record_stride,
                        left.callback_displacement,
                        left.callback_argument,
                        left.width) <
               std::tie(right.function_address,
                        right.call_instruction_address,
                        right.callback_load_instruction_address,
                        right.callback_sink_address,
                        right.header_table_pointer_displacement,
                        right.record_stride,
                        right.callback_displacement,
                        right.callback_argument,
                        right.width);
    };
    return sorted_unique_u32(contract.external_code_targets) &&
           sorted_unique_u32(contract.external_data_targets) &&
           sorted_unique_static_values(
               contract.external_callback_sinks, sink_less) &&
           sorted_unique_static_values(
               contract.external_persistent_pointer_sinks, sink_less) &&
           sorted_unique_static_values(
               contract.external_callback_field_sinks, field_less) &&
           sorted_unique_static_values(
               contract.external_callback_record_tables, table_less);
}

bool canonical_static_candidate_state(
    const LatentAotStaticCandidateState& state) {
    if (state.program.empty() || state.terminal_inventory_candidate_values ||
        state.key.byte_size == 0u ||
        state.published_entry_offsets.empty() ||
        !sorted_unique_u32(state.key.entry_offsets) ||
        !sorted_unique_u32(state.key.explicit_entry_offsets) ||
        !sorted_unique_u32(state.published_entry_offsets) ||
        !sorted_unique_u32(state.analysis_entry_offsets) ||
        !sorted_unique_u32(state.non_function_entry_offsets) ||
        !sorted_unique_u32(state.authoritative_tail_roots) ||
        !canonical_static_resolver_contract(state.resolver_contract) ||
        !std::includes(state.published_entry_offsets.begin(),
                       state.published_entry_offsets.end(),
                       state.key.entry_offsets.begin(),
                       state.key.entry_offsets.end()))
        return false;
    if (!std::is_sorted(state.key.source_bindings.begin(),
                        state.key.source_bindings.end(), source_binding_less) ||
        std::adjacent_find(state.key.source_bindings.begin(),
                           state.key.source_bindings.end()) !=
            state.key.source_bindings.end())
        return false;
    std::set<std::uint32_t> functions;
    std::set<std::uint32_t> blocks;
    for (const auto& function : state.program) {
        if (!functions.insert(function.entry_address).second) return false;
        for (const auto& block : function.blocks)
            if (!blocks.insert(block.start_address).second) return false;
    }
    for (const auto offset : state.analysis_entry_offsets)
        if (!functions.contains(state.key.source_address + offset)) return false;
    for (const auto offset : state.published_entry_offsets)
        if (!blocks.contains(state.key.source_address + offset)) return false;
    if (!std::is_sorted(
            state.external_dispatch_entries.begin(),
            state.external_dispatch_entries.end(),
            [](const auto& left, const auto& right) {
                return std::tie(left.address, left.kind) <
                       std::tie(right.address, right.kind);
            }) ||
        std::adjacent_find(
            state.external_dispatch_entries.begin(),
            state.external_dispatch_entries.end(),
            [](const auto& left, const auto& right) {
                return left.address == right.address && left.kind == right.kind;
            }) != state.external_dispatch_entries.end())
        return false;
    for (const auto offset : state.published_entry_offsets) {
        const auto address = state.key.source_address + offset;
        if (std::none_of(state.external_dispatch_entries.begin(),
                         state.external_dispatch_entries.end(),
                         [&](const auto& entry) {
                             return entry.address == address &&
                                    entry.kind == katana::ir::
                                        ExternalDispatchEntryKind::BlockEntry;
                         }))
            return false;
    }
    return true;
}

std::vector<std::uint8_t> serialize_static_candidate_state(
    const LatentAotStaticCandidateState& state) {
    if (!canonical_static_candidate_state(state))
        throw StaticCandidateCodecError{};
    StaticCandidateWriter body(
        maximum_latent_aot_module_static_cache_artifact_bytes);
    write_static_key(body, state.key);
    write_static_resolver_contract(body, state.resolver_contract);
    const auto ir_payload = serialize_ir_program_cache_payload(
        state.program, module_static_ir_limits);
    body.blob(ir_payload);
    write_static_u32s(body, state.published_entry_offsets);
    write_static_u32s(body, state.analysis_entry_offsets);
    write_static_u32s(body, state.non_function_entry_offsets);
    write_static_u32s(body, state.authoritative_tail_roots);
    write_static_vector(body, state.external_dispatch_entries,
                        [&](const auto& entry) {
                            body.u32(entry.address);
                            body.enumeration(entry.kind);
                        });
    write_static_hardware_audit(body, state.hardware_audit);
    body.boolean(state.preferred_runtime_base.has_value());
    if (state.preferred_runtime_base)
        body.u32(*state.preferred_runtime_base);
    body.boolean(state.preferred_runtime_base_identity_consistent);

    const auto key = static_candidate_cache_key(state.key);
    const auto body_view = std::string_view(
        reinterpret_cast<const char*>(body.bytes().data()), body.bytes().size());
    StaticCandidateWriter artifact(
        maximum_latent_aot_module_static_cache_artifact_bytes);
    artifact.raw(latent_aot_module_static_cache_magic);
    artifact.u32(latent_aot_module_static_cache_schema_version);
    artifact.text(key);
    artifact.text(katana::io::sha256_bytes(body_view));
    artifact.blob(body.bytes());
    return std::move(artifact).finish();
}

struct ParsedStaticCandidateState final {
    std::shared_ptr<LatentAotStaticCandidateState> state;
    bool corrupt = false;
};

ParsedStaticCandidateState parse_static_candidate_state(
    const std::string_view expected_key,
    const std::string_view artifact) {
    if (artifact.empty() || artifact.size() >
                                maximum_latent_aot_module_static_cache_artifact_bytes)
        return {nullptr, true};
    try {
        StaticCandidateReader envelope({
            reinterpret_cast<const std::uint8_t*>(artifact.data()),
            artifact.size()});
        const auto magic = envelope.raw(latent_aot_module_static_cache_magic.size());
        if (magic.size() != latent_aot_module_static_cache_magic.size() ||
            !std::equal(magic.begin(), magic.end(),
                        latent_aot_module_static_cache_magic.begin()))
            return {nullptr, false};
        if (envelope.u32() != latent_aot_module_static_cache_schema_version)
            return {nullptr, false};
        const auto stored_key = envelope.text(64u);
        if (stored_key != expected_key) return {nullptr, false};
        const auto stored_sha = envelope.text(64u);
        const auto body = envelope.blob(
            maximum_latent_aot_module_static_cache_artifact_bytes);
        if (!envelope.empty() ||
            stored_sha != katana::io::sha256_bytes(std::string_view(
                reinterpret_cast<const char*>(body.data()), body.size())))
            throw StaticCandidateCodecError{};
        StaticCandidateReader input(body);
        auto state = std::make_shared<LatentAotStaticCandidateState>();
        state->key = read_static_key(input);
        if (static_candidate_cache_key(state->key) != expected_key)
            throw StaticCandidateCodecError{};
        state->resolver_contract = read_static_resolver_contract(input);
        state->program = parse_ir_program_cache_payload(
            input.blob(module_static_ir_limits.maximum_payload_bytes),
            module_static_ir_limits);
        state->published_entry_offsets = read_static_u32s(input);
        state->analysis_entry_offsets = read_static_u32s(input);
        state->non_function_entry_offsets = read_static_u32s(input);
        state->authoritative_tail_roots = read_static_u32s(input);
        state->external_dispatch_entries =
            read_static_vector<katana::ir::ExternalDispatchEntry>(
                input, maximum_prepared_latent_aot_block_identities, [&] {
                    katana::ir::ExternalDispatchEntry entry;
                    entry.address = input.u32();
                    entry.kind = input.enumeration(
                        katana::ir::ExternalDispatchEntryKind::
                            InstructionContinuation);
                    return entry;
                });
        state->hardware_audit = read_static_hardware_audit(input);
        if (input.boolean()) state->preferred_runtime_base = input.u32();
        state->preferred_runtime_base_identity_consistent = input.boolean();
        if (!input.empty() || !canonical_static_candidate_state(*state))
            throw StaticCandidateCodecError{};
        const auto canonical = serialize_static_candidate_state(*state);
        if (canonical.size() != artifact.size() ||
            !std::equal(canonical.begin(), canonical.end(),
                        reinterpret_cast<const std::uint8_t*>(artifact.data())))
            throw StaticCandidateCodecError{};
        return {std::move(state), false};
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        return {nullptr, true};
    }
}

LatentAotResolverContract resolver_contract_from_options(
    const LatentAotDiscoveryOptions& options) {
    return {
        std::vector<std::uint32_t>(options.external_code_targets.begin(),
                                   options.external_code_targets.end()),
        std::vector<std::uint32_t>(options.external_data_targets.begin(),
                                   options.external_data_targets.end()),
        std::vector<LatentAotExternalCallbackSink>(
            options.external_callback_sinks.begin(),
            options.external_callback_sinks.end()),
        std::vector<LatentAotExternalPersistentPointerSink>(
            options.external_persistent_pointer_sinks.begin(),
            options.external_persistent_pointer_sinks.end()),
        std::vector<LatentAotExternalCallbackFieldSink>(
            options.external_callback_field_sinks.begin(),
            options.external_callback_field_sinks.end()),
        std::vector<LatentAotExternalCallbackRecordTable>(
            options.external_callback_record_tables.begin(),
            options.external_callback_record_tables.end())};
}

template <typename T, typename Less>
bool sorted_subset(const std::vector<T>& subset,
                   const std::vector<T>& superset,
                   Less less) {
    return std::includes(superset.begin(), superset.end(), subset.begin(),
                         subset.end(), less);
}

template <typename T>
bool callback_sink_mask_subset(const std::vector<T>& subset,
                               const std::vector<T>& superset) {
    for (const auto& required : subset) {
        const auto found = std::lower_bound(
            superset.begin(), superset.end(), required.function_address,
            [](const auto& sink, const std::uint32_t address) {
                return sink.function_address < address;
            });
        if (found == superset.end() ||
            found->function_address != required.function_address ||
            (required.argument_mask & found->argument_mask) !=
                required.argument_mask)
            return false;
        if constexpr (requires { required.record_argument_mask; }) {
            if ((required.record_argument_mask &
                 found->record_argument_mask) !=
                required.record_argument_mask)
                return false;
        }
    }
    return true;
}

bool callback_field_sink_mask_subset(
    const std::vector<LatentAotExternalCallbackFieldSink>& subset,
    const std::vector<LatentAotExternalCallbackFieldSink>& superset) {
    const auto less = [](const auto& left, const auto& right) {
        return std::tie(left.function_address,
                        left.call_instruction_address,
                        left.load_instruction_address,
                        left.displacement,
                        left.width,
                        left.call) <
               std::tie(right.function_address,
                        right.call_instruction_address,
                        right.load_instruction_address,
                        right.displacement,
                        right.width,
                        right.call);
    };
    for (const auto& required : subset) {
        const auto found = std::lower_bound(
            superset.begin(), superset.end(), required, less);
        if (found == superset.end() || less(required, *found) ||
            less(*found, required) ||
            (required.receiver_argument_mask &
             found->receiver_argument_mask) !=
                required.receiver_argument_mask)
            return false;
    }
    return true;
}

bool resolver_contract_is_monotonic_superset(
    const LatentAotResolverContract& previous,
    const LatentAotResolverContract& current) {
    const auto scalar_less = [](const auto left, const auto right) {
        return left < right;
    };
    const auto table_less = [](const auto& left, const auto& right) {
        return std::tie(left.function_address,
                        left.call_instruction_address,
                        left.callback_load_instruction_address,
                        left.callback_sink_address,
                        left.header_table_pointer_displacement,
                        left.record_stride,
                        left.callback_displacement,
                        left.callback_argument,
                        left.width) <
               std::tie(right.function_address,
                        right.call_instruction_address,
                        right.callback_load_instruction_address,
                        right.callback_sink_address,
                        right.header_table_pointer_displacement,
                        right.record_stride,
                        right.callback_displacement,
                        right.callback_argument,
                        right.width);
    };
    return sorted_subset(previous.external_code_targets,
                         current.external_code_targets, scalar_less) &&
           sorted_subset(previous.external_data_targets,
                         current.external_data_targets, scalar_less) &&
           callback_sink_mask_subset(previous.external_callback_sinks,
                                      current.external_callback_sinks) &&
           callback_sink_mask_subset(
               previous.external_persistent_pointer_sinks,
               current.external_persistent_pointer_sinks) &&
           callback_field_sink_mask_subset(
               previous.external_callback_field_sinks,
               current.external_callback_field_sinks) &&
           sorted_subset(previous.external_callback_record_tables,
                         current.external_callback_record_tables,
                         table_less);
}

LatentAotStaticCandidateKey make_static_candidate_key(
    const DiscFileCandidate& candidate,
    const LatentAotDiscoveryOptions& options,
    const std::string_view persistent_epoch_identity) {
    LatentAotStaticCandidateKey key;
    key.byte_identity = candidate.byte_identity;
    key.byte_size = candidate.size;
    key.source_address = candidate.source_address;
    key.entry_offsets = candidate.entry_offsets;
    key.explicit_entry_offsets = candidate.explicit_entry_offsets;
    key.authority_entries = candidate.authority_entries;
    key.module_class = candidate.module_class;
    key.exact_candidate = candidate_has_authoritative_entries(candidate);
    key.inferred_authoritative_entry_table =
        candidate.inferred_authoritative_entry_table;
    key.proven_runtime_base = candidate.proven_runtime_base;
    key.mode = static_cast<std::uint8_t>(options.mode);
    key.completeness_policy =
        static_cast<std::uint8_t>(options.completeness_policy);
    key.analyzer_abi = katana::analysis::abi_version;
    key.maximum_entry_scan_instructions =
        options.maximum_entry_scan_instructions;
    key.maximum_native_instructions =
        options.maximum_native_instructions_per_module;
    key.maximum_blocks = options.maximum_blocks_per_module;
    key.maximum_functions = options.maximum_functions_per_module;
    key.maximum_analysis_iterations = options.maximum_analysis_iterations;
    key.maximum_analysis_contexts = options.maximum_analysis_contexts;
    key.analysis_implementation_identity =
        options.analysis_implementation_identity;
    key.analysis_cache_implementation_identity =
        options.analysis_cache_implementation_identity;
    key.ir_product_implementation_identity =
        options.ir_product_implementation_identity;
    key.persistent_epoch_identity = std::string(persistent_epoch_identity);
    key.source_bindings = candidate.source_bindings;
    std::sort(key.entry_offsets.begin(), key.entry_offsets.end());
    key.entry_offsets.erase(
        std::unique(key.entry_offsets.begin(), key.entry_offsets.end()),
        key.entry_offsets.end());
    std::sort(key.explicit_entry_offsets.begin(),
              key.explicit_entry_offsets.end());
    key.explicit_entry_offsets.erase(
        std::unique(key.explicit_entry_offsets.begin(),
                    key.explicit_entry_offsets.end()),
        key.explicit_entry_offsets.end());
    std::sort(key.authority_entries.begin(), key.authority_entries.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.module_relative_offset, left.byte_size,
                                  left.byte_identity, left.kind) <
                         std::tie(right.module_relative_offset, right.byte_size,
                                  right.byte_identity, right.kind);
              });
    std::sort(key.source_bindings.begin(), key.source_bindings.end(),
              source_binding_less);
    return key;
}

std::size_t estimate_static_candidate_state_bytes(
    const LatentAotStaticCandidateState& state) noexcept {
    const auto maximum = std::numeric_limits<std::size_t>::max();
    std::size_t bytes = sizeof(LatentAotStaticCandidateState);
    const auto add = [&](const std::size_t value) {
        if (value > maximum - bytes)
            bytes = maximum;
        else
            bytes += value;
    };
    const auto add_count = [&](const std::size_t count,
                               const std::size_t element_size) {
        if (count == 0u || bytes == maximum) return;
        if (element_size > maximum / count) {
            bytes = maximum;
            return;
        }
        add(count * element_size);
    };
    const auto add_string = [&](const std::string& value) {
        add(value.capacity());
    };

    add_string(state.key.byte_identity);
    add_string(state.key.analysis_implementation_identity);
    add_string(state.key.analysis_cache_implementation_identity);
    add_string(state.key.ir_product_implementation_identity);
    add_string(state.key.persistent_epoch_identity);
    add_string(state.terminal_rejection_detail);
    add_string(state.persistent_cache_key);
    add_string(state.persistent_observed_payload);
    add_count(state.key.entry_offsets.capacity(), sizeof(std::uint32_t));
    add_count(state.key.explicit_entry_offsets.capacity(),
              sizeof(std::uint32_t));
    add_count(state.key.authority_entries.capacity(),
              sizeof(CompleteDisassemblyEntryAuthority));
    for (const auto& entry : state.key.authority_entries)
        add_string(entry.byte_identity);
    add_count(state.key.source_bindings.capacity(),
              sizeof(PreparedLatentAotSourceBinding));
    for (const auto& binding : state.key.source_bindings) {
        add_string(binding.id);
        add_string(binding.byte_identity);
    }

    add_count(state.resolver_contract.external_code_targets.capacity(),
              sizeof(std::uint32_t));
    add_count(state.resolver_contract.external_data_targets.capacity(),
              sizeof(std::uint32_t));
    add_count(state.resolver_contract.external_callback_sinks.capacity(),
              sizeof(LatentAotExternalCallbackSink));
    add_count(
        state.resolver_contract.external_persistent_pointer_sinks.capacity(),
        sizeof(LatentAotExternalPersistentPointerSink));
    add_count(state.resolver_contract.external_callback_field_sinks.capacity(),
              sizeof(LatentAotExternalCallbackFieldSink));
    add_count(
        state.resolver_contract.external_callback_record_tables.capacity(),
        sizeof(LatentAotExternalCallbackRecordTable));

    add_count(state.program.capacity(), sizeof(katana::ir::Function));
    for (const auto& function : state.program) {
        add_count(function.blocks.capacity(), sizeof(katana::ir::BasicBlock));
        add_count(function.direct_callees.capacity(), sizeof(std::uint32_t));
        add_count(function.indirect_call_sites.capacity(),
                  sizeof(std::uint32_t));
        for (const auto& block : function.blocks) {
            add_count(block.instructions.capacity(),
                      sizeof(katana::ir::Instruction));
            add_count(block.successors.capacity(), sizeof(std::uint32_t));
            add_count(block.guarded_case_ownership_targets.capacity(),
                      sizeof(std::uint32_t));
            for (const auto& instruction : block.instructions)
                add_count(instruction.resolved_targets.capacity(),
                          sizeof(std::uint32_t));
        }
    }

    add_count(state.published_entry_offsets.capacity(),
              sizeof(std::uint32_t));
    add_count(state.analysis_entry_offsets.capacity(), sizeof(std::uint32_t));
    add_count(state.non_function_entry_offsets.capacity(),
              sizeof(std::uint32_t));
    add_count(state.authoritative_tail_roots.capacity(),
              sizeof(std::uint32_t));
    add_count(state.external_dispatch_entries.capacity(),
              sizeof(katana::ir::ExternalDispatchEntry));

    // Hardware audit records contain several nested vectors and diagnostic
    // strings.  Charge a conservative per-record envelope in addition to the
    // outer capacities; an underestimate here would make the retained-byte
    // gate meaningless, while an overestimate merely declines an optimization.
    add_string(state.hardware_audit.scope);
    add_count(state.hardware_audit.unresolved_memory_instruction_sites.capacity(),
              64u);
    add_count(state.hardware_audit.instruction_diagnostics.capacity(),
              4096u);
    add_count(state.hardware_audit.references.capacity(), 2048u);
    add_count(state.hardware_audit.addresses.capacity(), 2048u);
    add_count(state.hardware_audit.loops.capacity(), 16384u);
    return bytes;
}

std::shared_ptr<LatentAotStaticCandidateState>
find_static_candidate_state(
    LatentAotStaticCandidateCache& cache,
    std::mutex& cache_mutex,
    const LatentAotStaticCandidateKey& key) {
    std::scoped_lock lock(cache_mutex);
    const auto found = std::find_if(
        cache.begin(), cache.end(), [&](const auto& state) {
            return state != nullptr && state->key == key;
        });
    return found == cache.end() ? nullptr : *found;
}

void store_static_candidate_state(
    LatentAotStaticCandidateCache& cache,
    std::mutex& cache_mutex,
    std::shared_ptr<LatentAotStaticCandidateState> state,
    std::size_t& retained_bytes,
    std::atomic_size_t* const budget_skips) {
    if (state == nullptr) return;
    const auto state_bytes = estimate_static_candidate_state_bytes(*state);
    std::scoped_lock lock(cache_mutex);
    const auto found = std::find_if(
        cache.begin(), cache.end(), [&](const auto& existing) {
            return existing != nullptr && existing->key == state->key;
        });
    const auto reject_for_budget = [&]() {
        if (budget_skips != nullptr)
            budget_skips->fetch_add(1u, std::memory_order_relaxed);
    };
    const std::size_t replaced_bytes =
        found == cache.end() ? 0u : (*found)->retained_bytes;
    const bool entry_available =
        found != cache.end() ||
        cache.size() < maximum_latent_aot_session_static_cache_entries;
    const bool retained_invariant = retained_bytes <=
                                    maximum_latent_aot_session_static_cache_bytes;
    const bool replacement_base_valid =
        replaced_bytes <= retained_bytes;
    const std::size_t retained_without_replacement =
        replacement_base_valid ? retained_bytes - replaced_bytes
                               : maximum_latent_aot_session_static_cache_bytes;
    if (!entry_available || !retained_invariant ||
        !replacement_base_valid ||
        state_bytes > maximum_latent_aot_session_static_cache_bytes ||
        retained_without_replacement >
            maximum_latent_aot_session_static_cache_bytes - state_bytes) {
        reject_for_budget();
        return;
    }
    state->retained_bytes = state_bytes;
    retained_bytes = retained_without_replacement + state_bytes;
    if (found == cache.end())
        cache.push_back(std::move(state));
    else
        *found = std::move(state);
}

struct CandidateAnalysisCacheCounters {
    std::atomic_size_t positive_hits = 0u;
    std::atomic_size_t negative_hits = 0u;
    std::atomic_size_t misses = 0u;
    std::atomic_size_t corrupt_entries = 0u;
    std::atomic_size_t stores = 0u;
    std::atomic_size_t full_pipeline_runs = 0u;
    std::atomic_size_t session_reuse_hits = 0u;
    std::atomic_size_t session_terminal_negative_hits = 0u;
    std::atomic_size_t session_cold_fallbacks = 0u;
    std::atomic_size_t session_cache_budget_skips = 0u;
    std::atomic_size_t module_static_hits = 0u;
    std::atomic_size_t module_static_misses = 0u;
    std::atomic_size_t module_static_cold_fallbacks = 0u;
    std::atomic_size_t module_static_corrupt_entries = 0u;
    std::atomic_size_t module_static_stores = 0u;
};

bool publish_static_candidate_state_now(
    CodegenCache& cache,
    LatentAotStaticCandidateState& state,
    CandidateAnalysisCacheCounters& counters) {
    if (state.terminal_inventory_candidate_values || state.program.empty())
        return false;
    const auto serialized_bytes = serialize_static_candidate_state(state);
    const std::string serialized(
        reinterpret_cast<const char*>(serialized_bytes.data()),
        serialized_bytes.size());
    bool published = false;
    if (serialized == state.persistent_observed_payload) {
        return true;
    } else if (!state.persistent_observed_payload.empty()) {
        published = cache.replace_bounded_if_matches(
            state.persistent_cache_key,
            latent_aot_module_static_cache_artifact,
            state.persistent_observed_payload,
            serialized,
            maximum_latent_aot_module_static_cache_artifact_bytes);
    } else {
        cache.store_bounded(
            state.persistent_cache_key,
            latent_aot_module_static_cache_artifact,
            serialized,
            maximum_latent_aot_module_static_cache_artifact_bytes);
        published = true;
    }
    if (published) {
        state.persistent_observed_payload = serialized;
        counters.module_static_stores.fetch_add(
            1u, std::memory_order_relaxed);
    }
    return published;
}

CandidateAnalysisOutcome reject_candidate(
    const LatentAotAnalysisRejection rejection,
    const bool deterministic = true,
    std::string rejection_detail = "none",
    const bool terminal_inventory_candidate_values = false) {
    return {std::nullopt, rejection, deterministic,
            std::move(rejection_detail),
            terminal_inventory_candidate_values};
}

std::string_view latent_aot_rejection_name(
    const LatentAotAnalysisRejection rejection) noexcept {
    switch (rejection) {
    case LatentAotAnalysisRejection::None:
        return "none";
    case LatentAotAnalysisRejection::NoEntryPoints:
        return "no-entry-points";
    case LatentAotAnalysisRejection::EntryDecodeFailed:
        return "entry-decode-failed";
    case LatentAotAnalysisRejection::ControlFlowIncomplete:
        return "control-flow-incomplete";
    case LatentAotAnalysisRejection::InventoryTruncated:
        return "inventory-truncated";
    case LatentAotAnalysisRejection::ProgramInvalid:
        return "program-invalid";
    case LatentAotAnalysisRejection::RelocationNotClosed:
        return "relocation-not-closed";
    case LatentAotAnalysisRejection::EntryBlockMissing:
        return "entry-block-missing";
    case LatentAotAnalysisRejection::FunctionBudgetExceeded:
        return "function-budget-exceeded";
    case LatentAotAnalysisRejection::BlockBudgetExceeded:
        return "block-budget-exceeded";
    case LatentAotAnalysisRejection::InstructionBudgetExceeded:
        return "instruction-budget-exceeded";
    case LatentAotAnalysisRejection::AnalysisIterationBudgetExceeded:
        return "analysis-iteration-budget-exceeded";
    case LatentAotAnalysisRejection::AnalysisContextBudgetExceeded:
        return "analysis-context-budget-exceeded";
    }
    return "unknown";
}

bool needs_sequential_explicit_hint_certification(
    const CandidateAnalysisOutcome& outcome,
    const bool explicit_entry_binding) noexcept {
    // A parallel worker can only be retried when the exact entry authority is
    // caller-supplied and the analyzer reported a non-deterministic exception.
    // Structural, budget, identity and ordinary deterministic rejections are
    // final and must never be softened by this certification lane.
    return explicit_entry_binding && !outcome.module.has_value() &&
           outcome.rejection == LatentAotAnalysisRejection::ProgramInvalid &&
           !outcome.deterministic;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>>
explicit_entry_unknown_before_control_flow(
    const std::span<const std::uint8_t> bytes,
    const std::span<const std::uint32_t> entry_offsets,
    const std::size_t maximum_scan_instructions) noexcept {
    const auto opcode_at = [&bytes](const std::uint32_t offset) {
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[offset]) |
            static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(bytes[offset + 1u]) << 8u));
    };
    for (const auto entry_offset : entry_offsets) {
        if (entry_offset > bytes.size() ||
            sizeof(std::uint16_t) > bytes.size() - entry_offset)
            continue;
        const auto available_instructions =
            (bytes.size() - entry_offset) / sizeof(std::uint16_t);
        const auto scan =
            std::min(maximum_scan_instructions, available_instructions);
        for (std::size_t instruction = 0u; instruction < scan; ++instruction) {
            const auto instruction_offset = entry_offset +
                static_cast<std::uint32_t>(
                    instruction * sizeof(std::uint16_t));
            const auto decoded = katana::sh4::decode(
                opcode_at(instruction_offset));
            if (!decoded.is_known())
                return std::pair{entry_offset, instruction_offset};
            if (decoded.changes_control_flow()) break;
        }
    }
    return std::nullopt;
}

void require_explicit_entry_prefixes(
    const std::span<const std::uint8_t> bytes,
    const std::span<const std::uint32_t> entry_offsets,
    const std::size_t maximum_scan_instructions) {
    const auto rejected = explicit_entry_unknown_before_control_flow(
        bytes, entry_offsets, maximum_scan_instructions);
    if (!rejected.has_value()) return;
    std::ostringstream message;
    message << "latent-aot-entry-hint-prefix-decode-invalid:entry-offset=0x"
            << std::hex << std::uppercase << rejected->first
            << ":instruction-offset=0x" << rejected->second;
    throw std::runtime_error(message.str());
}

std::optional<LatentAotAnalysisRejection>
candidate_source_shape_rejection(
    const DiscFileCandidate& candidate,
    const LatentAotDiscoveryOptions& options) {
    const bool exact_entry_binding =
        !candidate.explicit_entry_offsets.empty();
    const bool inferred_prefix_entry_table =
        candidate.inferred_authoritative_entry_table;
    const bool transformed_source =
        candidate_has_transformed_source(candidate);
    if (candidate.size != candidate.bytes.size() ||
        !valid_sha256_identity(candidate.byte_identity) ||
        (exact_entry_binding
             ? candidate.bytes.size() < 2u ||
                   (!transformed_source &&
                    (candidate.bytes.size() & 1u) != 0u)
             : transformed_source
                   ? candidate.bytes.size() < 2u
                   : candidate.bytes.size() < 4u ||
                         (candidate.bytes.size() & 3u) != 0u) ||
        !valid_candidate_entry_offsets(candidate))
        return candidate.entry_offsets.empty()
                   ? LatentAotAnalysisRejection::NoEntryPoints
                   : LatentAotAnalysisRejection::ProgramInvalid;
    const auto opcode_at =
        [&candidate](const std::uint32_t offset) {
            return static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(
                    candidate.bytes[offset]) |
                static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(
                        candidate.bytes[offset + 1u])
                    << 8u));
        };
    if (exact_entry_binding || inferred_prefix_entry_table) {
        if (std::any_of(
                candidate.entry_offsets.begin(),
                candidate.entry_offsets.end(),
                [&](const auto offset) {
                    return !katana::sh4::decode(
                                opcode_at(offset))
                                .is_known();
                }))
            return LatentAotAnalysisRejection::EntryDecodeFailed;
        if (exact_entry_binding) return std::nullopt;
    }
    const auto scan_entries = inferred_prefix_entry_table
                                  ? std::span<const std::uint32_t>{
                                        candidate.entry_offsets}
                                  : std::span<const std::uint32_t>{};
    if (!inferred_prefix_entry_table &&
        !katana::sh4::decode(opcode_at(0u)).is_known())
        return LatentAotAnalysisRejection::EntryDecodeFailed;
    const auto entry_has_early_control_flow =
        [&](const std::uint32_t entry_offset) {
            const auto available_instructions =
                (candidate.bytes.size() - entry_offset) / 2u;
            const auto entry_scan = std::min(
                options.maximum_entry_scan_instructions,
                available_instructions);
            for (std::size_t instruction = 0u;
                 instruction < entry_scan;
                 ++instruction) {
                const auto offset = entry_offset +
                    static_cast<std::uint32_t>(instruction * 2u);
                const auto decoded =
                    katana::sh4::decode(opcode_at(offset));
                if (!decoded.is_known()) return false;
                if (decoded.changes_control_flow()) return true;
            }
            return false;
        };
    if (inferred_prefix_entry_table) {
        if (std::any_of(scan_entries.begin(), scan_entries.end(),
                        [&](const auto offset) {
                            return !entry_has_early_control_flow(offset);
                        }))
            return LatentAotAnalysisRejection::EntryDecodeFailed;
    } else if (!entry_has_early_control_flow(0u)) {
        return LatentAotAnalysisRejection::EntryDecodeFailed;
    }
    return std::nullopt;
}

bool source_lowering_matches(
    const katana::ir::Instruction& cached,
    const katana::ir::Instruction& current) noexcept {
    return cached.source_address == current.source_address &&
           cached.original_opcode == current.original_opcode &&
           cached.original_operation == current.original_operation &&
           cached.operation == current.operation &&
           cached.widths == current.widths &&
           cached.status_effects == current.status_effects &&
           cached.memory_effects == current.memory_effects &&
           cached.accumulator_effects == current.accumulator_effects &&
           cached.destination_register == current.destination_register &&
           cached.source_register == current.source_register &&
           cached.branch_register == current.branch_register &&
           cached.immediate == current.immediate &&
           cached.displacement == current.displacement &&
           cached.special_register == current.special_register &&
           cached.effective_address == current.effective_address &&
           cached.target_address == current.target_address &&
           !cached.forwarded_value_register.has_value() &&
           cached.delay_slot == current.delay_slot &&
           cached.is_privileged == current.is_privileged &&
           cached.branch_register_relative ==
               current.branch_register_relative;
}

bool source_bound_unoptimized_program(
    const DiscFileCandidate& candidate,
    const std::span<const katana::ir::Function> program) {
    const auto module_end =
        static_cast<std::uint64_t>(candidate.source_address) +
        candidate.bytes.size();
    const auto fail = [](const char* const reason,
                         const std::uint32_t function,
                         const std::uint32_t block,
                         const std::uint32_t instruction) {
        std::fprintf(stderr,
                     "KATANA_LATENT_AOT_SOURCE_BOUND_FAILURE "
                     "reason=%s function=0x%08X block=0x%08X "
                     "instruction=0x%08X\n",
                     reason, function, block, instruction);
        return false;
    };
    if (!std::is_sorted(
            program.begin(),
            program.end(),
            [](const auto& left, const auto& right) {
                return left.entry_address < right.entry_address;
            }))
        return fail("program-order", 0u, 0u, 0u);
    std::set<std::uint32_t> function_entries;
    for (const auto& function : program) {
        if (!function_entries.insert(
                function.entry_address).second ||
            !std::is_sorted(
                function.blocks.begin(),
                function.blocks.end(),
                [](const auto& left, const auto& right) {
                    return left.start_address <
                           right.start_address;
                }))
            return fail("function-or-block-order",
                        function.entry_address, 0u, 0u);
        for (const auto& block : function.blocks) {
            for (std::size_t instruction_index = 0u;
                 instruction_index < block.instructions.size();
                 ++instruction_index) {
                const auto& instruction =
                    block.instructions[instruction_index];
                const auto address =
                    static_cast<std::uint64_t>(instruction.source_address);
                if (address < candidate.source_address ||
                    address + 2u > module_end)
                    return fail("source-extent",
                                function.entry_address,
                                block.start_address,
                                instruction.source_address);
                if (instruction_index != 0u &&
                    address !=
                        static_cast<std::uint64_t>(
                            block.instructions[
                                instruction_index - 1u]
                                    .source_address) +
                            2u)
                    return fail("source-continuity",
                                function.entry_address,
                                block.start_address,
                                instruction.source_address);
                const auto offset = static_cast<std::size_t>(
                    address - candidate.source_address);
                const auto opcode = static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(candidate.bytes[offset]) |
                    static_cast<std::uint16_t>(
                        static_cast<std::uint16_t>(
                            candidate.bytes[offset + 1u])
                        << 8u));
                if (instruction.original_opcode != opcode)
                    return fail("source-opcode",
                                function.entry_address,
                                block.start_address,
                                instruction.source_address);
                katana::sh4::DisassemblyLine line;
                line.address = instruction.source_address;
                line.opcode = opcode;
                line.instruction = katana::sh4::decode(opcode);
                if (!line.instruction.is_known())
                    return fail("source-decode",
                                function.entry_address,
                                block.start_address,
                                instruction.source_address);
                line.is_delay_slot =
                    instruction.delay_slot.role ==
                    katana::ir::DelaySlotRole::Slot;
                line.target_address =
                    katana::sh4::calculate_direct_branch_target(
                        line.instruction, line.address);
                const auto current =
                    katana::ir::lower_instruction(line);
                if (!source_lowering_matches(instruction, current))
                    return fail("source-lowering",
                                function.entry_address,
                                block.start_address,
                                instruction.source_address);
                if (current.dynamic_target_class ==
                        katana::ir::DynamicTargetClass::NotApplicable &&
                    (instruction.dynamic_target_class !=
                         katana::ir::DynamicTargetClass::NotApplicable ||
                     !instruction.resolved_targets.empty()))
                    return fail("unexpected-dynamic-target",
                                function.entry_address,
                                block.start_address,
                                instruction.source_address);
                // GuardedPartial retains a live indirect edge and is valid
                // only as positive AOT inventory: its known local candidates
                // are checked below, while every other live value still goes
                // through the loaded-module dispatcher. An entirely
                // unresolved site has no bounded positive source proof.
                if (instruction.dynamic_target_class ==
                    katana::ir::DynamicTargetClass::Unresolved)
                    return fail("incomplete-dynamic-target",
                                function.entry_address,
                                block.start_address,
                                instruction.source_address);
            }
        }
    }
    const auto has_function =
        [&function_entries](const std::uint32_t address) {
            return function_entries.contains(address);
        };
    for (const auto& function : program) {
        std::set<std::uint32_t> local_block_addresses;
        for (const auto& block : function.blocks)
            local_block_addresses.insert(block.start_address);
        const auto has_local_block =
            [&local_block_addresses](
                const std::uint32_t address) {
                return local_block_addresses.contains(address);
            };
        for (const auto& block : function.blocks) {
            if (block.instructions.empty())
                return fail("empty-block", function.entry_address,
                            block.start_address, 0u);
            const auto* control = &block.instructions.back();
            if (control->delay_slot.role ==
                    katana::ir::DelaySlotRole::Slot) {
                if (block.instructions.size() < 2u)
                    return fail("orphan-delay-slot",
                                function.entry_address,
                                block.start_address,
                                control->source_address);
                control = &block.instructions[
                    block.instructions.size() - 2u];
            }
            const auto fallthrough64 =
                static_cast<std::uint64_t>(
                    block.instructions.back().source_address) +
                2u;
            const auto has_fallthrough =
                fallthrough64 <=
                    std::numeric_limits<std::uint32_t>::max() &&
                (has_local_block(static_cast<std::uint32_t>(
                     fallthrough64)) ||
                 has_function(static_cast<std::uint32_t>(
                     fallthrough64)));
            switch (control->operation) {
            case katana::ir::Operation::Branch:
                if (!control->target_address ||
                    (!has_local_block(*control->target_address) &&
                     !has_function(*control->target_address)))
                    return fail("branch-target",
                                function.entry_address,
                                block.start_address,
                                control->source_address);
                break;
            case katana::ir::Operation::BranchIfTrue:
            case katana::ir::Operation::BranchIfFalse:
                if (!control->target_address ||
                    (!has_local_block(*control->target_address) &&
                     !has_function(*control->target_address)) ||
                    !has_fallthrough)
                    return fail("conditional-target",
                                function.entry_address,
                                block.start_address,
                                control->source_address);
                break;
            case katana::ir::Operation::Call:
                if (!control->target_address ||
                    !has_function(*control->target_address) ||
                    !has_fallthrough)
                    return fail("call-target",
                                function.entry_address,
                                block.start_address,
                                control->source_address);
                break;
            case katana::ir::Operation::CallRegister:
                if (!has_fallthrough ||
                    std::any_of(
                        control->resolved_targets.begin(),
                        control->resolved_targets.end(),
                        [&](const auto target) {
                             return !has_function(target);
                        })) {
                    const auto missing = std::find_if(
                        control->resolved_targets.begin(),
                        control->resolved_targets.end(),
                        [&](const auto target) {
                            return !has_function(target);
                        });
                    std::fprintf(
                        stderr,
                        "KATANA_LATENT_AOT_SOURCE_BOUND_FAILURE "
                        "reason=call-register function=0x%08X "
                        "block=0x%08X instruction=0x%08X "
                        "fallthrough=%u missing_target=0x%08X\n",
                        function.entry_address, block.start_address,
                        control->source_address,
                        static_cast<unsigned int>(has_fallthrough),
                        missing == control->resolved_targets.end()
                            ? 0u
                            : *missing);
                    return false;
                }
                break;
            case katana::ir::Operation::JumpRegister:
                if (std::any_of(
                        control->resolved_targets.begin(),
                        control->resolved_targets.end(),
                        [&](const auto target) {
                            return !has_local_block(target) &&
                                   !has_function(target);
                        })) {
                    const auto missing = std::find_if(
                        control->resolved_targets.begin(),
                        control->resolved_targets.end(),
                        [&](const auto target) {
                            return !has_local_block(target) &&
                                   !has_function(target);
                        });
                    std::fprintf(
                        stderr,
                        "KATANA_LATENT_AOT_SOURCE_BOUND_FAILURE "
                        "reason=jump-register function=0x%08X "
                        "block=0x%08X instruction=0x%08X "
                        "missing_target=0x%08X\n",
                        function.entry_address, block.start_address,
                        control->source_address,
                        missing == control->resolved_targets.end()
                            ? 0u
                            : *missing);
                    return false;
                }
                break;
            case katana::ir::Operation::Return:
            case katana::ir::Operation::ReturnFromException:
            case katana::ir::Operation::TrapAlways:
            case katana::ir::Operation::Sleep:
                break;
            default:
                if (!has_fallthrough)
                    return fail("fallthrough",
                                function.entry_address,
                                block.start_address,
                                control->source_address);
                break;
            }
        }
    }
    return true;
}

CandidateAnalysisOutcome finalize_candidate_program(
    const DiscFileCandidate& candidate,
    const std::span<const std::uint32_t> published_entry_offsets,
    std::vector<katana::ir::Function> program,
    katana::analysis::DreamcastHardwareAudit hardware_audit,
    const LatentAotDiscoveryOptions& options,
        const std::span<const katana::ir::ExternalDispatchEntry>
        external_dispatch_entries) {
    const auto required_public_roots = candidate_public_roots(candidate, program);
    if (const auto rejection =
            candidate_source_shape_rejection(
                candidate, options))
        return reject_candidate(*rejection);
    if (published_entry_offsets.empty() ||
        !std::is_sorted(published_entry_offsets.begin(),
                        published_entry_offsets.end()) ||
        std::adjacent_find(published_entry_offsets.begin(),
                           published_entry_offsets.end()) !=
            published_entry_offsets.end() ||
        std::any_of(
            required_public_roots.begin(),
            required_public_roots.end(),
            [&](const auto offset) {
                return !std::binary_search(
                    published_entry_offsets.begin(),
                    published_entry_offsets.end(), offset);
            }) ||
        std::any_of(
            published_entry_offsets.begin(),
            published_entry_offsets.end(),
            [&](const auto offset) {
                return (offset & 1u) != 0u ||
                       offset > candidate.bytes.size() ||
                       candidate.bytes.size() - offset <
                           sizeof(std::uint16_t);
            }))
        return reject_candidate(
            LatentAotAnalysisRejection::ProgramInvalid);
    if (program.empty())
        return reject_candidate(
            LatentAotAnalysisRejection::ProgramInvalid);
    if (program.size() > options.maximum_functions_per_module)
        return reject_candidate(
            LatentAotAnalysisRejection::FunctionBudgetExceeded);
    std::vector<PreparedLatentAotExternalTransfer>
        pending_external_transfers;
    std::vector<PreparedLatentAotExternalTransfer>
        pending_external_transfer_candidates;
    std::vector<PreparedLatentAotCodePointerEvidence>
        pending_external_callback_evidence;
    std::vector<PreparedLatentAotCodePointerEvidence>
        pending_external_record_table_evidence;
    std::vector<PreparedLatentAotPcLiteralEvidence> pc_literal_evidence;
    try {
        auto external_transfers = resolve_latent_literal_transfers(
            candidate, program, options.external_code_targets);
        auto external_callbacks = resolve_latent_external_callbacks(
            candidate, program, options.external_code_targets,
            options.external_callback_sinks,
            options.maximum_entry_scan_instructions);
        auto external_record_callbacks =
            resolve_latent_external_callback_record_tables(
                candidate, program, options.external_code_targets,
                options.external_data_targets,
                options.external_callback_record_tables,
                options.maximum_entry_scan_instructions);
        if (!source_bound_unoptimized_program(candidate, program)) {
            std::fprintf(stderr,
                         "KATANA_LATENT_AOT_PROGRAM_INVALID "
                         "stage=source-bound\n");
            return reject_candidate(
                LatentAotAnalysisRejection::ProgramInvalid);
        }
        const auto collected_pc_literal_evidence =
            collect_latent_pc_literal_evidence(
                candidate.source_address, candidate.bytes, program);
        if (!collected_pc_literal_evidence.has_value())
            return reject_candidate(
                LatentAotAnalysisRejection::ProgramInvalid);
        pc_literal_evidence = *collected_pc_literal_evidence;
        katana::ir::require_valid_program(program);
        // Preserve the cross-image facts across isolated optimization; they
        // are attached to the prepared module only after all local validation
        // and identity work succeeds.
        pending_external_transfers = std::move(external_transfers.admitted);
        pending_external_transfer_candidates =
            std::move(external_transfers.candidates);
        pending_external_callback_evidence =
            std::move(external_callbacks.external_evidence);
        pending_external_record_table_evidence =
            std::move(external_record_callbacks.external_evidence);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        std::fprintf(stderr,
                     "KATANA_LATENT_AOT_PROGRAM_INVALID stage=verify "
                     "error=%s\n",
                     error.what());
        return reject_candidate(
            LatentAotAnalysisRejection::ProgramInvalid);
    }
    std::size_t source_block_count = 0u;
    std::size_t source_instruction_count = 0u;
    for (const auto& function : program) {
        if (source_block_count > options.maximum_blocks_per_module ||
            function.blocks.size() >
                options.maximum_blocks_per_module -
                    source_block_count)
            return reject_candidate(
                LatentAotAnalysisRejection::BlockBudgetExceeded);
        source_block_count += function.blocks.size();
        for (const auto& block : function.blocks) {
            if (source_instruction_count >
                    options.maximum_native_instructions_per_module ||
                block.instructions.size() >
                    options.maximum_native_instructions_per_module -
                        source_instruction_count)
                return reject_candidate(
                    LatentAotAnalysisRejection::
                        InstructionBudgetExceeded);
            source_instruction_count += block.instructions.size();
        }
    }
    try {
        static_cast<void>(katana::ir::optimize_program(
            program,
            {},
            {},
            {external_dispatch_entries}));
        katana::ir::require_valid_program(program);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        std::fprintf(stderr,
                     "KATANA_LATENT_AOT_PROGRAM_INVALID stage=lower "
                     "error=%s\n",
                     error.what());
        return reject_candidate(
            LatentAotAnalysisRejection::ProgramInvalid);
    }
    if (!relocation_closed_impl(
            program, candidate.source_address, candidate.size))
        return reject_candidate(
            LatentAotAnalysisRejection::RelocationNotClosed);
    const auto final_ir_accepts_external_transfer =
        [&](const PreparedLatentAotExternalTransfer& transfer) {
            const auto address =
                candidate.source_address + transfer.source_offset;
            std::size_t matches = 0u;
            for (const auto& function : program) {
                for (const auto& block : function.blocks) {
                    for (const auto& instruction : block.instructions) {
                        if (instruction.source_address != address) continue;
                        ++matches;
                        if (classify_loaded_aot_external_transfer(
                                transfer, instruction, block) ==
                            LoadedAotExternalTransferClassification::Conflict)
                            return false;
                    }
                }
            }
            return matches != 0u;
        };
    for (auto transfer = pending_external_transfers.begin();
         transfer != pending_external_transfers.end();) {
        if (final_ir_accepts_external_transfer(*transfer)) {
            ++transfer;
            continue;
        }
        pending_external_transfer_candidates.push_back(*transfer);
        transfer = pending_external_transfers.erase(transfer);
    }
    std::sort(pending_external_transfer_candidates.begin(),
              pending_external_transfer_candidates.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.source_offset,
                                  left.target_address,
                                  left.kind) <
                         std::tie(right.source_offset,
                                  right.target_address,
                                  right.kind);
              });
    pending_external_transfer_candidates.erase(
        std::unique(pending_external_transfer_candidates.begin(),
                    pending_external_transfer_candidates.end()),
        pending_external_transfer_candidates.end());
    if (pending_external_transfer_candidates.size() >
        maximum_prepared_latent_aot_external_transfers)
        return reject_candidate(
            LatentAotAnalysisRejection::ProgramInvalid);
    const auto discard_pruned_transfers = [&](auto& transfers) {
        transfers.erase(
            std::remove_if(
                transfers.begin(), transfers.end(),
                [&](const auto& transfer) {
                    const auto address =
                        candidate.source_address + transfer.source_offset;
                    return std::none_of(
                        program.begin(), program.end(),
                        [&](const auto& function) {
                            return std::any_of(
                                function.blocks.begin(),
                                function.blocks.end(),
                                [&](const auto& block) {
                                    return std::any_of(
                                        block.instructions.begin(),
                                        block.instructions.end(),
                                        [&](const auto& instruction) {
                                            return instruction.source_address ==
                                                   address;
                                        });
                                });
                        });
                }),
            transfers.end());
    };
    discard_pruned_transfers(pending_external_transfers);
    discard_pruned_transfers(pending_external_transfer_candidates);
    discard_pruned_transfers(pending_external_callback_evidence);

    std::size_t block_count = 0u;
    std::size_t instruction_count = 0u;
    for (const auto& function : program) {
        if (block_count > options.maximum_blocks_per_module ||
            function.blocks.size() >
                options.maximum_blocks_per_module - block_count)
            return reject_candidate(
                LatentAotAnalysisRejection::BlockBudgetExceeded);
        block_count += function.blocks.size();
        for (const auto& block : function.blocks) {
            if (instruction_count >
                    options.maximum_native_instructions_per_module ||
                block.instructions.size() >
                    options.maximum_native_instructions_per_module -
                        instruction_count)
                return reject_candidate(
                    LatentAotAnalysisRejection::
                        InstructionBudgetExceeded);
            instruction_count += block.instructions.size();
        }
    }

    const auto module_end =
        static_cast<std::uint64_t>(candidate.source_address) +
        candidate.size;
    std::vector<PreparedLatentAotBlockIdentity> block_identities;
    block_identities.reserve(block_count);
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            const auto block_start =
                static_cast<std::uint64_t>(block.start_address);
            if (block.start_address < candidate.source_address ||
                block_start >= module_end)
                return reject_candidate(
                    LatentAotAnalysisRejection::ProgramInvalid);
            auto block_end = block_start + 2u;
            if (block_end > module_end)
                return reject_candidate(
                    LatentAotAnalysisRejection::ProgramInvalid);
            for (const auto& instruction : block.instructions) {
                const auto instruction_start =
                    static_cast<std::uint64_t>(
                        instruction.source_address);
                const auto instruction_end = instruction_start + 2u;
                if (instruction.source_address <
                        candidate.source_address ||
                    instruction_start >= module_end ||
                    instruction_end > module_end)
                    return reject_candidate(
                        LatentAotAnalysisRejection::ProgramInvalid);
                block_end = std::max(block_end, instruction_end);
            }
            if (block_end <= block_start ||
                block_end - block_start >
                    std::numeric_limits<std::uint32_t>::max())
                return reject_candidate(
                    LatentAotAnalysisRejection::ProgramInvalid);
            const auto append_identity = [&](const std::uint32_t entry) {
                const auto entry_start = static_cast<std::uint64_t>(entry);
                if (entry < candidate.source_address ||
                    entry_start < block_start || entry_start >= block_end ||
                    block_end - entry_start >
                        std::numeric_limits<std::uint32_t>::max())
                    return false;
                const auto source_offset =
                    entry - candidate.source_address;
                const auto entry_size =
                    static_cast<std::uint32_t>(block_end - entry_start);
                if (source_offset > candidate.bytes.size() ||
                    entry_size > candidate.bytes.size() - source_offset)
                    return false;
                const auto bytes = std::string_view(
                    reinterpret_cast<const char*>(
                        candidate.bytes.data() + source_offset),
                    entry_size);
                block_identities.push_back(
                    {source_offset,
                     entry_size,
                     "sha256:" + katana::io::sha256_bytes(bytes)});
                return true;
            };
            if (!append_identity(block.start_address))
                return reject_candidate(
                    LatentAotAnalysisRejection::ProgramInvalid);
            for (const auto resume :
                 detail::native_aot_internal_resume_entries(block)) {
                if (!append_identity(resume))
                    return reject_candidate(
                        LatentAotAnalysisRejection::ProgramInvalid);
            }
        }
    }
    std::sort(
        block_identities.begin(),
        block_identities.end(),
        [](const auto& left, const auto& right) {
            if (left.source_offset != right.source_offset)
                return left.source_offset < right.source_offset;
            if (left.size != right.size)
                return left.size < right.size;
            return left.sha256 < right.sha256;
        });
    std::vector<PreparedLatentAotBlockIdentity>
        unique_block_identities;
    unique_block_identities.reserve(block_identities.size());
    std::uint64_t identity_bytes = 0u;
    std::uint64_t active_identity_end = 0u;
    for (const auto& identity : block_identities) {
        if (!unique_block_identities.empty() &&
            unique_block_identities.back().source_offset ==
                identity.source_offset) {
            if (unique_block_identities.back() != identity)
                return reject_candidate(
                    LatentAotAnalysisRejection::ProgramInvalid);
            continue;
        }
        const auto identity_end =
            static_cast<std::uint64_t>(identity.source_offset) +
            identity.size;
        if (!unique_block_identities.empty() &&
            identity.source_offset < active_identity_end &&
            identity_end != active_identity_end)
            return reject_candidate(
                LatentAotAnalysisRejection::ProgramInvalid);
        if (identity.source_offset >= active_identity_end)
            active_identity_end = identity_end;
        if (identity.size >
            maximum_prepared_latent_aot_block_identity_bytes - identity_bytes)
            return reject_candidate(
                LatentAotAnalysisRejection::BlockBudgetExceeded);
        identity_bytes += identity.size;
        unique_block_identities.push_back(identity);
    }
    if (unique_block_identities.empty())
        return reject_candidate(
            LatentAotAnalysisRejection::ProgramInvalid);
    if (unique_block_identities.size() >
        maximum_prepared_latent_aot_block_identities)
        return reject_candidate(
            LatentAotAnalysisRejection::BlockBudgetExceeded);

    std::vector<PreparedLatentAotFunctionIdentity> function_identities;
    function_identities.reserve(program.size());
    std::uint64_t function_identity_bytes = 0u;
    for (const auto& function : program) {
        const auto function_start =
            static_cast<std::uint64_t>(function.entry_address);
        if ((function.entry_address & 1u) != 0u ||
            function.entry_address < candidate.source_address ||
            function_start >= module_end)
            continue;
        auto function_end = function_start;
        bool exact_extent = !function.blocks.empty();
        for (const auto& block : function.blocks) {
            if (block.instructions.empty() ||
                block.start_address < function.entry_address) {
                exact_extent = false;
                break;
            }
            for (const auto& instruction : block.instructions) {
                const auto instruction_start =
                    static_cast<std::uint64_t>(instruction.source_address);
                const auto instruction_end = instruction_start + 2u;
                if ((instruction.source_address & 1u) != 0u ||
                    instruction.source_address < function.entry_address ||
                    instruction_end > module_end) {
                    exact_extent = false;
                    break;
                }
                function_end = std::max(function_end, instruction_end);
            }
            if (!exact_extent) break;
        }
        if (!exact_extent || function_end <= function_start ||
            function_end - function_start >
                std::numeric_limits<std::uint32_t>::max())
            continue;
        const auto source_offset =
            function.entry_address - candidate.source_address;
        const auto function_size =
            static_cast<std::uint32_t>(function_end - function_start);
        if (source_offset > candidate.bytes.size() ||
            function_size > candidate.bytes.size() - source_offset ||
            function_size >
                maximum_prepared_latent_aot_function_identity_bytes -
                    function_identity_bytes)
            return reject_candidate(
                LatentAotAnalysisRejection::FunctionBudgetExceeded);
        const auto bytes = std::string_view(
            reinterpret_cast<const char*>(
                candidate.bytes.data() + source_offset),
            function_size);
        function_identities.push_back(
            {source_offset,
             function_size,
             "sha256:" + katana::io::sha256_bytes(bytes)});
        function_identity_bytes += function_size;
    }
    std::sort(function_identities.begin(), function_identities.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.source_offset, left.size, left.sha256) <
                         std::tie(right.source_offset, right.size, right.sha256);
              });
    std::vector<PreparedLatentAotFunctionIdentity>
        unique_function_identities;
    unique_function_identities.reserve(function_identities.size());
    for (const auto& identity : function_identities) {
        if (!unique_function_identities.empty() &&
            unique_function_identities.back().source_offset ==
                identity.source_offset) {
            if (unique_function_identities.back() != identity)
                return reject_candidate(
                    LatentAotAnalysisRejection::ProgramInvalid);
            continue;
        }
        unique_function_identities.push_back(identity);
    }
    function_identities = std::move(unique_function_identities);
    if (function_identities.size() >
        maximum_prepared_latent_aot_function_identities)
        return reject_candidate(
            LatentAotAnalysisRejection::FunctionBudgetExceeded);
    for (const auto offset : published_entry_offsets) {
        const auto entry_address =
            candidate.source_address + offset;
        const auto emitted = std::any_of(
            program.begin(),
            program.end(),
            [&](const auto& function) {
                return std::any_of(
                    function.blocks.begin(),
                    function.blocks.end(),
                    [&](const auto& block) {
                        return block.start_address ==
                               entry_address;
                    });
            });
        if (!emitted)
            return reject_candidate(
                LatentAotAnalysisRejection::EntryBlockMissing);
    }
    for (const auto& authority_entry : candidate.authority_entries) {
        const auto offset = authority_entry.module_relative_offset;
        if (offset > candidate.bytes.size() ||
            authority_entry.byte_size > candidate.bytes.size() - offset ||
            authority_entry.byte_size == 0u)
            return reject_candidate(LatentAotAnalysisRejection::ProgramInvalid);
        const auto bytes = std::string_view(
            reinterpret_cast<const char*>(candidate.bytes.data() + offset),
            authority_entry.byte_size);
        if ("sha256:" + katana::io::sha256_bytes(bytes) !=
            authority_entry.byte_identity)
            return reject_candidate(LatentAotAnalysisRejection::ProgramInvalid);
        const auto address = candidate.source_address + offset;
        const bool function_entry = std::any_of(
            program.begin(), program.end(),
            [&](const auto& function) {
                return function.entry_address == address;
            });
        const bool block_entry = std::any_of(
            program.begin(), program.end(),
            [&](const auto& function) {
                return std::any_of(
                    function.blocks.begin(), function.blocks.end(),
                    [&](const auto& block) {
                        return block.start_address == address;
                    });
            });
        const bool valid_semantics =
            (authority_entry.kind == CompleteDisassemblyEntryKind::DeclaredEntry &&
             function_entry) ||
            (authority_entry.kind == CompleteDisassemblyEntryKind::FunctionEntry &&
             function_entry) ||
            (authority_entry.kind == CompleteDisassemblyEntryKind::ControlFlowTarget &&
             block_entry) ||
            (authority_entry.kind == CompleteDisassemblyEntryKind::CodePointerTarget &&
             (function_entry || block_entry));
        if (!valid_semantics)
            return reject_candidate(LatentAotAnalysisRejection::ProgramInvalid);
    }
    std::vector<PreparedLatentAotCodePointerEvidence>
        external_code_pointer_evidence;
    external_code_pointer_evidence.reserve(std::min<std::size_t>(
        candidate.bytes.size() / sizeof(std::uint32_t),
        maximum_prepared_latent_aot_code_pointer_evidence));
    const auto module_begin = static_cast<std::uint64_t>(
        candidate.source_address);
    const auto module_limit = module_begin + candidate.size;
    for (std::size_t offset = 0u;
         offset + sizeof(std::uint32_t) <= candidate.bytes.size();
         offset += alignof(std::uint32_t)) {
        const auto value =
            static_cast<std::uint32_t>(candidate.bytes[offset]) |
            (static_cast<std::uint32_t>(candidate.bytes[offset + 1u]) << 8u) |
            (static_cast<std::uint32_t>(candidate.bytes[offset + 2u]) << 16u) |
            (static_cast<std::uint32_t>(candidate.bytes[offset + 3u]) << 24u);
        const auto normalized = latent_direct_code_address(value);
        if (!normalized.has_value()) continue;
        if (*normalized >= module_begin && *normalized < module_limit) continue;
        if (!std::binary_search(options.external_code_targets.begin(),
                                options.external_code_targets.end(),
                                *normalized))
            continue;
        external_code_pointer_evidence.push_back(
            {static_cast<std::uint32_t>(offset),
             *normalized,
             PreparedLatentAotCodePointerEvidenceKind::DeclaredPointerCell,
             0u,
             0xFFu});
    }
    // Direct literal register transfers are instruction-level evidence and
    // may discover a stripped primary-image entry which was not present in
    // the initial declaration set. Keep their targets in the same bounded,
    // sorted candidate inventory; admission remains a separate exporter-side
    // primary-image proof.
    const auto append_transfer_evidence =
        [&](const PreparedLatentAotExternalTransfer& transfer) {
            external_code_pointer_evidence.push_back(
                {transfer.source_offset,
                 transfer.target_address,
                 transfer.kind == PreparedLatentAotExternalTransferKind::Call
                     ? PreparedLatentAotCodePointerEvidenceKind::LiteralCall
                     : PreparedLatentAotCodePointerEvidenceKind::LiteralJump,
                 0u,
                 0xFFu});
        };
    for (const auto& transfer : pending_external_transfers)
        append_transfer_evidence(transfer);
    for (const auto& transfer : pending_external_transfer_candidates)
        append_transfer_evidence(transfer);
    external_code_pointer_evidence.insert(
        external_code_pointer_evidence.end(),
        pending_external_callback_evidence.begin(),
        pending_external_callback_evidence.end());
    // Record-table evidence is sourced from immutable module data rather
    // than from an IR instruction. It therefore deliberately bypasses the
    // instruction-pruning filter applied to literal transfer evidence.
    external_code_pointer_evidence.insert(
        external_code_pointer_evidence.end(),
        pending_external_record_table_evidence.begin(),
        pending_external_record_table_evidence.end());
    std::sort(external_code_pointer_evidence.begin(),
              external_code_pointer_evidence.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.source_offset,
                                  left.target_address,
                                  left.kind,
                                  left.sink_address,
                                  left.argument_index) <
                         std::tie(right.source_offset,
                                  right.target_address,
                                  right.kind,
                                  right.sink_address,
                                  right.argument_index);
              });
    external_code_pointer_evidence.erase(
        std::unique(external_code_pointer_evidence.begin(),
                    external_code_pointer_evidence.end()),
        external_code_pointer_evidence.end());
    if (external_code_pointer_evidence.size() >
        maximum_prepared_latent_aot_code_pointer_evidence)
        return reject_candidate(
            LatentAotAnalysisRejection::BlockBudgetExceeded);

    std::vector<std::uint32_t> external_code_pointer_candidates;
    external_code_pointer_candidates.reserve(
        external_code_pointer_evidence.size());
    for (const auto& evidence : external_code_pointer_evidence)
        external_code_pointer_candidates.push_back(evidence.target_address);
    std::sort(external_code_pointer_candidates.begin(),
              external_code_pointer_candidates.end());
    external_code_pointer_candidates.erase(
        std::unique(external_code_pointer_candidates.begin(),
                    external_code_pointer_candidates.end()),
        external_code_pointer_candidates.end());
    if (external_code_pointer_candidates.size() >
        maximum_prepared_latent_aot_external_code_pointer_candidates)
        return reject_candidate(
            LatentAotAnalysisRejection::BlockBudgetExceeded);
    auto module_id =
        "latent-aot-" + candidate.byte_identity.substr(7u) +
        "-" + std::to_string(candidate.size);
    return {
        PreparedLatentAotModule{
            std::move(module_id),
            candidate.byte_identity,
            candidate.size,
            candidate.source_address,
            candidate.source_bindings,
            std::move(pc_literal_evidence),
            std::vector<std::uint32_t>(
                published_entry_offsets.begin(),
                published_entry_offsets.end()),
            std::move(external_code_pointer_candidates),
            std::move(external_code_pointer_evidence),
            std::move(pending_external_transfers),
            std::move(unique_block_identities),
            std::move(function_identities),
            std::move(program),
            std::move(hardware_audit),
            candidate.authority_entries,
            candidate.module_class},
        LatentAotAnalysisRejection::None,
        true};
}

CandidateAnalysisOutcome analyze_candidate_uncached(
    const DiscFileCandidate& candidate,
    const LatentAotDiscoveryOptions& options,
    const katana::ProgressReporter& progress_reporter,
    const std::span<const std::uint8_t> persistent_epoch_import_blob,
    katana::analysis::PersistentFunctionAnalysisEpochPublishCallback
        persistent_epoch_publish_callback,
    LatentAotModuleAuditResult* const module_audit = nullptr,
    LatentAotStaticCandidateState* const static_state = nullptr) {
    if (const auto rejection =
            candidate_source_shape_rejection(
                candidate, options))
        return reject_candidate(*rejection);

    const bool exact_runtime_only_stop_on_miss =
        candidate_has_authoritative_entries(candidate) &&
        options.completeness_policy ==
            LatentAotCompletenessPolicy::ExactRuntimeOnlyStopOnMiss;
    katana::io::ExecutableImage image;
    // A hash-bound authoritative entry set is already the RuntimeOnly root
    // contract. It is either externally supplied exactly or derived from a
    // complete bounded table in the transformed bytes. Analyze its static
    // graph without the SuperHC value/candidate fixpoint; every omitted
    // dynamic destination remains a typed runtime miss. Strict/ordinary
    // heuristic discovery keeps the full ABI analysis unchanged.
    image.set_guest_call_abi(
        exact_runtime_only_stop_on_miss
            ? katana::io::GuestCallAbi::Unknown
            : katana::io::GuestCallAbi::SuperHC);
    image.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::ImmutableOnly);
    image.set_address_model(
        katana::io::ImageAddressModel::Sh4DirectMapped);
    katana::io::ImageSegment segment{
        ".latent-disc-module",
        candidate.source_address,
        candidate_has_transformed_source(candidate)
            ? 0u
            : candidate.source_bindings.front().disc_byte_offset,
        candidate.bytes.size(),
        katana::io::SegmentKind::Mixed,
        {true, true, true},
        candidate.bytes};
    segment.source_kind =
        katana::io::ImageSourceKind::DiscModule;
    segment.load_phase =
        katana::io::ImageLoadPhase::RuntimeModule;
    image.add_segment(std::move(segment));
    // A caller-proven runtime base is part of the exact latent-module binding,
    // not a heuristic relocation guess. Install that alias before CFA so
    // generation-bound code-pointer tables expressed in runtime addresses can
    // contribute guarded positive inventory. Inferred aliases remain on the
    // late audit-only path below and can never manufacture executable roots.
    if (candidate.proven_runtime_base.has_value() &&
        *candidate.proven_runtime_base != candidate.source_address) {
        image.add_address_alias(
            {candidate.source_address,
             *candidate.proven_runtime_base,
             candidate.size});
    }
    auto analysis_entry_offsets = candidate_analysis_roots(candidate);
    for (const auto offset : analysis_entry_offsets)
        image.add_entry_point(candidate.source_address + offset);
    // A transformed PRS module is admitted only with an exact decoded-byte
    // identity. Root-set changes advance the analysis revision while keeping
    // this authenticated byte proof bound for every discovery pass.
    image.add_immutable_range(
        {candidate.source_address,
         candidate.bytes.size(),
         candidate.byte_identity,
         0u});

    katana::analysis::ControlFlowAnalysisResult analysis;
    detail::StructuredControlFlowProgress control_flow_progress(
        progress_reporter,
        "latent-aot-module-" +
            std::to_string(
                candidate.source_bindings.front()
                    .disc_byte_offset));
    katana::analysis::ControlFlowAnalysisOptions analysis_options;
    analysis_options.maximum_fixpoint_iterations =
        options.maximum_analysis_iterations;
    analysis_options.maximum_instructions =
        options.maximum_native_instructions_per_module;
    analysis_options.maximum_contexts =
        options.maximum_analysis_contexts;
    auto& analysis_memory_budget =
        katana::analysis::global_analysis_memory_budget();
    katana::analysis::AnalysisMemoryBudget function_value_cache_budget(
        static_cast<std::size_t>(
            latent_aot_function_value_cache_budget_bytes),
        &analysis_memory_budget,
        static_cast<std::size_t>(
            latent_aot_function_value_ready_budget_bytes));
    katana::analysis::AnalysisMemoryBudget function_value_ready_budget(
        static_cast<std::size_t>(
            latent_aot_function_value_ready_budget_bytes),
        &analysis_memory_budget);
    analysis_options.function_value_cache_memory_budget =
        &function_value_cache_budget;
    analysis_options.pre_reserved_function_value_ready_budget =
        &function_value_ready_budget;
    analysis_options.persistent_function_analysis_epoch_import_blob =
        persistent_epoch_import_blob;
    analysis_options
        .persistent_function_analysis_epoch_implementation_identity =
            options.analysis_implementation_identity;
    analysis_options.persistent_function_analysis_epoch_publish_callback =
        std::move(persistent_epoch_publish_callback);
    katana::analysis::ControlFlowAnalysisSession analysis_session;
    std::atomic resolution_retention_limit_reason{
        katana::analysis::ResolutionRetentionLimitReason::None};
    bool runtime_alias_entry_fixpoint_complete = false;
    std::vector<katana::ir::Function> stable_discovery_program;
    std::vector<std::uint32_t> non_function_entry_offsets;
    std::vector<std::uint32_t> authoritative_tail_roots;
    for (std::size_t pass = 0u;
         pass < maximum_latent_aot_runtime_alias_entry_passes; ++pass) {
        resolution_retention_limit_reason.store(
            katana::analysis::ResolutionRetentionLimitReason::None,
            std::memory_order_relaxed);
        try {
            analysis = analysis_session.analyze(
                image,
                nullptr,
                [&control_flow_progress, &resolution_retention_limit_reason](
                    const katana::analysis::
                        ControlFlowAnalysisProgress& progress) {
                    if (progress
                            .function_value_resolution_retention_limit_reason !=
                        katana::analysis::ResolutionRetentionLimitReason::None)
                        resolution_retention_limit_reason.store(
                            progress
                                .function_value_resolution_retention_limit_reason,
                            std::memory_order_relaxed);
                    control_flow_progress.update(progress);
                },
                analysis_options);
            // A retained session may consume a persisted FVA epoch only on
            // its cold admission.  Subsequent alias-root passes reuse the
            // authoritative in-process epoch; replaying the import would
            // deliberately invalidate that root-delta path.
            analysis_options.persistent_function_analysis_epoch_import_blob =
                {};
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception&) {
            return reject_candidate(
                LatentAotAnalysisRejection::ProgramInvalid,
                false);
        }
        switch (analysis.termination_reason) {
        case katana::analysis::ControlFlowAnalysisTerminationReason::
            AnalysisIterationBudgetExceeded:
            return reject_candidate(
                LatentAotAnalysisRejection::
                    AnalysisIterationBudgetExceeded);
        case katana::analysis::ControlFlowAnalysisTerminationReason::
            InstructionBudgetExceeded:
            return reject_candidate(
                LatentAotAnalysisRejection::InstructionBudgetExceeded);
        case katana::analysis::ControlFlowAnalysisTerminationReason::
            AnalysisContextBudgetExceeded:
            return reject_candidate(
                LatentAotAnalysisRejection::
                    AnalysisContextBudgetExceeded);
        case katana::analysis::ControlFlowAnalysisTerminationReason::None:
            break;
        }

        std::vector<std::uint32_t> discovered_offsets;
        std::vector<std::uint32_t> demoted_entry_offsets;
        std::vector<std::uint32_t> strict_interior_addresses;
        std::vector<katana::ir::Function> discovery_program;
        bool explicit_tail_prologue_discovered = false;
        bool authoritative_tail_discovered = false;
        std::vector<std::uint32_t> rooted_explicit_tail_offsets;
        std::vector<std::uint32_t> rooted_authoritative_tail_offsets;
        try {
            const auto safepoints =
                katana::ir::architectural_safepoint_block_leaders(analysis);
            discovery_program =
                katana::ir::lower_program(analysis, safepoints);
            if (module_audit != nullptr) {
                const auto audit_resolver = make_latent_code_address_resolver(
                    candidate, discovery_program,
                    options.external_code_targets);
                module_audit->inferred_runtime_base =
                    audit_resolver.preferred_runtime_base.value_or(0u);
                module_audit->inferred_runtime_base_identity_consistent =
                    audit_resolver
                        .preferred_runtime_base_identity_consistent;
                module_audit->analyzed_function_offsets.clear();
                module_audit->analyzed_function_offsets.reserve(
                    discovery_program.size());
                for (const auto& function : discovery_program) {
                    if (function.entry_address < candidate.source_address)
                        continue;
                    const auto offset =
                        function.entry_address - candidate.source_address;
                    if (offset < candidate.bytes.size())
                        module_audit->analyzed_function_offsets.push_back(
                            offset);
                }
                std::sort(
                    module_audit->analyzed_function_offsets.begin(),
                    module_audit->analyzed_function_offsets.end());
                module_audit->analyzed_function_offsets.erase(
                    std::unique(
                        module_audit->analyzed_function_offsets.begin(),
                        module_audit->analyzed_function_offsets.end()),
                    module_audit->analyzed_function_offsets.end());
            }
            strict_interior_addresses =
                latent_program_strict_interior_addresses(
                    discovery_program);
            const auto explicit_tail_prologue_offsets =
                latent_explicit_tail_prologue_entry_offsets(
                    candidate, discovery_program);
            for (const auto offset : explicit_tail_prologue_offsets) {
                if (!std::binary_search(analysis_entry_offsets.begin(),
                                        analysis_entry_offsets.end(),
                                        offset))
                    discovered_offsets.push_back(offset);
            }
            explicit_tail_prologue_discovered =
                !discovered_offsets.empty();
            rooted_explicit_tail_offsets = discovered_offsets;
            // Loader epilogues are a separate, source-rooted proof family.
            // Run this strict IR proof before demotion and before the other
            // discovery lanes so it cannot disappear behind another family.
            const auto authoritative_tail_offsets =
                latent_runtime_alias_call_entry_offsets(
                    candidate, discovery_program,
                    options.external_code_targets,
                    options.maximum_entry_scan_instructions,
                    authoritative_tail_roots, true, pass,
                    module_audit != nullptr
                        ? &module_audit->loader_tail_diagnostics
                        : nullptr);
            for (const auto offset : authoritative_tail_offsets) {
                if (!std::binary_search(analysis_entry_offsets.begin(),
                                        analysis_entry_offsets.end(), offset)) {
                    discovered_offsets.push_back(offset);
                    authoritative_tail_roots.push_back(offset);
                    rooted_authoritative_tail_offsets.push_back(offset);
                    authoritative_tail_discovered = true;
                }
            }
            std::sort(authoritative_tail_roots.begin(),
                      authoritative_tail_roots.end());
            authoritative_tail_roots.erase(
                std::unique(authoritative_tail_roots.begin(),
                            authoritative_tail_roots.end()),
                authoritative_tail_roots.end());
            for (const auto offset : analysis_entry_offsets) {
                const auto entry_address =
                    candidate.source_address + offset;
                // A backward tail transfer does not by itself turn an exact
                // callback root into an interior entry.  Demotion is valid
                // only when another retained function already owns the entry
                // instruction.  Otherwise removing the root loses its prefix
                // blocks even though the earlier shared tail body remains
                // locally reachable from that function.
                if (latent_function_root_reaches_before_entry(
                        discovery_program, entry_address) &&
                    std::binary_search(strict_interior_addresses.begin(),
                                       strict_interior_addresses.end(),
                                       entry_address))
                    demoted_entry_offsets.push_back(offset);
            }
            if (demoted_entry_offsets.empty()) {
                discovered_offsets = latent_runtime_alias_call_entry_offsets(
                    candidate, discovery_program,
                    options.external_code_targets,
                    options.maximum_entry_scan_instructions,
                    authoritative_tail_roots);
                // Preserve the independent loader-tail proof when the
                // general alias families are merged into the same pass.
                discovered_offsets.insert(
                    discovered_offsets.end(),
                    rooted_explicit_tail_offsets.begin(),
                    rooted_explicit_tail_offsets.end());
                discovered_offsets.insert(
                    discovered_offsets.end(),
                    rooted_authoritative_tail_offsets.begin(),
                    rooted_authoritative_tail_offsets.end());
                const auto indexed_call_table_offsets =
                    latent_indexed_call_table_entry_offsets(
                        candidate, discovery_program,
                        options.external_code_targets,
                        options.maximum_entry_scan_instructions);
                discovered_offsets.insert(
                    discovered_offsets.end(),
                    indexed_call_table_offsets.begin(),
                    indexed_call_table_offsets.end());
                const auto callback_resolution =
                    resolve_latent_external_callbacks(
                        candidate, discovery_program,
                        options.external_code_targets,
                        options.external_callback_sinks,
                        options.maximum_entry_scan_instructions);
                discovered_offsets.insert(
                    discovered_offsets.end(),
                    callback_resolution.local_entry_offsets.begin(),
                    callback_resolution.local_entry_offsets.end());
                const auto record_table_callback_resolution =
                    resolve_latent_external_callback_record_tables(
                        candidate, discovery_program,
                        options.external_code_targets,
                        options.external_data_targets,
                        options.external_callback_record_tables,
                        options.maximum_entry_scan_instructions);
                discovered_offsets.insert(
                    discovered_offsets.end(),
                    record_table_callback_resolution.local_entry_offsets.begin(),
                    record_table_callback_resolution.local_entry_offsets.end());
                const auto record_callback_offsets =
                    latent_record_callback_entry_offsets(
                        candidate, discovery_program,
                        options.external_code_targets,
                        options.external_persistent_pointer_sinks,
                        options.external_callback_field_sinks,
                        callback_resolution.local_record_entry_offsets,
                        options.maximum_entry_scan_instructions);
                discovered_offsets.insert(
                    discovered_offsets.end(),
                    record_callback_offsets.begin(),
                    record_callback_offsets.end());
                const auto mutual_record_callback_offsets =
                    latent_mutual_record_table_callback_entry_offsets(
                        candidate, discovery_program,
                        options.external_code_targets,
                        options.external_callback_field_sinks,
                        options.maximum_entry_scan_instructions);
                discovered_offsets.insert(
                    discovered_offsets.end(),
                    mutual_record_callback_offsets.begin(),
                    mutual_record_callback_offsets.end());
                const auto local_descriptor_callback_offsets =
                    latent_local_persisted_descriptor_callback_entry_offsets(
                        candidate, discovery_program,
                        options.external_code_targets,
                        options.maximum_entry_scan_instructions);
                discovered_offsets.insert(
                    discovered_offsets.end(),
                    local_descriptor_callback_offsets.begin(),
                    local_descriptor_callback_offsets.end());
                const auto descriptor_callback_offsets =
                    latent_persisted_descriptor_callback_entry_offsets(
                        candidate, discovery_program,
                        options.external_code_targets,
                        options.external_persistent_pointer_sinks,
                        options.external_callback_field_sinks,
                        options.maximum_entry_scan_instructions);
                discovered_offsets.insert(
                    discovered_offsets.end(),
                    descriptor_callback_offsets.begin(),
                    descriptor_callback_offsets.end());
                std::sort(discovered_offsets.begin(),
                          discovered_offsets.end());
                discovered_offsets.erase(
                    std::unique(discovered_offsets.begin(),
                                discovered_offsets.end()),
                    discovered_offsets.end());

                // All lanes above except the source-rooted loader-tail
                // families are heuristic/transitive. Keep exact roots hard,
                // but discard an unauthenticated proposal before it can make
                // random module data part of the authoritative CFA graph.
                std::set<std::uint32_t> known_function_entries;
                for (const auto& function : discovery_program)
                    known_function_entries.insert(function.entry_address);
                const auto source_rooted = [&](const std::uint32_t offset) {
                    return std::binary_search(
                               rooted_explicit_tail_offsets.begin(),
                               rooted_explicit_tail_offsets.end(), offset) ||
                           std::binary_search(
                               rooted_authoritative_tail_offsets.begin(),
                               rooted_authoritative_tail_offsets.end(),
                               offset);
                };
                discovered_offsets.erase(
                    std::remove_if(
                        discovered_offsets.begin(),
                        discovered_offsets.end(),
                        [&](const auto offset) {
                            return !source_rooted(offset) &&
                                   !latent_entry_has_bounded_complete_local_control_flow(
                                       candidate,
                                       candidate.source_address + offset,
                                       options.maximum_entry_scan_instructions,
                                       known_function_entries);
                        }),
                    discovered_offsets.end());
            }
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception&) {
            return reject_candidate(
                LatentAotAnalysisRejection::ProgramInvalid);
        }
        // Every discovery family ultimately proposes an architectural entry,
        // but a separately rooted analysis can lose the contextual
        // `is_delay_slot` marker. Reject the physical slot from the
        // authenticated module bytes before it can become an entry point or
        // an external IR-dispatch requirement. The owning transfer remains
        // analyzable and no target-set completeness is inferred here.
        const auto contains_physical_delay_slot =
            [&](const std::vector<std::uint32_t>& offsets) {
                return std::any_of(
                    offsets.begin(), offsets.end(),
                    [&](const auto offset) {
                        return latent_candidate_entry_is_physical_delay_slot(
                            candidate, offset);
                    });
            };
        // Heuristic/transitive discovery may conservatively discard a false
        // root. An identity-bound explicit or authoritative root is a
        // provenance conflict instead: silently deleting it would make the
        // published root set differ from the caller's authenticated contract.
        if (contains_physical_delay_slot(rooted_explicit_tail_offsets) ||
            contains_physical_delay_slot(rooted_authoritative_tail_offsets) ||
            contains_physical_delay_slot(authoritative_tail_roots))
            return reject_candidate(
                LatentAotAnalysisRejection::ProgramInvalid);
        const auto discard_physical_delay_slots =
            [&](std::vector<std::uint32_t>& offsets) {
                offsets.erase(
                    std::remove_if(
                        offsets.begin(), offsets.end(),
                        [&](const auto offset) {
                            return latent_candidate_entry_is_physical_delay_slot(
                                candidate, offset);
                        }),
                    offsets.end());
            };
        discard_physical_delay_slots(discovered_offsets);
        if (explicit_tail_prologue_discovered ||
            authoritative_tail_discovered) {
            if (analysis_entry_offsets.size() >
                    options.maximum_functions_per_module ||
                discovered_offsets.size() >
                    options.maximum_functions_per_module -
                        analysis_entry_offsets.size())
                return reject_candidate(
                    LatentAotAnalysisRejection::FunctionBudgetExceeded);
            for (const auto offset : discovered_offsets)
                image.add_entry_point(candidate.source_address + offset);
            merge_entry_offsets(analysis_entry_offsets, discovered_offsets);
            continue;
        }
        if (!demoted_entry_offsets.empty()) {
            // An authoritative callback/continuation address must remain a
            // dispatch requirement, but it is not a second ABI function when
            // its lowered graph reaches backward into another owner. Rebuild
            // the root set without those entries; the final EntryBlockMissing
            // gate still requires every original candidate entry to be
            // materialized by the retained owner.
            if (demoted_entry_offsets.size() == analysis_entry_offsets.size())
                return reject_candidate(
                    LatentAotAnalysisRejection::ProgramInvalid);
            merge_entry_offsets(non_function_entry_offsets,
                                demoted_entry_offsets);
            analysis_entry_offsets.erase(
                std::remove_if(
                    analysis_entry_offsets.begin(),
                    analysis_entry_offsets.end(),
                    [&](const auto offset) {
                        return std::binary_search(
                            demoted_entry_offsets.begin(),
                            demoted_entry_offsets.end(), offset);
                    }),
                analysis_entry_offsets.end());
            std::vector<std::uint32_t> retained_entry_addresses;
            retained_entry_addresses.reserve(analysis_entry_offsets.size());
            for (const auto offset : analysis_entry_offsets)
                retained_entry_addresses.push_back(
                    candidate.source_address + offset);
            image.replace_entry_points(retained_entry_addresses);
            continue;
        }
        discovered_offsets.erase(
            std::remove_if(
                discovered_offsets.begin(), discovered_offsets.end(),
                [&](const auto offset) {
                    if (std::binary_search(analysis_entry_offsets.begin(),
                                           analysis_entry_offsets.end(),
                                           offset))
                        return true;
                    if (std::binary_search(non_function_entry_offsets.begin(),
                                           non_function_entry_offsets.end(),
                                           offset))
                        return true;
                    // A locally discovered callback value which names an
                    // instruction already owned by the current reachable
                    // program is not an independent function root. Re-seeding
                    // it would analyze backward loops under a second ABI
                    // context and can produce divergent IR owners. Exact
                    // candidate entries were installed before this fixpoint;
                    // inferred interior labels therefore remain fail-closed.
                    return std::binary_search(
                        strict_interior_addresses.begin(),
                        strict_interior_addresses.end(),
                        candidate.source_address + offset);
                }),
            discovered_offsets.end());
        if (discovered_offsets.empty()) {
            stable_discovery_program = std::move(discovery_program);
            runtime_alias_entry_fixpoint_complete = true;
            break;
        }
        if (analysis_entry_offsets.size() >
                options.maximum_functions_per_module ||
            discovered_offsets.size() >
                options.maximum_functions_per_module -
                    analysis_entry_offsets.size())
            return reject_candidate(
                LatentAotAnalysisRejection::FunctionBudgetExceeded);
        for (const auto offset : discovered_offsets)
            image.add_entry_point(candidate.source_address + offset);
        merge_entry_offsets(analysis_entry_offsets, discovered_offsets);
    }
    if (!runtime_alias_entry_fixpoint_complete)
        return reject_candidate(
            LatentAotAnalysisRejection::AnalysisIterationBudgetExceeded);
    // analysis_entry_offsets is the authoritative output of the complete
    // local discovery fixpoint.  The input candidate remains the cache-key
    // identity, but publication must retain both its configured dispatch
    // entries (including any demoted interior continuations) and every newly
    // discovered function root.  Reusing candidate.entry_offsets here used
    // to analyze loader tails successfully and then silently drop them from
    // the prepared module and generated dispatch.
    auto published_entry_offsets = candidate_public_roots(
        candidate, stable_discovery_program);
    merge_entry_offsets(published_entry_offsets, analysis_entry_offsets);

    // Positive Guarded-AOT inventory is already canonicalized, structurally
    // validated and bound to the immutable segment generation by CFA.  The
    // optimizer therefore materializes each guest address as an external
    // BlockEntry below.  The loaded-module binder uses entry_offsets as its
    // exact ingress bitmap, so omitting the same addresses here compiled the
    // callback/vtable targets but rejected them at runtime with an entry-
    // identity miss.  Publish every in-module positive entry as one global
    // family; typed rejections and merely conservative candidates never
    // appear in analysis.guarded_aot_entries and cannot cross this boundary.
    std::vector<std::uint32_t> guarded_entry_offsets;
    guarded_entry_offsets.reserve(analysis.guarded_aot_entries.size());
    const auto module_begin =
        static_cast<std::uint64_t>(candidate.source_address);
    const auto module_end = module_begin + candidate.bytes.size();
    for (const auto& guarded : analysis.guarded_aot_entries) {
        const auto address =
            static_cast<std::uint64_t>(guarded.guest_address);
        if (address < module_begin || address >= module_end) continue;
        const auto offset = static_cast<std::uint32_t>(address - module_begin);
        if (guarded.source_identity != candidate.byte_identity ||
            guarded.source_byte_offset != offset ||
            (guarded.entry_byte_extent != 2u &&
             guarded.entry_byte_extent != 4u) ||
            guarded.entry_byte_extent > candidate.bytes.size() - offset)
            return reject_candidate(
                LatentAotAnalysisRejection::ProgramInvalid);
        const auto entry_bytes = std::string_view(
            reinterpret_cast<const char*>(candidate.bytes.data() + offset),
            guarded.entry_byte_extent);
        if (guarded.entry_byte_identity !=
            "sha256:" + katana::io::sha256_bytes(entry_bytes))
            return reject_candidate(
                LatentAotAnalysisRejection::ProgramInvalid);
        guarded_entry_offsets.push_back(offset);
    }
    merge_entry_offsets(published_entry_offsets, guarded_entry_offsets);
    // Discovery may independently recognize an exact block as a function
    // root.  An explicit complete-disassembly ControlFlowTarget is a stricter
    // publication contract: retain the compiled block, but never expose it as
    // a callable loaded-module ingress merely because a later heuristic or
    // guarded inventory rediscovered the same address.
    remove_authority_block_only_roots(
        published_entry_offsets, candidate.authority_entries);
    if (module_audit != nullptr)
        module_audit->final_entry_offsets = published_entry_offsets;
    control_flow_progress.complete(
        analysis.fixpoint_iterations,
        analysis.termination_reason ==
                katana::analysis::ControlFlowAnalysisTerminationReason::None &&
            !analysis.function_budget_exhausted);
    if (!complete_native_graph(
            analysis, exact_runtime_only_stop_on_miss,
            resolution_retention_limit_reason.load(
                std::memory_order_relaxed)))
    {
        const bool inventory_complete =
            katana::analysis::guarded_aot_inventory_complete(analysis);
        const bool terminal_inventory_candidate_values =
            !inventory_complete &&
            analysis.guarded_code_inventory_walk
                .inventory_candidate_values_truncated;
        return reject_candidate(
            inventory_complete
                ? LatentAotAnalysisRejection::ControlFlowIncomplete
                : LatentAotAnalysisRejection::InventoryTruncated,
            true,
            inventory_complete
                ? incomplete_native_graph_summary(analysis)
                : guarded_aot_inventory_loss_summary(analysis),
            terminal_inventory_candidate_values);
    }

    // Hardware classification must use the same identity-bound relocation
    // view as latent control-flow discovery.  Without this late audit alias,
    // PC-relative data accesses inside a module synthesized at 0x88... are
    // canonicalized as 0x08... apertures even though the loaded module lives
    // in ordinary 0x8c... title RAM.  Keep the alias audit-only and add it
    // after the analysis fixed point: inferred load bases remain positive
    // evidence and never become authoritative CFG edges or runtime bindings.
    const auto audit_address_resolver = make_latent_code_address_resolver(
        candidate, stable_discovery_program,
        options.external_code_targets);
    if (audit_address_resolver.preferred_runtime_base.has_value() &&
        *audit_address_resolver.preferred_runtime_base !=
            candidate.source_address &&
        (!candidate.proven_runtime_base.has_value() ||
         *candidate.proven_runtime_base !=
             *audit_address_resolver.preferred_runtime_base)) {
        try {
            image.add_address_alias(
                {candidate.source_address,
                 *audit_address_resolver.preferred_runtime_base,
                 candidate.size});
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception&) {
            return reject_candidate(
                LatentAotAnalysisRejection::ProgramInvalid);
        }
    }

    auto hardware_audit =
        katana::analysis::audit_dreamcast_hardware(image, analysis);
    hardware_audit.scope = "latent-aot-module";
    std::vector<katana::ir::Function> program;
    std::vector<katana::ir::Function> static_program;
    std::vector<katana::ir::ExternalDispatchEntry>
        external_dispatch_entries;
    try {
        const auto architectural_safepoints =
            katana::ir::architectural_safepoint_block_leaders(
                analysis);
        static_program = stable_discovery_program;
        program = std::move(stable_discovery_program);
        if (program.empty())
            throw std::runtime_error("latent-aot-stable-program-missing");
        std::set<std::uint32_t> dispatch_block_entries;
        for (const auto offset : published_entry_offsets)
            dispatch_block_entries.insert(
                candidate.source_address + offset);
        // Authority entries are all block inventory, but only callable
        // entries are present in published_entry_offsets/public roots.
        for (const auto offset : candidate.entry_offsets)
            dispatch_block_entries.insert(candidate.source_address + offset);
        dispatch_block_entries.insert(
            architectural_safepoints.begin(),
            architectural_safepoints.end());
        for (const auto& guarded : analysis.guarded_aot_entries) {
            if (guarded.guest_address != 0u)
                dispatch_block_entries.insert(
                    guarded.guest_address);
            if (guarded.shared_body_address != 0u)
                dispatch_block_entries.insert(
                    guarded.shared_body_address);
        }
        external_dispatch_entries.reserve(
            dispatch_block_entries.size());
        for (const auto address : dispatch_block_entries)
            external_dispatch_entries.push_back(
                {address,
                 katana::ir::ExternalDispatchEntryKind::BlockEntry});
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        return reject_candidate(
            LatentAotAnalysisRejection::ProgramInvalid);
    }
    // Preserve the source-bound, pre-optimization graph only after the full
    // finalizer accepts it.  A rejected candidate must never poison the
    // in-process replay state with an incomplete or merely diagnostic graph.
    auto static_hardware_audit = hardware_audit;
    auto static_dispatch_entries = external_dispatch_entries;
    auto analyzed = finalize_candidate_program(
        candidate,
        published_entry_offsets,
        std::move(program),
        std::move(hardware_audit),
        options,
        external_dispatch_entries);
    if (analyzed.module && static_state != nullptr) {
        static_state->resolver_contract =
            resolver_contract_from_options(options);
        static_state->program = std::move(static_program);
        static_state->published_entry_offsets = published_entry_offsets;
        static_state->analysis_entry_offsets = analysis_entry_offsets;
        static_state->non_function_entry_offsets =
            non_function_entry_offsets;
        static_state->authoritative_tail_roots = authoritative_tail_roots;
        static_state->external_dispatch_entries =
            std::move(static_dispatch_entries);
        static_state->hardware_audit = std::move(static_hardware_audit);
        static_state->preferred_runtime_base =
            audit_address_resolver.preferred_runtime_base;
        static_state->preferred_runtime_base_identity_consistent =
            audit_address_resolver
                .preferred_runtime_base_identity_consistent;
    }
    return analyzed;
}

struct LatentAotCachedStaticExpansion final {
    std::vector<std::uint32_t> additional_entry_offsets;
    std::vector<std::uint32_t> additional_authoritative_tail_roots;
};

// Replaying a static graph is valid only when the new resolver wave is a
// monotonic extension and every newly proven local callback/table target is
// already represented by an emitted block.  A target that would require a
// new CFA root is deliberately reported as a cache miss; the caller then
// executes the existing cold candidate pipeline and replaces the session
// state with the larger graph.
std::optional<LatentAotCachedStaticExpansion>
inspect_cached_static_candidate(
    const DiscFileCandidate& candidate,
    const LatentAotDiscoveryOptions& options,
    const LatentAotStaticCandidateState& state) {
    const auto current_contract = resolver_contract_from_options(options);
    if (!resolver_contract_is_monotonic_superset(
            state.resolver_contract, current_contract) ||
        state.program.empty() ||
        !source_bound_unoptimized_program(candidate, state.program))
        return std::nullopt;

    const auto current_resolver = make_latent_code_address_resolver(
        candidate, state.program, options.external_code_targets);
    if (current_resolver.preferred_runtime_base !=
            state.preferred_runtime_base ||
        current_resolver.preferred_runtime_base_identity_consistent !=
            state.preferred_runtime_base_identity_consistent)
        return std::nullopt;

    if (state.resolver_contract == current_contract)
        return LatentAotCachedStaticExpansion{};
    if (state.key.authority_entries.size() >
            maximum_complete_disassembly_entries ||
        std::any_of(state.key.authority_entries.begin(),
                    state.key.authority_entries.end(), [](const auto& entry) {
                        return entry.byte_size == 0u ||
                               entry.byte_identity.empty();
                    }))
        return std::nullopt;

    const auto strict_interior_addresses =
        latent_program_strict_interior_addresses(state.program);
    std::set<std::uint32_t> emitted_block_entries;
    for (const auto& function : state.program)
        for (const auto& block : function.blocks)
            emitted_block_entries.insert(block.start_address);

    LatentAotCachedStaticExpansion expansion;
    std::vector<std::uint32_t> authoritative_tail_roots =
        state.authoritative_tail_roots;
    std::sort(authoritative_tail_roots.begin(),
              authoritative_tail_roots.end());
    authoritative_tail_roots.erase(
        std::unique(authoritative_tail_roots.begin(),
                    authoritative_tail_roots.end()),
        authoritative_tail_roots.end());

    const auto validate_cached_offset =
        [&](const std::uint32_t offset,
            const bool require_function_entry = false)
        -> std::optional<bool> {
        if ((offset & 1u) != 0u || offset > candidate.bytes.size() ||
            candidate.bytes.size() - offset < sizeof(std::uint16_t) ||
            static_cast<std::uint64_t>(candidate.source_address) + offset >
                std::numeric_limits<std::uint32_t>::max())
            return std::nullopt;
        const auto entry_address = candidate.source_address + offset;
        if (!emitted_block_entries.contains(entry_address))
            return std::nullopt;
        if (require_function_entry &&
            std::none_of(
                state.program.begin(), state.program.end(),
                [&](const auto& function) {
                    return function.entry_address == entry_address;
                }))
            return std::nullopt;
        if (std::binary_search(state.analysis_entry_offsets.begin(),
                               state.analysis_entry_offsets.end(), offset))
            return false;
        if (std::binary_search(authoritative_tail_roots.begin(),
                               authoritative_tail_roots.end(), offset))
            return false;
        // Cold discovery may demote a callback entry or reject an interior
        // label.  A newly authoritative tail naming either shape is a root
        // set deviation, not a safe replay no-op: fall back and let the cold
        // demotion/closure ordering decide it.
        if (std::binary_search(state.non_function_entry_offsets.begin(),
                               state.non_function_entry_offsets.end(),
                               offset) ||
            std::binary_search(strict_interior_addresses.begin(),
                               strict_interior_addresses.end(),
                               entry_address))
            return std::nullopt;
        return true;
    };

    std::vector<std::uint32_t> discovered_offsets;
    const auto append_offsets = [&](const std::vector<std::uint32_t>& values) {
        discovered_offsets.insert(discovered_offsets.end(), values.begin(),
                                  values.end());
    };
    try {
        // Cold discovery runs the strict authoritative-tail lane before the
        // general alias lanes and feeds every newly accepted root back into
        // the next CFA pass.  Replay must reach the same root fixpoint; one
        // stale pass would miss a chained loader epilogue made visible by an
        // additive external-code declaration.
        bool authoritative_tail_fixpoint = false;
        for (std::size_t pass = 0u;
             pass < maximum_latent_aot_runtime_alias_entry_passes; ++pass) {
            const auto authoritative_tail_offsets =
                latent_runtime_alias_call_entry_offsets(
                    candidate, state.program,
                    options.external_code_targets,
                    options.maximum_entry_scan_instructions,
                    authoritative_tail_roots, true, pass);
            bool roots_changed = false;
            for (const auto offset : authoritative_tail_offsets) {
                const auto accepted = validate_cached_offset(offset, true);
                if (!accepted.has_value()) return std::nullopt;
                if (!*accepted) continue;
                authoritative_tail_roots.push_back(offset);
                expansion.additional_authoritative_tail_roots.push_back(
                    offset);
                expansion.additional_entry_offsets.push_back(offset);
                discovered_offsets.push_back(offset);
                roots_changed = true;
            }
            if (!roots_changed) {
                authoritative_tail_fixpoint = true;
                break;
            }
            std::sort(authoritative_tail_roots.begin(),
                      authoritative_tail_roots.end());
            authoritative_tail_roots.erase(
                std::unique(authoritative_tail_roots.begin(),
                            authoritative_tail_roots.end()),
                authoritative_tail_roots.end());
        }
        if (!authoritative_tail_fixpoint) return std::nullopt;

        append_offsets(latent_runtime_alias_call_entry_offsets(
            candidate, state.program, options.external_code_targets,
            options.maximum_entry_scan_instructions,
            authoritative_tail_roots));
        append_offsets(latent_indexed_call_table_entry_offsets(
            candidate, state.program, options.external_code_targets,
            options.maximum_entry_scan_instructions));
        const auto callback_resolution =
            resolve_latent_external_callbacks(
                candidate, state.program,
                options.external_code_targets,
                options.external_callback_sinks,
                options.maximum_entry_scan_instructions);
        append_offsets(callback_resolution.local_entry_offsets);
        append_offsets(resolve_latent_external_callback_record_tables(
                           candidate, state.program,
                           options.external_code_targets,
                           options.external_data_targets,
                           options.external_callback_record_tables,
                           options.maximum_entry_scan_instructions)
                           .local_entry_offsets);
        append_offsets(latent_record_callback_entry_offsets(
            candidate, state.program, options.external_code_targets,
            options.external_persistent_pointer_sinks,
            options.external_callback_field_sinks,
            callback_resolution.local_record_entry_offsets,
            options.maximum_entry_scan_instructions));
        append_offsets(latent_mutual_record_table_callback_entry_offsets(
            candidate, state.program, options.external_code_targets,
            options.external_callback_field_sinks,
            options.maximum_entry_scan_instructions));
        append_offsets(latent_local_persisted_descriptor_callback_entry_offsets(
            candidate, state.program, options.external_code_targets,
            options.maximum_entry_scan_instructions));
        append_offsets(latent_persisted_descriptor_callback_entry_offsets(
            candidate, state.program, options.external_code_targets,
            options.external_persistent_pointer_sinks,
            options.external_callback_field_sinks,
            options.maximum_entry_scan_instructions));
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        return std::nullopt;
    }

    std::sort(discovered_offsets.begin(), discovered_offsets.end());
    discovered_offsets.erase(
        std::unique(discovered_offsets.begin(), discovered_offsets.end()),
        discovered_offsets.end());

    for (const auto offset : discovered_offsets) {
        const auto accepted = validate_cached_offset(offset);
        if (!accepted.has_value()) return std::nullopt;
        if (!*accepted)
            continue;
        expansion.additional_entry_offsets.push_back(offset);
    }
    std::sort(expansion.additional_entry_offsets.begin(),
              expansion.additional_entry_offsets.end());
    expansion.additional_entry_offsets.erase(
        std::unique(expansion.additional_entry_offsets.begin(),
                    expansion.additional_entry_offsets.end()),
        expansion.additional_entry_offsets.end());
    std::sort(expansion.additional_authoritative_tail_roots.begin(),
              expansion.additional_authoritative_tail_roots.end());
    expansion.additional_authoritative_tail_roots.erase(
        std::unique(expansion.additional_authoritative_tail_roots.begin(),
                    expansion.additional_authoritative_tail_roots.end()),
        expansion.additional_authoritative_tail_roots.end());
    // Adding a new published entry is not a resolver-only operation: cold
    // discovery installs it in ExecutableImage and reruns CFA, which may
    // change function ownership, block leaders and FVA context.  An emitted
    // block alone cannot prove that those products are unchanged.  Keep the
    // authoritative-tail proof above (including its propagated fixpoint),
    // but take the required cold path for every non-empty root delta.
    if (!expansion.additional_entry_offsets.empty()) return std::nullopt;
    if (state.analysis_entry_offsets.size() >
            options.maximum_functions_per_module ||
        expansion.additional_entry_offsets.size() >
            options.maximum_functions_per_module -
                state.analysis_entry_offsets.size())
        return std::nullopt;
    return expansion;
}

CandidateAnalysisOutcome finalize_cached_static_candidate(
    const DiscFileCandidate& candidate,
    const LatentAotDiscoveryOptions& options,
    const LatentAotStaticCandidateState& state,
    const LatentAotCachedStaticExpansion& expansion) {
    auto published_entry_offsets = state.published_entry_offsets;
    merge_entry_offsets(published_entry_offsets,
                        expansion.additional_entry_offsets);
    remove_authority_block_only_roots(
        published_entry_offsets, candidate.authority_entries);

    auto external_dispatch_entries = state.external_dispatch_entries;
    for (const auto offset : published_entry_offsets)
        external_dispatch_entries.push_back(
            {candidate.source_address + offset,
             katana::ir::ExternalDispatchEntryKind::BlockEntry});
    std::sort(external_dispatch_entries.begin(),
              external_dispatch_entries.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.address, left.kind) <
                         std::tie(right.address, right.kind);
              });
    external_dispatch_entries.erase(
        std::unique(external_dispatch_entries.begin(),
                    external_dispatch_entries.end(),
                    [](const auto& left, const auto& right) {
                        return left.address == right.address &&
                               left.kind == right.kind;
                    }),
        external_dispatch_entries.end());

    return finalize_candidate_program(
        candidate,
        published_entry_offsets,
        state.program,
        state.hardware_audit,
        options,
        external_dispatch_entries);
}

LatentAotAnalysisCacheKeyInputs candidate_cache_key_inputs(
    const DiscFileCandidate& candidate,
    const LatentAotDiscoveryOptions& options) {
    LatentAotAnalysisCacheKeyInputs inputs;
    inputs.byte_sha256 = candidate.byte_identity.substr(7u);
    inputs.byte_size = candidate.size;
    inputs.entry_offsets = candidate.entry_offsets;
    inputs.exact_candidate =
        candidate_has_authoritative_entries(candidate);
    inputs.source_address = candidate.source_address;
    inputs.maximum_entry_scan_instructions =
        options.maximum_entry_scan_instructions;
    inputs.maximum_native_instructions =
        options.maximum_native_instructions_per_module;
    inputs.maximum_blocks =
        options.maximum_blocks_per_module;
    inputs.maximum_functions =
        options.maximum_functions_per_module;
    inputs.maximum_analysis_iterations =
        options.maximum_analysis_iterations;
    inputs.maximum_analysis_contexts =
        options.maximum_analysis_contexts;
    inputs.analyzer_abi = katana::analysis::abi_version;
    std::ostringstream external_contract;
    const std::string_view cache_implementation_identity =
        !options.analysis_cache_implementation_identity.empty()
            ? std::string_view{options.analysis_cache_implementation_identity}
            : std::string_view{options.analysis_implementation_identity};
    external_contract << 's'
                      << cache_implementation_identity.size()
                      << ':' << cache_implementation_identity
                      << ';';
    const std::string_view product_implementation_identity =
        !options.ir_product_implementation_identity.empty()
            ? std::string_view{options.ir_product_implementation_identity}
            : cache_implementation_identity;
    external_contract << 'o'
                      << product_implementation_identity.size()
                      << ':' << product_implementation_identity
                      << ';' << 't' << options.external_code_targets.size()
                      << ';';
    for (const auto target : options.external_code_targets)
        external_contract << target << ';';
    if (!options.external_data_targets.empty()) {
        external_contract << 'd' << options.external_data_targets.size()
                          << ';';
        for (const auto target : options.external_data_targets)
            external_contract << target << ';';
    }
    external_contract << 'c' << options.external_callback_sinks.size()
                      << ';';
    for (const auto& sink : options.external_callback_sinks)
        external_contract << sink.function_address << ':'
                          << +sink.argument_mask << ':'
                          << +sink.record_argument_mask << ';';
    external_contract << 'p'
                      << options.external_persistent_pointer_sinks.size()
                      << ';';
    for (const auto& sink : options.external_persistent_pointer_sinks)
        external_contract << sink.function_address << ':'
                          << +sink.argument_mask << ';';
    external_contract << 'f'
                      << options.external_callback_field_sinks.size()
                      << ';';
    for (const auto& sink : options.external_callback_field_sinks)
        external_contract << sink.function_address << ':'
                          << sink.call_instruction_address << ':'
                          << sink.load_instruction_address << ':'
                          << sink.displacement << ':' << +sink.width << ':'
                          << sink.call << ':'
                          << +sink.receiver_argument_mask << ';';
    if (!options.external_callback_record_tables.empty()) {
        external_contract << 'r'
                          << options.external_callback_record_tables.size()
                          << ';';
        for (const auto& table : options.external_callback_record_tables)
            external_contract << table.function_address << ':'
                              << table.call_instruction_address << ':'
                              << table.callback_load_instruction_address
                              << ':' << table.callback_sink_address << ':'
                              << table.header_table_pointer_displacement
                              << ':' << table.record_stride << ':'
                              << table.callback_displacement << ':'
                              << +table.callback_argument << ':'
                              << +table.width << ';';
    }
    inputs.analyzer_implementation_id =
        std::string(latent_aot_analysis_implementation_id) + "-" +
        katana::io::sha256_bytes(external_contract.str());
    return inputs;
}

std::string candidate_epoch_cache_key(
    const DiscFileCandidate& candidate,
    const LatentAotDiscoveryOptions& options) {
    std::ostringstream material;
    const auto append_field =
        [&material](const std::string_view value) {
            material << 's' << value.size() << ':' << value << ';';
        };
    const auto append_value =
        [&material](const auto value) {
            material << 'i' << +value << ';';
        };
    append_field("katana-latent-persistent-function-analysis-epoch-key-v2");
    append_value(katana::analysis::abi_version);
    append_value(katana::build_contract::aot_runtime_abi_version);
    append_field(options.analysis_implementation_identity);
    append_field(candidate.byte_identity);
    append_value(candidate.size);
    append_value(candidate.source_address);
    append_value(latent_aot_analysis_address_layout_schema);
    append_value(candidate_has_authoritative_entries(candidate));
    auto entry_offsets = candidate.entry_offsets;
    std::sort(entry_offsets.begin(), entry_offsets.end());
    entry_offsets.erase(
        std::unique(entry_offsets.begin(), entry_offsets.end()),
        entry_offsets.end());
    append_value(entry_offsets.size());
    for (const auto entry : entry_offsets)
        append_value(entry);
    append_value(options.external_code_targets.size());
    for (const auto target : options.external_code_targets)
        append_value(target);
    append_value(options.external_data_targets.size());
    for (const auto target : options.external_data_targets)
        append_value(target);
    append_value(options.external_callback_sinks.size());
    for (const auto& sink : options.external_callback_sinks) {
        append_value(sink.function_address);
        append_value(sink.argument_mask);
        append_value(sink.record_argument_mask);
    }
    append_value(options.external_persistent_pointer_sinks.size());
    for (const auto& sink : options.external_persistent_pointer_sinks) {
        append_value(sink.function_address);
        append_value(sink.argument_mask);
    }
    append_value(options.external_callback_field_sinks.size());
    for (const auto& sink : options.external_callback_field_sinks) {
        append_value(sink.function_address);
        append_value(sink.call_instruction_address);
        append_value(sink.load_instruction_address);
        append_value(sink.displacement);
        append_value(sink.width);
        append_value(sink.call);
        append_value(sink.receiver_argument_mask);
    }
    append_value(options.external_callback_record_tables.size());
    for (const auto& table : options.external_callback_record_tables) {
        append_value(table.function_address);
        append_value(table.call_instruction_address);
        append_value(table.callback_load_instruction_address);
        append_value(table.callback_sink_address);
        append_value(table.header_table_pointer_displacement);
        append_value(table.record_stride);
        append_value(table.callback_displacement);
        append_value(table.callback_argument);
        append_value(table.width);
    }
    append_value(options.maximum_entry_scan_instructions);
    append_value(options.maximum_native_instructions_per_module);
    append_value(options.maximum_analysis_iterations);
    append_value(options.maximum_analysis_contexts);
    return katana::io::sha256_bytes(material.str());
}

CandidateAnalysisOutcome analyze_candidate(
    const DiscFileCandidate& candidate,
    const LatentAotDiscoveryOptions& options,
    CodegenCache* const cache,
    CandidateAnalysisCacheCounters& counters,
    const katana::ProgressReporter& progress_reporter,
    LatentAotStaticCandidateCache* const static_cache = nullptr,
    std::mutex* const static_cache_mutex = nullptr,
    std::size_t* const static_cache_retained_bytes = nullptr,
    std::atomic_size_t* const static_cache_budget_skips = nullptr) {
    constexpr std::string_view epoch_artifact_name{
        "function-analysis-epoch.bin"};
    std::string epoch_cache_key;
    std::string epoch_import_blob;
    if (cache != nullptr &&
        !options.analysis_implementation_identity.empty()) {
        try {
            epoch_cache_key =
                candidate_epoch_cache_key(candidate, options);
            if (auto stored = cache->load_bounded(
                    epoch_cache_key,
                    epoch_artifact_name,
                    katana::analysis::
                        maximum_persistent_function_analysis_epoch_blob_bytes))
                epoch_import_blob = std::move(*stored);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception&) {
            epoch_cache_key.clear();
            epoch_import_blob.clear();
        }
    }

    std::string persistent_epoch_identity;
    if (!epoch_import_blob.empty())
        persistent_epoch_identity =
            "sha256:" + katana::io::sha256_bytes(epoch_import_blob);
    const auto static_key = make_static_candidate_key(
        candidate, options, persistent_epoch_identity);
    std::string persistent_static_key;
    std::string persistent_static_observed_payload;
    if (!candidate_source_shape_rejection(candidate, options)) {
        std::shared_ptr<LatentAotStaticCandidateState> state;
        if (static_cache != nullptr && static_cache_mutex != nullptr)
            state = find_static_candidate_state(
                *static_cache, *static_cache_mutex, static_key);
        if (state == nullptr && cache != nullptr &&
            options.module_static_cache_enabled) {
            try {
                persistent_static_key =
                    static_candidate_cache_key(static_key);
                const auto loaded = cache->load_bounded_state(
                    persistent_static_key,
                    latent_aot_module_static_cache_artifact,
                    maximum_latent_aot_module_static_cache_artifact_bytes);
                if (loaded.state == CodegenCacheLoadState::Hit) {
                    persistent_static_observed_payload = loaded.content;
                    auto parsed = parse_static_candidate_state(
                        persistent_static_key, loaded.content);
                    if (parsed.state != nullptr &&
                        parsed.state->key == static_key) {
                        state = std::move(parsed.state);
                        state->persistent_cache_key =
                            persistent_static_key;
                        state->persistent_observed_payload =
                            persistent_static_observed_payload;
                        state->imported_from_persistent_cache = true;
                        if (static_cache != nullptr &&
                            static_cache_mutex != nullptr &&
                            static_cache_retained_bytes != nullptr) {
                            store_static_candidate_state(
                                *static_cache, *static_cache_mutex, state,
                                *static_cache_retained_bytes,
                                static_cache_budget_skips);
                        }
                    } else {
                        counters.module_static_misses.fetch_add(
                            1u, std::memory_order_relaxed);
                        if (parsed.corrupt)
                            counters.module_static_corrupt_entries.fetch_add(
                                1u, std::memory_order_relaxed);
                    }
                } else {
                    counters.module_static_misses.fetch_add(
                        1u, std::memory_order_relaxed);
                    if (loaded.state != CodegenCacheLoadState::Missing)
                        counters.module_static_corrupt_entries.fetch_add(
                            1u, std::memory_order_relaxed);
                }
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const std::exception&) {
                counters.module_static_misses.fetch_add(
                    1u, std::memory_order_relaxed);
                counters.module_static_corrupt_entries.fetch_add(
                    1u, std::memory_order_relaxed);
            }
        }
        if (state != nullptr) {
            const bool persistent_replay =
                state->imported_from_persistent_cache;
            const auto current_contract =
                resolver_contract_from_options(options);
            if (state->terminal_inventory_candidate_values &&
                state->resolver_contract == current_contract) {
                counters.session_terminal_negative_hits.fetch_add(
                    1u, std::memory_order_relaxed);
                return reject_candidate(
                    LatentAotAnalysisRejection::InventoryTruncated,
                    true,
                    state->terminal_rejection_detail,
                    true);
            }
            std::optional<LatentAotCachedStaticExpansion> expansion;
            if (!state->terminal_inventory_candidate_values)
                expansion = inspect_cached_static_candidate(
                    candidate, options, *state);
            if (expansion) {
                auto replayed = finalize_cached_static_candidate(
                    candidate, options, *state, *expansion);
                if (replayed.module) {
                    if (persistent_replay)
                        counters.module_static_hits.fetch_add(
                            1u, std::memory_order_relaxed);
                    else
                        counters.session_reuse_hits.fetch_add(
                            1u, std::memory_order_relaxed);
                    std::unique_lock<std::mutex> lock;
                    if (static_cache_mutex != nullptr)
                        lock = std::unique_lock<std::mutex>(*static_cache_mutex);
                    state->resolver_contract = current_contract;
                    merge_entry_offsets(
                        state->published_entry_offsets,
                        expansion->additional_entry_offsets);
                    merge_entry_offsets(
                        state->analysis_entry_offsets,
                        expansion->additional_entry_offsets);
                    merge_entry_offsets(
                        state->authoritative_tail_roots,
                        expansion->additional_authoritative_tail_roots);
                    for (const auto offset :
                         expansion->additional_entry_offsets)
                        state->external_dispatch_entries.push_back(
                            {candidate.source_address + offset,
                             katana::ir::ExternalDispatchEntryKind::BlockEntry});
                    std::sort(
                        state->external_dispatch_entries.begin(),
                        state->external_dispatch_entries.end(),
                        [](const auto& left, const auto& right) {
                            return std::tie(left.address, left.kind) <
                                   std::tie(right.address, right.kind);
                        });
                    state->external_dispatch_entries.erase(
                        std::unique(
                            state->external_dispatch_entries.begin(),
                            state->external_dispatch_entries.end(),
                            [](const auto& left, const auto& right) {
                                return left.address == right.address &&
                                       left.kind == right.kind;
                            }),
                        state->external_dispatch_entries.end());

                    if (static_cache != nullptr &&
                        static_cache_retained_bytes != nullptr) {
                        const auto retained_state = std::find_if(
                            static_cache->begin(), static_cache->end(),
                            [&](const auto& candidate_state) {
                                return candidate_state.get() == state.get();
                            });
                        if (retained_state == static_cache->end()) {
                            if (lock.owns_lock()) lock.unlock();
                            return replayed;
                        }
                        const auto previous_retained =
                            state->retained_bytes;
                        const auto updated_retained =
                            estimate_static_candidate_state_bytes(*state);
                        const bool retained_invariant =
                            *static_cache_retained_bytes <=
                            maximum_latent_aot_session_static_cache_bytes;
                        const bool previous_retained_valid =
                            previous_retained <=
                            *static_cache_retained_bytes;
                        const auto retained_without_state =
                            previous_retained_valid
                                ? *static_cache_retained_bytes -
                                      previous_retained
                                : maximum_latent_aot_session_static_cache_bytes;
                        const bool update_fits =
                            retained_invariant && previous_retained_valid &&
                            updated_retained <=
                                maximum_latent_aot_session_static_cache_bytes &&
                            retained_without_state <=
                                maximum_latent_aot_session_static_cache_bytes -
                                    updated_retained;
                        if (update_fits) {
                            state->retained_bytes = updated_retained;
                            *static_cache_retained_bytes =
                                retained_without_state + updated_retained;
                        } else {
                            // Never retain a partially updated graph.  The
                            // current replay result is already finalized and
                            // remains valid; discard this complete cache entry
                            // so the next resolver wave falls back cold.
                            const auto erased = std::find_if(
                                static_cache->begin(), static_cache->end(),
                                [&](const auto& candidate_state) {
                                    return candidate_state.get() == state.get();
                                });
                            if (erased != static_cache->end()) {
                                if (previous_retained_valid)
                                    *static_cache_retained_bytes -=
                                        previous_retained;
                                else
                                    *static_cache_retained_bytes = 0u;
                                static_cache->erase(erased);
                            }
                            if (static_cache_budget_skips != nullptr)
                                static_cache_budget_skips->fetch_add(
                                    1u, std::memory_order_relaxed);
                        }
                    }
                    if (lock.owns_lock()) lock.unlock();
                    if (options.persistent_cache_writes_enabled &&
                        cache != nullptr) {
                        try {
                            static_cast<void>(publish_static_candidate_state_now(
                                *cache, *state, counters));
                        } catch (const std::bad_alloc&) {
                            throw;
                        } catch (const std::exception&) {
                            // A cache publication failure cannot invalidate
                            // the freshly revalidated replay product.
                        }
                    }
                    return replayed;
                }
            }
            if (persistent_replay)
                counters.module_static_cold_fallbacks.fetch_add(
                    1u, std::memory_order_relaxed);
            else
                counters.session_cold_fallbacks.fetch_add(
                    1u, std::memory_order_relaxed);
        }
    }

    std::string cache_key;
    bool cache_entry_absent = true;
    if (cache != nullptr &&
        (!options.analysis_cache_implementation_identity.empty() ||
         !options.analysis_implementation_identity.empty())) {
        try {
            cache_key = make_latent_aot_analysis_cache_key(
                candidate_cache_key_inputs(candidate, options));
            const auto artifact = cache->load_bounded(
                cache_key,
                latent_aot_analysis_cache_artifact,
                maximum_latent_aot_analysis_cache_artifact_bytes);
            if (artifact) {
                cache_entry_absent = false;
                auto parsed =
                    parse_latent_aot_analysis_cache(
                        cache_key,
                        std::span<const std::uint8_t>(
                            reinterpret_cast<
                                const std::uint8_t*>(
                                artifact->data()),
                            artifact->size()));
                if (parsed.state ==
                    LatentAotAnalysisCacheState::Negative) {
                    // Only a rejection derived cheaply from the current
                    // source bytes and exact-entry shape may authenticate a
                    // negative artifact. Analysis-derived rejections remain
                    // hints: remove them below and run the complete pipeline.
                    const auto current_rejection =
                        candidate_source_shape_rejection(
                            candidate, options);
                    if (current_rejection &&
                        *current_rejection == parsed.rejection) {
                        counters.negative_hits.fetch_add(
                            1u, std::memory_order_relaxed);
                        return reject_candidate(
                            *current_rejection);
                    }
                } else if (parsed.state ==
                    LatentAotAnalysisCacheState::Positive) {
                    // Current bytes authenticate retained instructions, not
                    // the absence of an omitted FVA/inventory-discovered
                    // function. Positive artifacts are therefore always
                    // removed and reanalyzed through the complete pipeline.
                } else if (
                    parsed.state ==
                    LatentAotAnalysisCacheState::Corrupt) {
                    counters.corrupt_entries.fetch_add(
                        1u, std::memory_order_relaxed);
                }
                cache_entry_absent =
                    options.persistent_cache_writes_enabled &&
                    cache->erase_bounded_if_matches(
                        cache_key,
                        latent_aot_analysis_cache_artifact,
                        *artifact,
                        maximum_latent_aot_analysis_cache_artifact_bytes);
            }
            counters.misses.fetch_add(
                1u, std::memory_order_relaxed);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception&) {
            cache_entry_absent = false;
            counters.corrupt_entries.fetch_add(
                1u, std::memory_order_relaxed);
            counters.misses.fetch_add(
                1u, std::memory_order_relaxed);
        }
    }

    counters.full_pipeline_runs.fetch_add(
        1u, std::memory_order_relaxed);
    std::string published_epoch_identity;
    katana::analysis::PersistentFunctionAnalysisEpochPublishCallback
        epoch_publish_callback;
    if (options.persistent_cache_writes_enabled &&
        cache != nullptr && !epoch_cache_key.empty()) {
        epoch_publish_callback =
            [cache,
             epoch_cache_key,
             &epoch_import_blob,
             &published_epoch_identity,
             epoch_artifact_name](
                const std::span<const std::uint8_t> blob) {
                const auto serialized = std::string_view(
                    reinterpret_cast<const char*>(blob.data()),
                    blob.size());
                // Bind the in-process static state to the epoch produced by
                // this analysis even when the optional persistent-cache write
                // fails.  Leaving the state keyed as the imported E0 after it
                // was derived from E1 would authorize an invalid warm reuse;
                // an E1 key safely misses until E1 is actually persisted.
                published_epoch_identity =
                    "sha256:" + katana::io::sha256_bytes(serialized);
                if (!epoch_import_blob.empty() &&
                    epoch_import_blob != serialized)
                    static_cast<void>(
                        cache->erase_bounded_if_matches(
                            epoch_cache_key,
                            epoch_artifact_name,
                            epoch_import_blob,
                            katana::analysis::
                                maximum_persistent_function_analysis_epoch_blob_bytes));
                cache->store_bounded(
                    epoch_cache_key,
                    epoch_artifact_name,
                    serialized,
                    katana::analysis::
                        maximum_persistent_function_analysis_epoch_blob_bytes);
            };
    }
    std::shared_ptr<LatentAotStaticCandidateState> generated_static_state;
    if ((static_cache != nullptr && static_cache_mutex != nullptr) ||
        (cache != nullptr && options.module_static_cache_enabled)) {
        generated_static_state =
            std::make_shared<LatentAotStaticCandidateState>();
        generated_static_state->key = static_key;
    }
    auto analyzed = analyze_candidate_uncached(
        candidate,
        options,
        progress_reporter,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(
                epoch_import_blob.data()),
            epoch_import_blob.size()),
        std::move(epoch_publish_callback),
        nullptr,
        generated_static_state.get());
    // The callback may publish a newer FVA epoch than the one imported into
    // static_key.  Bind the retained state to the epoch that was actually
    // produced so the immediately following identical fixpoint round can
    // reuse it instead of missing on E0 -> E1 solely because publication
    // happened during this analysis. A failed persistent write leaves an E1
    // state which conservatively misses the still-persisted E0 key.
    if (generated_static_state != nullptr &&
        !published_epoch_identity.empty())
        generated_static_state->key.persistent_epoch_identity =
            std::move(published_epoch_identity);
    if (generated_static_state != nullptr) {
        const bool same_persistent_key =
            generated_static_state->key == static_key &&
            !persistent_static_key.empty();
        generated_static_state->persistent_cache_key =
            same_persistent_key
                ? persistent_static_key
                : static_candidate_cache_key(generated_static_state->key);
        if (same_persistent_key)
            generated_static_state->persistent_observed_payload =
                std::move(persistent_static_observed_payload);
    }
    if (generated_static_state != nullptr &&
        analyzed.terminal_inventory_candidate_values) {
        generated_static_state->resolver_contract =
            resolver_contract_from_options(options);
        generated_static_state->terminal_inventory_candidate_values = true;
        generated_static_state->terminal_rejection_detail =
            analyzed.rejection_detail;
    }
    if ((analyzed.module ||
         analyzed.terminal_inventory_candidate_values) &&
        generated_static_state != nullptr &&
        static_cache != nullptr && static_cache_mutex != nullptr &&
        static_cache_retained_bytes != nullptr)
        store_static_candidate_state(
            *static_cache, *static_cache_mutex,
            generated_static_state,
            *static_cache_retained_bytes,
            static_cache_budget_skips);
    if (analyzed.module && generated_static_state != nullptr &&
        options.module_static_cache_enabled &&
        options.persistent_cache_writes_enabled && cache != nullptr) {
        try {
            static_cast<void>(publish_static_candidate_state_now(
                *cache, *generated_static_state, counters));
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception&) {
            // The authoritative cold result remains valid; cache publication
            // is an optimization and is retried by a later complete run.
        }
    }
    if (!options.persistent_cache_writes_enabled ||
        cache == nullptr || cache_key.empty() ||
        !cache_entry_absent || !analyzed.deterministic)
        return analyzed;
    try {
        if (analyzed.module)
            return analyzed;
        const auto current_rejection =
            candidate_source_shape_rejection(candidate, options);
        if (!current_rejection ||
            *current_rejection != analyzed.rejection)
            return analyzed;
        auto artifact = serialize_latent_aot_negative_cache(
            cache_key, analyzed.rejection);
        if (artifact.size() <=
            maximum_latent_aot_analysis_cache_artifact_bytes) {
            cache->store_bounded(
                cache_key,
                latent_aot_analysis_cache_artifact,
                std::string_view(
                    reinterpret_cast<const char*>(
                        artifact.data()),
                    artifact.size()),
                maximum_latent_aot_analysis_cache_artifact_bytes);
            counters.stores.fetch_add(
                1u, std::memory_order_relaxed);
        }
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        // Cache population is an optimization. The freshly revalidated
        // deterministic analysis remains authoritative.
    }
    return analyzed;
}
} // namespace

namespace {

bool valid_complete_disassembly_component(
    const std::string_view value) noexcept {
    return !value.empty() && value.size() <= 128u &&
           std::all_of(value.begin(), value.end(), [](const char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '-' || character == '_' ||
                      character == '.';
           });
}

void append_complete_disassembly_identity_field(
    std::string& material,
    const std::string_view value) {
    material += std::to_string(value.size());
    material.push_back(':');
    material.append(value);
    material.push_back(';');
}

template <typename Value>
void append_complete_disassembly_identity_number(
    std::string& material,
    const Value value) {
    append_complete_disassembly_identity_field(
        material, std::to_string(value));
}

std::string complete_disassembly_authority_identity_unchecked(
    const CompleteDisassemblyAuthority& authority,
    const bool include_module_class) {
    constexpr std::string_view legacy_identity_domain{
        "katana-complete-disassembly-authority-v1"};
    constexpr std::string_view current_identity_domain{
        "katana-complete-disassembly-authority-v2"};
    const auto identity_domain = include_module_class
        ? current_identity_domain
        : legacy_identity_domain;
    std::string material{identity_domain};
    material.push_back('\0');
    append_complete_disassembly_identity_number(
        material, authority.contract_version);
    append_complete_disassembly_identity_field(material, authority.project_id);
    append_complete_disassembly_identity_field(
        material, authority.project_version);
    append_complete_disassembly_identity_number(
        material, authority.modules.size());
    for (const auto& module : authority.modules) {
        append_complete_disassembly_identity_field(material, module.module_id);
        if (include_module_class)
            append_complete_disassembly_identity_number(
                material, static_cast<unsigned>(module.module_class));
        append_complete_disassembly_identity_number(
            material, static_cast<unsigned>(module.transform));
        append_complete_disassembly_identity_field(
            material, module.encoded_byte_identity);
        append_complete_disassembly_identity_number(
            material, module.disc_byte_offset);
        append_complete_disassembly_identity_number(
            material, module.encoded_byte_size);
        append_complete_disassembly_identity_field(
            material, module.decoded_byte_identity);
        append_complete_disassembly_identity_number(
            material, module.decoded_byte_size);
        append_complete_disassembly_identity_number(
            material, module.source_address);
        append_complete_disassembly_identity_number(
            material, module.runtime_address);
        append_complete_disassembly_identity_field(
            material, module.disassembly_identity);
        append_complete_disassembly_identity_number(
            material, module.entries.size());
        for (const auto& entry : module.entries) {
            append_complete_disassembly_identity_number(
                material, entry.module_relative_offset);
            append_complete_disassembly_identity_number(
                material, entry.byte_size);
            append_complete_disassembly_identity_field(
                material, entry.byte_identity);
            append_complete_disassembly_identity_number(
                material, static_cast<unsigned>(entry.kind));
        }
    }
    return "sha256:" + katana::io::sha256_bytes(material);
}

} // namespace

CompleteDisassemblyAuthority normalize_complete_disassembly_authority(
    CompleteDisassemblyAuthority authority) {
    constexpr std::uint32_t page_size = 4096u;
    constexpr std::uint32_t maximum_entry_probe_bytes = 256u;
    const bool legacy_contract =
        authority.contract_version ==
        complete_disassembly_authority_legacy_contract_version;
    const bool current_contract =
        authority.contract_version ==
        complete_disassembly_authority_contract_version;
    if ((!legacy_contract && !current_contract) ||
        !valid_complete_disassembly_component(authority.project_id) ||
        !valid_complete_disassembly_component(authority.project_version) ||
        authority.modules.empty() ||
        authority.modules.size() > maximum_complete_disassembly_modules)
        throw std::invalid_argument(
            "complete-disassembly-authority-definition");
    if (!authority.authority_identity.empty() &&
        !valid_sha256_identity(authority.authority_identity))
        throw std::invalid_argument(
            "complete-disassembly-authority-identity");

    std::sort(
        authority.modules.begin(), authority.modules.end(),
        [](const auto& left, const auto& right) {
            if (left.module_id != right.module_id)
                return left.module_id < right.module_id;
            if (left.decoded_byte_identity != right.decoded_byte_identity)
                return left.decoded_byte_identity < right.decoded_byte_identity;
            return left.source_address < right.source_address;
        });

    std::size_t total_entries = 0u;
    std::uint64_t total_decoded_bytes = 0u;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> source_ranges;
    source_ranges.reserve(authority.modules.size());
    std::optional<std::string_view> previous_module_id;
    for (auto& module : authority.modules) {
        if (legacy_contract)
            module.module_class = CompleteDisassemblyModuleClass::LatentLoaded;
        const bool valid_module_class =
            module.module_class == CompleteDisassemblyModuleClass::LatentLoaded ||
            module.module_class == CompleteDisassemblyModuleClass::PrimaryStatic ||
            module.module_class == CompleteDisassemblyModuleClass::FixedRuntimeImage;
        const bool valid_transform =
            module.transform == LatentAotSourceTransform::Identity ||
            module.transform == LatentAotSourceTransform::SegaPrs;
        const auto source_end =
            static_cast<std::uint64_t>(module.source_address) +
            module.decoded_byte_size;
        const auto runtime_end =
            static_cast<std::uint64_t>(module.runtime_address) +
            module.decoded_byte_size;
        if (!valid_complete_disassembly_component(module.module_id) ||
            (previous_module_id.has_value() &&
             *previous_module_id == module.module_id) ||
            !valid_transform || !valid_module_class ||
            !valid_sha256_identity(module.encoded_byte_identity) ||
            !valid_sha256_identity(module.decoded_byte_identity) ||
            !valid_sha256_identity(module.disassembly_identity) ||
            module.encoded_byte_size == 0u ||
            module.encoded_byte_size >
                katana::runtime::maximum_native_aot_template_extent ||
            module.decoded_byte_size < 2u ||
            module.decoded_byte_size > 4u * 1024u * 1024u ||
            module.disc_byte_offset % iso_sector_size != 0u ||
            module.disc_byte_offset >
                std::numeric_limits<std::uint64_t>::max() -
                    module.encoded_byte_size ||
            (module.source_address != 0u &&
             (module.source_address & (page_size - 1u)) != 0u) ||
            (module.runtime_address & (page_size - 1u)) != 0u ||
            module.runtime_address == 0u ||
            (module.source_address != 0u &&
             source_end > 0x1'0000'0000ull) ||
            module.runtime_address < latent_aot_main_ram_begin ||
            runtime_end > latent_aot_main_ram_end ||
            module.entries.empty() ||
            (module.transform == LatentAotSourceTransform::Identity &&
             (module.encoded_byte_identity != module.decoded_byte_identity ||
              module.encoded_byte_size != module.decoded_byte_size)))
            throw std::invalid_argument(
                "complete-disassembly-module-definition");
        previous_module_id = module.module_id;
        if (total_decoded_bytes >
            maximum_validated_latent_aot_total_module_bytes -
                module.decoded_byte_size)
            throw std::invalid_argument(
                "complete-disassembly-module-byte-budget");
        total_decoded_bytes += module.decoded_byte_size;
        if (module.entries.size() >
            maximum_complete_disassembly_entries -
                std::min(total_entries,
                         maximum_complete_disassembly_entries))
            throw std::invalid_argument(
                "complete-disassembly-entry-budget");
        total_entries += module.entries.size();
        if (module.source_address != 0u)
            source_ranges.emplace_back(module.source_address, source_end);

        std::sort(
            module.entries.begin(), module.entries.end(),
            [](const auto& left, const auto& right) {
                if (left.module_relative_offset !=
                    right.module_relative_offset)
                    return left.module_relative_offset <
                           right.module_relative_offset;
                if (left.byte_size != right.byte_size)
                    return left.byte_size < right.byte_size;
                if (left.byte_identity != right.byte_identity)
                    return left.byte_identity < right.byte_identity;
                return static_cast<unsigned>(left.kind) <
                       static_cast<unsigned>(right.kind);
            });
        std::optional<std::uint32_t> previous_offset;
        for (const auto& entry : module.entries) {
            const auto entry_end =
                static_cast<std::uint64_t>(entry.module_relative_offset) +
                entry.byte_size;
            const bool valid_kind =
                entry.kind == CompleteDisassemblyEntryKind::DeclaredEntry ||
                entry.kind == CompleteDisassemblyEntryKind::FunctionEntry ||
                entry.kind ==
                    CompleteDisassemblyEntryKind::ControlFlowTarget ||
                entry.kind ==
                    CompleteDisassemblyEntryKind::CodePointerTarget;
            if ((entry.module_relative_offset & 1u) != 0u ||
                entry.byte_size < 2u ||
                entry.byte_size > maximum_entry_probe_bytes ||
                (entry.byte_size & 1u) != 0u ||
                entry_end > module.decoded_byte_size ||
                !valid_sha256_identity(entry.byte_identity) || !valid_kind ||
                (previous_offset.has_value() &&
                 entry.module_relative_offset == *previous_offset))
                throw std::invalid_argument(
                    "complete-disassembly-entry-definition");
            previous_offset = entry.module_relative_offset;
        }
    }

    std::sort(source_ranges.begin(), source_ranges.end());
    for (std::size_t index = 1u; index < source_ranges.size(); ++index) {
        if (source_ranges[index].first < source_ranges[index - 1u].second)
            throw std::invalid_argument(
                "complete-disassembly-source-overlap");
    }

    const auto supplied_identity = authority.authority_identity;
    authority.authority_identity =
        complete_disassembly_authority_identity_unchecked(
            authority, current_contract);
    if (!supplied_identity.empty() &&
        supplied_identity != authority.authority_identity)
        throw std::invalid_argument(
            "complete-disassembly-authority-identity-mismatch");
    return authority;
}

std::string complete_disassembly_authority_identity(
    const CompleteDisassemblyAuthority& authority) {
    auto copy = authority;
    copy.authority_identity.clear();
    return normalize_complete_disassembly_authority(std::move(copy))
        .authority_identity;
}

std::vector<LatentAotEntryHint>
complete_disassembly_coverage_entry_hints(
    const CompleteDisassemblyAuthority& authority) {
    auto normalized = normalize_complete_disassembly_authority(authority);
    std::vector<LatentAotEntryHint> hints;
    std::size_t count = 0u;
    for (const auto& module : normalized.modules) {
        if (module.entries.size() > maximum_prepared_latent_aot_entry_hints -
                std::min(count, maximum_prepared_latent_aot_entry_hints))
            throw std::invalid_argument("complete-disassembly-entry-hint-budget");
        count += module.entries.size();
    }
    hints.reserve(count);
    for (const auto& module : normalized.modules) {
        for (const auto& entry : module.entries) {
            hints.push_back(
                {module.decoded_byte_identity,
                 module.disc_byte_offset,
                 module.encoded_byte_size,
                 entry.module_relative_offset,
                 module.source_address,
                 module.runtime_address,
                 entry.kind,
                 module.module_class,
                 entry.byte_size,
                 entry.byte_identity});
        }
    }
    return hints;
}

struct LatentAotDiscoverySession::Impl final {
    struct CatalogStatistics final {
        std::size_t examined_files = 0u;
        std::size_t rejected_files = 0u;
        std::size_t duplicate_files = 0u;
        std::uint64_t examined_bytes = 0u;
        std::size_t prs_files_examined = 0u;
        std::size_t prs_files_decoded = 0u;
        std::size_t prs_files_rejected = 0u;
        std::size_t prs_candidates_admitted = 0u;
        std::uint64_t prs_decoded_bytes = 0u;
        bool prs_decoded_budget_exhausted = false;
    } catalog_statistics;

    // A successful catalog build is distinct from a non-empty candidate
    // vector.  An empty, but valid, ISO catalog must be reusable; conversely,
    // a failed build must never become a reusable empty catalog.
    bool catalog_ready = false;
    std::shared_ptr<const katana::runtime::DiscSource> source;
    std::string catalog_key;
    std::vector<std::pair<std::string, katana::runtime::Iso9660Entry>> files;
    std::vector<DiscFileCandidate> candidates;
    std::vector<bool> candidates_have_explicit_entries;
    LatentAotStaticCandidateCache static_candidates;
    std::size_t static_candidate_cache_retained_bytes = 0u;

    void clear() noexcept {
        catalog_ready = false;
        source.reset();
        catalog_key.clear();
        files.clear();
        candidates.clear();
        candidates_have_explicit_entries.clear();
        static_candidates.clear();
        static_candidate_cache_retained_bytes = 0u;
        catalog_statistics = {};
    }
};

LatentAotDiscoverySession::LatentAotDiscoverySession()
    : impl_(std::make_unique<Impl>()) {}

LatentAotDiscoverySession::~LatentAotDiscoverySession() = default;

LatentAotDiscoverySession::LatentAotDiscoverySession(
    LatentAotDiscoverySession&&) noexcept = default;

LatentAotDiscoverySession& LatentAotDiscoverySession::operator=(
    LatentAotDiscoverySession&&) noexcept = default;

void LatentAotDiscoverySession::reset() noexcept {
    if (impl_ != nullptr) impl_->clear();
}

std::vector<LatentAotModuleStaticCachePublication>
stage_latent_aot_module_static_cache_publications(
    const LatentAotDiscoverySession& session,
    const LatentAotDiscoveryOptions& options) {
    std::vector<LatentAotModuleStaticCachePublication> publications;
    if (session.impl_ == nullptr || !options.module_static_cache_enabled ||
        options.analysis_cache_root.empty() ||
        (options.analysis_implementation_identity.empty() &&
         options.analysis_cache_implementation_identity.empty()))
        return publications;
    if (session.impl_->static_candidates.size() >
        maximum_latent_aot_session_static_cache_entries)
        throw std::runtime_error(
            "Latent-AOT-Modulcache ueberschreitet sein Publikationsbudget.");
    publications.reserve(session.impl_->static_candidates.size());
    for (const auto& state : session.impl_->static_candidates) {
        if (state == nullptr || state->program.empty() ||
            state->terminal_inventory_candidate_values)
            continue;
        std::vector<std::uint8_t> bytes;
        try {
            bytes = serialize_static_candidate_state(*state);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& error) {
            // Module-static persistence is an optimization over an already
            // completed, source-bound cold analysis.  The immediate-write
            // path follows the same rule: an owner whose complete state is
            // not representable by the bounded cache codec remains a cold
            // owner and must not invalidate the authoritative analysis
            // product.  Keep global publication invariants (entry budget and
            // duplicate keys) fail-closed below.
            std::fprintf(
                stderr,
                "KATANA_LATENT_AOT_MODULE_STATIC_STAGE_SKIP "
                "identity=%s byte_size=%u retained_bytes=%zu "
                "functions=%zu reason=%s\n",
                state->key.byte_identity.c_str(),
                state->key.byte_size,
                state->retained_bytes,
                state->program.size(),
                error.what());
            continue;
        }
        std::string validated(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (validated == state->persistent_observed_payload) continue;
        publications.push_back(
            {options.analysis_cache_root,
             state->persistent_cache_key.empty()
                 ? static_candidate_cache_key(state->key)
                 : state->persistent_cache_key,
             state->persistent_observed_payload,
             std::move(validated)});
    }
    std::sort(publications.begin(), publications.end(),
              [](const auto& left, const auto& right) {
                  return left.cache_key < right.cache_key;
              });
    if (std::adjacent_find(
            publications.begin(), publications.end(),
            [](const auto& left, const auto& right) {
                return left.cache_key == right.cache_key;
            }) != publications.end())
        throw std::runtime_error(
            "Latent-AOT-Modulcache besitzt doppelte Publikationskeys.");
    return publications;
}

bool publish_latent_aot_module_static_cache_publications(
    std::vector<LatentAotModuleStaticCachePublication>& pending) noexcept {
    bool complete = true;
    auto current = pending.begin();
    while (current != pending.end()) {
        bool published = false;
        try {
            CodegenCache cache(current->cache_root);
            const auto observed = cache.load_bounded_state(
                current->cache_key,
                latent_aot_module_static_cache_artifact,
                maximum_latent_aot_module_static_cache_artifact_bytes);
            if (observed.state == CodegenCacheLoadState::Hit &&
                observed.content == current->validated_payload) {
                published = true;
            } else if (!current->observed_payload.empty()) {
                if (observed.state == CodegenCacheLoadState::Hit &&
                    observed.content == current->observed_payload)
                    published = cache.replace_bounded_if_matches(
                        current->cache_key,
                        latent_aot_module_static_cache_artifact,
                        current->observed_payload,
                        current->validated_payload,
                        maximum_latent_aot_module_static_cache_artifact_bytes);
            } else if (observed.state != CodegenCacheLoadState::Hit) {
                cache.store_bounded(
                    current->cache_key,
                    latent_aot_module_static_cache_artifact,
                    current->validated_payload,
                    maximum_latent_aot_module_static_cache_artifact_bytes);
                published = true;
            }
            if (published) {
                const auto verified = cache.load_bounded(
                    current->cache_key,
                    latent_aot_module_static_cache_artifact,
                    maximum_latent_aot_module_static_cache_artifact_bytes);
                published = verified.has_value() &&
                            *verified == current->validated_payload;
            }
        } catch (const std::exception&) {
            published = false;
        }
        if (published)
            current = pending.erase(current);
        else {
            complete = false;
            ++current;
        }
    }
    return complete;
}

std::string_view latent_aot_loader_tail_audit_status_name(
    const LatentAotLoaderTailAuditStatus status) noexcept {
    switch (status) {
    case LatentAotLoaderTailAuditStatus::RuntimeBaseMissing:
        return "runtime-base-missing";
    case LatentAotLoaderTailAuditStatus::RootOutOfRange:
        return "root-out-of-range";
    case LatentAotLoaderTailAuditStatus::RootFunctionMissing:
        return "root-function-missing";
    case LatentAotLoaderTailAuditStatus::TerminalDelaySlotMissing:
        return "terminal-delay-slot-missing";
    case LatentAotLoaderTailAuditStatus::TerminalRegisterJumpMissing:
        return "terminal-register-jump-missing";
    case LatentAotLoaderTailAuditStatus::PrRestoreMissing:
        return "pr-restore-missing";
    case LatentAotLoaderTailAuditStatus::EpilogueInvalid:
        return "epilogue-invalid";
    case LatentAotLoaderTailAuditStatus::TargetLiteralMissing:
        return "target-literal-missing";
    case LatentAotLoaderTailAuditStatus::TargetUnresolved:
        return "target-unresolved";
    case LatentAotLoaderTailAuditStatus::RuntimeBaseIdentityMismatch:
        return "runtime-base-identity-mismatch";
    case LatentAotLoaderTailAuditStatus::TargetOutOfRange:
        return "target-out-of-range";
    case LatentAotLoaderTailAuditStatus::TargetControlFlowMissing:
        return "target-control-flow-missing";
    case LatentAotLoaderTailAuditStatus::Accepted:
        return "accepted";
    }
    return "unknown";
}

namespace {

LatentAotModuleAuditResult audit_latent_aot_module_impl(
    const std::span<const std::uint8_t> decoded_bytes,
    const std::uint32_t source_address,
    const std::span<const std::uint32_t> entry_offsets,
    const LatentAotDiscoveryOptions& options,
    const LatentAotSourceTransform source_transform,
    const std::string_view source_byte_identity,
    const std::uint32_t source_byte_size,
    const std::optional<std::uint32_t> proven_runtime_base) {
    if (decoded_bytes.empty() ||
        decoded_bytes.size() >
            katana::runtime::maximum_native_aot_template_extent ||
        decoded_bytes.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        (source_address & 3u) != 0u ||
        static_cast<std::uint64_t>(source_address) +
                decoded_bytes.size() >
            0x1'0000'0000ull ||
        entry_offsets.empty() ||
        (proven_runtime_base.has_value() &&
         !valid_latent_runtime_base(
             *proven_runtime_base,
             static_cast<std::uint32_t>(decoded_bytes.size()))))
        throw std::invalid_argument(
            "Latenter Modulaudit besitzt ungueltige Grenzen.");

    LatentAotModuleAuditResult result;
    result.byte_size = static_cast<std::uint32_t>(decoded_bytes.size());
    result.source_address = source_address;
    result.initial_entry_offsets.assign(
        entry_offsets.begin(), entry_offsets.end());
    std::sort(result.initial_entry_offsets.begin(),
              result.initial_entry_offsets.end());
    result.initial_entry_offsets.erase(
        std::unique(result.initial_entry_offsets.begin(),
                    result.initial_entry_offsets.end()),
        result.initial_entry_offsets.end());
    if (std::any_of(
            result.initial_entry_offsets.begin(),
            result.initial_entry_offsets.end(),
            [&](const auto offset) {
                return (offset & 1u) != 0u ||
                       offset > decoded_bytes.size() ||
                       decoded_bytes.size() - offset <
                           sizeof(std::uint16_t);
            }))
        throw std::invalid_argument(
            "Latenter Modulaudit besitzt einen ungueltigen Entry-Offset.");

    const auto byte_view = std::string_view(
        reinterpret_cast<const char*>(decoded_bytes.data()),
        decoded_bytes.size());
    result.byte_identity = "sha256:" + katana::io::sha256_bytes(byte_view);
    DiscFileCandidate candidate;
    candidate.size = result.byte_size;
    candidate.source_address = source_address;
    candidate.bytes.assign(decoded_bytes.begin(), decoded_bytes.end());
    candidate.byte_identity = result.byte_identity;
    candidate.source_bindings.push_back(
        {"module-audit-source",
         source_transform,
         std::string(source_byte_identity),
         0u,
         source_byte_size});
    candidate.entry_offsets = result.initial_entry_offsets;
    candidate.explicit_entry_offsets = result.initial_entry_offsets;
    candidate.proven_runtime_base = proven_runtime_base;

    auto effective_options = options;
    effective_options.mode = LatentAotDiscoveryMode::ExactOnly;
    effective_options.completeness_policy =
        LatentAotCompletenessPolicy::ExactRuntimeOnlyStopOnMiss;
    const auto analyzed = analyze_candidate_uncached(
        candidate,
        effective_options,
        effective_options.progress,
        std::span<const std::uint8_t>{},
        {},
        &result);
    result.admitted = analyzed.module.has_value();
    result.rejection = std::string(
        latent_aot_rejection_name(analyzed.rejection));
    result.rejection_detail = analyzed.rejection_detail;
    if (analyzed.module) {
        result.final_entry_offsets = analyzed.module->entry_offsets;
        result.emitted_block_offsets.reserve(
            analyzed.module->block_identities.size());
        for (const auto& identity : analyzed.module->block_identities)
            result.emitted_block_offsets.push_back(identity.source_offset);
        result.emitted_function_offsets.reserve(
            analyzed.module->function_identities.size());
        for (const auto& identity : analyzed.module->function_identities)
            result.emitted_function_offsets.push_back(identity.source_offset);
    }
    if (result.final_entry_offsets.empty())
        result.final_entry_offsets = result.initial_entry_offsets;
    std::sort(result.loader_tail_diagnostics.begin(),
              result.loader_tail_diagnostics.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(
                             left.analysis_pass,
                             left.root_offset,
                             left.block_offset,
                             left.literal_offset,
                             left.raw_target,
                             left.target_offset,
                             left.status) <
                         std::tie(
                             right.analysis_pass,
                             right.root_offset,
                             right.block_offset,
                             right.literal_offset,
                             right.raw_target,
                             right.target_offset,
                             right.status);
              });
    result.loader_tail_diagnostics.erase(
        std::unique(result.loader_tail_diagnostics.begin(),
                    result.loader_tail_diagnostics.end(),
                    [](const auto& left, const auto& right) {
                        return left.analysis_pass == right.analysis_pass &&
                               left.root_offset == right.root_offset &&
                               left.block_offset == right.block_offset &&
                               left.literal_offset == right.literal_offset &&
                               left.raw_target == right.raw_target &&
                               left.target_offset == right.target_offset &&
                               left.status == right.status;
                    }),
        result.loader_tail_diagnostics.end());
    return result;
}

} // namespace

LatentAotModuleAuditResult audit_latent_aot_module(
    const std::span<const std::uint8_t> decoded_bytes,
    const std::uint32_t source_address,
    const std::span<const std::uint32_t> entry_offsets,
    const LatentAotDiscoveryOptions& options) {
    const auto byte_view = std::string_view(
        reinterpret_cast<const char*>(decoded_bytes.data()),
        decoded_bytes.size());
    return audit_latent_aot_module_impl(
        decoded_bytes, source_address, entry_offsets, options,
        LatentAotSourceTransform::Identity,
        "sha256:" + katana::io::sha256_bytes(byte_view),
        static_cast<std::uint32_t>(decoded_bytes.size()), std::nullopt);
}

LatentAotModuleAuditResult audit_latent_aot_module(
    const std::span<const std::uint8_t> decoded_bytes,
    const std::uint32_t source_address,
    const std::span<const std::uint32_t> entry_offsets,
    const std::uint32_t proven_runtime_base,
    const LatentAotDiscoveryOptions& options) {
    const auto byte_view = std::string_view(
        reinterpret_cast<const char*>(decoded_bytes.data()),
        decoded_bytes.size());
    return audit_latent_aot_module_impl(
        decoded_bytes, source_address, entry_offsets, options,
        LatentAotSourceTransform::Identity,
        "sha256:" + katana::io::sha256_bytes(byte_view),
        static_cast<std::uint32_t>(decoded_bytes.size()),
        proven_runtime_base);
}

LatentAotModuleAuditResult audit_latent_aot_sega_prs_module(
    const std::span<const std::uint8_t> encoded_bytes,
    const std::uint32_t source_address,
    const std::span<const std::uint32_t> entry_offsets,
    const LatentAotDiscoveryOptions& options) {
    if (encoded_bytes.empty() ||
        encoded_bytes.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument(
            "Latenter PRS-Modulaudit besitzt ungueltige Quellgrenzen.");
    const auto source_view = std::string_view(
        reinterpret_cast<const char*>(encoded_bytes.data()),
        encoded_bytes.size());
    const auto source_identity =
        "sha256:" + katana::io::sha256_bytes(source_view);
    const auto decoded_bytes = katana::detail::decompress_sega_prs(
        encoded_bytes,
        katana::runtime::maximum_native_aot_template_extent,
        katana::runtime::maximum_native_aot_template_extent);
    return audit_latent_aot_module_impl(
        decoded_bytes, source_address, entry_offsets, options,
        LatentAotSourceTransform::SegaPrs, source_identity,
        static_cast<std::uint32_t>(encoded_bytes.size()), std::nullopt);
}

LatentAotModuleAuditResult audit_latent_aot_sega_prs_module(
    const std::span<const std::uint8_t> encoded_bytes,
    const std::uint32_t source_address,
    const std::span<const std::uint32_t> entry_offsets,
    const std::uint32_t proven_runtime_base,
    const LatentAotDiscoveryOptions& options) {
    if (encoded_bytes.empty() ||
        encoded_bytes.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument(
            "Latenter PRS-Modulaudit besitzt ungueltige Quellgrenzen.");
    const auto source_view = std::string_view(
        reinterpret_cast<const char*>(encoded_bytes.data()),
        encoded_bytes.size());
    const auto source_identity =
        "sha256:" + katana::io::sha256_bytes(source_view);
    const auto decoded_bytes = katana::detail::decompress_sega_prs(
        encoded_bytes,
        katana::runtime::maximum_native_aot_template_extent,
        katana::runtime::maximum_native_aot_template_extent);
    return audit_latent_aot_module_impl(
        decoded_bytes, source_address, entry_offsets, options,
        LatentAotSourceTransform::SegaPrs, source_identity,
        static_cast<std::uint32_t>(encoded_bytes.size()),
        proven_runtime_base);
}

bool latent_aot_program_is_relocation_closed(
    const std::span<const katana::ir::Function> program,
    const std::uint32_t source_start,
    const std::uint32_t extent) noexcept {
    return extent != 0u &&
           static_cast<std::uint64_t>(source_start) + extent <= 0x1'0000'0000ull &&
           relocation_closed_impl(program, source_start, extent);
}

namespace {

std::string latent_aot_catalog_key(
    const katana::runtime::DiscSource& source,
    const std::uint32_t volume_start_lba,
    const std::uint32_t extent_lba_bias,
    const std::span<const std::string> excluded_byte_identities,
    const LatentAotDiscoveryOptions& options,
    const std::span<const LatentAotOccupiedRange> occupied_source_ranges,
    const std::span<const LatentAotEntryHint> entry_hints,
    const std::span<const std::string> prioritized_file_references) {
    std::ostringstream material;
    const auto append_string = [&material](const std::string_view value) {
        material << 's' << value.size() << ':' << value << ';';
    };
    const auto append_value = [&material](const auto value) {
        material << 'i' << +value << ';';
    };
    append_string("katana-latent-aot-catalog-v3");
    append_string(source.identity());
    append_value(source.size());
    append_value(volume_start_lba);
    append_value(extent_lba_bias);
    append_value(static_cast<std::uint8_t>(options.mode));
    append_value(static_cast<std::uint8_t>(options.completeness_policy));
    append_value(options.maximum_directory_entries);
    append_value(options.maximum_directory_bytes);
    append_value(options.maximum_total_directory_bytes);
    append_value(options.maximum_candidate_files);
    append_value(options.maximum_file_bytes);
    append_value(options.maximum_total_file_bytes);
    append_value(options.maximum_total_transform_source_bytes);
    append_value(options.maximum_total_transformed_bytes);
    append_value(options.maximum_transformed_candidate_files);
    append_value(options.maximum_workers);
    append_value(options.maximum_entry_scan_instructions);
    append_value(options.maximum_native_instructions_per_module);
    append_value(options.maximum_blocks_per_module);
    append_value(options.maximum_functions_per_module);
    append_value(options.maximum_analysis_iterations);
    append_value(options.maximum_analysis_contexts);
    append_value(options.source_address_begin);
    append_value(options.source_address_end);
    append_string(options.analysis_implementation_identity);
    append_string(options.analysis_cache_implementation_identity);
    append_string(options.ir_product_implementation_identity);

    std::vector<std::string> excluded(excluded_byte_identities.begin(),
                                      excluded_byte_identities.end());
    std::sort(excluded.begin(), excluded.end());
    excluded.erase(std::unique(excluded.begin(), excluded.end()),
                   excluded.end());
    append_value(excluded.size());
    for (const auto& identity : excluded) append_string(identity);

    std::vector<LatentAotOccupiedRange> occupied(
        occupied_source_ranges.begin(), occupied_source_ranges.end());
    std::sort(occupied.begin(), occupied.end(), [](const auto left,
                                                   const auto right) {
        return std::tie(left.start, left.size) <
               std::tie(right.start, right.size);
    });
    append_value(occupied.size());
    for (const auto range : occupied) {
        append_value(range.start);
        append_value(range.size);
    }

    append_value(entry_hints.size());
    for (const auto& hint : entry_hints) {
        append_string(hint.byte_identity);
        append_value(hint.disc_byte_offset);
        append_value(hint.byte_size);
        append_value(hint.module_relative_offset);
        append_value(hint.source_address);
        append_value(hint.proven_runtime_base);
    }
    append_value(prioritized_file_references.size());
    for (const auto& reference : prioritized_file_references)
        append_string(reference);
    return katana::io::sha256_bytes(material.str());
}

} // namespace

LatentAotDiscovery discover_latent_aot_modules_impl(
    std::shared_ptr<const katana::runtime::DiscSource> source,
    const std::uint32_t volume_start_lba,
    const std::uint32_t extent_lba_bias,
    const std::span<const std::string> excluded_byte_identities,
    const LatentAotDiscoveryOptions& options,
    const std::span<const LatentAotOccupiedRange> occupied_source_ranges,
    const std::span<const LatentAotEntryHint> entry_hints,
    const std::span<const std::string> prioritized_file_references,
    LatentAotDiscoverySession::Impl* const session) {
    auto discovery_progress = options.progress.begin(
        katana::ProgressOperation::LatentAotAnalysis,
        katana::ProgressUnit::Bytes,
        std::nullopt,
        "latent-aot-discovery");
    bool heuristic_discovery = false;
    switch (options.mode) {
    case LatentAotDiscoveryMode::HintsAndHeuristics:
        heuristic_discovery = true;
        break;
    case LatentAotDiscoveryMode::ExactOnly:
        break;
    default:
        throw std::invalid_argument(
            "Latente AOT-Discovery besitzt einen ungueltigen Modus.");
    }
    switch (options.completeness_policy) {
    case LatentAotCompletenessPolicy::Strict:
    case LatentAotCompletenessPolicy::ExactRuntimeOnlyStopOnMiss:
        break;
    default:
        throw std::invalid_argument(
            "Latente AOT-Discovery besitzt eine ungueltige Vollstaendigkeitspolitik.");
    }
    const auto minimum_candidate_bytes =
        heuristic_discovery ? std::size_t{4u} : std::size_t{2u};
    if (!source || options.maximum_directory_entries == 0u ||
        options.maximum_directory_bytes == 0u ||
        options.maximum_directory_bytes >
            std::numeric_limits<std::uint32_t>::max() ||
        options.maximum_total_directory_bytes < options.maximum_directory_bytes ||
        (heuristic_discovery &&
         (options.maximum_candidate_files == 0u ||
          options.maximum_transformed_candidate_files == 0u)) ||
        options.maximum_file_bytes < minimum_candidate_bytes ||
        options.maximum_file_bytes >
            katana::runtime::maximum_native_aot_template_extent ||
        options.maximum_total_file_bytes < minimum_candidate_bytes ||
        options.maximum_total_transform_source_bytes < 1u ||
        options.maximum_total_transformed_bytes < 2u ||
        options.maximum_workers == 0u ||
        options.maximum_entry_scan_instructions == 0u ||
        options.maximum_native_instructions_per_module == 0u ||
        options.maximum_blocks_per_module == 0u ||
        options.maximum_functions_per_module == 0u ||
        options.maximum_analysis_iterations == 0u ||
        options.maximum_analysis_contexts == 0u ||
        options.analysis_implementation_identity.size() >
            maximum_analysis_implementation_identity_bytes ||
        options.analysis_cache_implementation_identity.size() >
            maximum_analysis_implementation_identity_bytes ||
        options.ir_product_implementation_identity.size() >
            maximum_analysis_implementation_identity_bytes ||
        options.external_code_targets.size() > 65'536u ||
        !std::is_sorted(options.external_code_targets.begin(),
                        options.external_code_targets.end()) ||
        std::adjacent_find(options.external_code_targets.begin(),
                           options.external_code_targets.end()) !=
            options.external_code_targets.end() ||
        std::any_of(options.external_code_targets.begin(),
                    options.external_code_targets.end(),
                    [](const std::uint32_t address) {
                        return (address & 1u) != 0u ||
                               (address >> 29u) != 4u;
                    }) ||
        options.external_data_targets.size() > 65'536u ||
        !std::is_sorted(options.external_data_targets.begin(),
                        options.external_data_targets.end()) ||
        std::adjacent_find(options.external_data_targets.begin(),
                           options.external_data_targets.end()) !=
            options.external_data_targets.end() ||
        std::any_of(options.external_data_targets.begin(),
                    options.external_data_targets.end(),
                    [](const std::uint32_t address) {
                        return (address & 3u) != 0u ||
                               (address >> 29u) != 4u;
                    }) ||
        options.external_callback_sinks.size() > 65'536u ||
        !std::is_sorted(
            options.external_callback_sinks.begin(),
            options.external_callback_sinks.end(),
            [](const auto& left, const auto& right) {
                return left.function_address < right.function_address;
            }) ||
        std::adjacent_find(
            options.external_callback_sinks.begin(),
            options.external_callback_sinks.end(),
            [](const auto& left, const auto& right) {
                return left.function_address == right.function_address;
            }) != options.external_callback_sinks.end() ||
        std::any_of(
            options.external_callback_sinks.begin(),
            options.external_callback_sinks.end(),
            [&](const auto& sink) {
                return sink.argument_mask == 0u ||
                       (sink.argument_mask & 0xf0u) != 0u ||
                       (sink.record_argument_mask & 0xf0u) != 0u ||
                       (sink.record_argument_mask & sink.argument_mask) !=
                           sink.record_argument_mask ||
                       !std::binary_search(
                           options.external_code_targets.begin(),
                           options.external_code_targets.end(),
                           sink.function_address);
            }) ||
        options.external_persistent_pointer_sinks.size() > 65'536u ||
        !std::is_sorted(
            options.external_persistent_pointer_sinks.begin(),
            options.external_persistent_pointer_sinks.end(),
            [](const auto& left, const auto& right) {
                return left.function_address < right.function_address;
            }) ||
        std::adjacent_find(
            options.external_persistent_pointer_sinks.begin(),
            options.external_persistent_pointer_sinks.end(),
            [](const auto& left, const auto& right) {
                return left.function_address == right.function_address;
            }) != options.external_persistent_pointer_sinks.end() ||
        std::any_of(
            options.external_persistent_pointer_sinks.begin(),
            options.external_persistent_pointer_sinks.end(),
            [&](const auto& sink) {
                return sink.argument_mask == 0u ||
                       (sink.argument_mask & 0xf0u) != 0u ||
                       !std::binary_search(
                           options.external_code_targets.begin(),
                           options.external_code_targets.end(),
                           sink.function_address);
            }) ||
        options.external_callback_field_sinks.size() > 65'536u ||
        !std::is_sorted(
            options.external_callback_field_sinks.begin(),
            options.external_callback_field_sinks.end(),
            [](const auto& left, const auto& right) {
                return std::tie(left.function_address,
                                left.call_instruction_address,
                                left.load_instruction_address,
                                left.displacement,
                                left.width,
                                left.call) <
                       std::tie(right.function_address,
                                right.call_instruction_address,
                                right.load_instruction_address,
                                right.displacement,
                                right.width,
                                right.call);
            }) ||
        std::adjacent_find(
            options.external_callback_field_sinks.begin(),
            options.external_callback_field_sinks.end(),
            [](const auto& left, const auto& right) {
                return std::tie(left.function_address,
                                left.call_instruction_address,
                                left.load_instruction_address,
                                left.displacement,
                                left.width,
                                left.call) ==
                       std::tie(right.function_address,
                                right.call_instruction_address,
                                right.load_instruction_address,
                                right.displacement,
                                right.width,
                                right.call);
            }) !=
            options.external_callback_field_sinks.end() ||
        std::any_of(
            options.external_callback_field_sinks.begin(),
            options.external_callback_field_sinks.end(),
            [&](const auto& sink) {
                return sink.width != 4u || sink.displacement < 0 ||
                       (sink.receiver_argument_mask & 0xf0u) != 0u ||
                       (sink.displacement & 3) != 0 ||
                       (sink.function_address & 1u) != 0u ||
                       (sink.call_instruction_address & 1u) != 0u ||
                       (sink.load_instruction_address & 1u) != 0u ||
                       (sink.function_address >> 29u) != 4u ||
                       (sink.call_instruction_address >> 29u) != 4u ||
                       (sink.load_instruction_address >> 29u) != 4u ||
                       !std::binary_search(
                           options.external_code_targets.begin(),
                           options.external_code_targets.end(),
                           sink.function_address);
            }) ||
        options.external_callback_record_tables.size() > 65'536u ||
        !std::is_sorted(
            options.external_callback_record_tables.begin(),
            options.external_callback_record_tables.end(),
            [](const auto& left, const auto& right) {
                return std::tie(
                           left.function_address,
                           left.call_instruction_address,
                           left.callback_load_instruction_address,
                           left.callback_sink_address,
                           left.header_table_pointer_displacement,
                           left.record_stride,
                           left.callback_displacement,
                           left.callback_argument,
                           left.width) <
                       std::tie(
                           right.function_address,
                           right.call_instruction_address,
                           right.callback_load_instruction_address,
                           right.callback_sink_address,
                           right.header_table_pointer_displacement,
                           right.record_stride,
                           right.callback_displacement,
                           right.callback_argument,
                           right.width);
            }) ||
        std::adjacent_find(
            options.external_callback_record_tables.begin(),
            options.external_callback_record_tables.end()) !=
            options.external_callback_record_tables.end() ||
        std::any_of(
            options.external_callback_record_tables.begin(),
            options.external_callback_record_tables.end(),
            [&](const auto& table) {
                const auto p1_even = [](const std::uint32_t address) {
                    return (address & 1u) == 0u &&
                           (address >> 29u) == 4u;
                };
                return table.width != 4u ||
                       table.callback_argument >= 4u ||
                       table.header_table_pointer_displacement < 4 ||
                       (table.header_table_pointer_displacement & 3) != 0 ||
                       table.header_table_pointer_displacement > 4096 ||
                       table.record_stride < 4u ||
                       table.record_stride >
                           maximum_latent_aot_descriptor_table_stride ||
                       (table.record_stride & 3u) != 0u ||
                       table.callback_displacement < 0 ||
                       (table.callback_displacement & 3) != 0 ||
                       static_cast<std::uint64_t>(
                           table.callback_displacement) + 4u >
                           table.record_stride ||
                       !p1_even(table.function_address) ||
                       !p1_even(table.call_instruction_address) ||
                       !p1_even(
                           table.callback_load_instruction_address) ||
                       !p1_even(table.callback_sink_address) ||
                       table.call_instruction_address <
                           table.function_address ||
                       table.callback_load_instruction_address <
                           table.function_address ||
                       !std::binary_search(
                           options.external_code_targets.begin(),
                           options.external_code_targets.end(),
                           table.function_address) ||
                       !std::binary_search(
                           options.external_code_targets.begin(),
                           options.external_code_targets.end(),
                           table.callback_sink_address);
            }) ||
        options.source_address_begin >= options.source_address_end ||
        (options.source_address_begin & 3u) != 0u ||
        (options.source_address_end & 3u) != 0u)
        throw std::invalid_argument("Latente AOT-Discovery besitzt ungueltige Grenzen.");
    if (std::any_of(occupied_source_ranges.begin(),
                    occupied_source_ranges.end(),
                    [](const auto range) { return !valid_linear_physical_range(range); }))
        throw std::invalid_argument("Latente AOT-Discovery besitzt ungueltige belegte Ranges.");
    const auto normalized_entry_hints = normalize_entry_hints(entry_hints);
    const auto normalized_file_references =
        normalize_file_references(prioritized_file_references);
    std::vector<bool> matched_entry_hints(normalized_entry_hints.size(), false);
    if (!heuristic_discovery && normalized_entry_hints.empty()) {
        discovery_progress.skipped();
        return {};
    }

    const auto catalog_key = latent_aot_catalog_key(
        *source, volume_start_lba, extent_lba_bias,
        excluded_byte_identities, options, occupied_source_ranges,
        normalized_entry_hints, normalized_file_references);
    // DiscSource implementations in this tree authenticate their content
    // with a non-empty identity.  Do not retain or reuse a session for a
    // custom source that violates that contract.
    const bool source_identity_valid = !source->identity().empty();
    const bool reuse_catalog =
        source_identity_valid && session != nullptr &&
        session->catalog_ready &&
        session->source.get() == source.get() &&
        session->catalog_key == catalog_key;
    if (session != nullptr && !reuse_catalog) session->clear();

    LatentAotDiscovery result;
    std::unique_ptr<katana::runtime::Iso9660Filesystem> filesystem;
    std::vector<std::pair<std::string, katana::runtime::Iso9660Entry>> files;
    if (reuse_catalog) {
        // The source identity is part of catalog_key.  Moving the bounded
        // metadata out lets the analysis retain one authoritative copy while
        // avoiding a second directory walk and a second metadata allocation.
        session->catalog_ready = false;
        files = std::move(session->files);
        result.examined_files = session->catalog_statistics.examined_files;
        result.rejected_files = session->catalog_statistics.rejected_files;
        result.duplicate_files = session->catalog_statistics.duplicate_files;
        result.examined_bytes = session->catalog_statistics.examined_bytes;
        result.prs_files_examined =
            session->catalog_statistics.prs_files_examined;
        result.prs_files_decoded =
            session->catalog_statistics.prs_files_decoded;
        result.prs_files_rejected =
            session->catalog_statistics.prs_files_rejected;
        result.prs_candidates_admitted =
            session->catalog_statistics.prs_candidates_admitted;
        result.prs_decoded_bytes =
            session->catalog_statistics.prs_decoded_bytes;
        result.prs_decoded_budget_exhausted =
            session->catalog_statistics.prs_decoded_budget_exhausted;
        std::fill(matched_entry_hints.begin(), matched_entry_hints.end(), true);
    } else {
        filesystem = std::make_unique<katana::runtime::Iso9660Filesystem>(
            source, iso_sector_size, volume_start_lba, extent_lba_bias);
        struct PendingDirectory {
            std::string path;
            std::size_t depth = 0u;
            katana::runtime::Iso9660Entry entry;
        };
        std::vector<PendingDirectory> pending{{
            "/", 0u, filesystem->root_directory()}};
        std::size_t directory_entries = 0u;
        std::size_t directory_bytes = 0u;
        while (!pending.empty()) {
            auto directory = std::move(pending.back());
            pending.pop_back();
            if (directory.depth > 32u)
                throw std::runtime_error(
                    "ISO9660-Verzeichnistiefe ueberschreitet das AOT-Budget.");
            if (directory.entry.size > options.maximum_directory_bytes ||
                directory.entry.size >
                    options.maximum_total_directory_bytes - directory_bytes)
                throw std::runtime_error(
                    "ISO9660-Verzeichnisse ueberschreiten das AOT-I/O-Budget.");
            directory_bytes += directory.entry.size;
            auto entries = filesystem->list_directory(
                directory.entry,
                {options.maximum_directory_entries - directory_entries,
                 static_cast<std::uint32_t>(options.maximum_directory_bytes)});
            std::sort(
                entries.begin(), entries.end(),
                [](const auto& left, const auto& right) {
                    if (left.name != right.name) return left.name < right.name;
                    if (left.lba != right.lba) return left.lba < right.lba;
                    return left.size < right.size;
                });
            if (entries.size() >
                options.maximum_directory_entries - directory_entries)
                throw std::runtime_error(
                    "ISO9660-Dateiregistry ueberschreitet das AOT-Budget.");
            directory_entries += entries.size();
            for (const auto& entry : entries) {
                if (!safe_component(entry.name))
                    throw std::runtime_error(
                        "ISO9660-Dateiregistry enthaelt unsicheren Namen.");
                auto path = directory.path;
                if (path.size() != 1u) path += '/';
                path += entry.name;
                if (entry.directory)
                    pending.push_back(
                        {std::move(path), directory.depth + 1u, entry});
                else
                    files.emplace_back(std::move(path), entry);
            }
        }
        std::sort(
            files.begin(), files.end(),
            [](const auto& left, const auto& right) {
                if (left.second.lba != right.second.lba)
                    return left.second.lba < right.second.lba;
                if (left.second.size != right.second.size)
                    return left.second.size < right.second.size;
                return left.first < right.first;
            });
    }
    std::map<std::string, std::size_t> file_basename_counts;
    for (const auto& file : files)
        ++file_basename_counts[std::string(
            disc_basename(normalize_disc_reference(file.first)))];
    std::vector<std::size_t> heuristic_file_order(files.size());
    std::iota(heuristic_file_order.begin(), heuristic_file_order.end(), 0u);
    const auto file_is_referenced = [&](const std::size_t index) {
        if (normalized_file_references.empty()) return false;
        const auto path = normalize_disc_reference(files[index].first);
        if (std::binary_search(normalized_file_references.begin(),
                               normalized_file_references.end(), path))
            return true;
        const auto basename = std::string(disc_basename(path));
        const auto count = file_basename_counts.find(basename);
        if (count == file_basename_counts.end() || count->second != 1u)
            return false;
        return std::any_of(
            normalized_file_references.begin(),
            normalized_file_references.end(),
            [&](const auto& reference) {
                return disc_basename(reference) == basename;
            });
    };
    const auto first_unreferenced = std::stable_partition(
        heuristic_file_order.begin(), heuristic_file_order.end(),
        file_is_referenced);
    // Compressed executable overlays are invisible to the raw opcode shape
    // filter. Examine strict PRS sources before unrelated raw data, while
    // preserving explicit executable-string references as the first tier.
    std::stable_partition(
        first_unreferenced, heuristic_file_order.end(),
        [&](const std::size_t index) {
            return disc_file_uses_sega_prs(files[index].first);
        });
    const auto first_non_prs = std::find_if(
        first_unreferenced, heuristic_file_order.end(),
        [&](const std::size_t index) {
            return !disc_file_uses_sega_prs(files[index].first);
        });
    std::stable_sort(
        first_unreferenced, first_non_prs,
        [&](const std::size_t left, const std::size_t right) {
            if (files[left].second.size != files[right].second.size)
                return files[left].second.size < files[right].second.size;
            return left < right;
        });
    {
        katana::ProgressCounterSnapshot counters;
        counters.discovered = files.size();
        discovery_progress.update(counters);
    }

    std::vector<DiscFileCandidate> candidates;
    std::vector<bool> candidates_have_explicit_entries;
    if (reuse_catalog) {
        candidates = std::move(session->candidates);
        candidates_have_explicit_entries =
            std::move(session->candidates_have_explicit_entries);
    } else {
    candidates.reserve(
        std::min(files.size(), options.maximum_candidate_files) +
        std::min(files.size(), normalized_entry_hints.size()) +
        std::min(files.size(),
                 maximum_latent_aot_inferred_authoritative_candidates));
    candidates_have_explicit_entries.reserve(candidates.capacity());
    std::map<std::pair<std::string, std::uint32_t>, std::size_t>
        candidate_by_byte_identity_and_size;
    const std::set<std::string> excluded_identities(excluded_byte_identities.begin(),
                                                    excluded_byte_identities.end());
    std::set<std::string> known_identities(excluded_byte_identities.begin(),
                                           excluded_byte_identities.end());
    auto next_source = options.source_address_begin;
    std::vector<LatentAotOccupiedRange> occupied(occupied_source_ranges.begin(),
                                                 occupied_source_ranges.end());
    const auto disc_byte_offset_for = [&](const katana::runtime::Iso9660Entry& entry) {
        const auto absolute_lba = static_cast<std::uint64_t>(extent_lba_bias) + entry.lba;
        if (absolute_lba >
            std::numeric_limits<std::uint64_t>::max() / iso_sector_size)
            throw std::overflow_error("Latenter Discdateioffset laeuft ueber.");
        return absolute_lba * iso_sector_size;
    };
    const auto place_candidate = [&](
        DiscFileCandidate candidate,
        const bool explicit_entries,
        const std::optional<std::uint32_t> requested_source_address =
            std::nullopt) {
        if (requested_source_address.has_value()) {
            const auto source_begin =
                static_cast<std::uint64_t>(*requested_source_address);
            const auto source_end = source_begin + candidate.bytes.size();
            if ((*requested_source_address & 0xFFFu) != 0u ||
                source_begin < options.source_address_begin ||
                source_end > options.source_address_end)
                throw std::runtime_error(
                    "latent-aot-entry-hint-source-address-invalid");
            const LatentAotOccupiedRange proposed{
                *requested_source_address, candidate.bytes.size()};
            if (std::any_of(occupied.begin(), occupied.end(),
                            [&](const auto range) {
                                return physical_overlap(proposed, range);
                            }))
                throw std::runtime_error(
                    "latent-aot-entry-hint-source-address-collision");
            candidate.source_address = *requested_source_address;
            occupied.push_back(proposed);
            candidates.push_back(std::move(candidate));
            candidates_have_explicit_entries.push_back(explicit_entries);
            return true;
        }
        bool placed = false;
        next_source = align_up(next_source, 4096u);
        while (static_cast<std::uint64_t>(next_source) + candidate.bytes.size() <=
               options.source_address_end) {
            const LatentAotOccupiedRange proposed{next_source, candidate.bytes.size()};
            if (std::none_of(occupied.begin(), occupied.end(), [&](const auto range) {
                    return physical_overlap(proposed, range);
                })) {
                placed = true;
                break;
            }
            const auto advanced =
                static_cast<std::uint64_t>(next_source) + 4096u;
            if (advanced > std::numeric_limits<std::uint32_t>::max()) break;
            next_source = align_up(static_cast<std::uint32_t>(advanced), 4096u);
        }
        if (!placed) return false;
        const auto source_end =
            static_cast<std::uint64_t>(next_source) + candidate.bytes.size();
        candidate.source_address = next_source;
        occupied.push_back({next_source, candidate.size});
        next_source = static_cast<std::uint32_t>(source_end);
        candidates.push_back(std::move(candidate));
        candidates_have_explicit_entries.push_back(explicit_entries);
        return true;
    };

    std::uint64_t examined_file_bytes = 0u;
    std::uint64_t examined_transform_source_bytes = 0u;
    std::size_t source_binding_count = 0u;
    std::size_t transformed_candidate_bytes = 0u;
    std::set<std::pair<std::uint64_t, std::uint32_t>> exact_file_extents;
    std::vector<std::size_t> exact_hint_file_order(files.size());
    std::iota(exact_hint_file_order.begin(), exact_hint_file_order.end(), 0u);
    std::stable_partition(
        exact_hint_file_order.begin(), exact_hint_file_order.end(),
        [&](const auto index) {
            const auto& entry = files[index].second;
            const auto disc_byte_offset = disc_byte_offset_for(entry);
            return std::any_of(
                normalized_entry_hints.begin(), normalized_entry_hints.end(),
                [&](const auto& hint) {
                    return hint.source_address != 0u &&
                           hint.disc_byte_offset == disc_byte_offset &&
                           hint.byte_size == entry.size;
                });
        });
    // Reserve exact pinned ranges before automatic placement reaches them.
    // Pinned candidates do not advance next_source; processing them first
    // therefore preserves the addresses of every unpinned candidate while
    // making the requested range unavailable to later placement.
    for (const auto file_index : exact_hint_file_order) {
        const auto& file = files[file_index];
        const auto& entry = file.second;
        const auto disc_byte_offset = disc_byte_offset_for(entry);
        std::vector<std::size_t> extent_hint_indices;
        for (std::size_t hint_index = 0u;
             hint_index < normalized_entry_hints.size();
             ++hint_index) {
            const auto& hint = normalized_entry_hints[hint_index];
            // byte_size binds the exact encoded disc extent; for PRS the
            // identity and module-relative bounds are checked after decode.
            if (hint.disc_byte_offset == disc_byte_offset &&
                hint.byte_size == entry.size)
                extent_hint_indices.push_back(hint_index);
        }
        if (extent_hint_indices.empty())
            continue;
        const bool sega_prs = disc_file_uses_sega_prs(file.first);
        if (disc_byte_offset > source->size() ||
            entry.size > source->size() - disc_byte_offset)
            throw std::runtime_error("Latente Discdatei liegt ausserhalb der Discquelle.");
        if (entry.size > options.maximum_file_bytes)
            throw std::runtime_error(
                "latent-aot-entry-hint-file-budget");
        const auto source_budget_used =
            sega_prs ? examined_transform_source_bytes : examined_file_bytes;
        const auto source_budget =
            sega_prs ? options.maximum_total_transform_source_bytes
                     : options.maximum_total_file_bytes;
        if (entry.size > source_budget - source_budget_used)
            throw std::runtime_error(
                "latent-aot-entry-hint-total-budget");
        auto source_bytes = filesystem->read_file(entry, entry.size);
        if (source_bytes.size() != entry.size)
            throw std::runtime_error("Latente Discdatei wurde abgeschnitten gelesen.");
        if (sega_prs)
            examined_transform_source_bytes += source_bytes.size();
        else
            examined_file_bytes += source_bytes.size();
        ++result.examined_files;
        result.examined_bytes += source_bytes.size();
        {
            katana::ProgressCounterSnapshot counters;
            counters.discovered = files.size();
            counters.started = result.examined_files;
            discovery_progress.update(
                result.examined_bytes,
                std::move(counters));
        }
        const auto source_byte_identity =
            "sha256:" + katana::io::sha256_bytes(std::string_view(
                            reinterpret_cast<const char*>(source_bytes.data()),
                            source_bytes.size()));
        auto transform = LatentAotSourceTransform::Identity;
        auto bytes = std::move(source_bytes);
        if (sega_prs) {
            ++result.prs_files_examined;
            try {
                bytes = katana::detail::decompress_sega_prs(
                    bytes, options.maximum_file_bytes,
                    options.maximum_file_bytes);
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const katana::detail::PrsDecodeError&) {
                ++result.prs_files_rejected;
                continue;
            }
            ++result.prs_files_decoded;
            result.prs_decoded_bytes += bytes.size();
            transform = LatentAotSourceTransform::SegaPrs;
            if (bytes.size() >
                options.maximum_total_transformed_bytes -
                    transformed_candidate_bytes)
                throw std::runtime_error(
                    "latent-aot-entry-hint-transformed-budget");
            transformed_candidate_bytes += bytes.size();
        }
        const auto candidate_size = static_cast<std::uint32_t>(bytes.size());
        const auto byte_identity =
            "sha256:" + katana::io::sha256_bytes(std::string_view(
                            reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        std::vector<std::size_t> matching_entry_hints;
        std::vector<std::uint32_t> explicit_entry_offsets;
        std::vector<CompleteDisassemblyEntryAuthority> authority_entries;
        std::optional<CompleteDisassemblyModuleClass> requested_module_class;
        std::optional<std::uint32_t> requested_source_address;
        std::optional<std::uint32_t> requested_runtime_base;
        for (const auto hint_index : extent_hint_indices) {
            const auto& hint = normalized_entry_hints[hint_index];
            if (hint.byte_identity == byte_identity) {
                matching_entry_hints.push_back(hint_index);
                explicit_entry_offsets.push_back(hint.module_relative_offset);
                if (hint.entry_byte_size != 0u) {
                    authority_entries.push_back({
                        hint.module_relative_offset,
                        hint.entry_byte_size,
                        hint.entry_byte_identity,
                        hint.entry_kind});
                    if (requested_module_class.has_value() &&
                        *requested_module_class != hint.module_class)
                        throw std::runtime_error(
                            "latent-aot-entry-hint-module-class-conflict");
                    requested_module_class = hint.module_class;
                }
                if (hint.source_address != 0u) {
                    if (requested_source_address.has_value() &&
                        *requested_source_address != hint.source_address)
                        throw std::runtime_error(
                            "latent-aot-entry-hint-source-address-conflict");
                    requested_source_address = hint.source_address;
                }
                if (hint.proven_runtime_base != 0u) {
                    if (requested_runtime_base.has_value() &&
                        *requested_runtime_base !=
                            hint.proven_runtime_base)
                        throw std::runtime_error(
                            "latent-aot-entry-hint-runtime-base-conflict");
                    requested_runtime_base =
                        hint.proven_runtime_base;
                }
            }
        }
        if (matching_entry_hints.empty() ||
            excluded_identities.contains(byte_identity))
            continue;
        std::sort(explicit_entry_offsets.begin(), explicit_entry_offsets.end());
        explicit_entry_offsets.erase(
            std::unique(explicit_entry_offsets.begin(), explicit_entry_offsets.end()),
            explicit_entry_offsets.end());
        if (std::any_of(
                explicit_entry_offsets.begin(), explicit_entry_offsets.end(),
                [&](const auto offset) {
                    return (offset & 1u) != 0u ||
                           static_cast<std::uint64_t>(offset) + 2u >
                               candidate_size;
                }))
            throw std::runtime_error("latent-aot-entry-hint-offset-invalid");
        require_explicit_entry_prefixes(
            bytes, explicit_entry_offsets,
            options.maximum_entry_scan_instructions);
        if (requested_runtime_base.has_value() &&
            !valid_latent_runtime_base(
                *requested_runtime_base, candidate_size))
            throw std::runtime_error(
                "latent-aot-entry-hint-runtime-base-invalid");
        auto source_binding = make_source_binding(
            transform, source_byte_identity,
            disc_byte_offset, entry.size);
        auto entry_offsets = explicit_entry_offsets;
        if (heuristic_discovery) {
            auto inferred_entry_offsets =
                sega_prs ? latent_prefix_entry_table_offsets(bytes)
                         : std::vector<std::uint32_t>{};
            const bool inferred_authoritative_entry_table =
                !inferred_entry_offsets.empty();
            if (inferred_entry_offsets.empty())
                inferred_entry_offsets.push_back(0u);
            DiscFileCandidate inferred_candidate{
                candidate_size,
                0u,
                std::move(bytes),
                byte_identity,
                {source_binding},
                inferred_entry_offsets,
                {},
                requested_runtime_base,
                inferred_authoritative_entry_table};
            const bool inferred_candidate_valid =
                !candidate_source_shape_rejection(
                    inferred_candidate, options).has_value();
            bytes = std::move(inferred_candidate.bytes);
            if (inferred_candidate_valid)
                merge_entry_offsets(entry_offsets,
                                    inferred_entry_offsets);
        }
        const auto candidate_key =
            std::pair{byte_identity, candidate_size};
        const auto existing =
            candidate_by_byte_identity_and_size.find(candidate_key);
        if (existing != candidate_by_byte_identity_and_size.end()) {
            auto& candidate = candidates[existing->second];
            if (candidate.size != candidate_size)
                throw std::runtime_error(
                    "latent-aot-entry-hint-byte-identity-size-mismatch");
            if (requested_source_address.has_value() &&
                candidate.source_address != *requested_source_address)
                throw std::runtime_error(
                    "latent-aot-entry-hint-source-address-conflict");
            if (requested_runtime_base.has_value() &&
                candidate.proven_runtime_base.has_value() &&
                candidate.proven_runtime_base != requested_runtime_base)
                throw std::runtime_error(
                    "latent-aot-entry-hint-runtime-base-conflict");
            if (requested_runtime_base.has_value())
                candidate.proven_runtime_base = requested_runtime_base;
            if (requested_module_class.has_value() &&
                !candidate.authority_entries.empty() &&
                candidate.module_class != *requested_module_class)
                throw std::runtime_error(
                    "latent-aot-entry-hint-module-class-conflict");
            if (source_binding_count >= maximum_latent_aot_source_bindings)
                throw std::runtime_error(
                    "latent-aot-source-binding-budget");
            if (insert_source_binding(candidate, std::move(source_binding)))
                ++source_binding_count;
            merge_entry_offsets(candidate.entry_offsets,
                                entry_offsets);
            merge_authority_entries(candidate.authority_entries,
                                    std::move(authority_entries));
            if (requested_module_class.has_value())
                candidate.module_class = *requested_module_class;
            candidate.explicit_entry_offsets = candidate.entry_offsets;
            candidate.inferred_authoritative_entry_table = false;
        } else {
            if (known_identities.contains(byte_identity))
                throw std::runtime_error(
                    "latent-aot-entry-hint-byte-identity-ambiguous");
            if (source_binding_count >= maximum_latent_aot_source_bindings)
                throw std::runtime_error(
                    "latent-aot-source-binding-budget");
            if (!place_candidate(
                    {candidate_size,
                     0u,
                     std::move(bytes),
                     byte_identity,
                     {std::move(source_binding)},
                     entry_offsets,
                     entry_offsets,
                     requested_runtime_base,
                     false,
                     std::move(authority_entries),
                     requested_module_class.value_or(
                         CompleteDisassemblyModuleClass::LatentLoaded)},
                    true,
                    requested_source_address))
                break;
            candidate_by_byte_identity_and_size.emplace(
                std::move(candidate_key), candidates.size() - 1u);
            known_identities.insert(byte_identity);
            ++source_binding_count;
            if (sega_prs) ++result.prs_candidates_admitted;
        }
        for (const auto hint_index : matching_entry_hints)
            matched_entry_hints[hint_index] = true;
        exact_file_extents.emplace(disc_byte_offset, entry.size);
    }

    std::size_t identity_heuristic_candidate_count = 0u;
    std::size_t transformed_heuristic_candidate_count = 0u;
    std::size_t inferred_authoritative_candidate_count = 0u;
    if (heuristic_discovery) {
        for (const auto file_index : heuristic_file_order) {
            const auto& file = files[file_index];
            const auto& entry = file.second;
            if (source_binding_count >=
                maximum_latent_aot_source_bindings)
                break;
            const bool sega_prs = disc_file_uses_sega_prs(file.first);
            if (entry.size == 0u || entry.size > options.maximum_file_bytes ||
                (!sega_prs &&
                 (entry.size < 4u || (entry.size & 3u) != 0u)))
                continue;
            const auto disc_byte_offset = disc_byte_offset_for(entry);
            if (exact_file_extents.contains({disc_byte_offset, entry.size}))
                continue;
            std::vector<std::size_t> extent_hint_indices;
            for (std::size_t hint_index = 0u;
                 hint_index < normalized_entry_hints.size();
                 ++hint_index) {
                const auto& hint = normalized_entry_hints[hint_index];
                if (hint.disc_byte_offset == disc_byte_offset &&
                    hint.byte_size == entry.size)
                    extent_hint_indices.push_back(hint_index);
            }
            const auto& source_budget_used =
                sega_prs ? examined_transform_source_bytes
                         : examined_file_bytes;
            const auto source_budget =
                sega_prs ? options.maximum_total_transform_source_bytes
                         : options.maximum_total_file_bytes;
            if (entry.size > source_budget - source_budget_used)
                continue;
            if (disc_byte_offset > source->size() ||
                entry.size > source->size() - disc_byte_offset)
                throw std::runtime_error(
                    "Latente Discdatei liegt ausserhalb der Discquelle.");
            auto source_bytes = filesystem->read_file(
                entry, static_cast<std::uint32_t>(options.maximum_file_bytes));
            if (source_bytes.size() != entry.size)
                throw std::runtime_error(
                    "Latente Discdatei wurde abgeschnitten gelesen.");
            ++result.examined_files;
            result.examined_bytes += source_bytes.size();
            if (sega_prs)
                examined_transform_source_bytes += source_bytes.size();
            else
                examined_file_bytes += source_bytes.size();
            {
                katana::ProgressCounterSnapshot counters;
                counters.discovered = files.size();
                counters.started = result.examined_files;
                discovery_progress.update(
                    result.examined_bytes,
                    std::move(counters));
            }
            const auto source_byte_identity =
                "sha256:" + katana::io::sha256_bytes(std::string_view(
                                reinterpret_cast<const char*>(
                                    source_bytes.data()),
                                source_bytes.size()));
            auto transform = LatentAotSourceTransform::Identity;
            auto bytes = std::move(source_bytes);
            if (sega_prs) {
                ++result.prs_files_examined;
                try {
                    bytes = katana::detail::decompress_sega_prs(
                        bytes, options.maximum_file_bytes,
                        options.maximum_file_bytes);
                } catch (const std::bad_alloc&) {
                    throw;
                } catch (const katana::detail::PrsDecodeError&) {
                    ++result.prs_files_rejected;
                    continue;
                }
                ++result.prs_files_decoded;
                result.prs_decoded_bytes += bytes.size();
                transform = LatentAotSourceTransform::SegaPrs;
            }
            const auto candidate_size =
                static_cast<std::uint32_t>(bytes.size());
            auto byte_identity =
                "sha256:" + katana::io::sha256_bytes(std::string_view(
                                reinterpret_cast<const char*>(bytes.data()),
                                bytes.size()));
            std::vector<std::size_t> matching_entry_hints;
            std::vector<std::uint32_t> explicit_entry_offsets;
            std::optional<std::uint32_t> requested_source_address;
            std::optional<std::uint32_t> requested_runtime_base;
            for (const auto hint_index : extent_hint_indices) {
                const auto& hint = normalized_entry_hints[hint_index];
                if (hint.byte_identity == byte_identity) {
                    matching_entry_hints.push_back(hint_index);
                    explicit_entry_offsets.push_back(
                        hint.module_relative_offset);
                    if (hint.source_address != 0u) {
                        if (requested_source_address.has_value() &&
                            *requested_source_address !=
                                hint.source_address)
                            throw std::runtime_error(
                                "latent-aot-entry-hint-source-address-conflict");
                        requested_source_address = hint.source_address;
                    }
                    if (hint.proven_runtime_base != 0u) {
                        if (requested_runtime_base.has_value() &&
                            *requested_runtime_base !=
                                hint.proven_runtime_base)
                            throw std::runtime_error(
                                "latent-aot-entry-hint-runtime-base-conflict");
                        requested_runtime_base =
                            hint.proven_runtime_base;
                    }
                }
            }
            std::sort(explicit_entry_offsets.begin(),
                      explicit_entry_offsets.end());
            explicit_entry_offsets.erase(
                std::unique(explicit_entry_offsets.begin(),
                            explicit_entry_offsets.end()),
                explicit_entry_offsets.end());
            if (std::any_of(
                    explicit_entry_offsets.begin(),
                    explicit_entry_offsets.end(),
                    [&](const auto offset) {
                        return (offset & 1u) != 0u ||
                               static_cast<std::uint64_t>(offset) + 2u >
                                   candidate_size;
                    }))
                throw std::runtime_error(
                    "latent-aot-entry-hint-offset-invalid");
            require_explicit_entry_prefixes(
                bytes, explicit_entry_offsets,
                options.maximum_entry_scan_instructions);
            if (requested_runtime_base.has_value() &&
                !valid_latent_runtime_base(
                    *requested_runtime_base, candidate_size))
                throw std::runtime_error(
                    "latent-aot-entry-hint-runtime-base-invalid");
            const auto candidate_key =
                std::pair{byte_identity, candidate_size};
            if (!known_identities.insert(byte_identity).second) {
                ++result.duplicate_files;
                const auto existing =
                    candidate_by_byte_identity_and_size.find(
                        candidate_key);
                if (existing !=
                    candidate_by_byte_identity_and_size.end()) {
                    auto& candidate = candidates[existing->second];
                    if (!matching_entry_hints.empty() &&
                        requested_source_address.has_value() &&
                        candidate.source_address !=
                            *requested_source_address)
                        throw std::runtime_error(
                            "latent-aot-entry-hint-source-address-conflict");
                    if (!matching_entry_hints.empty() &&
                        requested_runtime_base.has_value() &&
                        candidate.proven_runtime_base.has_value() &&
                        candidate.proven_runtime_base !=
                            requested_runtime_base)
                        throw std::runtime_error(
                            "latent-aot-entry-hint-runtime-base-conflict");
                    auto source_binding = make_source_binding(
                        transform, source_byte_identity,
                        disc_byte_offset, entry.size);
                    if (insert_source_binding(
                            candidate, std::move(source_binding)))
                        ++source_binding_count;
                    if (!matching_entry_hints.empty()) {
                        if (requested_runtime_base.has_value())
                            candidate.proven_runtime_base =
                                requested_runtime_base;
                        merge_entry_offsets(candidate.entry_offsets,
                                            explicit_entry_offsets);
                        candidate.explicit_entry_offsets =
                            candidate.entry_offsets;
                        candidate.inferred_authoritative_entry_table = false;
                        for (const auto hint_index : matching_entry_hints)
                            matched_entry_hints[hint_index] = true;
                        exact_file_extents.emplace(disc_byte_offset,
                                                   entry.size);
                    }
                }
                continue;
            }
            auto source_binding = make_source_binding(
                transform, source_byte_identity, disc_byte_offset,
                entry.size);
            auto entry_offsets = sega_prs
                                     ? latent_prefix_entry_table_offsets(bytes)
                                     : std::vector<std::uint32_t>{};
            const bool inferred_prefix_entry_table =
                !entry_offsets.empty();
            if (entry_offsets.empty()) entry_offsets.push_back(0u);
            if (!matching_entry_hints.empty()) {
                merge_entry_offsets(entry_offsets, explicit_entry_offsets);
                explicit_entry_offsets = entry_offsets;
            }
            DiscFileCandidate candidate{
                candidate_size,
                0u,
                std::move(bytes),
                byte_identity,
                {std::move(source_binding)},
                entry_offsets,
                matching_entry_hints.empty() ? std::vector<std::uint32_t>{}
                                              : explicit_entry_offsets,
                requested_runtime_base,
                matching_entry_hints.empty()
                    ? inferred_prefix_entry_table
                    : false};
            // Raw and decoded heuristics may both outnumber their candidate
            // caps before a cold cache can retain negative results. A
            // deterministic source-shape failure must therefore be rejected
            // before it consumes any candidate slot or analysis lane. The
            // predicate is cheaper than hashing and serializing the same
            // negative result; full CFG, relocation and emitted-block closure
            // still run for every admitted candidate.
            if (candidate_source_shape_rejection(candidate, options)) {
                ++result.rejected_files;
                continue;
            }
            if (sega_prs &&
                candidate.bytes.size() >
                    options.maximum_total_transformed_bytes -
                        transformed_candidate_bytes) {
                result.prs_decoded_budget_exhausted = true;
                throw std::runtime_error(
                    candidate.inferred_authoritative_entry_table
                        ? "latent-aot-authoritative-transformed-byte-budget"
                        : "latent-aot-heuristic-transformed-byte-budget");
            }
            // A complete table embedded in immutable transformed bytes is an
            // authoritative RuntimeOnly root contract, not an ordinary
            // opcode-shape heuristic.  Keep it behind its own hard cap so a
            // directory full of smaller generic candidates cannot crowd a
            // later statically linked module out.  Crossing that cap aborts
            // instead of silently losing an authoritative root.
            if (candidate.inferred_authoritative_entry_table) {
                if (inferred_authoritative_candidate_count ==
                    maximum_latent_aot_inferred_authoritative_candidates)
                    throw std::runtime_error(
                        "latent-aot-authoritative-candidate-budget");
            } else if (sega_prs) {
                if (transformed_heuristic_candidate_count ==
                    options.maximum_transformed_candidate_files)
                    throw std::runtime_error(
                        "latent-aot-transformed-heuristic-candidate-budget");
            } else if (identity_heuristic_candidate_count ==
                       options.maximum_candidate_files) {
                throw std::runtime_error(
                    "latent-aot-identity-heuristic-candidate-budget");
            }
            if (sega_prs)
                transformed_candidate_bytes += candidate.bytes.size();
            const bool inferred_authoritative_entry_table =
                candidate.inferred_authoritative_entry_table;
            if (!place_candidate(
                    std::move(candidate),
                    !matching_entry_hints.empty(),
                    requested_source_address)) {
                throw std::runtime_error(
                    inferred_authoritative_entry_table
                        ? "latent-aot-authoritative-source-range-budget"
                        : "latent-aot-heuristic-source-range-budget");
            }
            candidate_by_byte_identity_and_size.emplace(
                candidate_key, candidates.size() - 1u);
            ++source_binding_count;
            if (inferred_authoritative_entry_table)
                ++inferred_authoritative_candidate_count;
            else if (sega_prs)
                ++transformed_heuristic_candidate_count;
            else
                ++identity_heuristic_candidate_count;
            if (sega_prs) ++result.prs_candidates_admitted;
            for (const auto hint_index : matching_entry_hints)
                matched_entry_hints[hint_index] = true;
            if (!matching_entry_hints.empty())
                exact_file_extents.emplace(disc_byte_offset, entry.size);
        }
    }
    }

    if (session != nullptr && !reuse_catalog) {
        session->catalog_ready = false;
        session->catalog_key = catalog_key;
        session->catalog_statistics.examined_files = result.examined_files;
        session->catalog_statistics.rejected_files = result.rejected_files;
        session->catalog_statistics.duplicate_files = result.duplicate_files;
        session->catalog_statistics.examined_bytes = result.examined_bytes;
        session->catalog_statistics.prs_files_examined =
            result.prs_files_examined;
        session->catalog_statistics.prs_files_decoded =
            result.prs_files_decoded;
        session->catalog_statistics.prs_files_rejected =
            result.prs_files_rejected;
        session->catalog_statistics.prs_candidates_admitted =
            result.prs_candidates_admitted;
        session->catalog_statistics.prs_decoded_bytes =
            result.prs_decoded_bytes;
        session->catalog_statistics.prs_decoded_budget_exhausted =
            result.prs_decoded_budget_exhausted;
    }

    if (std::any_of(matched_entry_hints.begin(), matched_entry_hints.end(),
                    [](const bool matched) { return !matched; }))
        throw std::runtime_error("latent-aot-entry-hint-unmatched");

    std::unique_ptr<CodegenCache> analysis_cache;
    if (!options.analysis_cache_root.empty() &&
        (!options.analysis_implementation_identity.empty() ||
         !options.analysis_cache_implementation_identity.empty()))
        analysis_cache =
            std::make_unique<CodegenCache>(
                options.analysis_cache_root);
    CandidateAnalysisCacheCounters cache_counters;
    LatentAotStaticCandidateCache* const static_cache =
        session != nullptr ? &session->static_candidates : nullptr;
    std::mutex static_cache_mutex;
    std::vector<CandidateAnalysisOutcome> analyzed(
        candidates.size());
    std::vector<std::uint64_t> analysis_candidate_duration_ms(
        candidates.size());
    auto candidate_progress =
        discovery_progress.child_reporter().begin(
            katana::ProgressOperation::CandidateResolution,
            katana::ProgressUnit::Modules,
            candidates.size(),
            "latent-aot-candidates");
    const auto requested_candidate_workers = std::min(
        {candidates.size(),
         options.maximum_workers,
         katana::analysis::global_analysis_executor().maximum_jobs()});
    auto& analysis_executor =
        katana::analysis::global_analysis_executor();
    const auto maximum_candidate_bytes =
        std::accumulate(
            candidates.begin(),
            candidates.end(),
            std::size_t{0u},
            [](const std::size_t current,
               const DiscFileCandidate& candidate) {
                return std::max(current, candidate.bytes.size());
            });
    katana::analysis::AnalysisWorkDescriptor candidate_work;
    candidate_work.phase =
        katana::analysis::AnalysisWorkPhase::LatentAot;
    candidate_work.subject_kind =
        katana::analysis::AnalysisWorkSubjectKind::Module;
    candidate_work.estimated_cost =
        std::max(std::size_t{1u}, maximum_candidate_bytes);
    candidate_work.fanout = candidates.size();
    candidate_work.priority =
        katana::analysis::AnalysisWorkPriorityKind::Throughput;
    if (latent_aot_module_transient_bytes_overflow(
            maximum_candidate_bytes))
        throw katana::analysis::AnalysisMemoryBudgetExceeded(
            std::numeric_limits<std::size_t>::max(),
            analysis_executor.memory_budget().capacity());
    candidate_work.transient_bytes =
        latent_aot_module_transient_bytes(maximum_candidate_bytes);
    // Keep one complete resolution-ready arena available independently of
    // candidate count.  Its 1 GiB cap is a pre-existing logical contract;
    // this is admission only, not a smaller budget.  The cache arenas are
    // parent-accounted and preserve the same headroom before retaining a
    // replay alias, so a wave cannot fill the executor and strand every
    // canonical root waiting for its first finalized result.
    const auto memory_capacity =
        analysis_executor.memory_budget().capacity();
    constexpr auto ready_headroom = static_cast<std::size_t>(
        latent_aot_function_value_ready_budget_bytes);
    if (candidate_work.transient_bytes > memory_capacity ||
        ready_headroom > memory_capacity -
                             candidate_work.transient_bytes)
        throw katana::analysis::AnalysisMemoryBudgetExceeded(
            candidate_work.transient_bytes >
                    std::numeric_limits<std::size_t>::max() -
                        ready_headroom
                ? std::numeric_limits<std::size_t>::max()
                : candidate_work.transient_bytes + ready_headroom,
            memory_capacity);
    const auto memory_admitted_candidate_workers =
        (memory_capacity - ready_headroom) /
        candidate_work.transient_bytes;
    const auto configured_candidate_workers = std::min(
        requested_candidate_workers,
        std::max<std::size_t>(1u, memory_admitted_candidate_workers));
    candidate_work.quantum = 1u;
    std::atomic_size_t started_candidates = 0u;
    {
        katana::ProgressCounterSnapshot counters;
        counters.configured_workers = configured_candidate_workers;
        counters.queued_work = candidates.size();
        counters.started = 0u;
        counters.committed_work = 0u;
        append_executor_snapshot(
            counters, analysis_executor.snapshot());
        candidate_progress.update(std::move(counters));
    }
    katana::analysis::parallel_analysis_for(
        analysis_executor,
        std::move(candidate_work),
        candidates.size(),
        configured_candidate_workers,
        nullptr,
        [&](const std::size_t index) {
            const auto started =
                started_candidates.fetch_add(
                    1u, std::memory_order_relaxed) +
                1u;
            {
                katana::ProgressCounterSnapshot counters;
                counters.configured_workers =
                    configured_candidate_workers;
                counters.queued_work =
                    candidates.size() - started;
                counters.started = started;
                counters.cache_hits =
                    cache_counters.positive_hits.load(
                        std::memory_order_relaxed) +
                    cache_counters.negative_hits.load(
                        std::memory_order_relaxed);
                counters.cache_misses =
                    cache_counters.misses.load(
                        std::memory_order_relaxed);
                append_executor_snapshot(
                    counters, analysis_executor.snapshot());
                candidate_progress.update(
                    std::move(counters));
            }
            const auto analysis_started =
                std::chrono::steady_clock::now();
            analyzed[index] =
                analyze_candidate(
                    candidates[index],
                    options,
                    analysis_cache.get(),
                    cache_counters,
                    candidate_progress.child_reporter(),
                    static_cache,
                    static_cache != nullptr ? &static_cache_mutex : nullptr,
                    session != nullptr
                        ? &session->static_candidate_cache_retained_bytes
                        : nullptr,
                    session != nullptr
                        ? &cache_counters.session_cache_budget_skips
                        : nullptr);
            analysis_candidate_duration_ms[index] =
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() -
                        analysis_started)
                        .count());
            // A session deliberately retains the bounded source catalog for
            // the next cross-image discovery call.  The legacy no-session
            // path keeps the old eager release behavior so a one-shot scan
            // does not retain source bytes past candidate analysis.
            if (session == nullptr &&
                !needs_sequential_explicit_hint_certification(
                    analyzed[index], candidates_have_explicit_entries[index]))
                std::vector<std::uint8_t>{}.swap(
                    candidates[index].bytes);
            candidate_progress.advance(1u);
            katana::ProgressCounterSnapshot counters;
            counters.configured_workers =
                configured_candidate_workers;
            counters.queued_work =
                candidates.size() -
                started_candidates.load(
                    std::memory_order_relaxed);
            counters.started =
                started_candidates.load(
                    std::memory_order_relaxed);
            counters.committed_work =
                candidate_progress.completed();
            counters.cache_hits =
                cache_counters.positive_hits.load(
                    std::memory_order_relaxed) +
                cache_counters.negative_hits.load(
                    std::memory_order_relaxed);
            counters.cache_misses =
                cache_counters.misses.load(
                    std::memory_order_relaxed);
            append_executor_snapshot(
                counters, analysis_executor.snapshot());
            candidate_progress.update(
                std::move(counters));
        });
    // Exact hints are authority, not heuristic candidates. If their parallel
    // analysis alone observed a non-deterministic ProgramInvalid exception,
    // certify that one candidate once, sequentially and without consulting a
    // negative cache. This avoids losing an otherwise valid module to
    // transient parallel pressure while every repeated or typed rejection
    // remains fail-closed.
    for (std::size_t index = 0u; index < analyzed.size(); ++index) {
        if (!needs_sequential_explicit_hint_certification(
                analyzed[index], candidates_have_explicit_entries[index]))
            continue;
        cache_counters.full_pipeline_runs.fetch_add(
            1u, std::memory_order_relaxed);
        std::fprintf(
            stderr,
            "KATANA_LATENT_AOT_SEQUENTIAL_CERTIFICATION_RETRY "
            "candidate=%zu source=0x%08X identity=%s\n",
            index, candidates[index].source_address,
            candidates[index].byte_identity.c_str());
        analyzed[index] = analyze_candidate_uncached(
            candidates[index], options, candidate_progress.child_reporter(),
            std::span<const std::uint8_t>{}, {});
        if (session == nullptr)
            std::vector<std::uint8_t>{}.swap(candidates[index].bytes);
    }
    {
        katana::ProgressCounterSnapshot counters;
        counters.configured_workers = configured_candidate_workers;
        counters.queued_work = 0u;
        counters.started = started_candidates.load(
            std::memory_order_relaxed);
        counters.committed_work = candidate_progress.completed();
        counters.cache_hits =
            cache_counters.positive_hits.load(
                std::memory_order_relaxed) +
            cache_counters.negative_hits.load(
                std::memory_order_relaxed);
        counters.cache_misses = cache_counters.misses.load(
            std::memory_order_relaxed);
        append_executor_snapshot(
            counters, analysis_executor.snapshot());
        candidate_progress.update(std::move(counters));
    }
    candidate_progress.complete();
    result.analysis_candidate_duration_ms =
        std::move(analysis_candidate_duration_ms);
    result.analysis_candidate_diagnostics.reserve(candidates.size());
    for (std::size_t index = 0u; index < candidates.size(); ++index) {
        const auto& candidate = candidates[index];
        const auto source_byte_size =
            candidate.source_bindings.empty()
                ? 0u
                : candidate.source_bindings.front().byte_size;
        result.analysis_candidate_diagnostics.push_back(
            {candidate.size,
             source_byte_size,
             static_cast<std::uint32_t>(candidate.entry_offsets.size()),
             candidate_has_transformed_source(candidate),
             analyzed[index].module.has_value(),
             std::string(latent_aot_rejection_name(
                 analyzed[index].rejection)),
             analyzed[index].rejection_detail});
    }
    result.analysis_cache_positive_hits =
        cache_counters.positive_hits.load(
            std::memory_order_relaxed);
    result.analysis_cache_negative_hits =
        cache_counters.negative_hits.load(
            std::memory_order_relaxed);
    result.analysis_cache_misses =
        cache_counters.misses.load(
            std::memory_order_relaxed);
    result.analysis_cache_corrupt_entries =
        cache_counters.corrupt_entries.load(
            std::memory_order_relaxed);
    result.analysis_cache_stores =
        cache_counters.stores.load(
            std::memory_order_relaxed);
    result.analysis_full_pipeline_runs =
        cache_counters.full_pipeline_runs.load(
            std::memory_order_relaxed);
    result.module_static_cache_hits =
        cache_counters.module_static_hits.load(std::memory_order_relaxed);
    result.module_static_cache_misses =
        cache_counters.module_static_misses.load(std::memory_order_relaxed);
    result.module_static_cache_cold_fallbacks =
        cache_counters.module_static_cold_fallbacks.load(
            std::memory_order_relaxed);
    result.module_static_cache_corrupt_entries =
        cache_counters.module_static_corrupt_entries.load(
            std::memory_order_relaxed);
    result.module_static_cache_stores =
        cache_counters.module_static_stores.load(std::memory_order_relaxed);
    if (session != nullptr)
        std::fprintf(
            stderr,
            "KATANA_LATENT_AOT_SESSION_STATS reuse=%zu cold-fallback=%zu "
            "terminal-negative=%zu full-pipeline=%zu candidates=%zu "
            "module-static-hits=%zu module-static-misses=%zu "
            "module-static-cold-fallbacks=%zu module-static-corrupt=%zu "
            "module-static-stores=%zu "
            "cache-entries=%zu "
            "cache-bytes=%zu cache-entry-cap=%zu cache-byte-cap=%zu "
            "cache-budget-skips=%zu\n",
            cache_counters.session_reuse_hits.load(
                std::memory_order_relaxed),
            cache_counters.session_cold_fallbacks.load(
                std::memory_order_relaxed),
            cache_counters.session_terminal_negative_hits.load(
                std::memory_order_relaxed),
            result.analysis_full_pipeline_runs,
            candidates.size(),
            result.module_static_cache_hits,
            result.module_static_cache_misses,
            result.module_static_cache_cold_fallbacks,
            result.module_static_cache_corrupt_entries,
            result.module_static_cache_stores,
            static_cache->size(),
            session->static_candidate_cache_retained_bytes,
            maximum_latent_aot_session_static_cache_entries,
            maximum_latent_aot_session_static_cache_bytes,
            cache_counters.session_cache_budget_skips.load(
                std::memory_order_relaxed));
    for (std::size_t index = 0u; index < analyzed.size(); ++index) {
        auto& candidate = analyzed[index];
        if (candidate.module)
            result.modules.push_back(
                std::move(*candidate.module));
        else {
            if (candidates_have_explicit_entries[index]) {
                const auto& rejected = candidates[index];
                std::ostringstream message;
                message << "latent-aot-entry-hint-analysis-rejected:"
                        << latent_aot_rejection_name(candidate.rejection)
                        << ":candidate-index=" << index
                        << ":source-address=0x" << std::hex
                        << std::uppercase << rejected.source_address
                        << ":decoded-identity=" << rejected.byte_identity
                        << ":decoded-size=0x" << rejected.size
                        << ":entry-offsets=";
                for (std::size_t entry_index = 0u;
                     entry_index < rejected.explicit_entry_offsets.size();
                     ++entry_index) {
                    if (entry_index != 0u) message << ',';
                    message << "0x"
                            << rejected.explicit_entry_offsets[entry_index];
                }
                if (!rejected.source_bindings.empty()) {
                    const auto& binding = rejected.source_bindings.front();
                    message << ":source-binding=" << binding.byte_identity
                            << "@0x" << binding.disc_byte_offset
                            << ":0x" << binding.byte_size
                            << ":transform="
                            << (binding.transform ==
                                        LatentAotSourceTransform::SegaPrs
                                    ? "sega-prs"
                                    : "identity");
                }
                message << ":detail=" << candidate.rejection_detail;
                throw std::runtime_error(message.str());
            }
            ++result.rejected_files;
        }
    }
    if (session != nullptr && source_identity_valid) {
        session->source = source;
        session->files = std::move(files);
        session->candidates = std::move(candidates);
        session->candidates_have_explicit_entries =
            std::move(candidates_have_explicit_entries);
        session->catalog_ready = true;
    }
    discovery_progress.complete(
        result.examined_bytes);
    return result;
}

bool validate_latent_aot_discovery_source_binding(
    std::shared_ptr<const katana::runtime::DiscSource> source,
    const LatentAotDiscovery& discovery) noexcept {
    try {
        if (!source ||
            discovery.modules.size() >
                maximum_latent_aot_source_bindings)
            return false;
        std::vector<LatentAotOccupiedRange> occupied;
        occupied.reserve(discovery.modules.size());
        std::uint64_t total_module_bytes = 0u;
        std::uint64_t total_source_bytes = 0u;
        std::uint64_t total_block_identity_bytes = 0u;
        std::uint64_t total_function_identity_bytes = 0u;
        std::size_t total_source_bindings = 0u;
        std::size_t total_blocks = 0u;
        std::size_t total_functions = 0u;
        std::size_t total_block_identities = 0u;
        std::size_t total_function_identities = 0u;
        for (const auto& module : discovery.modules) {
            if (module.id.empty() ||
                !valid_sha256_identity(module.byte_identity) ||
                module.byte_size == 0u ||
                module.byte_size >
                    katana::runtime::maximum_native_aot_template_extent ||
                (module.source_address & 3u) != 0u ||
                static_cast<std::uint64_t>(module.source_address) +
                        module.byte_size >
                    0x1'0000'0000ull ||
                module.source_bindings.empty() ||
                module.source_bindings.size() >
                    maximum_latent_aot_source_bindings ||
                !std::is_sorted(module.source_bindings.begin(),
                                module.source_bindings.end(),
                                source_binding_less) ||
                std::adjacent_find(module.source_bindings.begin(),
                                   module.source_bindings.end()) !=
                    module.source_bindings.end() ||
                module.entry_offsets.empty() ||
                !std::is_sorted(module.entry_offsets.begin(),
                                module.entry_offsets.end()) ||
                std::adjacent_find(module.entry_offsets.begin(),
                                   module.entry_offsets.end()) !=
                    module.entry_offsets.end() ||
                module.program.empty() ||
                !latent_aot_program_is_relocation_closed(
                    module.program,
                    module.source_address,
                    module.byte_size))
                return false;
            if (module.byte_size >
                    maximum_validated_latent_aot_total_module_bytes -
                        total_module_bytes ||
                module.source_bindings.size() >
                    maximum_latent_aot_source_bindings -
                        total_source_bindings ||
                module.block_identities.size() >
                    maximum_prepared_latent_aot_total_block_identities -
                        total_block_identities ||
                module.function_identities.size() >
                    maximum_prepared_latent_aot_total_function_identities -
                        total_function_identities ||
                module.program.size() >
                    maximum_prepared_latent_aot_total_functions -
                        total_functions)
                return false;
            total_module_bytes += module.byte_size;
            total_source_bindings += module.source_bindings.size();
            total_block_identities += module.block_identities.size();
            total_function_identities += module.function_identities.size();
            total_functions += module.program.size();

            const LatentAotOccupiedRange range{
                module.source_address, module.byte_size};
            if (std::any_of(occupied.begin(), occupied.end(),
                            [&](const auto existing) {
                                return physical_overlap(range, existing);
                            }))
                return false;
            occupied.push_back(range);

            std::vector<std::uint8_t> decoded;
            for (const auto& binding : module.source_bindings) {
                if (binding.id.empty() ||
                    !valid_source_transform(binding.transform) ||
                    !valid_sha256_identity(binding.byte_identity) ||
                    binding.byte_size == 0u ||
                    binding.byte_size >
                        katana::runtime::maximum_native_aot_template_extent ||
                    binding.disc_byte_offset > source->size() ||
                    binding.byte_size >
                        source->size() - binding.disc_byte_offset)
                    return false;
                if (binding.byte_size >
                    maximum_validated_latent_aot_total_source_bytes -
                        total_source_bytes)
                    return false;
                total_source_bytes += binding.byte_size;
                auto encoded = source->read(binding.disc_byte_offset,
                                            binding.byte_size);
                const auto encoded_identity =
                    "sha256:" + katana::io::sha256_bytes(
                        std::string_view(
                            reinterpret_cast<const char*>(encoded.data()),
                            encoded.size()));
                if (encoded_identity != binding.byte_identity)
                    return false;
                if (binding.transform ==
                    LatentAotSourceTransform::SegaPrs) {
                    decoded = katana::detail::decompress_sega_prs(
                        encoded,
                        katana::runtime::maximum_native_aot_template_extent,
                        katana::runtime::maximum_native_aot_template_extent);
                } else {
                    decoded = std::move(encoded);
                }
                if (decoded.size() != module.byte_size ||
                    "sha256:" + katana::io::sha256_bytes(
                        std::string_view(
                            reinterpret_cast<const char*>(decoded.data()),
                            decoded.size())) != module.byte_identity)
                    return false;
            }
            if (!validate_latent_pc_literal_evidence(module, decoded))
                return false;

            katana::ir::require_valid_program(module.program);
            std::set<std::uint32_t> block_entries;
            for (const auto& function : module.program) {
                if (function.blocks.size() >
                    maximum_prepared_latent_aot_total_blocks -
                        total_blocks)
                    return false;
                total_blocks += function.blocks.size();
                for (const auto& block : function.blocks) {
                    block_entries.insert(block.start_address);
                    for (const auto& instruction : block.instructions) {
                        if ((instruction.source_address & 1u) != 0u ||
                            instruction.source_address <
                                module.source_address)
                            return false;
                        const auto offset =
                            instruction.source_address -
                            module.source_address;
                        if (offset > decoded.size() ||
                            decoded.size() - offset < 2u)
                            return false;
                        const auto opcode = static_cast<std::uint16_t>(
                            static_cast<std::uint16_t>(decoded[offset]) |
                            (static_cast<std::uint16_t>(
                                 decoded[offset + 1u])
                             << 8u));
                        if (opcode != instruction.original_opcode ||
                            !katana::sh4::decode(opcode).is_known())
                            return false;
                    }
                }
            }
            for (const auto offset : module.entry_offsets) {
                if ((offset & 1u) != 0u || offset >= module.byte_size ||
                    !block_entries.contains(module.source_address + offset))
                    return false;
            }
            const auto valid_identity = [&](const auto& identity) {
                return identity.size != 0u &&
                       valid_sha256_identity(identity.sha256) &&
                       identity.source_offset <= decoded.size() &&
                       identity.size <=
                           decoded.size() - identity.source_offset &&
                       identity.sha256 ==
                           "sha256:" + katana::io::sha256_bytes(
                               std::string_view(
                                   reinterpret_cast<const char*>(
                                       decoded.data() +
                                       identity.source_offset),
                                   identity.size));
            };
            for (const auto& identity : module.block_identities) {
                if (identity.size >
                    maximum_prepared_latent_aot_block_identity_bytes -
                        total_block_identity_bytes)
                    return false;
                total_block_identity_bytes += identity.size;
            }
            for (const auto& identity : module.function_identities) {
                if (identity.size >
                    maximum_prepared_latent_aot_function_identity_bytes -
                        total_function_identity_bytes)
                    return false;
                total_function_identity_bytes += identity.size;
            }
            if (module.block_identities.empty() ||
                !std::all_of(module.block_identities.begin(),
                             module.block_identities.end(),
                             valid_identity) ||
                !std::all_of(module.function_identities.begin(),
                             module.function_identities.end(),
                             valid_identity))
                return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

LatentAotDiscovery discover_latent_aot_modules(
    std::shared_ptr<const katana::runtime::DiscSource> source,
    const std::uint32_t volume_start_lba,
    const std::uint32_t extent_lba_bias,
    const std::span<const std::string> excluded_byte_identities,
    const LatentAotDiscoveryOptions& options,
    const std::span<const LatentAotOccupiedRange> occupied_source_ranges,
    const std::span<const LatentAotEntryHint> entry_hints,
    const std::span<const std::string> prioritized_file_references) {
    return discover_latent_aot_modules_impl(
        std::move(source), volume_start_lba, extent_lba_bias,
        excluded_byte_identities, options, occupied_source_ranges,
        entry_hints, prioritized_file_references, nullptr);
}

LatentAotDiscovery discover_latent_aot_modules(
    std::shared_ptr<const katana::runtime::DiscSource> source,
    const std::uint32_t volume_start_lba,
    const std::uint32_t extent_lba_bias,
    const std::span<const std::string> excluded_byte_identities,
    const LatentAotDiscoveryOptions& options,
    const std::span<const LatentAotOccupiedRange> occupied_source_ranges,
    const std::span<const LatentAotEntryHint> entry_hints,
    const std::span<const std::string> prioritized_file_references,
    LatentAotDiscoverySession& session) {
    if (session.impl_ == nullptr) session.impl_ =
        std::make_unique<LatentAotDiscoverySession::Impl>();
    return discover_latent_aot_modules_impl(
        std::move(source), volume_start_lba, extent_lba_bias,
        excluded_byte_identities, options, occupied_source_ranges,
        entry_hints, prioritized_file_references, session.impl_.get());
}

LatentAotDiscovery discover_latent_aot_modules(
    std::shared_ptr<const katana::runtime::DiscSource> source,
    const std::uint32_t volume_start_lba,
    const std::uint32_t extent_lba_bias,
    const std::span<const std::string> excluded_byte_identities,
    const LatentAotDiscoveryOptions& options,
    const std::span<const LatentAotOccupiedRange> occupied_source_ranges,
    const std::span<const LatentAotEntryHint> entry_hints) {
    return discover_latent_aot_modules(
        std::move(source), volume_start_lba, extent_lba_bias,
        excluded_byte_identities, options, occupied_source_ranges,
        entry_hints, {});
}

} // namespace katana::codegen
