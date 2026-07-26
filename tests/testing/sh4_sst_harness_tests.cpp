#include "katana/testing/sh4_sst_harness.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using katana::testing::ResultClassification;
using namespace katana::testing::sh4_sst;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

katana::testing::SstTestCase fixture() {
    katana::testing::SstTestCase test;
    test.initial.pc = 0x1000u;
    test.final = test.initial;
    test.final.pc = 0x1008u;
    for (std::size_t index = 0u; index < test.cycles.size(); ++index) {
        test.cycles[index].actions = katana::testing::SstCycle::fetch_action;
        test.cycles[index].fetch_address = 0x1000u + static_cast<std::uint32_t>(index * 2u);
    }
    test.cycles[2].actions |=
        katana::testing::SstCycle::read_action | katana::testing::SstCycle::write_action;
    test.cycles[2].read_address = 0x2000u;
    test.cycles[2].read_size = 4u;
    test.cycles[2].read_value = 0x12345678u;
    test.cycles[2].write_address = 0x2004u;
    test.cycles[2].write_size = 2u;
    test.cycles[2].write_value = 0xABCDu;
    return test;
}

struct ValidComparison {
    katana::testing::SstTestCase test = fixture();
    NativeExecutionTrace trace;
    std::vector<katana::testing::SstMemoryObservation> memory;
    CaseComparisonInput input;

    explicit ValidComparison(const bool delay_slot = true) {
        trace.instructions = expected_native_instructions(test, delay_slot);
        memory = expected_memory_observations(test);
        input.test = &test;
        input.native_trace = &trace;
        input.actual_memory = &memory;
        input.actual_state = test.final;
        input.tested_has_delay_slot = delay_slot;
    }
};

void require_classification(const CaseComparisonInput& input,
                            const ResultClassification expected,
                            const std::string& diagnostic) {
    const auto result = compare_case_result(input);
    require(result.classification == expected,
            diagnostic + ": expected " + katana::testing::result_classification_name(expected) +
                ", got " + katana::testing::result_classification_name(result.classification));
}

} // namespace

int main() {
    try {
        {
            ValidComparison value;
            require(value.memory.size() == 2u &&
                        value.memory[0].executing_guest_pc == value.test.cycles[1].fetch_address &&
                        value.memory[1].executing_guest_pc == value.test.cycles[1].fetch_address,
                    "SST cycle data access was not attributed to the preceding fetch");
            require_classification(value.input, ResultClassification::Pass, "valid control");
        }
        {
            ValidComparison value;
            ++value.input.actual_state.r[3];
            require_classification(value.input, ResultClassification::FailState, "wrong register");
        }
        {
            ValidComparison value;
            value.input.actual_state.sr ^= 1u;
            require_classification(
                value.input, ResultClassification::FailState, "wrong status bit");
        }
        {
            ValidComparison value;
            ++value.trace.instructions[1].guest_pc;
            require_classification(
                value.input, ResultClassification::FailControlFlow, "wrong guest PC");
        }
        {
            ValidComparison value;
            value.trace.instructions[2].delay_slot = false;
            require_classification(
                value.input, ResultClassification::FailDelaySlot, "missing delay slot");
        }
        {
            ValidComparison value(false);
            value.trace.instructions[2].delay_slot = true;
            require_classification(
                value.input, ResultClassification::FailDelaySlot, "extra delay slot");
        }
        {
            ValidComparison value;
            ++value.memory[0].address;
            require_classification(
                value.input, ResultClassification::FailMemoryAddress, "wrong memory address");
        }
        {
            ValidComparison value;
            ++value.memory[0].value;
            require_classification(
                value.input, ResultClassification::FailMemoryValue, "wrong memory value");
        }
        {
            ValidComparison value;
            value.memory[0].width = 2u;
            require_classification(
                value.input, ResultClassification::FailMemoryWidth, "wrong memory width");
        }
        {
            ValidComparison value;
            value.memory.push_back(value.memory.back());
            require_classification(
                value.input, ResultClassification::FailExtraSideEffect, "extra memory access");
        }
        {
            ValidComparison value;
            value.memory.pop_back();
            require_classification(
                value.input, ResultClassification::FailMemoryOrder, "missing memory access");
        }
        {
            ValidComparison value;
            value.input.execution_failure = ResultClassification::FailUnboundTarget;
            value.input.execution_failure_detail = "negative AOT-dispatch control";
            require_classification(
                value.input, ResultClassification::FailUnboundTarget, "unbound target");
        }
        {
            auto value = fixture();
            value.cycles[2].actions = katana::testing::SstCycle::fetch_action;
            value.cycles[0].actions |= katana::testing::SstCycle::read_action;
            value.cycles[0].read_address = 0x2000u;
            value.cycles[0].read_size = 4u;
            value.cycles[0].read_value = 0x12345678u;
            bool rejected = false;
            try {
                static_cast<void>(expected_memory_observations(value));
            } catch (const katana::testing::SstHarnessInvalid&) {
                rejected = true;
            }
            require(rejected, "unattributable cycle-zero data access was accepted");
        }
        {
            auto value = fixture();
            value.cycles[2].read_address = 0x8C000000u;
            value.cycles[2].write_address = 0x8C000004u;
            value.initial.sr &= ~katana::runtime::sr_md_mask;
            const auto user = classify_case_applicability(
                value, true, katana::testing::MemoryProfile::NativeProductMemory);
            require(!user.applicable && user.classification ==
                                            ResultClassification::NotApplicableReferenceException,
                    "user-mode access to a privileged product segment was gated");

            value.initial.sr |= katana::runtime::sr_md_mask;
            const auto privileged = classify_case_applicability(
                value, true, katana::testing::MemoryProfile::NativeProductMemory);
            require(privileged.applicable, "privileged direct product-RAM access was rejected");
        }
        {
            auto value = fixture();
            value.initial.pc = 0x8C001000u;
            value.cycles[2].read_address = 0x8C000000u;
            value.cycles[2].write_address = 0x8C000004u;
            for (std::size_t index = 0u; index < value.cycles.size(); ++index)
                value.cycles[index].fetch_address =
                    value.initial.pc + static_cast<std::uint32_t>(index * 2u);
            value.initial.sr &= ~katana::runtime::sr_md_mask;
            const auto user = classify_case_applicability(
                value, true, katana::testing::MemoryProfile::NativeProductMemory);
            require(!user.applicable && user.classification ==
                                            ResultClassification::NotApplicableReferenceException,
                    "user-mode fetch from a privileged product segment was gated");

            value.initial.sr |= katana::runtime::sr_md_mask;
            const auto privileged = classify_case_applicability(
                value, true, katana::testing::MemoryProfile::NativeProductMemory);
            require(privileged.applicable, "privileged product-segment fetch was rejected");
        }
        {
            katana::runtime::CpuState cpu;
            constexpr std::uint32_t target = 0x8C123456u;
            bool rejected = false;
            try {
                reject_native_dispatch(cpu, target, true);
            } catch (const AotDispatchError& error) {
                rejected = error.target() == target && error.call() && cpu.pc == target;
            }
            require(rejected, "unbound native dispatch did not produce its typed AOT error");
        }

        std::cout << "SH-4 SST harness negative controls passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
