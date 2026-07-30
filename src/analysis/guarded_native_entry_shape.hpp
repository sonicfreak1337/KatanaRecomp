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

enum class GuardedNativeEntryShapeStatus : std::uint8_t {
    Valid,
    StructurallyInvalid,
    OutsideImage,
    ShapeBudgetExceeded
};

struct GuardedNativeEntryShapeStatistics {
    std::size_t work = 0u;
    std::size_t work_budget = 0u;
    std::size_t valid = 0u;
    std::size_t structurally_invalid = 0u;
    std::size_t outside_image = 0u;
    std::size_t shape_budget_exceeded = 0u;
};

class GuardedNativeEntryShapeCache {
  public:
    explicit GuardedNativeEntryShapeCache(const katana::io::ExecutableImage& image)
        : image_(image) {
        statistics_.work_budget = maximum_total_instructions;
    }

    [[nodiscard]] GuardedNativeEntryShapeStatus
    classify(const std::uint32_t address) {
        if (const auto cached = results_.find(address); cached != results_.end())
            return cached->second;
        const auto validation = validate_decode_candidate(image_, address);
        if (!validation.valid()) {
            return remember(address,
                            GuardedNativeEntryShapeStatus::OutsideImage);
        }
        const auto canonical_address = validation.resolved_address;
        if (const auto cached = results_.find(canonical_address); cached != results_.end()) {
            results_.insert_or_assign(address, cached->second);
            return cached->second;
        }
        const auto result = validate(canonical_address);
        static_cast<void>(remember(canonical_address, result));
        results_.insert_or_assign(address, result);
        return result;
    }

    [[nodiscard]] const GuardedNativeEntryShapeStatistics&
    statistics() const noexcept {
        return statistics_;
    }

  private:
    static constexpr std::size_t maximum_instructions = 4'096u;
    // Prevent invalid data candidates from expanding validation work beyond the
    // worst-case work admitted entries could have consumed.  This cache lives
    // across the outer control-flow fixpoint, so the limit is global to the
    // complete analysis rather than resetting on each iteration.
    static constexpr std::size_t maximum_total_instructions =
        maximum_instructions * 1'024u;

    struct DecodeResult {
        GuardedNativeEntryShapeStatus status =
            GuardedNativeEntryShapeStatus::StructurallyInvalid;
        std::optional<katana::sh4::DecodedInstruction> instruction;
    };

    [[nodiscard]] GuardedNativeEntryShapeStatus
    remember(const std::uint32_t address,
             const GuardedNativeEntryShapeStatus status) {
        const auto [iterator, inserted] = results_.try_emplace(address, status);
        if (!inserted) return iterator->second;
        switch (status) {
        case GuardedNativeEntryShapeStatus::Valid:
            ++statistics_.valid;
            break;
        case GuardedNativeEntryShapeStatus::StructurallyInvalid:
            ++statistics_.structurally_invalid;
            break;
        case GuardedNativeEntryShapeStatus::OutsideImage:
            ++statistics_.outside_image;
            break;
        case GuardedNativeEntryShapeStatus::ShapeBudgetExceeded:
            ++statistics_.shape_budget_exceeded;
            break;
        }
        return status;
    }

    [[nodiscard]] DecodeResult
    decode_at(const std::uint32_t address,
              std::size_t& candidate_work) {
        const auto validation = validate_decode_candidate(image_, address);
        if (!validation.valid() || validation.segment == nullptr)
            return {GuardedNativeEntryShapeStatus::OutsideImage, std::nullopt};
        if (candidate_work >= maximum_instructions ||
            statistics_.work >= statistics_.work_budget)
            return {GuardedNativeEntryShapeStatus::ShapeBudgetExceeded,
                    std::nullopt};
        ++candidate_work;
        ++statistics_.work;
        const auto offset = validation.segment->byte_offset(validation.resolved_address);
        if (!offset.has_value() || *offset > validation.segment->bytes.size() ||
            validation.segment->bytes.size() - *offset < 2u)
            return {GuardedNativeEntryShapeStatus::OutsideImage, std::nullopt};
        const auto instruction = katana::sh4::decode(
            katana::io::read_u16_le(validation.segment->bytes, *offset));
        if (!instruction.is_known())
            return {GuardedNativeEntryShapeStatus::StructurallyInvalid,
                    std::nullopt};
        return {GuardedNativeEntryShapeStatus::Valid, instruction};
    }

