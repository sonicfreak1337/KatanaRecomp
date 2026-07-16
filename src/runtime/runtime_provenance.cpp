#include "katana/runtime/runtime_provenance.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

namespace katana::runtime {
namespace {

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setfill('0')
           << std::setw(8) << value;
    return output.str();
}

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
                    output << "\\u" << std::hex << std::uppercase << std::setfill('0')
                           << std::setw(4) << static_cast<unsigned>(character) << std::dec;
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    output << '"';
    return output.str();
}

std::string portable_label(const std::string_view value) {
    const bool portable = !value.empty()
        && std::all_of(value.begin(), value.end(), [](const unsigned char character) {
            return std::isalnum(character) != 0 || character == '.' || character == '_'
                || character == '-' || character == ':' || character == '>';
        });
    return portable ? std::string(value) : "redacted";
}

}

std::string serialize_runtime_provenance_json(
    const ExecutableCodeTracker& code,
    const FirmwareHandoffMap& firmware
) {
    auto blocks = code.blocks();
    std::sort(blocks.begin(), blocks.end(), [](const auto& left, const auto& right) {
        return std::tie(left.block.physical_start, left.block.identity)
            < std::tie(right.block.physical_start, right.block.identity);
    });
    auto copies = firmware.copies();
    std::sort(copies.begin(), copies.end(), [](const auto& left, const auto& right) {
        return std::tie(left.destination_physical, left.source_physical, left.size, left.provenance)
            < std::tie(right.destination_physical, right.source_physical, right.size, right.provenance);
    });
    auto symbols = firmware.runtime_symbols();
    std::sort(symbols.begin(), symbols.end(), [](const auto& left, const auto& right) {
        return std::tie(left.guest_cycle, left.virtual_address, left.name)
            < std::tie(right.guest_cycle, right.virtual_address, right.name);
    });
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<FirmwareMapping>> aliases;
    for (const auto& mapping : firmware.mappings()) {
        aliases[{mapping.physical_start, mapping.size}].push_back(mapping);
    }
    for (auto& [key, mappings] : aliases) {
        static_cast<void>(key);
        std::sort(mappings.begin(), mappings.end(), [](const auto& left, const auto& right) {
            return std::tie(left.virtual_start, left.name) < std::tie(right.virtual_start, right.name);
        });
    }

    std::ostringstream output;
    output << "{\"schema\":\"katana-runtime-provenance\",\"report_version\":1"
           << ",\"provenance_version\":" << runtime_provenance_schema_version
           << ",\"status\":\"success\",\"dropped_invalidation_events\":"
           << code.dropped_provenance_events() << ",\"blocks\":[";
    for (std::size_t index = 0u; index < blocks.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& tracked = blocks[index];
        output << "{\"identity\":" << quote_json(portable_label(tracked.block.identity))
               << ",\"physical_start\":\"" << hex32(tracked.block.physical_start)
               << "\",\"size\":" << tracked.block.size
               << ",\"origin\":" << quote_json(executable_block_origin_name(tracked.block.origin))
               << ",\"provenance\":" << quote_json(portable_label(tracked.block.provenance))
               << ",\"valid\":" << (tracked.valid ? "true" : "false")
               << ",\"incoming_links\":[";
        std::size_t link_index = 0u;
        for (const auto& link : tracked.block.incoming_links) {
            if (link_index++ != 0u) output << ',';
            output << quote_json(portable_label(link));
        }
        output << "]}";
    }
    output << "],\"alias_groups\":[";
    std::size_t alias_index = 0u;
    for (const auto& [key, mappings] : aliases) {
        if (alias_index++ != 0u) output << ',';
        output << "{\"physical_start\":\"" << hex32(key.first)
               << "\",\"size\":" << key.second << ",\"aliases\":[";
        for (std::size_t index = 0u; index < mappings.size(); ++index) {
            if (index != 0u) output << ',';
            output << "{\"virtual_start\":\"" << hex32(mappings[index].virtual_start)
                   << "\",\"name\":" << quote_json(portable_label(mappings[index].name))
                   << ",\"kind\":" << quote_json(firmware_segment_kind_name(mappings[index].kind))
                   << '}';
        }
        output << "]}";
    }
    output << "],\"code_copies\":[";
    for (std::size_t index = 0u; index < copies.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& copy = copies[index];
        output << "{\"source_physical\":\"" << hex32(copy.source_physical)
               << "\",\"destination_physical\":\"" << hex32(copy.destination_physical)
               << "\",\"size\":" << copy.size
               << ",\"provenance\":" << quote_json(portable_label(copy.provenance))
               << ",\"byte_verified\":" << (copy.byte_verified ? "true" : "false")
               << ",\"changed_after_copy\":" << (copy.changed_after_copy ? "true" : "false")
               << '}';
    }
    output << "],\"runtime_symbols\":[";
    for (std::size_t index = 0u; index < symbols.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& symbol = symbols[index];
        output << "{\"guest_cycle\":" << symbol.guest_cycle
               << ",\"name\":" << quote_json(portable_label(symbol.name))
               << ",\"virtual_address\":\"" << hex32(symbol.virtual_address)
               << "\",\"physical_address\":\"" << hex32(symbol.physical_address)
               << "\",\"provenance\":" << quote_json(portable_label(symbol.provenance)) << '}';
    }
    output << "],\"invalidations\":[";
    const auto& invalidations = code.invalidation_events();
    for (std::size_t index = 0u; index < invalidations.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& event = invalidations[index];
        output << "{\"sequence\":" << event.sequence
               << ",\"virtual_address\":\"" << hex32(event.virtual_address)
               << "\",\"physical_address\":\"" << hex32(event.physical_address)
               << "\",\"size\":" << event.size
               << ",\"source\":" << quote_json(code_write_source_name(event.source))
               << ",\"byte_identical\":" << (event.byte_identical ? "true" : "false")
               << ",\"pages\":[";
        for (std::size_t page = 0u; page < event.pages.size(); ++page) {
            if (page != 0u) output << ',';
            output << "{\"physical_page\":\"" << hex32(event.pages[page].physical_page)
                   << "\",\"generation\":" << event.pages[page].generation << '}';
        }
        output << "],\"invalidated_blocks\":[";
        for (std::size_t block = 0u; block < event.invalidated_blocks.size(); ++block) {
            if (block != 0u) output << ',';
            output << quote_json(portable_label(event.invalidated_blocks[block]));
        }
        output << "],\"unlinked_sources\":[";
        for (std::size_t link = 0u; link < event.unlinked_sources.size(); ++link) {
            if (link != 0u) output << ',';
            output << quote_json(portable_label(event.unlinked_sources[link]));
        }
        output << "]}";
    }
    output << "]}";
    return output.str();
}

}
