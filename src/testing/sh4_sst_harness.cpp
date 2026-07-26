#include "katana/testing/sh4_sst_harness.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace katana::testing::sh4_sst {
namespace {

std::string hex_value(const std::uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << value;
    return output.str();
}

std::string memory_operation_name(const SstMemoryOperation operation) {
    return operation == SstMemoryOperation::Read ? "read" : "write";
}

void append_expected_memory(const SstCycle& cycle,
                            const std::uint32_t executing_pc,
                            std::vector<SstMemoryObservation>& result) {
    if (cycle.has_read()) {
        result.push_back({executing_pc,
                          result.size(),
                          SstMemoryOperation::Read,
                          cycle.read_address,
                          cycle.read_size,
                          cycle.read_value});
    }
    if (cycle.has_write()) {
        result.push_back({executing_pc,
                          result.size(),
                          SstMemoryOperation::Write,
                          cycle.write_address,
                          cycle.write_size,
                          cycle.write_value});
    }
}

} // namespace

std::vector<NativeInstructionObservation>
expected_native_instructions(const SstTestCase& test, const bool tested_has_delay_slot) {
    std::vector<NativeInstructionObservation> result;
    result.reserve(test.cycles.size());
    for (std::size_t cycle_index = 0u; cycle_index < test.cycles.size(); ++cycle_index) {
        const auto& cycle = test.cycles[cycle_index];
        if (!cycle.has_fetch()) {
            throw SstHarnessInvalid("SST cycle " + std::to_string(cycle_index) +
                                    " lacks the required fetch oracle");
        }
        result.push_back({cycle.fetch_address, tested_has_delay_slot && cycle_index == 2u});
    }
    return result;
}

std::vector<SstMemoryObservation> expected_memory_observations(const SstTestCase& test) {
    std::vector<SstMemoryObservation> result;
    result.reserve(test.cycles.size() * 2u);
    for (std::size_t cycle_index = 0u; cycle_index < test.cycles.size(); ++cycle_index) {
        const auto& cycle = test.cycles[cycle_index];
        if (!cycle.has_fetch()) {
            throw SstHarnessInvalid(
                "Cannot attribute SST data access without a fetch-address oracle");
        }
        if (!cycle.has_read() && !cycle.has_write()) continue;
        if (cycle_index == 0u || !test.cycles[cycle_index - 1u].has_fetch()) {
            throw SstHarnessInvalid(
                "SST data access precedes the instruction fetch needed for attribution");
        }

        // The upstream recorder stores a data access in the entry after the
        // instruction's fetch. This is a trace-format convention, not evidence
        // of a hardware pipeline. Attribute the event to the preceding fetch.
        append_expected_memory(cycle, test.cycles[cycle_index - 1u].fetch_address, result);
    }
    return result;
}

HarnessComparison compare_native_instructions(const SstTestCase& test,
                                              const bool tested_has_delay_slot,
                                              const NativeExecutionTrace& actual) {
    const auto expected = expected_native_instructions(test, tested_has_delay_slot);
    if (actual.instructions.size() != expected.size()) {
        return {ResultClassification::FailControlFlow,
                "expected " + std::to_string(expected.size()) +
                    " native instruction observations, got " +
                    std::to_string(actual.instructions.size())};
    }
    for (std::size_t index = 0u; index < expected.size(); ++index) {
        if (actual.instructions[index].guest_pc != expected[index].guest_pc) {
            return {ResultClassification::FailControlFlow,
                    "instruction " + std::to_string(index) + " expected guest PC " +
                        hex_value(expected[index].guest_pc) + ", got " +
                        hex_value(actual.instructions[index].guest_pc)};
        }
        if (actual.instructions[index].delay_slot != expected[index].delay_slot) {
            return {ResultClassification::FailDelaySlot,
                    "instruction " + std::to_string(index) +
                        (expected[index].delay_slot ? " is missing its delay-slot marker"
                                                    : " has an extra delay-slot marker")};
        }
    }
    return {};
}

