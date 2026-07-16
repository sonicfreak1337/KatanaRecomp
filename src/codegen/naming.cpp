#include "katana/codegen/naming.hpp"

#include "katana/ir/serialize.hpp"

#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace katana::codegen {
namespace {

std::uint64_t fnv1a64(const std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto byte : text) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

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
    std::vector<katana::ir::Function> selected;
    selected.reserve(partition.function_indices.size());
    for (const auto index : partition.function_indices) {
        if (index >= functions.size()) {
            throw std::out_of_range("Codegen-Partition verweist auf eine fehlende Funktion.");
        }
        selected.push_back(functions[index]);
    }
    const auto canonical_ir = katana::ir::emit_ir_json(selected);
    std::ostringstream output;
    output << "unit-" << std::dec << std::setfill('0') << std::setw(5) << partition.index << "-v"
           << std::uppercase << std::hex << std::setw(8) << partition.first_entry_address << "-"
           << std::setw(8) << partition.last_entry_address << "-" << std::nouppercase
           << std::setw(16) << fnv1a64(canonical_ir) << suffix;
    return output.str();
}

} // namespace katana::codegen
