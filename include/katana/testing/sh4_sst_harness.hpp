#pragma once

#include "katana/testing/sh4_sst.hpp"
#include "katana/testing/sh4_sst_generated.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace katana::testing::sh4_sst {

struct HarnessComparison {
    ResultClassification classification = ResultClassification::Pass;
    std::string detail;

    [[nodiscard]] bool passed() const noexcept {
        return classification == ResultClassification::Pass;
    }
};

struct CaseComparisonInput {
    const SstTestCase* test = nullptr;
    const NativeExecutionTrace* native_trace = nullptr;
    const std::vector<SstMemoryObservation>* actual_memory = nullptr;
    SstState actual_state;
    SstStateComparison internal_state;
    bool tested_has_delay_slot = false;
    FpuComparisonMode fpu_comparison = FpuComparisonMode::Strict;
    std::optional<ResultClassification> execution_failure;
    std::string execution_failure_detail;
};

[[nodiscard]] std::vector<NativeInstructionObservation>
expected_native_instructions(const SstTestCase& test, bool tested_has_delay_slot);

[[nodiscard]] std::vector<SstMemoryObservation>
expected_memory_observations(const SstTestCase& test);

[[nodiscard]] HarnessComparison compare_native_instructions(const SstTestCase& test,
                                                            bool tested_has_delay_slot,
                                                            const NativeExecutionTrace& actual);

[[nodiscard]] HarnessComparison
compare_memory_observations(const std::vector<SstMemoryObservation>& expected,
                            const std::vector<SstMemoryObservation>& actual);

[[nodiscard]] HarnessComparison compare_case_result(const CaseComparisonInput& input);

struct Applicability {
    bool applicable = true;
    ResultClassification classification = ResultClassification::Pass;
    std::string detail;
};

[[nodiscard]] bool is_direct_product_ram_access(std::uint32_t address,
                                                std::uint32_t width) noexcept;

[[nodiscard]] Applicability classify_case_applicability(const SstTestCase& test,
                                                        bool katana_family_supported,
                                                        MemoryProfile profile);

class MemoryTraceRecorder final {
  public:
    [[nodiscard]] runtime::GuestMemoryAccessSink sink() noexcept;
    void clear() noexcept;
    [[nodiscard]] const std::vector<SstMemoryObservation>& observations() const noexcept;
    [[nodiscard]] bool complete() const noexcept;

  private:
    static void record(void* context, const runtime::GuestMemoryAccessEvent& event) noexcept;

    std::vector<SstMemoryObservation> observations_;
    bool allocation_failed_ = false;
};

} // namespace katana::testing::sh4_sst
