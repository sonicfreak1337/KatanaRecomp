#include "katana/codegen/latent_aot_registry.hpp"

#include "native_aot_resume.hpp"
#include "structured_control_flow_progress.hpp"

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/hardware_audit.hpp"
#include "katana/analysis/abi.hpp"
#include "katana/analysis/parallel_work.hpp"
#include "katana/codegen/cache.hpp"
#include "katana/codegen/latent_aot_analysis_cache.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/runtime/iso9660.hpp"
#include "katana/runtime/block_table.hpp"
#include "katana/sh4/decoder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <map>
#include <new>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace katana::codegen {
namespace {

constexpr std::uint32_t iso_sector_size = 2048u;
constexpr std::size_t maximum_latent_aot_entry_hints = 1024u;
constexpr std::size_t maximum_latent_aot_source_bindings = 1024u;
constexpr std::size_t maximum_analysis_implementation_identity_bytes = 4096u;
constexpr std::uint64_t latent_aot_module_analysis_base_reserve_bytes =
    2'560ull * 1024ull * 1024ull;
constexpr std::uint64_t latent_aot_function_value_ready_budget_bytes =
    1'024ull * 1024ull * 1024ull;
constexpr std::uint64_t latent_aot_module_analysis_reserve_bytes =
    latent_aot_module_analysis_base_reserve_bytes +
    latent_aot_function_value_ready_budget_bytes;
constexpr std::string_view latent_aot_analysis_cache_artifact{
    "module-analysis.bin"};

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
};

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
    return valid_sha256_identity(hint.byte_identity) && hint.byte_size >= 2u &&
           (hint.byte_size & 1u) == 0u &&
           (hint.module_relative_offset & 1u) == 0u &&
           hint.module_relative_offset <= hint.byte_size - 2u;
}

