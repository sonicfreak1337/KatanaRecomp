#pragma once

#include "katana/io/executable_image.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace katana::analysis {

struct SymbolicAddress {
    std::uint32_t address = 0u;
    std::uint32_t symbol_address = 0u;
    std::uint32_t offset = 0u;
    std::string name;
    katana::io::SymbolKind kind = katana::io::SymbolKind::Unknown;
    katana::io::SymbolBinding binding = katana::io::SymbolBinding::Unknown;
    bool exact = false;
};

class SymbolNameIndex final {
public:
    explicit SymbolNameIndex(std::span<const katana::io::ImageSymbol> symbols);
    explicit SymbolNameIndex(const katana::io::ExecutableImage& image);

    [[nodiscard]] std::optional<SymbolicAddress> resolve(std::uint32_t address) const;
    [[nodiscard]] std::span<const katana::io::ImageSymbol> symbols() const noexcept;

private:
    std::vector<katana::io::ImageSymbol> symbols_;
};

[[nodiscard]] const SymbolicAddress* find_symbolic_address(
    std::span<const SymbolicAddress> symbols,
    std::uint32_t address
) noexcept;
[[nodiscard]] std::string format_symbolic_address(const SymbolicAddress& symbol);

}
