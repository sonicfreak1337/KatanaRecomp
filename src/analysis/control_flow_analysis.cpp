#include "katana/analysis/control_flow_analysis.hpp"

#include "katana/analysis/code_address.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

namespace katana::analysis {
namespace {

std::string hex_address(const std::uint32_t address) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << address;
    return output.str();
}

[[noreturn]] void override_error(
    const AnalysisOverrides& overrides,
    const std::size_t line,
    const std::uint32_t address,
    const std::string& cause
) {
    throw std::runtime_error(
        "Override-Fehler in " + overrides.source_path.string() + " in Zeile "
        + std::to_string(line) + " bei " + hex_address(address) + ": " + cause + "."
    );
}

void require_override_code_address(
    const katana::io::ExecutableImage& image,
    const AnalysisOverrides& overrides,
    const std::size_t line,
    const std::uint32_t address
) {
    const auto validation = validate_committed_code_address(image, address);
    if (!validation.valid()) {
        override_error(
            overrides, line, address,
            code_address_status_name(validation.status)
        );
    }
}

bool add_seed(
    std::map<std::uint32_t, std::set<FunctionOrigin>>& seeds,
    const std::uint32_t address,
    const std::span<const FunctionOrigin> origins = {}
) {
    const auto [iterator, inserted] = seeds.try_emplace(address);
    bool changed = inserted;
    for (const auto origin : origins) {
        changed = iterator->second.insert(origin).second || changed;
    }
    return changed;
}

RecursiveAnalysisOptions make_options(
    const std::map<std::uint32_t, std::set<FunctionOrigin>>& seeds
) {
    RecursiveAnalysisOptions options;
    options.additional_seeds.reserve(seeds.size());
    for (const auto& [address, origins] : seeds) {
        AnalysisSeed seed;
        seed.address = address;
        seed.function_origins.assign(origins.begin(), origins.end());
        options.additional_seeds.push_back(std::move(seed));
    }
    return options;
}

const katana::sh4::DisassemblyLine* find_instruction(
    const RecursiveAnalysisResult& result,
    const std::uint32_t address
) {
    const auto iterator = std::lower_bound(
        result.instructions.begin(), result.instructions.end(), address,
        [](const auto& line, const std::uint32_t candidate) {
            return line.address < candidate;
        }
    );
    return iterator != result.instructions.end() && iterator->address == address
        ? &*iterator
        : nullptr;
}

}

ControlFlowAnalysisResult analyze_control_flow(
    const katana::io::ExecutableImage& image,
    const AnalysisOverrides* overrides
) {
    std::map<std::uint32_t, std::set<FunctionOrigin>> seeds;
    if (overrides != nullptr) {
        for (const auto& function : overrides->functions) {
            require_override_code_address(
                image, *overrides, function.line, function.address
            );
            const std::array origins{FunctionOrigin::UserOverride};
            static_cast<void>(add_seed(seeds, function.address, origins));
        }
    }

    ControlFlowAnalysisResult analysis;
    for (;;) {
        ++analysis.fixpoint_iterations;
        analysis.recursive = analyze_reachable_code(image, make_options(seeds));
        analysis.indirect_control_flow = resolve_indirect_control_flow(
            analysis.recursive.instructions, image
        );
        analysis.jump_tables.clear();
        bool missing_override_dispatch = false;

        if (overrides != nullptr) {
            for (const auto& jump : overrides->jumps) {
                const auto resolution = std::find_if(
                    analysis.indirect_control_flow.begin(),
                    analysis.indirect_control_flow.end(),
                    [&jump](const auto& candidate) {
                        return candidate.instruction_address == jump.instruction_address;
                    }
                );
                if (resolution == analysis.indirect_control_flow.end()) {
                    missing_override_dispatch = true;
                    continue;
                }
                require_override_code_address(
                    image, *overrides, jump.line, jump.target
                );
                resolution->status = ResolutionStatus::Resolved;
                resolution->target = jump.target;
                resolution->reason = "user-override";
            }

            for (const auto& table : overrides->jump_tables) {
                require_override_code_address(
                    image, *overrides, table.line, table.dispatch_address
                );
                const auto* dispatch = find_instruction(
                    analysis.recursive, table.dispatch_address
                );
                if (dispatch == nullptr) {
                    missing_override_dispatch = true;
                    continue;
                }
                if (dispatch->instruction.kind != katana::sh4::InstructionKind::Jmp
                    && dispatch->instruction.kind != katana::sh4::InstructionKind::Jsr) {
                    override_error(
                        *overrides, table.line, table.dispatch_address,
                        "dispatch-not-jmp-or-jsr"
                    );
                }
                auto jump_table = analyze_jump_table(
                    image,
                    table.dispatch_address,
                    table.table_address,
                    table.entry_count
                );
                jump_table.dispatch_kind = dispatch->instruction.kind
                        == katana::sh4::InstructionKind::Jsr
                    ? JumpTableDispatchKind::Call
                    : JumpTableDispatchKind::Jump;
                analysis.jump_tables.push_back(std::move(jump_table));
            }
        }

        bool changed = false;
        for (const auto& resolution : analysis.indirect_control_flow) {
            if (resolution.status != ResolutionStatus::Resolved
                || !resolution.target.has_value()) {
                continue;
            }
            if (resolution.kind == IndirectControlFlowKind::Call) {
                if (resolution.reason == "user-override") {
                    const std::array origins{
                        FunctionOrigin::IndirectCall,
                        FunctionOrigin::UserOverride
                    };
                    changed = add_seed(seeds, *resolution.target, origins) || changed;
                } else {
                    const std::array origins{FunctionOrigin::IndirectCall};
                    changed = add_seed(seeds, *resolution.target, origins) || changed;
                }
            } else {
                changed = add_seed(seeds, *resolution.target) || changed;
            }
        }
        if (overrides != nullptr) {
            for (std::size_t index = 0u; index < analysis.jump_tables.size(); ++index) {
                const auto& table = analysis.jump_tables[index];
                if (!table.resolved) {
                    continue;
                }
                const bool is_call = table.dispatch_kind == JumpTableDispatchKind::Call;
                for (const auto& entry : table.entries) {
                    if (is_call) {
                        const std::array origins{
                            FunctionOrigin::JumpTableCall,
                            FunctionOrigin::UserOverride
                        };
                        changed = add_seed(seeds, entry.target, origins) || changed;
                    } else {
                        changed = add_seed(seeds, entry.target) || changed;
                    }
                }
            }
        }
        if (!changed && missing_override_dispatch && overrides != nullptr) {
            for (const auto& jump : overrides->jumps) {
                const auto resolution = std::find_if(
                    analysis.indirect_control_flow.begin(),
                    analysis.indirect_control_flow.end(),
                    [&jump](const auto& candidate) {
                        return candidate.instruction_address == jump.instruction_address;
                    }
                );
                if (resolution == analysis.indirect_control_flow.end()) {
                    override_error(
                        *overrides, jump.line, jump.instruction_address,
                        "dispatch-not-discovered-indirect-control-flow"
                    );
                }
            }
            for (const auto& table : overrides->jump_tables) {
                if (find_instruction(analysis.recursive, table.dispatch_address) == nullptr) {
                    override_error(
                        *overrides, table.line, table.dispatch_address,
                        "dispatch-not-discovered"
                    );
                }
            }
        }
        if (!changed) {
            break;
        }
    }
    return analysis;
}

}
