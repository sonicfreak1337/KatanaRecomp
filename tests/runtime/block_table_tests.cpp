#include "katana/runtime/block_table.hpp"
#include "katana/runtime/code_invalidation.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

const RuntimeBlock& resolved(const RuntimeBlockTable& table,
                             const std::optional<RuntimeBlockHandle> handle) {
    require(handle.has_value(), "Blockhandle fehlt.");
    const auto block = table.resolve(*handle);
    require(block.has_value(), "Blockhandle ist unerwartet stale.");
    return block->get();
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

template <typename Function> bool throws_any(Function function) {
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    try {
        RuntimeBlockTable table;
        const BlockVariantKey base{1u, 2u, 3u, 4u, 5u};
        static_cast<void>(table.register_static({0x8C001000u,
                               0x0C001000u,
                               8u,
                               BlockEndKind::StaticBranch,
                               base,
                               block_a,
                               "rom-reset",
                               false}));
        static_cast<void>(table.register_runtime(
            {0xAC001000u, 0x0C001000u, 8u, BlockEndKind::Return, base, block_b, "ram-copy", false}));
        require(table.size() == 2u,
                "Statische und dynamische Eintraege teilen nicht dieselbe Tabelle.");
        require(resolved(table, table.lookup(0x8C001000u, base)).function == block_a,
                "Deterministischer Lookup schlug fehl.");
        require(resolved(table, table.lookup(0xAC001000u, base)).runtime_registered,
                "Laufzeitprovenienz ging verloren.");
        const auto aliases = table.aliases(0xAC001000u);
        require(aliases.size() == 2u && table.resolve(aliases[0])->get().virtual_start !=
                                            table.resolve(aliases[1])->get().virtual_start,
                "P1-/P2-Aliase bewahren ihre virtuellen Diagnosen nicht.");

        RuntimeBlock same_identity = resolved(table, table.lookup(0x8C001000u, base));
        same_identity.function = block_b;
        require(stable_runtime_block_identity(resolved(table, table.lookup(0x8C001000u, base))) ==
                    stable_runtime_block_identity(same_identity),
                "Hostfunktionszeiger beeinflusst die stabile Blockidentitaet.");

        require(throws_with(
                    [&] {
                        static_cast<void>(table.register_runtime({0x8C001004u,
                                                0x0C002000u,
                                                8u,
                                                BlockEndKind::Call,
                                                base,
                                                block_b,
                                                "overlap",
                                                false}));
                    },
                    "rom-reset",
                    "overlap"),
                "Ueberlappung nennt nicht beide Provenienzen.");

        auto other_variant = base;
        ++other_variant.mmu_generation;
        static_cast<void>(table.register_runtime({0x8C001000u,
                                0x0D001000u,
                                8u,
                                BlockEndKind::Return,
                                other_variant,
                                block_b,
                                "tlb-remap",
                                false}));
        require(resolved(table, table.lookup(0x8C001000u, other_variant)).physical_origin ==
                    0x0D001000u,
                "Blockvarianten koennen keine geaenderte physische Herkunft tragen.");
        const auto dynamic_handle = table.lookup(0xAC001000u, base);
        const auto dynamic_block = resolved(table, dynamic_handle);
        const auto identity = stable_runtime_block_identity(dynamic_block);
        require(table.erase_identity(identity) && !table.lookup(0xAC001000u, base).has_value() &&
                    table.size() == 2u && !table.erase_identity(identity),
                "Gezielte Blockinvalidierung entfernt nicht genau die stabile Identitaet.");
        require(!table.resolve(*dynamic_handle), "Erase liess ein altes Handle aktiv.");
        const auto reactivated = table.register_runtime(dynamic_block);
        require(reactivated.id == dynamic_handle->id &&
                    reactivated.generation != dynamic_handle->generation &&
                    table.resolve(reactivated).has_value() && !table.resolve(*dynamic_handle),
                "Dynamische Reaktivierung recycelt Generation oder Record-ID falsch.");

        const auto retained = table.lookup(0x8C001000u, base);
        for (std::uint32_t index = 0u; index < 10'000u; ++index) {
            const auto address = 0x20000000u + index * 4u;
            static_cast<void>(table.register_runtime({address,
                                                      address,
                                                      4u,
                                                      BlockEndKind::Fallthrough,
                                                      {},
                                                      block_a,
                                                      "growth-" + std::to_string(index),
                                                      false}));
        }
        require(retained.has_value() && table.resolve(*retained).has_value() &&
                    table.resolve(*retained)->get().provenance == "rom-reset",
                "Containerwachstum entwertet ein bestaetigtes Blockhandle.");

        RuntimeBlockTable guarded;
        ExecutableCodeTracker tracker;
        RuntimeBlock tracked{0x8C100000u,
                             0x0C100000u,
                             16u,
                             BlockEndKind::Return,
                             {},
                             block_a,
                             "tracker-race",
                             false};
        const auto tracked_identity = stable_runtime_block_identity(tracked);
        static_cast<void>(tracker.register_block(
            {tracked_identity, tracked.physical_origin, tracked.size, tracked.provenance, {}}));
        guarded.bind_code_tracker(&tracker);
        const auto tracked_handle = guarded.register_runtime(tracked);
        require(guarded.lookup(tracked.virtual_start, {}).has_value(),
                "Trackergebundener Block ist vor Invalidierung nicht sichtbar.");
        static_cast<void>(tracker.observe_write(
            tracked.virtual_start + 4u, 1u, CodeWriteSource::Cpu));
        require(!guarded.resolve(tracked_handle) &&
                    !guarded.lookup(tracked.virtual_start, {}).has_value(),
                "Trackerinvalidierung zwischen Lookup und Resolve fuehrt stale Code aus.");

        RuntimeBlockTable rejected;
        require(throws_any([&] {
                    static_cast<void>(rejected.register_runtime(
                        {0x1000u, 0x1000u, 0u, {}, {}, block_a, "zero", false}));
                }) &&
                    throws_any([&] {
                        static_cast<void>(rejected.register_runtime(
                            {0x1000u, 0x1000u, 4u, {}, {}, nullptr, "null", false}));
                    }) &&
                    throws_any([&] {
                        static_cast<void>(rejected.register_runtime(
                            {0x1000u, 0x1000u, 4u, {}, {}, block_a, "", false}));
                    }) &&
                    throws_any([&] {
                        static_cast<void>(rejected.register_runtime(
                            {0xFFFFFFFEu, 0x1000u, 4u, {}, {}, block_a, "overflow", false}));
                    }),
                "Ungueltige Groesse, Funktion, Provenienz oder Adressraumgrenze wurde akzeptiert.");
        static_cast<void>(rejected.register_static(
            {0x2000u, 0x2000u, 4u, {}, {}, block_a, "sealed", false}));
        rejected.seal_static();
        require(throws_any([&] {
                    static_cast<void>(rejected.register_static(
                        {0x3000u, 0x3000u, 4u, {}, {}, block_a, "late-static", false}));
                }),
                "Statische Registrierung nach der Versiegelung wurde akzeptiert.");

        RuntimeBlockTable bulk;
        std::vector<RuntimeBlock> blocks;
        blocks.reserve(100'000u);
        for (std::uint32_t index = 0u; index < 100'000u; ++index) {
            const auto address = 0x1000u + index * 4u;
            blocks.push_back({address,
                              address,
                              4u,
                              BlockEndKind::Fallthrough,
                              {},
                              block_a,
                              "bulk-" + std::to_string(index),
                              false});
        }
        const auto registration_start = std::chrono::steady_clock::now();
        const auto handles = bulk.register_static_bulk(std::move(blocks));
        const auto registration_elapsed =
            std::chrono::steady_clock::now() - registration_start;
        require(handles.size() == 100'000u && bulk.size() == 100'000u &&
                    bulk.lookup(0x1000u, {}).has_value() &&
                    bulk.lookup(0x1000u + 50'000u * 4u, {}).has_value() &&
                    bulk.lookup(0x1000u + 99'999u * 4u, {}).has_value() &&
                    !bulk.lookup(0x0FFCu, {}).has_value(),
                "100.000-Block-Bulkregistry verliert Treffer oder Misses.");
        const auto lookup_start = std::chrono::steady_clock::now();
        for (std::uint32_t index = 0u; index < 100'000u; ++index) {
            const auto address = 0x1000u + index * 4u;
            const auto by_virtual = bulk.lookup(address, {});
            const auto by_physical = bulk.lookup_physical(address, {});
            require(by_virtual.has_value() && by_physical.has_value() &&
                        by_virtual->id == handles[index].id &&
                        by_physical->id == handles[index].id,
                    "Bulkregistry weicht von der unabhaengigen Adressabbildung ab.");
        }
        const auto lookup_elapsed = std::chrono::steady_clock::now() - lookup_start;
        require(registration_elapsed < std::chrono::seconds(30) &&
                    lookup_elapsed < std::chrono::seconds(15),
                "Bulkregistrierung oder 200.000 geordnete Lookups sprengen das Lastbudget.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