    [[nodiscard]] GuardedNativeEntryShapeStatus
    validate(const std::uint32_t entry_address) {
        std::deque<std::uint32_t> pending{entry_address};
        std::unordered_set<std::uint32_t> visited;
        visited.reserve(maximum_instructions);
        std::size_t candidate_work = 0u;
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
            if (address != entry_address) {
                const auto cached = results_.find(address);
                if (cached != results_.end()) {
                    if (cached->second ==
                        GuardedNativeEntryShapeStatus::Valid)
                        continue;
                    return cached->second;
                }
            }

            const auto decoded = decode_at(address, candidate_work);
            if (!decoded.instruction.has_value()) return decoded.status;
            const auto& instruction = *decoded.instruction;
            if (instruction.has_delay_slot) {
                if (address > std::numeric_limits<std::uint32_t>::max() - 2u)
                    return GuardedNativeEntryShapeStatus::OutsideImage;
                const auto delay = decode_at(address + 2u, candidate_work);
                if (!delay.instruction.has_value()) return delay.status;
                if (delay.instruction->changes_control_flow())
                    return GuardedNativeEntryShapeStatus::StructurallyInvalid;
            }

            switch (instruction.control_flow) {
            case katana::sh4::ControlFlowKind::None:
                if (!enqueue_fallthrough(address, instruction.has_delay_slot))
                    return GuardedNativeEntryShapeStatus::OutsideImage;
                break;
            case katana::sh4::ControlFlowKind::ConditionalBranch: {
                const auto target =
                    katana::sh4::calculate_direct_branch_target(instruction, address);
                if (!target.has_value() ||
                    !enqueue_fallthrough(address, instruction.has_delay_slot))
                    return GuardedNativeEntryShapeStatus::StructurallyInvalid;
                pending.push_back(*target);
                break;
            }
            case katana::sh4::ControlFlowKind::Call:
            case katana::sh4::ControlFlowKind::IndirectCall:
                // A candidate entry owns its local continuation, not the
                // independently validated native entry of a callee.
                if (!enqueue_fallthrough(address, instruction.has_delay_slot))
                    return GuardedNativeEntryShapeStatus::OutsideImage;
                break;
            case katana::sh4::ControlFlowKind::UnconditionalBranch: {
                const auto target =
                    katana::sh4::calculate_direct_branch_target(instruction, address);
                if (!target.has_value())
                    return GuardedNativeEntryShapeStatus::StructurallyInvalid;
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
        // A successful walk proves every ordinary CFG node reached from this
        // entry.  Cache those suffixes as well; delay-slot-only decodes were
        // never inserted into visited and therefore never become standalone
        // entry proofs.  Large families of tiny wrappers and shared tails no
        // longer repeat the same 4K walk for every candidate.
        for (const auto address : visited) {
            if (address != entry_address)
                results_.try_emplace(
                    address, GuardedNativeEntryShapeStatus::Valid);
        }
        return GuardedNativeEntryShapeStatus::Valid;
    }

    const katana::io::ExecutableImage& image_;
    std::unordered_map<std::uint32_t, GuardedNativeEntryShapeStatus> results_;
    GuardedNativeEntryShapeStatistics statistics_;
};

[[nodiscard]] FunctionValueAnalysisResult
analyze_function_values_with_guarded_entry_cache(
    const katana::io::ExecutableImage& image,
    std::span<const katana::sh4::DisassemblyLine> lines,
    std::span<const FunctionBoundary> function_boundaries,
    std::span<const ResolvedControlFlowEdge> resolved_edges,
    const FunctionValueAnalysisProgressCallback& progress_callback,
    GuardedNativeEntryShapeCache& guarded_native_entry_shapes,
    const AbiContractObserver& abi_contract_observer = {});

} // namespace katana::analysis::detail
