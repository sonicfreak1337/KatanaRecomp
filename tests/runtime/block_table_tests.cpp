#include "katana/runtime/block_table.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace katana::runtime;

BlockExit block_a(CpuState&, BlockExecutionContext&) {
    return {};
}
BlockExit block_b(CpuState&, BlockExecutionContext&) {
    return {};
}

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
bool throws_with(Function function, const std::string& first, const std::string& second) {
    try {
        function();
    } catch (const std::exception& error) {
        const std::string text = error.what();
        return text.find(first) != std::string::npos && text.find(second) != std::string::npos;
    }
    return false;
}

} // namespace

int main() {
    try {
        RuntimeBlockTable table;
        const BlockVariantKey base{1u, 2u, 3u, 4u, 5u};
        table.register_static({0x8C001000u,
                               0x0C001000u,
                               8u,
                               BlockEndKind::StaticBranch,
                               base,
                               block_a,
                               "rom-reset",
                               false});
        table.register_runtime(
            {0xAC001000u, 0x0C001000u, 8u, BlockEndKind::Return, base, block_b, "ram-copy", false});
        require(table.size() == 2u,
                "Statische und dynamische Eintraege teilen nicht dieselbe Tabelle.");
        require(table.lookup(0x8C001000u, base)->function == block_a,
                "Deterministischer Lookup schlug fehl.");
        require(table.lookup(0xAC001000u, base)->runtime_registered,
                "Laufzeitprovenienz ging verloren.");
        const auto aliases = table.aliases(0xAC001000u);
        require(aliases.size() == 2u && aliases[0]->virtual_start != aliases[1]->virtual_start,
                "P1-/P2-Aliase bewahren ihre virtuellen Diagnosen nicht.");

        RuntimeBlock same_identity = *table.lookup(0x8C001000u, base);
        same_identity.function = block_b;
        require(stable_runtime_block_identity(*table.lookup(0x8C001000u, base)) ==
                    stable_runtime_block_identity(same_identity),
                "Hostfunktionszeiger beeinflusst die stabile Blockidentitaet.");

        require(throws_with(
                    [&] {
                        table.register_runtime({0x8C001004u,
                                                0x0C002000u,
                                                8u,
                                                BlockEndKind::Call,
                                                base,
                                                block_b,
                                                "overlap",
                                                false});
                    },
                    "rom-reset",
                    "overlap"),
                "Ueberlappung nennt nicht beide Provenienzen.");

        auto other_variant = base;
        ++other_variant.mmu_generation;
        table.register_runtime({0x8C001000u,
                                0x0D001000u,
                                8u,
                                BlockEndKind::Return,
                                other_variant,
                                block_b,
                                "tlb-remap",
                                false});
        require(table.lookup(0x8C001000u, other_variant)->physical_origin == 0x0D001000u,
                "Blockvarianten koennen keine geaenderte physische Herkunft tragen.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
