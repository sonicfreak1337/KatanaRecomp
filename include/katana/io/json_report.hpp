#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

namespace katana::io {

inline constexpr std::uint32_t json_report_version = 1u;

[[nodiscard]] std::string quote_json(std::string_view value);

void write_json_report_header(std::ostream& output,
                              std::string_view schema,
                              std::string_view report_type,
                              std::string_view status = "success");

} // namespace katana::io
