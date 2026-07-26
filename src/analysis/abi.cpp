#include "katana/analysis/abi.hpp"

namespace katana::analysis::detail {

template <>
void require_analyzer_abi<abi_version>() noexcept {}

} // namespace katana::analysis::detail
