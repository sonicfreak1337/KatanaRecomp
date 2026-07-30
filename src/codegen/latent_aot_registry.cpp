#include "katana/codegen/latent_aot_registry.hpp"

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
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
#include <future>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace katana::codegen {
namespace {

constexpr std::uint32_t iso_sector_size = 2048u;
constexpr std::size_t maximum_latent_aot_entry_hints = 1024u;

struct DiscFileCandidate {
    std::string path;
    std::uint32_t lba = 0u;
    std::uint32_t size = 0u;
    std::uint32_t source_address = 0u;
    std::uint64_t disc_byte_offset = 0u;
    std::vector<std::uint8_t> bytes;
    std::string byte_identity;
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

bool valid_candidate_entry_offsets(const DiscFileCandidate& candidate) noexcept {
    if (candidate.entry_offsets.empty() ||
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

class AnalysisBudgetExceeded final : public std::runtime_error {
  public:
    AnalysisBudgetExceeded() : std::runtime_error("latent-aot-analysis-budget") {}
};

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

bool complete_native_graph(const katana::analysis::ControlFlowAnalysisResult& analysis) {
    if (std::any_of(analysis.recursive.diagnostics.begin(),
                    analysis.recursive.diagnostics.end(),
                    katana::analysis::analysis_diagnostic_blocks_codegen) ||
        !katana::analysis::guarded_aot_inventory_complete(analysis))
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

std::optional<PreparedLatentAotModule>
analyze_candidate(DiscFileCandidate candidate, const LatentAotDiscoveryOptions& options) {
    try {
        const bool exact_entry_binding = !candidate.explicit_entry_offsets.empty();
        if ((exact_entry_binding
                 ? candidate.bytes.size() < 2u ||
                       (candidate.bytes.size() & 1u) != 0u
                 : candidate.bytes.size() < 4u ||
                       (candidate.bytes.size() & 3u) != 0u) ||
            !valid_candidate_entry_offsets(candidate))
            return std::nullopt;
        const auto opcode_at = [&candidate](const std::uint32_t offset) {
            return static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(candidate.bytes[offset]) |
                static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(candidate.bytes[offset + 1u]) << 8u));
        };
        if (exact_entry_binding) {
            if (std::any_of(candidate.explicit_entry_offsets.begin(),
                            candidate.explicit_entry_offsets.end(),
                            [&](const auto offset) {
                                return !katana::sh4::decode(opcode_at(offset)).is_known();
                            }))
                return std::nullopt;
        } else {
            if (!katana::sh4::decode(opcode_at(0u)).is_known())
                return std::nullopt;
            bool early_control_flow = false;
            const auto entry_scan = std::min(
                options.maximum_entry_scan_instructions, candidate.bytes.size() / 2u);
            for (std::size_t instruction = 0u; instruction < entry_scan; ++instruction) {
                const auto offset = static_cast<std::uint32_t>(instruction * 2u);
                const auto decoded = katana::sh4::decode(opcode_at(offset));
                if (!decoded.is_known()) break;
                if (decoded.changes_control_flow()) {
                    early_control_flow = true;
                    break;
                }
            }
            if (!early_control_flow) return std::nullopt;
        }

        katana::io::ExecutableImage image;
        image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
        image.set_initial_snapshot_policy(katana::io::InitialSnapshotPolicy::ImmutableOnly);
        image.set_address_model(katana::io::ImageAddressModel::Sh4DirectMapped);
        katana::io::ImageSegment segment{
            ".latent-disc-module",
            candidate.source_address,
            candidate.disc_byte_offset,
            candidate.bytes.size(),
            katana::io::SegmentKind::Mixed,
            {true, true, true},
            std::move(candidate.bytes)};
        segment.source_kind = katana::io::ImageSourceKind::DiscModule;
        segment.load_phase = katana::io::ImageLoadPhase::RuntimeModule;
        image.add_segment(std::move(segment));
        for (const auto offset : candidate.entry_offsets)
            image.add_entry_point(candidate.source_address + offset);

        const auto analysis = katana::analysis::analyze_control_flow(
            image,
            nullptr,
            [&options](const katana::analysis::ControlFlowAnalysisProgress& progress) {
                if (progress.iteration > options.maximum_analysis_iterations ||
                    progress.instructions >
                        options.maximum_native_instructions_per_module ||
                    progress.contexts > options.maximum_analysis_contexts)
                    throw AnalysisBudgetExceeded();
            });
        if (!complete_native_graph(analysis)) return std::nullopt;
        const auto architectural_safepoints =
            katana::ir::architectural_safepoint_block_leaders(analysis);
        auto program =
            katana::ir::lower_program(analysis, architectural_safepoints);
        if (program.empty() || program.size() > options.maximum_functions_per_module)
            return std::nullopt;
        static_cast<void>(katana::ir::optimize_program(program));
        katana::ir::require_valid_program(program);
        if (!relocation_closed_impl(program, candidate.source_address, candidate.size))
            return std::nullopt;
        std::size_t block_count = 0u;
        std::size_t instruction_count = 0u;
        for (const auto& function : program) {
            if (function.blocks.size() > options.maximum_blocks_per_module - block_count)
                return std::nullopt;
            block_count += function.blocks.size();
            for (const auto& block : function.blocks) {
                if (block.instructions.size() >
                    options.maximum_native_instructions_per_module - instruction_count)
                    return std::nullopt;
                instruction_count += block.instructions.size();
            }
        }
        const auto module_end =
            static_cast<std::uint64_t>(candidate.source_address) + candidate.size;
        for (const auto& function : program) {
            for (const auto& block : function.blocks) {
                if (block.start_address < candidate.source_address ||
                    block.start_address >= module_end)
                    return std::nullopt;
                for (const auto& instruction : block.instructions) {
                    if (instruction.source_address < candidate.source_address ||
                        instruction.source_address >= module_end)
                        return std::nullopt;
                }
            }
        }
        std::vector<PreparedLatentAotBlockIdentity> block_identities;
        block_identities.reserve(block_count);
        for (const auto& function : program) {
            for (const auto& block : function.blocks) {
                const auto block_start =
                    static_cast<std::uint64_t>(block.start_address);
                auto block_end = block_start + 2u;
                if (block_end > module_end) return std::nullopt;
                for (const auto& instruction : block.instructions) {
                    const auto instruction_end =
                        static_cast<std::uint64_t>(
                            instruction.source_address) +
                        2u;
                    if (instruction_end > module_end)
                        return std::nullopt;
                    block_end = std::max(block_end, instruction_end);
                }
                if (block_end <= block_start ||
                    block_end - block_start >
                        std::numeric_limits<std::uint32_t>::max())
                    return std::nullopt;
                const auto block_size =
                    static_cast<std::uint32_t>(block_end - block_start);
                const auto* const source_segment =
                    image.find_segment(block.start_address, block_size);
                const auto byte_offset =
                    source_segment != nullptr
                        ? source_segment->byte_offset(block.start_address)
                        : std::optional<std::size_t>{};
                if (source_segment == nullptr || !byte_offset.has_value() ||
                    *byte_offset > source_segment->bytes.size() ||
                    block_size >
                        source_segment->bytes.size() - *byte_offset)
                    return std::nullopt;
                const auto source_offset =
                    block.start_address - candidate.source_address;
                const auto bytes = std::string_view(
                    reinterpret_cast<const char*>(
                        source_segment->bytes.data() + *byte_offset),
                    block_size);
                block_identities.push_back(
                    {source_offset,
                     block_size,
                     "sha256:" + katana::io::sha256_bytes(bytes)});
            }
        }
        std::sort(
            block_identities.begin(),
            block_identities.end(),
            [](const auto& left, const auto& right) {
                if (left.source_offset != right.source_offset)
                    return left.source_offset < right.source_offset;
                if (left.size != right.size) return left.size < right.size;
                return left.sha256 < right.sha256;
            });
        std::vector<PreparedLatentAotBlockIdentity> unique_block_identities;
        unique_block_identities.reserve(block_identities.size());
        std::uint64_t identity_bytes = 0u;
        for (const auto& identity : block_identities) {
            if (!unique_block_identities.empty() &&
                unique_block_identities.back().source_offset ==
                    identity.source_offset) {
                if (unique_block_identities.back() != identity)
                    return std::nullopt;
                continue;
            }
            if (!unique_block_identities.empty() &&
                identity.source_offset <
                    static_cast<std::uint64_t>(
                        unique_block_identities.back().source_offset) +
                        unique_block_identities.back().size)
                return std::nullopt;
            identity_bytes += identity.size;
            if (identity_bytes > candidate.size)
                return std::nullopt;
            unique_block_identities.push_back(identity);
        }
        if (unique_block_identities.empty() ||
            unique_block_identities.size() >
                options.maximum_blocks_per_module)
            return std::nullopt;
        for (const auto offset : candidate.entry_offsets) {
            const auto entry_address = candidate.source_address + offset;
            const auto emitted = std::any_of(
                program.begin(), program.end(), [&](const auto& function) {
                    return std::any_of(function.blocks.begin(), function.blocks.end(),
                                       [&](const auto& block) {
                                           return block.start_address == entry_address;
                                       });
                });
            if (!emitted) return std::nullopt;
        }
        auto module_id = "latent-aot-" + candidate.byte_identity.substr(7u);
        if (exact_entry_binding)
            module_id += "-" + std::to_string(candidate.disc_byte_offset) + "-" +
                         std::to_string(candidate.size);
        return PreparedLatentAotModule{
            std::move(module_id),
            std::move(candidate.byte_identity),
            candidate.disc_byte_offset,
            candidate.size,
            candidate.source_address,
            std::move(candidate.entry_offsets),
            std::move(unique_block_identities),
            std::move(program)};
    } catch (const std::exception&) {
        return std::nullopt;
    }
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
    if (!source || options.maximum_directory_entries == 0u ||
        options.maximum_directory_bytes == 0u ||
        options.maximum_directory_bytes >
            std::numeric_limits<std::uint32_t>::max() ||
        options.maximum_total_directory_bytes < options.maximum_directory_bytes ||
        options.maximum_candidate_files == 0u || options.maximum_file_bytes < 4u ||
        options.maximum_total_file_bytes < 4u || options.maximum_workers == 0u ||
        options.maximum_entry_scan_instructions == 0u ||
        options.maximum_native_instructions_per_module == 0u ||
        options.maximum_blocks_per_module == 0u ||
        options.maximum_functions_per_module == 0u ||
        options.maximum_analysis_iterations == 0u ||
        options.maximum_analysis_contexts == 0u ||
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

    LatentAotDiscovery result;
    std::vector<DiscFileCandidate> candidates;
    candidates.reserve(std::min(files.size(), options.maximum_candidate_files) +
                       std::min(files.size(), normalized_entry_hints.size()));
    std::vector<bool> candidates_have_explicit_entries;
    candidates_have_explicit_entries.reserve(candidates.capacity());
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
    std::set<std::pair<std::uint64_t, std::uint32_t>> exact_file_extents;
    for (const auto& [path, entry] : files) {
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
        if (known_identities.contains(byte_identity))
            throw std::runtime_error(
                "latent-aot-entry-hint-byte-identity-ambiguous");
        std::sort(explicit_entry_offsets.begin(), explicit_entry_offsets.end());
        explicit_entry_offsets.erase(
            std::unique(explicit_entry_offsets.begin(), explicit_entry_offsets.end()),
            explicit_entry_offsets.end());
        const auto entry_offsets = explicit_entry_offsets;
        const auto placed = place_candidate(
            {path,
             entry.lba,
             entry.size,
             0u,
             disc_byte_offset,
             std::move(bytes),
             byte_identity,
             entry_offsets,
             explicit_entry_offsets},
            true);
        if (!placed) break;
        for (const auto hint_index : matching_entry_hints)
            matched_entry_hints[hint_index] = true;
        exact_file_extents.emplace(disc_byte_offset, entry.size);
        known_identities.insert(std::move(byte_identity));
    }

    std::size_t heuristic_candidate_count = 0u;
    for (const auto& [path, entry] : files) {
        if (heuristic_candidate_count == options.maximum_candidate_files) break;
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
            throw std::runtime_error("Latente Discdatei liegt ausserhalb der Discquelle.");
        auto bytes = filesystem.read_file(
            entry, static_cast<std::uint32_t>(options.maximum_file_bytes));
        if (bytes.size() != entry.size)
            throw std::runtime_error("Latente Discdatei wurde abgeschnitten gelesen.");
        ++result.examined_files;
        result.examined_bytes += bytes.size();
        examined_file_bytes += bytes.size();
        auto byte_identity =
            "sha256:" + katana::io::sha256_bytes(std::string_view(
                            reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (!known_identities.insert(byte_identity).second) {
            ++result.duplicate_files;
            continue;
        }
        const std::vector<std::uint32_t> entry_offsets{0u};
        if (!place_candidate({path,
                              entry.lba,
                              entry.size,
                              0u,
                              disc_byte_offset,
                              std::move(bytes),
                              std::move(byte_identity),
                              entry_offsets,
                              {}},
                             false))
            break;
        ++heuristic_candidate_count;
    }

    if (std::any_of(matched_entry_hints.begin(), matched_entry_hints.end(),
                    [](const bool matched) { return !matched; }))
        throw std::runtime_error("latent-aot-entry-hint-unmatched");

    std::vector<std::optional<PreparedLatentAotModule>> analyzed(candidates.size());
    std::atomic_size_t next_candidate = 0u;
    const auto worker_count =
        std::min({options.maximum_workers, candidates.size(), std::size_t{12u}});
    std::vector<std::future<void>> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0u; worker < worker_count; ++worker) {
        workers.push_back(std::async(std::launch::async, [&] {
            for (;;) {
                const auto index = next_candidate.fetch_add(1u, std::memory_order_relaxed);
                if (index >= candidates.size()) return;
                analyzed[index] =
                    analyze_candidate(std::move(candidates[index]), options);
            }
        }));
    }
    for (auto& worker : workers) worker.get();
    for (std::size_t index = 0u; index < analyzed.size(); ++index) {
        auto& candidate = analyzed[index];
        if (candidate)
            result.modules.push_back(std::move(*candidate));
        else {
            if (candidates_have_explicit_entries[index])
                throw std::runtime_error("latent-aot-entry-hint-analysis-rejected");
            ++result.rejected_files;
        }
    }
    return result;
}

} // namespace katana::codegen
