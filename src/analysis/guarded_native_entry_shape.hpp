#pragma once

#include "katana/analysis/code_address.hpp"
#include "katana/analysis/function_value_analysis.hpp"
#include "katana/io/binary_reader.hpp"
#include "katana/sh4/decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace katana::analysis::detail {

class GuardedNativeEntryShapeCache {
  public:
    explicit GuardedNativeEntryShapeCache(const katana::io::ExecutableImage& image)
        : image_(image) {}

    [[nodiscard]] bool valid(const std::uint32_t address) {
        const auto validation = validate_decode_candidate(image_, address);
        if (!validation.valid()) {
            results_.insert_or_assign(address, false);
            return false;
        }
        const auto canonical_address = validation.resolved_address;
        if (const auto cached = results_.find(canonical_address); cached != results_.end()) {
            results_.insert_or_assign(address, cached->second);
            return cached->second;
        }
        const auto result = validate(canonical_address);
        results_.insert_or_assign(canonical_address, result);
        results_.insert_or_assign(address, result);
        return result;
    }

  private:
    static constexpr std::size_t maximum_instructions = 4'096u;

    [[nodiscard]] std::optional<katana::sh4::DecodedInstruction>
    decode_at(const std::uint32_t address) const {
        const auto validation = validate_decode_candidate(image_, address);
        if (!validation.valid() || validation.segment == nullptr) return std::nullopt;
        const auto offset = validation.segment->byte_offset(validation.resolved_address);
        if (!offset.has_value() || *offset > validation.segment->bytes.size() ||
            validation.segment->bytes.size() - *offset < 2u)
            return std::nullopt;
        const auto instruction = katana::sh4::decode(
            katana::io::read_u16_le(validation.segment->bytes, *offset));
        return instruction.is_known()
                   ? std::optional<katana::sh4::DecodedInstruction>{instruction}
                   : std::nullopt;
    }

    [[nodiscard]] bool validate(const std::uint32_t entry_address) const {
        std::deque<std::uint32_t> pending{entry_address};
        std::unordered_set<std::uint32_t> visited;
        visited.reserve(maximum_instructions);
        const auto enqueue_fallthrough =
            [&pending](const std::uint32_t address, const bool has_delay_slot) {
                const auto distance = has_delay_slot ? 4u : 2u;
                if (address > std::numeric_limits<std::uint32_t>::max() - distance)
                    return false;
                pending.push_back(address + distance);
                return true;
            };

        while (!pending.empty()) {
            const auto address = pending.front();
            pending.pop_front();
            if (!visited.insert(address).second) continue;
            if (visited.size() > maximum_instructions) return false;

            const auto instruction = decode_at(address);
            if (!instruction.has_value()) return false;
            if (instruction->has_delay_slot) {
                if (address > std::numeric_limits<std::uint32_t>::max() - 2u)
                    return false;
                const auto delay = decode_at(address + 2u);
                if (!delay.has_value() || delay->changes_control_flow()) return false;
            }

            switch (instruction->control_flow) {
            case katana::sh4::ControlFlowKind::None:
                if (!enqueue_fallthrough(address, instruction->has_delay_slot)) return false;
                break;
            case katana::sh4::ControlFlowKind::ConditionalBranch: {
                const auto target =
                    katana::sh4::calculate_direct_branch_target(*instruction, address);
                if (!target.has_value() ||
                    !enqueue_fallthrough(address, instruction->has_delay_slot))
                    return false;
                pending.push_back(*target);
                break;
            }
            case katana::sh4::ControlFlowKind::Call:
            case katana::sh4::ControlFlowKind::IndirectCall:
                // A candidate entry owns its local continuation, not the
                // independently validated native entry of a callee.
                if (!enqueue_fallthrough(address, instruction->has_delay_slot)) return false;
                break;
            case katana::sh4::ControlFlowKind::UnconditionalBranch: {
                const auto target =
                    katana::sh4::calculate_direct_branch_target(*instruction, address);
                if (!target.has_value()) return false;
                pending.push_back(*target);
                break;
            }
            case katana::sh4::ControlFlowKind::Return:
            case katana::sh4::ControlFlowKind::IndirectBranch:
            case katana::sh4::ControlFlowKind::Trap:
            case katana::sh4::ControlFlowKind::ExceptionReturn:
            case katana::sh4::ControlFlowKind::Halt:
                break;
            }
        }
        return true;
    }

    const katana::io::ExecutableImage& image_;
    std::unordered_map<std::uint32_t, bool> results_;
};

[[nodiscard]] FunctionValueAnalysisResult
analyze_function_values_with_guarded_entry_cache(
    const katana::io::ExecutableImage& image,
    std::span<const katana::sh4::DisassemblyLine> lines,
    std::span<const FunctionBoundary> function_boundaries,
    std::span<const ResolvedControlFlowEdge> resolved_edges,
    const FunctionValueAnalysisProgressCallback& progress_callback,
    GuardedNativeEntryShapeCache& guarded_native_entry_shapes);

} // namespace katana::analysis::detail
