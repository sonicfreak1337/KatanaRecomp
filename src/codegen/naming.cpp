#include "katana/codegen/naming.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace katana::codegen {
namespace {

bool safe_suffix(const std::string_view suffix) noexcept {
    if (suffix.empty() || suffix.front() != '.') {
        return false;
    }
    for (const auto character : suffix.substr(1u)) {
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '-' &&
            character != '_') {
            return false;
        }
    }
    return true;
}

} // namespace

std::string
deterministic_translation_unit_name(const TranslationUnitPartition& partition,
                                    const std::span<const katana::ir::Function> functions,
                                    const std::string_view suffix) {
    if (partition.function_indices.empty()) {
        throw std::invalid_argument("Leere Codegen-Partition besitzt keinen Dateinamen.");
    }
    if (!safe_suffix(suffix)) {
        throw std::invalid_argument("Codegen-Dateiendung ist nicht portabel.");
    }
    for (const auto index : partition.function_indices) {
        if (index >= functions.size()) {
            throw std::out_of_range("Codegen-Partition verweist auf eine fehlende Funktion.");
        }
    }
    if (partition.content_sha256.size() < 16u) {
        throw std::invalid_argument("Codegen-Partition besitzt keinen kanonischen SHA-256-Hash.");
    }
    std::ostringstream output;
    output << "unit-" << std::dec << std::setfill('0') << std::setw(5) << partition.index << "-v"
           << std::uppercase << std::hex << std::setw(8) << partition.first_entry_address << "-"
           << std::setw(8) << partition.last_entry_address << "-" << std::nouppercase
           << partition.content_sha256.substr(0u, 16u) << suffix;
    return output.str();
}

} // namespace katana::codegen
