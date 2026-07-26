#include "katana/analysis/abi.hpp"

int main() {
    katana::analysis::detail::require_analyzer_abi<
        katana::analysis::abi_version + 1u>();
    return 0;
}
