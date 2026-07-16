#include "katana/runtime/code_invalidation.hpp"

#include <iostream>
#include <stdexcept>

using namespace katana::runtime;
namespace { void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); } }

int main() {
    try {
        ExecutableCodeTracker tracker;
        tracker.register_block({"a", 0x0C001FFCu, 8u, "static-a", {"caller-1", "caller-2"}});
        tracker.register_block({"b", 0x0C002004u, 4u, "static-b", {"caller-2"}});
        tracker.register_block({"c", 0x0C004000u, 4u, "static-c", {}});

        const auto cpu = tracker.observe_write(0xAC001FFEu, 8u, CodeWriteSource::Cpu);
        require(cpu.invalidated_blocks.size() == 2u && !tracker.valid("a") && !tracker.valid("b") &&
                tracker.valid("c") && cpu.changed_pages.size() == 2u &&
                cpu.unlinked_sources.size() == 2u,
            "Aliaswrite invalidiert nicht jeden ueberlappenden Block oder loest Links falsch.");
        require(tracker.page_generation(0x8C001000u) == 1u &&
                tracker.page_generation(0xAC002000u) == 1u,
            "P1-/P2-Aliase teilen keine Seitengeneration.");

        const auto identical = tracker.observe_write(0x8C004000u, 4u, CodeWriteSource::Dma, false);
        require(identical.byte_identical && tracker.valid("c") && identical.changed_pages.empty(),
            "Nachweislich bytegleicher DMA-Write invalidiert Code.");
        const auto copy = tracker.observe_write(0x8C004000u, 4u, CodeWriteSource::Copy);
        require(copy.source == CodeWriteSource::Copy && !tracker.valid("c") &&
                tracker.hotspots().at(0x0C004000u) == 1u && tracker.invalidation_count() == 3u,
            "Copy-Pfad oder Hotspotdiagnose umgeht die Invalidierung.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    return 0;
}
