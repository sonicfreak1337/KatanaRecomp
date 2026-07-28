#pragma once

#include <cstdint>

namespace katana::runtime {

enum class ObservationRestorePolicy : std::uint8_t {
    PreserveCapturedDiagnostics,
    FreshProductEpoch,
};

} // namespace katana::runtime
