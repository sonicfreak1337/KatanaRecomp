#include "katana/io/json_report.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>

namespace katana::io {

std::string quote_json(const std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20u) {
                    output << "\\u" << std::hex << std::uppercase
                           << std::setw(4) << std::setfill('0')
                           << static_cast<unsigned>(character) << std::dec;
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    output << '"';
    return output.str();
}

void write_json_report_header(
    std::ostream& output,
    const std::string_view schema,
    const std::string_view report_type,
    const std::string_view status
) {
    output << "{\"schema\":" << quote_json(schema)
           << ",\"report_version\":" << json_report_version
           << ",\"report_type\":" << quote_json(report_type)
           << ",\"status\":" << quote_json(status);
}

}