bool entry_hint_less(const LatentAotEntryHint& left,
                     const LatentAotEntryHint& right) noexcept {
    if (left.byte_identity != right.byte_identity)
        return left.byte_identity < right.byte_identity;
    if (left.disc_byte_offset != right.disc_byte_offset)
        return left.disc_byte_offset < right.disc_byte_offset;
    if (left.byte_size != right.byte_size) return left.byte_size < right.byte_size;
    return left.module_relative_offset < right.module_relative_offset;
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

bool source_binding_less(const PreparedLatentAotSourceBinding& left,
                         const PreparedLatentAotSourceBinding& right) noexcept {
    if (left.disc_byte_offset != right.disc_byte_offset)
        return left.disc_byte_offset < right.disc_byte_offset;
    if (left.byte_size != right.byte_size)
        return left.byte_size < right.byte_size;
    return left.id < right.id;
}

PreparedLatentAotSourceBinding make_source_binding(
    const std::string_view byte_identity,
    const std::uint64_t disc_byte_offset,
    const std::uint32_t byte_size) {
    return {"latent-aot-source-" + std::string(byte_identity.substr(7u)) + "-" +
                std::to_string(disc_byte_offset) + "-" +
                std::to_string(byte_size),
            disc_byte_offset,
            byte_size};
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
                               binding.byte_size != candidate.size ||
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
    if (candidate.explicit_entry_offsets.empty()) {
        if (candidate.entry_offsets.size() != 1u ||
            candidate.entry_offsets.front() != 0u)
            return false;
    } else if (candidate.entry_offsets != candidate.explicit_entry_offsets) {
        return false;
    }
    const auto valid_offset = [&candidate](const std::uint32_t offset) {
        return (offset & 1u) == 0u &&
               static_cast<std::uint64_t>(offset) + 2u <= candidate.bytes.size();
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
        walk.abi_stack_base_unresolved;
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
};

struct CandidateAnalysisCacheCounters {
    std::atomic_size_t positive_hits = 0u;
    std::atomic_size_t negative_hits = 0u;
    std::atomic_size_t misses = 0u;
    std::atomic_size_t corrupt_entries = 0u;
    std::atomic_size_t stores = 0u;
    std::atomic_size_t full_pipeline_runs = 0u;
};

CandidateAnalysisOutcome reject_candidate(
    const LatentAotAnalysisRejection rejection,
    const bool deterministic = true) {
    return {std::nullopt, rejection, deterministic};
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

std::optional<LatentAotAnalysisRejection>
candidate_source_shape_rejection(
    const DiscFileCandidate& candidate,
    const LatentAotDiscoveryOptions& options) {
    const bool exact_entry_binding =
        !candidate.explicit_entry_offsets.empty();
    if ((exact_entry_binding
             ? candidate.bytes.size() < 2u ||
                   (candidate.bytes.size() & 1u) != 0u
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
    if (exact_entry_binding) {
        if (std::any_of(
                candidate.explicit_entry_offsets.begin(),
                candidate.explicit_entry_offsets.end(),
                [&](const auto offset) {
                    return !katana::sh4::decode(
                                opcode_at(offset))
                                .is_known();
                }))
            return LatentAotAnalysisRejection::EntryDecodeFailed;
        return std::nullopt;
    }
    if (!katana::sh4::decode(opcode_at(0u)).is_known())
        return LatentAotAnalysisRejection::EntryDecodeFailed;
    bool early_control_flow = false;
    const auto entry_scan = std::min(
        options.maximum_entry_scan_instructions,
        candidate.bytes.size() / 2u);
    for (std::size_t instruction = 0u;
         instruction < entry_scan;
         ++instruction) {
        const auto offset =
            static_cast<std::uint32_t>(instruction * 2u);
        const auto decoded =
            katana::sh4::decode(opcode_at(offset));
        if (!decoded.is_known())
            break;
        if (decoded.changes_control_flow()) {
            early_control_flow = true;
            break;
        }
    }
    if (!early_control_flow)
        return LatentAotAnalysisRejection::EntryDecodeFailed;
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
                if (instruction.dynamic_target_class ==
                        katana::ir::DynamicTargetClass::GuardedPartial ||
                    instruction.dynamic_target_class ==
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
    std::vector<katana::ir::Function> program,
    katana::analysis::DreamcastHardwareAudit hardware_audit,
    const LatentAotDiscoveryOptions& options,
    const std::span<const katana::ir::ExternalDispatchEntry>
        external_dispatch_entries) {
    if (const auto rejection =
            candidate_source_shape_rejection(
                candidate, options))
        return reject_candidate(*rejection);
    if (program.empty())
        return reject_candidate(
            LatentAotAnalysisRejection::ProgramInvalid);
    if (program.size() > options.maximum_functions_per_module)
        return reject_candidate(
            LatentAotAnalysisRejection::FunctionBudgetExceeded);
    try {
        if (!source_bound_unoptimized_program(candidate, program)) {
            std::fprintf(stderr,
                         "KATANA_LATENT_AOT_PROGRAM_INVALID "
                         "stage=source-bound\n");
            return reject_candidate(
                LatentAotAnalysisRejection::ProgramInvalid);
        }
        katana::ir::require_valid_program(program);
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
    for (const auto offset : candidate.entry_offsets) {
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
            candidate.entry_offsets,
            std::move(unique_block_identities),
            std::move(program),
            std::move(hardware_audit)},
        LatentAotAnalysisRejection::None,
        true};
}

CandidateAnalysisOutcome analyze_candidate_uncached(
    const DiscFileCandidate& candidate,
    const LatentAotDiscoveryOptions& options,
    const katana::ProgressReporter& progress_reporter,
    const std::span<const std::uint8_t> persistent_epoch_import_blob,
    katana::analysis::PersistentFunctionAnalysisEpochPublishCallback
        persistent_epoch_publish_callback) {
    if (const auto rejection =
            candidate_source_shape_rejection(
                candidate, options))
        return reject_candidate(*rejection);

    const bool exact_runtime_only_stop_on_miss =
        !candidate.explicit_entry_offsets.empty() &&
        options.completeness_policy ==
            LatentAotCompletenessPolicy::ExactRuntimeOnlyStopOnMiss;
    katana::io::ExecutableImage image;
    // A hash-bound explicit entry table is already the authoritative
    // RuntimeOnly call-target contract. Analyze its static graph without the
    // SuperHC value/candidate fixpoint; every omitted dynamic destination
    // remains a typed runtime miss. Strict/heuristic discovery keeps the full
    // ABI analysis unchanged.
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
        candidate.source_bindings.front().disc_byte_offset,
        candidate.bytes.size(),
        katana::io::SegmentKind::Mixed,
        {true, true, true},
        candidate.bytes};
    segment.source_kind =
        katana::io::ImageSourceKind::DiscModule;
    segment.load_phase =
        katana::io::ImageLoadPhase::RuntimeModule;
    image.add_segment(std::move(segment));
    for (const auto offset : candidate.entry_offsets)
        image.add_entry_point(candidate.source_address + offset);

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
    katana::analysis::AnalysisMemoryBudget function_value_ready_budget(
        static_cast<std::size_t>(
            latent_aot_function_value_ready_budget_bytes));
    analysis_options.pre_reserved_function_value_ready_budget =
        &function_value_ready_budget;
    analysis_options.persistent_function_analysis_epoch_import_blob =
        persistent_epoch_import_blob;
    analysis_options
        .persistent_function_analysis_epoch_implementation_identity =
            options.analysis_implementation_identity;
    analysis_options.persistent_function_analysis_epoch_publish_callback =
        std::move(persistent_epoch_publish_callback);
    std::atomic resolution_retention_limit_reason{
        katana::analysis::ResolutionRetentionLimitReason::None};
    try {
        analysis = katana::analysis::analyze_control_flow(
            image,
            nullptr,
            [&control_flow_progress, &resolution_retention_limit_reason](
                const katana::analysis::
                    ControlFlowAnalysisProgress& progress) {
                if (progress.function_value_resolution_retention_limit_reason !=
                    katana::analysis::ResolutionRetentionLimitReason::None)
                    resolution_retention_limit_reason.store(
                        progress
                            .function_value_resolution_retention_limit_reason,
                        std::memory_order_relaxed);
                control_flow_progress.update(progress);
            },
            analysis_options);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        return reject_candidate(
            LatentAotAnalysisRejection::ProgramInvalid,
            false);
    }
    control_flow_progress.complete(
        analysis.fixpoint_iterations,
        analysis.termination_reason ==
                katana::analysis::ControlFlowAnalysisTerminationReason::None &&
            !analysis.function_budget_exhausted);
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
    if (!complete_native_graph(
            analysis, exact_runtime_only_stop_on_miss,
            resolution_retention_limit_reason.load(
                std::memory_order_relaxed)))
        return reject_candidate(
            katana::analysis::guarded_aot_inventory_complete(
                analysis)
                ? LatentAotAnalysisRejection::
                      ControlFlowIncomplete
                : LatentAotAnalysisRejection::
                      InventoryTruncated);

    auto hardware_audit =
        katana::analysis::audit_dreamcast_hardware(image, analysis);
    hardware_audit.scope = "latent-aot-module";
    std::vector<katana::ir::Function> program;
    std::vector<katana::ir::ExternalDispatchEntry>
        external_dispatch_entries;
    try {
        const auto architectural_safepoints =
            katana::ir::architectural_safepoint_block_leaders(
                analysis);
        program = katana::ir::lower_program(
            analysis, architectural_safepoints);
        std::set<std::uint32_t> dispatch_block_entries;
        for (const auto offset : candidate.entry_offsets)
            dispatch_block_entries.insert(
                candidate.source_address + offset);
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
    return finalize_candidate_program(
        candidate,
        std::move(program),
        std::move(hardware_audit),
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
        !candidate.explicit_entry_offsets.empty();
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
    inputs.analyzer_implementation_id =
        std::string(latent_aot_analysis_implementation_id) + "-" +
        katana::io::sha256_bytes(
            options.analysis_cache_implementation_identity);
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
    append_field("katana-latent-persistent-function-analysis-epoch-key-v1");
    append_value(katana::analysis::abi_version);
    append_field(options.analysis_implementation_identity);
    append_field(candidate.byte_identity);
    append_value(candidate.size);
    append_value(candidate.source_address);
    append_value(latent_aot_analysis_address_layout_schema);
    append_value(!candidate.explicit_entry_offsets.empty());
    auto entry_offsets = candidate.entry_offsets;
    std::sort(entry_offsets.begin(), entry_offsets.end());
    entry_offsets.erase(
        std::unique(entry_offsets.begin(), entry_offsets.end()),
        entry_offsets.end());
    append_value(entry_offsets.size());
    for (const auto entry : entry_offsets)
        append_value(entry);
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
    const katana::ProgressReporter& progress_reporter) {
    constexpr std::string_view epoch_artifact_name{
        "function-analysis-epoch.bin"};
    std::string cache_key;
    bool cache_entry_absent = true;
    if (cache != nullptr &&
        !options.analysis_cache_implementation_identity.empty()) {
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
    katana::analysis::PersistentFunctionAnalysisEpochPublishCallback
        epoch_publish_callback;
    if (cache != nullptr && !epoch_cache_key.empty()) {
        epoch_publish_callback =
            [cache,
             epoch_cache_key,
             &epoch_import_blob,
             epoch_artifact_name](
                const std::span<const std::uint8_t> blob) {
                const auto serialized = std::string_view(
                    reinterpret_cast<const char*>(blob.data()),
                    blob.size());
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
    auto analyzed = analyze_candidate_uncached(
        candidate,
        options,
        progress_reporter,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(
                epoch_import_blob.data()),
            epoch_import_blob.size()),
        std::move(epoch_publish_callback));
    if (cache == nullptr || cache_key.empty() ||
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

bool latent_aot_program_is_relocation_closed(
    const std::span<const katana::ir::Function> program,
    const std::uint32_t source_start,
    const std::uint32_t extent) noexcept {
    return extent != 0u &&
           static_cast<std::uint64_t>(source_start) + extent <= 0x1'0000'0000ull &&
           relocation_closed_impl(program, source_start, extent);
}

LatentAotDiscovery discover_latent_aot_modules(
    std::shared_ptr<const katana::runtime::DiscSource> source,
    const std::uint32_t volume_start_lba,
    const std::uint32_t extent_lba_bias,
    const std::span<const std::string> excluded_byte_identities,
    const LatentAotDiscoveryOptions& options,
    const std::span<const LatentAotOccupiedRange> occupied_source_ranges,
    const std::span<const LatentAotEntryHint> entry_hints) {
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
        (heuristic_discovery && options.maximum_candidate_files == 0u) ||
        options.maximum_file_bytes < minimum_candidate_bytes ||
        options.maximum_file_bytes >
            katana::runtime::maximum_native_aot_template_extent ||
        options.maximum_total_file_bytes < minimum_candidate_bytes ||
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
        options.source_address_begin >= options.source_address_end ||
        (options.source_address_begin & 3u) != 0u ||
        (options.source_address_end & 3u) != 0u)
        throw std::invalid_argument("Latente AOT-Discovery besitzt ungueltige Grenzen.");
    if (std::any_of(occupied_source_ranges.begin(),
                    occupied_source_ranges.end(),
                    [](const auto range) { return !valid_linear_physical_range(range); }))
        throw std::invalid_argument("Latente AOT-Discovery besitzt ungueltige belegte Ranges.");
    const auto normalized_entry_hints = normalize_entry_hints(entry_hints);
    std::vector<bool> matched_entry_hints(normalized_entry_hints.size(), false);
    if (!heuristic_discovery && normalized_entry_hints.empty()) {
        discovery_progress.skipped();
        return {};
    }

    katana::runtime::Iso9660Filesystem filesystem(
        source, iso_sector_size, volume_start_lba, extent_lba_bias);
    struct PendingDirectory {
        std::string path;
        std::size_t depth = 0u;
        katana::runtime::Iso9660Entry entry;
    };
    std::vector<PendingDirectory> pending{{"/", 0u, filesystem.root_directory()}};
    std::vector<std::pair<std::string, katana::runtime::Iso9660Entry>> files;
    std::size_t directory_entries = 0u;
    std::size_t directory_bytes = 0u;
    while (!pending.empty()) {
        auto directory = std::move(pending.back());
        pending.pop_back();
        if (directory.depth > 32u)
            throw std::runtime_error("ISO9660-Verzeichnistiefe ueberschreitet das AOT-Budget.");
        if (directory.entry.size > options.maximum_directory_bytes ||
            directory.entry.size >
                options.maximum_total_directory_bytes - directory_bytes)
            throw std::runtime_error("ISO9660-Verzeichnisse ueberschreiten das AOT-I/O-Budget.");
        directory_bytes += directory.entry.size;
        auto entries = filesystem.list_directory(
            directory.entry,
            {options.maximum_directory_entries - directory_entries,
             static_cast<std::uint32_t>(options.maximum_directory_bytes)});
        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            if (left.name != right.name) return left.name < right.name;
            if (left.lba != right.lba) return left.lba < right.lba;
            return left.size < right.size;
        });
        if (entries.size() > options.maximum_directory_entries - directory_entries)
            throw std::runtime_error("ISO9660-Dateiregistry ueberschreitet das AOT-Budget.");
        directory_entries += entries.size();
        for (const auto& entry : entries) {
            if (!safe_component(entry.name))
                throw std::runtime_error("ISO9660-Dateiregistry enthaelt unsicheren Namen.");
            auto path = directory.path;
            if (path.size() != 1u) path += '/';
            path += entry.name;
            if (entry.directory)
                pending.push_back({std::move(path), directory.depth + 1u, entry});
            else
                files.emplace_back(std::move(path), entry);
        }
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        if (left.second.lba != right.second.lba)
            return left.second.lba < right.second.lba;
        if (left.second.size != right.second.size)
            return left.second.size < right.second.size;
        return left.first < right.first;
    });
    {
        katana::ProgressCounterSnapshot counters;
        counters.discovered = files.size();
        discovery_progress.update(counters);
    }

    LatentAotDiscovery result;
    std::vector<DiscFileCandidate> candidates;
    candidates.reserve(std::min(files.size(), options.maximum_candidate_files) +
                       std::min(files.size(), normalized_entry_hints.size()));
    std::vector<bool> candidates_have_explicit_entries;
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
    const auto place_candidate = [&](DiscFileCandidate candidate,
                                     const bool explicit_entries) {
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
    std::size_t source_binding_count = 0u;
    std::set<std::pair<std::uint64_t, std::uint32_t>> exact_file_extents;
    for (const auto& file : files) {
        const auto& entry = file.second;
        const auto disc_byte_offset = disc_byte_offset_for(entry);
        std::vector<std::size_t> extent_hint_indices;
        for (std::size_t hint_index = 0u;
             hint_index < normalized_entry_hints.size();
             ++hint_index) {
            const auto& hint = normalized_entry_hints[hint_index];
            if (hint.disc_byte_offset == disc_byte_offset &&
                hint.byte_size == entry.size)
                extent_hint_indices.push_back(hint_index);
        }
        if (extent_hint_indices.empty() ||
            exact_file_extents.contains({disc_byte_offset, entry.size}))
            continue;
        if (disc_byte_offset > source->size() ||
            entry.size > source->size() - disc_byte_offset)
            throw std::runtime_error("Latente Discdatei liegt ausserhalb der Discquelle.");
        if (entry.size > options.maximum_file_bytes)
            throw std::runtime_error(
                "latent-aot-entry-hint-file-budget");
        if (entry.size >
            options.maximum_total_file_bytes - examined_file_bytes)
            throw std::runtime_error(
                "latent-aot-entry-hint-total-budget");
        auto bytes = filesystem.read_file(entry, entry.size);
        if (bytes.size() != entry.size)
            throw std::runtime_error("Latente Discdatei wurde abgeschnitten gelesen.");
        examined_file_bytes += bytes.size();
        ++result.examined_files;
        result.examined_bytes += bytes.size();
        {
            katana::ProgressCounterSnapshot counters;
            counters.discovered = files.size();
            counters.started = result.examined_files;
            discovery_progress.update(
                result.examined_bytes,
                std::move(counters));
        }
        auto byte_identity =
            "sha256:" + katana::io::sha256_bytes(std::string_view(
                            reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        std::vector<std::size_t> matching_entry_hints;
        std::vector<std::uint32_t> explicit_entry_offsets;
        for (const auto hint_index : extent_hint_indices) {
            const auto& hint = normalized_entry_hints[hint_index];
            if (hint.byte_identity == byte_identity) {
                matching_entry_hints.push_back(hint_index);
                explicit_entry_offsets.push_back(hint.module_relative_offset);
            }
        }
        if (matching_entry_hints.empty() ||
            excluded_identities.contains(byte_identity))
            continue;
        std::sort(explicit_entry_offsets.begin(), explicit_entry_offsets.end());
        explicit_entry_offsets.erase(
            std::unique(explicit_entry_offsets.begin(), explicit_entry_offsets.end()),
            explicit_entry_offsets.end());
        auto source_binding =
            make_source_binding(byte_identity, disc_byte_offset, entry.size);
        const auto candidate_key =
            std::pair{byte_identity, entry.size};
        const auto existing =
            candidate_by_byte_identity_and_size.find(candidate_key);
        if (existing != candidate_by_byte_identity_and_size.end()) {
            auto& candidate = candidates[existing->second];
            if (candidate.size != entry.size)
                throw std::runtime_error(
                    "latent-aot-entry-hint-byte-identity-size-mismatch");
            if (source_binding_count >= maximum_latent_aot_source_bindings)
                throw std::runtime_error(
                    "latent-aot-source-binding-budget");
            if (insert_source_binding(candidate, std::move(source_binding)))
                ++source_binding_count;
            merge_entry_offsets(candidate.entry_offsets,
                                explicit_entry_offsets);
            merge_entry_offsets(candidate.explicit_entry_offsets,
                                explicit_entry_offsets);
        } else {
            if (known_identities.contains(byte_identity))
                throw std::runtime_error(
                    "latent-aot-entry-hint-byte-identity-ambiguous");
            if (source_binding_count >= maximum_latent_aot_source_bindings)
                throw std::runtime_error(
                    "latent-aot-source-binding-budget");
            const auto entry_offsets = explicit_entry_offsets;
            if (!place_candidate(
                    {entry.size,
                     0u,
                     std::move(bytes),
                     byte_identity,
                     {std::move(source_binding)},
                     entry_offsets,
                     explicit_entry_offsets},
                    true))
                break;
            candidate_by_byte_identity_and_size.emplace(
                std::move(candidate_key), candidates.size() - 1u);
            known_identities.insert(byte_identity);
            ++source_binding_count;
        }
        for (const auto hint_index : matching_entry_hints)
            matched_entry_hints[hint_index] = true;
        exact_file_extents.emplace(disc_byte_offset, entry.size);
    }

    std::size_t heuristic_candidate_count = 0u;
    if (heuristic_discovery) {
        for (const auto& file : files) {
            const auto& entry = file.second;
            if (source_binding_count >=
                maximum_latent_aot_source_bindings)
                break;
            if (entry.size < 4u || entry.size > options.maximum_file_bytes ||
                (entry.size & 3u) != 0u)
                continue;
            const auto disc_byte_offset = disc_byte_offset_for(entry);
            if (exact_file_extents.contains({disc_byte_offset, entry.size}))
                continue;
            if (entry.size >
                options.maximum_total_file_bytes - examined_file_bytes)
                break;
            if (disc_byte_offset > source->size() ||
                entry.size > source->size() - disc_byte_offset)
                throw std::runtime_error(
                    "Latente Discdatei liegt ausserhalb der Discquelle.");
            auto bytes = filesystem.read_file(
                entry, static_cast<std::uint32_t>(options.maximum_file_bytes));
            if (bytes.size() != entry.size)
                throw std::runtime_error(
                    "Latente Discdatei wurde abgeschnitten gelesen.");
            ++result.examined_files;
            result.examined_bytes += bytes.size();
            examined_file_bytes += bytes.size();
            {
                katana::ProgressCounterSnapshot counters;
                counters.discovered = files.size();
                counters.started = result.examined_files;
                discovery_progress.update(
                    result.examined_bytes,
                    std::move(counters));
            }
            auto byte_identity =
                "sha256:" + katana::io::sha256_bytes(std::string_view(
                                reinterpret_cast<const char*>(bytes.data()),
                                bytes.size()));
            const auto candidate_key =
                std::pair{byte_identity, entry.size};
            if (!known_identities.insert(byte_identity).second) {
                ++result.duplicate_files;
                const auto existing =
                    candidate_by_byte_identity_and_size.find(
                        candidate_key);
                if (existing !=
                    candidate_by_byte_identity_and_size.end()) {
                    auto source_binding = make_source_binding(
                        byte_identity,
                        disc_byte_offset,
                        entry.size);
                    if (insert_source_binding(
                            candidates[existing->second],
                            std::move(source_binding)))
                        ++source_binding_count;
                }
                continue;
            }
            if (heuristic_candidate_count ==
                options.maximum_candidate_files)
                continue;
            auto source_binding =
                make_source_binding(byte_identity, disc_byte_offset, entry.size);
            const std::vector<std::uint32_t> entry_offsets{0u};
            if (!place_candidate({entry.size,
                                  0u,
                                  std::move(bytes),
                                  std::move(byte_identity),
                                  {std::move(source_binding)},
                                  entry_offsets,
                                  {}},
                                 false))
                break;
            candidate_by_byte_identity_and_size.emplace(
                candidate_key, candidates.size() - 1u);
            ++source_binding_count;
            ++heuristic_candidate_count;
        }
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
    const auto configured_candidate_workers = std::min(
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
        options.maximum_workers,
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
                    candidate_progress.child_reporter());
            analysis_candidate_duration_ms[index] =
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() -
                        analysis_started)
                        .count());
            // Analysis artifacts never retain source bytes. Release each
            // candidate buffer as soon as its worker has finished so a cold
            // multi-module scan does not hold both every file and every IR
            // result until the final collection pass.
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
    for (std::size_t index = 0u; index < analyzed.size(); ++index) {
        auto& candidate = analyzed[index];
        if (candidate.module)
            result.modules.push_back(
                std::move(*candidate.module));
        else {
            if (candidates_have_explicit_entries[index])
                throw std::runtime_error(
                    "latent-aot-entry-hint-analysis-rejected:" +
                    std::string(latent_aot_rejection_name(
                        candidate.rejection)));
            ++result.rejected_files;
        }
    }
    discovery_progress.complete(
        result.examined_bytes);
    return result;
}

} // namespace katana::codegen