HarnessComparison compare_memory_observations(const std::vector<SstMemoryObservation>& expected,
                                              const std::vector<SstMemoryObservation>& actual) {
    const auto common = std::min(expected.size(), actual.size());
    for (std::size_t index = 0u; index < common; ++index) {
        const auto& wanted = expected[index];
        const auto& observed = actual[index];
        if (wanted.operation != observed.operation) {
            return {ResultClassification::FailMemoryOrder,
                    "memory event " + std::to_string(index) + " expected " +
                        memory_operation_name(wanted.operation) + ", got " +
                        memory_operation_name(observed.operation)};
        }
        if (wanted.executing_guest_pc != observed.executing_guest_pc) {
            return {ResultClassification::FailMemoryOrder,
                    "memory event " + std::to_string(index) + " was attributed to guest PC " +
                        hex_value(observed.executing_guest_pc) + " instead of " +
                        hex_value(wanted.executing_guest_pc)};
        }
        if (wanted.address != observed.address) {
            return {ResultClassification::FailMemoryAddress,
                    "memory event " + std::to_string(index) + " expected address " +
                        hex_value(wanted.address) + ", got " + hex_value(observed.address)};
        }
        if (wanted.width != observed.width) {
            return {ResultClassification::FailMemoryWidth,
                    "memory event " + std::to_string(index) + " expected width " +
                        std::to_string(wanted.width) + ", got " + std::to_string(observed.width)};
        }
        if (wanted.value != observed.value) {
            return {ResultClassification::FailMemoryValue,
                    "memory event " + std::to_string(index) + " expected value " +
                        hex_value(wanted.value) + ", got " + hex_value(observed.value)};
        }
    }
    if (actual.size() > expected.size()) {
        return {ResultClassification::FailExtraSideEffect,
                "observed " + std::to_string(actual.size() - expected.size()) +
                    " extra memory event(s)"};
    }
    if (actual.size() < expected.size()) {
        return {ResultClassification::FailMemoryOrder,
                "missing " + std::to_string(expected.size() - actual.size()) + " memory event(s)"};
    }
    return {};
}

HarnessComparison compare_case_result(const CaseComparisonInput& input) {
    if (input.test == nullptr || input.native_trace == nullptr || input.actual_memory == nullptr) {
        return {ResultClassification::HarnessInvalid,
                "case comparison is missing test, trace, or memory input"};
    }
    if (input.execution_failure) {
        return {*input.execution_failure, input.execution_failure_detail};
    }
    if (const auto control = compare_native_instructions(
            *input.test, input.tested_has_delay_slot, *input.native_trace);
        !control.passed()) {
        return control;
    }
    const auto expected_memory = expected_memory_observations(*input.test);
    if (const auto memory = compare_memory_observations(expected_memory, *input.actual_memory);
        !memory.passed()) {
        return memory;
    }
    const auto state =
        compare_sst_states(input.test->final, input.actual_state, input.fpu_comparison);
    if (!state.matches()) {
        return {ResultClassification::FailState,
                "final architectural state differs at " + state.differences.front().path};
    }
    if (!input.internal_state.matches()) {
        return {ResultClassification::FailExtraSideEffect,
                "Katana-internal state changed at " +
                    input.internal_state.differences.front().path};
    }
    return {};
}

bool is_direct_product_ram_access(const std::uint32_t address, const std::uint32_t width) noexcept {
    if (width != 1u && width != 2u && width != 4u) return false;
    if ((address & (width - 1u)) != 0u) return false;
    if (address > std::numeric_limits<std::uint32_t>::max() - (width - 1u)) return false;

    const auto segment = address & 0xE0000000u;
    if (segment != 0u && segment != 0x80000000u && segment != 0xA0000000u) return false;
    constexpr auto main_ram_begin = runtime::dreamcast_main_ram_area_bases.front();
    constexpr auto main_ram_end =
        main_ram_begin + static_cast<std::uint32_t>(runtime::dreamcast_main_ram_size *
                                                    runtime::dreamcast_main_ram_mirrors_per_area);
    const auto physical = address & 0x1FFFFFFFu;
    return physical >= main_ram_begin &&
           static_cast<std::uint64_t>(physical) + width <= main_ram_end;
}

