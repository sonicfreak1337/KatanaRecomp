#include "katana/runtime/store_queue.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace katana::runtime;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        Memory memory(0u);
        memory.map_region("ram", 0x0C000000u, std::make_shared<LinearMemoryDevice>(0x20000u));
        ExecutableCodeTracker tracker;
        static_cast<void>(tracker.register_block({"ram-code", 0x0C001000u, 32u, "generated", {}}));
        std::vector<StoreQueueTransfer> transfers;
        auto queues = std::make_unique<Sh4StoreQueues>(
            memory, [&](const auto& transfer) { transfers.push_back(transfer); }, &tracker);
        queues->write_qacr(0u, 0x0Cu);
        queues->write_qacr(1u, 0x10u);
        for (std::uint32_t offset = 0u; offset < 32u; offset += 4u) {
            queues->write_p4(
                0xE0000000u + offset, 0x03020100u + offset * 0x01010101u, MemoryAccessWidth::Word);
            queues->write_p4(
                0xE2000000u + offset, 0x83828180u + offset * 0x01010101u, MemoryAccessWidth::Word);
        }
        require(queues->prefetch(0xE0001000u) && queues->prefetch(0xE2002000u) &&
                    transfers.size() == 2u,
                "SQ0/SQ1 werden nicht getrennt ueber ihr P4-Fenster transferiert.");
        require(transfers[0].queue == 0u && transfers[0].target_address == 0x0C001000u &&
                    transfers[0].bytes == queues->queue(0u) && transfers[1].queue == 1u &&
                    transfers[1].target_address == 0x12002000u &&
                    transfers[1].target == StoreQueueTarget::TileAccelerator &&
                    transfers[1].bytes == queues->queue(1u),
                "QACR-Zielbildung oder exakter 32-Byte-Inhalt ist falsch.");
        require(!queues->prefetch(0x8C001000u) && queues->transfer_count() == 2u,
                "Normales PREF ausserhalb des SQ-Fensters loest einen Transfer aus.");

        const auto icbi = queues->maintain(CacheMaintenanceOperation::Icbi, 0xAC001000u);
        require(icbi.invalidated_code && !tracker.valid("ram-code"),
                "ICBI an ausfuehrbarem RAM hinterlaesst einen stale Block.");
        const auto ocbwb = queues->maintain(CacheMaintenanceOperation::Ocbwb, 0x8C001000u);
        require(!ocbwb.invalidated_code && !ocbwb.wrote_memory,
                "OCBWB behauptet im Profil nicht modellierte Daten- oder Codeeffekte.");

        std::vector<std::uint32_t> ta_offsets;
        memory.map_region("ta",
                          0x12000000u,
                          std::make_shared<MmioMemoryDevice>(
                              0x10000u,
                              [](std::uint32_t, MemoryAccessWidth) { return 0u; },
                              [&](std::uint32_t offset, std::uint32_t, MemoryAccessWidth width) {
                                  require(width == MemoryAccessWidth::Byte,
                                          "TA-SQ-Transfer umgeht den MMIO-Breitenvertrag.");
                                  ta_offsets.push_back(offset);
                              }));
        ExecutableCodeTracker direct_tracker;
        static_cast<void>(
            direct_tracker.register_block({"sq-code", 0x0C000040u, 32u, "generated", {}}));
        auto direct = std::make_unique<Sh4StoreQueues>(
            memory, StoreQueueSink{}, &direct_tracker, OperandCacheRamProfile::Modeled);
        direct->write_qacr(0u, 0x0Cu);
        direct->write_p4(0xE0000040u, 0x44332211u, MemoryAccessWidth::Word);
        require(direct->prefetch(0xE0000040u) && memory.read_u32(0x0C000040u) == 0x44332211u &&
                    !direct_tracker.valid("sq-code"),
                "RAM-SQ-Ziel umgeht Speichernebenwirkung oder Codeinvalidierung.");
        direct->write_qacr(1u, 0x10u);
        direct->write_p4(0xE2000020u, 0x88776655u, MemoryAccessWidth::Word);
        require(direct->prefetch(0xE2000020u) && ta_offsets.size() == 32u &&
                    ta_offsets.front() == 0x20u && ta_offsets.back() == 0x3Fu,
                "TA-SQ-Ziel durchlaeuft seine MMIO-Nebenwirkungen nicht exakt.");

        ExecutableCodeTracker movca_tracker;
        static_cast<void>(
            movca_tracker.register_block({"movca-code", 0x0C000040u, 4u, "generated", {}}));
        auto cache_ops = std::make_unique<Sh4StoreQueues>(
            memory, StoreQueueSink{}, &movca_tracker, OperandCacheRamProfile::Modeled);
        const auto movca =
            cache_ops->maintain(CacheMaintenanceOperation::MovcaLong, 0x0C000040u, 0xAABBCCDDu);
        require(movca.wrote_memory && movca.invalidated_code &&
                    memory.read_u32(0x0C000040u) == 0xAABBCCDDu,
                "MOVCA.L umgeht Speichernebenwirkung oder Codeinvalidierung.");
        cache_ops->set_operand_cache_ram_enabled(true);
        cache_ops->write_operand_cache_ram(7u, 0x5Au);
        require(cache_ops->read_operand_cache_ram(7u) == 0x5Au,
                "Explizit modellierter Operand-Cache-RAM besitzt keinen getrennten Zustand.");
        bool rejected = false;
        try {
            queues->set_operand_cache_ram_enabled(true);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected,
                "Nicht modellierter CCR-Operand-Cache-RAM-LLE-Pfad wird nicht sichtbar abgelehnt.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
