#include "katana/analysis/symbol_names.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace katana::analysis {
namespace {

int binding_rank(const katana::io::SymbolBinding binding) noexcept {
    switch (binding) {
    case katana::io::SymbolBinding::Global:
        return 0;
    case katana::io::SymbolBinding::Weak:
        return 1;
    case katana::io::SymbolBinding::Local:
        return 2;
    case katana::io::SymbolBinding::Unknown:
        return 3;
    }
    return 4;
}

int kind_rank(const katana::io::SymbolKind kind) noexcept {
    switch (kind) {
    case katana::io::SymbolKind::Function:
        return 0;
    case katana::io::SymbolKind::Object:
        return 1;
    case katana::io::SymbolKind::Unknown:
        return 2;
    }
    return 3;
}

bool preferred_symbol(const katana::io::ImageSymbol& left, const katana::io::ImageSymbol& right) {
    if (left.address != right.address) return left.address < right.address;
    if (binding_rank(left.binding) != binding_rank(right.binding)) {
        return binding_rank(left.binding) < binding_rank(right.binding);
    }
    if (kind_rank(left.kind) != kind_rank(right.kind)) {
        return kind_rank(left.kind) < kind_rank(right.kind);
    }
    if (left.size != right.size) return left.size > right.size;
    return left.name < right.name;
}

} // namespace

SymbolNameIndex::SymbolNameIndex(const std::span<const katana::io::ImageSymbol> symbols)
    : symbols_(symbols.begin(), symbols.end()) {
    std::sort(symbols_.begin(), symbols_.end(), preferred_symbol);
}

SymbolNameIndex::SymbolNameIndex(const katana::io::ExecutableImage& image)
    : SymbolNameIndex(image.symbols()) {}

std::optional<SymbolicAddress> SymbolNameIndex::resolve(const std::uint32_t address) const {
    const auto upper = std::upper_bound(symbols_.begin(),
                                        symbols_.end(),
                                        address,
                                        [](const std::uint32_t candidate, const auto& symbol) {
                                            return candidate < symbol.address;
                                        });
    auto group_end = upper;
    while (group_end != symbols_.begin()) {
        auto group_start = group_end - 1;
        while (group_start != symbols_.begin() &&
               (group_start - 1)->address == group_start->address) {
            --group_start;
        }
        const auto offset = static_cast<std::uint64_t>(address) - group_start->address;
        for (auto candidate = group_start; candidate != group_end; ++candidate) {
            if (offset == 0u || (candidate->size != 0u && offset < candidate->size)) {
                return SymbolicAddress{address,
                                       candidate->address,
                                       static_cast<std::uint32_t>(offset),
                                       candidate->name,
                                       candidate->kind,
                                       candidate->binding,
                                       offset == 0u};
            }
        }
        group_end = group_start;
    }
    return std::nullopt;
}

std::span<const katana::io::ImageSymbol> SymbolNameIndex::symbols() const noexcept {
    return symbols_;
}

const SymbolicAddress* find_symbolic_address(const std::span<const SymbolicAddress> symbols,
                                             const std::uint32_t address) noexcept {
    const auto iterator = std::lower_bound(symbols.begin(),
                                           symbols.end(),
                                           address,
                                           [](const auto& symbol, const std::uint32_t candidate) {
                                               return symbol.address < candidate;
                                           });
    return iterator != symbols.end() && iterator->address == address ? &*iterator : nullptr;
}

std::string format_symbolic_address(const SymbolicAddress& symbol) {
    if (symbol.offset == 0u) return symbol.name;
    std::ostringstream output;
    output << symbol.name << "+0x" << std::hex << std::uppercase << symbol.offset;
    return output.str();
}

} // namespace katana::analysis
