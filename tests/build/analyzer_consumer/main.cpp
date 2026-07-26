#include "katana/analysis/function_value_analysis.hpp"

static_assert(katana::analysis::abi_version == KATANA_EXPECTED_ANALYZER_ABI);
static_assert(katana::build_contract::analyzer_abi_version ==
              KATANA_EXPECTED_ANALYZER_ABI);

int main() {
    const katana::analysis::FunctionValueAnalysisResult result;
    return result.guarded_code_inventory.candidate_count == 0u ? 0 : 1;
}
