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

struct DiscFileCandidate {
    std::string path;
    std::uint32_t lba = 0u;
    std::uint32_t size = 0u;
    std::uint32_t source_address = 0u;
    std::uint64_t disc_byte_offset = 0u;
    std::vector<std::uint8_t> bytes;
    std::string byte_identity;
};

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
    if (!analysis.recursive.diagnostics.empty() || analysis.function_budget_exhausted)
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
        if (candidate.bytes.size() < 4u || (candidate.bytes.size() & 3u) != 0u)
            return std::nullopt;
        const auto first_opcode = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(candidate.bytes[0]) |
            static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(candidate.bytes[1]) << 8u));
        if (!katana::sh4::decode(first_opcode).is_known()) return std::nullopt;
        bool early_control_flow = false;
        const auto entry_scan = std::min(
            options.maximum_entry_scan_instructions, candidate.bytes.size() / 2u);
        for (std::size_t instruction = 0u; instruction < entry_scan; ++instruction) {
            const auto offset = instruction * 2u;
            const auto opcode = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(candidate.bytes[offset]) |
                static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(candidate.bytes[offset + 1u]) << 8u));
            const auto decoded = katana::sh4::decode(opcode);
            if (!decoded.is_known()) break;
            if (decoded.changes_control_flow()) {
                early_control_flow = true;
                break;
            }
        }
        if (!early_control_flow) return std::nullopt;

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
        image.add_entry_point(candidate.source_address);

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
        auto program = katana::ir::lower_program(analysis);
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
        return PreparedLatentAotModule{
            "latent-aot-" + candidate.byte_identity.substr(7u),
            std::move(candidate.byte_identity),
            candidate.disc_byte_offset,
            candidate.size,
            candidate.source_address,
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
    const std::span<const LatentAotOccupiedRange> occupied_source_ranges) {
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
    candidates.reserve(std::min(files.size(), options.maximum_candidate_files));
    std::set<std::string> known_identities(excluded_byte_identities.begin(),
                                           excluded_byte_identities.end());
    auto next_source = options.source_address_begin;
    std::vector<LatentAotOccupiedRange> occupied(occupied_source_ranges.begin(),
                                                 occupied_source_ranges.end());
    for (const auto& [path, entry] : files) {
        if (candidates.size() == options.maximum_candidate_files) break;
        if (entry.size < 4u || entry.size > options.maximum_file_bytes ||
            (entry.size & 3u) != 0u)
            continue;
        if (entry.size > options.maximum_total_file_bytes - result.examined_bytes) break;
        const auto absolute_lba = static_cast<std::uint64_t>(extent_lba_bias) + entry.lba;
        if (absolute_lba >
            std::numeric_limits<std::uint64_t>::max() / iso_sector_size)
            throw std::overflow_error("Latenter Discdateioffset laeuft ueber.");
        const auto disc_byte_offset = absolute_lba * iso_sector_size;
        if (disc_byte_offset > source->size() ||
            entry.size > source->size() - disc_byte_offset)
            throw std::runtime_error("Latente Discdatei liegt ausserhalb der Discquelle.");
        auto bytes = filesystem.read_file(entry, static_cast<std::uint32_t>(
                                                    options.maximum_file_bytes));
        if (bytes.size() != entry.size)
            throw std::runtime_error("Latente Discdatei wurde abgeschnitten gelesen.");
        ++result.examined_files;
        result.examined_bytes += bytes.size();
        auto byte_identity =
            "sha256:" + katana::io::sha256_bytes(std::string_view(
                            reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (!known_identities.insert(byte_identity).second) {
            ++result.duplicate_files;
            continue;
        }
        bool placed = false;
        next_source = align_up(next_source, 4096u);
        while (static_cast<std::uint64_t>(next_source) + bytes.size() <=
               options.source_address_end) {
            const LatentAotOccupiedRange proposed{next_source, bytes.size()};
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
        if (!placed) break;
        const auto source_end = static_cast<std::uint64_t>(next_source) + bytes.size();
        candidates.push_back({path,
                              entry.lba,
                              entry.size,
                              next_source,
                              disc_byte_offset,
                              std::move(bytes),
                              std::move(byte_identity)});
        occupied.push_back({next_source, entry.size});
        next_source = static_cast<std::uint32_t>(source_end);
    }

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
    for (auto& candidate : analyzed) {
        if (candidate)
            result.modules.push_back(std::move(*candidate));
        else
            ++result.rejected_files;
    }
    return result;
}

} // namespace katana::codegen