Applicability classify_case_applicability(const SstTestCase& test,
                                          const bool katana_family_supported,
                                          const MemoryProfile profile) {
    if (!katana_family_supported) {
        return {false,
                ResultClassification::NotApplicableKatanaRestricted,
                "ISA family is declared restricted by Katana"};
    }

    if (profile == MemoryProfile::NativeProductMemory &&
        (test.initial.sr & runtime::sr_md_mask) == 0u &&
        std::any_of(test.cycles.begin(), test.cycles.end(), [](const auto& cycle) {
            return cycle.has_fetch() && cycle.fetch_address >= 0x80000000u;
        })) {
        return Applicability{false,
                             ResultClassification::NotApplicableReferenceException,
                             "upstream flat-memory reference fetched from a privileged segment "
                             "with SR.MD clear"};
    }

    for (const auto& cycle : test.cycles) {
        const auto classify_access =
            [&](const bool active,
                const std::uint32_t address,
                const std::uint32_t width) -> std::optional<Applicability> {
            if (!active) return std::nullopt;
            if (width == 8u) {
                return Applicability{false,
                                     ResultClassification::NotApplicableAccessShape,
                                     "8-byte SST access has no single-event Katana runtime "
                                     "contract"};
            }
            if (width != 1u && width != 2u && width != 4u) {
                return Applicability{false,
                                     ResultClassification::CorpusInvalid,
                                     "SST access width is not 1, 2, 4, or 8 bytes"};
            }
            if ((address & (width - 1u)) != 0u) {
                return Applicability{
                    false,
                    ResultClassification::NotApplicableReferenceAlignment,
                    "upstream flat-memory reference performed a misaligned access"};
            }
            if (profile == MemoryProfile::NativeProductMemory && address >= 0x80000000u &&
                (test.initial.sr & runtime::sr_md_mask) == 0u) {
                return Applicability{false,
                                     ResultClassification::NotApplicableReferenceException,
                                     "upstream flat-memory reference accessed a privileged segment "
                                     "with SR.MD clear"};
            }
            if (profile == MemoryProfile::NativeProductMemory &&
                !is_direct_product_ram_access(address, width)) {
                return Applicability{false,
                                     ResultClassification::NotApplicableReferenceMmio,
                                     "address is outside directly translated Dreamcast main RAM"};
            }
            return std::nullopt;
        };
        if (const auto result =
                classify_access(cycle.has_read(), cycle.read_address, cycle.read_size)) {
            return *result;
        }
        if (const auto result =
                classify_access(cycle.has_write(), cycle.write_address, cycle.write_size)) {
            return *result;
        }
    }
    return {};
}

runtime::GuestMemoryAccessSink MemoryTraceRecorder::sink() noexcept {
    return {this, &MemoryTraceRecorder::record};
}

void MemoryTraceRecorder::clear() noexcept {
    observations_.clear();
    allocation_failed_ = false;
}

const std::vector<SstMemoryObservation>& MemoryTraceRecorder::observations() const noexcept {
    return observations_;
}

bool MemoryTraceRecorder::complete() const noexcept {
    return !allocation_failed_;
}

void MemoryTraceRecorder::record(void* const context,
                                 const runtime::GuestMemoryAccessEvent& event) noexcept {
    auto& self = *static_cast<MemoryTraceRecorder*>(context);
    if (self.allocation_failed_) return;
    try {
        self.observations_.push_back({event.instruction.runtime_pc,
                                      self.observations_.size(),
                                      event.operation == runtime::MemoryAccessOperation::Read
                                          ? SstMemoryOperation::Read
                                          : SstMemoryOperation::Write,
                                      event.virtual_address,
                                      static_cast<std::uint32_t>(event.size),
                                      event.value});
    } catch (...) {
        self.allocation_failed_ = true;
    }
}

} // namespace katana::testing::sh4_sst
