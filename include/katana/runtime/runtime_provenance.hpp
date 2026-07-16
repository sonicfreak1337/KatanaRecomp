#pragma once

#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/firmware_handoff.hpp"

#include <cstdint>
#include <string>

namespace katana::runtime {

inline constexpr std::uint32_t runtime_provenance_schema_version = 1u;

[[nodiscard]] std::string serialize_runtime_provenance_json(const ExecutableCodeTracker& code,
                                                            const FirmwareHandoffMap& firmware);

} // namespace katana::runtime
