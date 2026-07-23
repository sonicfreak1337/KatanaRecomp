#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/exception.hpp"
#include "katana/runtime/store_queue.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace katana::runtime;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template <typename Action> void require_rejected(Action&& action, const char* message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
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
        queues->write_p4(0xE0000000u, 0x11111111u, MemoryAccessWidth::Word);
        queues->write_p4(0xE2000000u, 0x22222222u, MemoryAccessWidth::Word);
        require(queues->queue(0u)[0] == 0x22u && queues->queue(1u)[0] == 0u,
                "Bit 25 waehlt faelschlich eine andere Store Queue.");
        for (std::uint32_t offset = 0u; offset < 32u; offset += 4u) {
            queues->write_p4(
                0xE0000000u + offset, 0x03020100u + offset * 0x01010101u, MemoryAccessWidth::Word);
            queues->write_p4(
                0xE0000020u + offset, 0x83828180u + offset * 0x01010101u, MemoryAccessWidth::Word);
        }
        require(queues->read_p4(0xFF001000u, MemoryAccessWidth::Word) == 0x03020100u &&
                    queues->read_p4(0xFF001020u, MemoryAccessWidth::Word) == 0x83828180u,
                "Das privilegierte SQ-Lesefenster waehlt Queue oder Longword falsch.");
        require(queues->prefetch(0xE0001000u) && queues->prefetch(0xE2002020u) &&
                    transfers.size() == 2u,
                "SQ0/SQ1 werden nicht getrennt ueber ihr P4-Fenster transferiert.");
        require(transfers[0].queue == 0u && transfers[0].target_address == 0x0C001000u &&
                    transfers[0].bytes == queues->queue(0u) && transfers[1].queue == 1u &&
                    transfers[1].target_address == 0x12002020u &&
                    transfers[1].target == StoreQueueTarget::TileAccelerator &&
                    transfers[1].bytes == queues->queue(1u),
                "QACR-Zielbildung oder exakter 32-Byte-Inhalt ist falsch.");
        require_rejected(
            [&] { static_cast<void>(queues->read_p4(0xE0000000u, MemoryAccessWidth::Word)); },
            "Das SQ-Schreibfenster wurde faelschlich als Lesefenster akzeptiert.");
        require_rejected(
            [&] { static_cast<void>(queues->read_p4(0xFF001001u, MemoryAccessWidth::Word)); },
            "Ein fehlausgerichteter SQ-Longword-Read wurde akzeptiert.");
        require_rejected(
            [&] { queues->write_p4(0xE000001Fu, 0xAABBu, MemoryAccessWidth::Halfword); },
            "Ein Store ueber die Queuegrenze wurde nicht atomar abgelehnt.");
        require_rejected([&] { queues->write_qacr(0u, 0x20u); },
                         "Reservierte QACR-Bits wurden akzeptiert.");
        require(!queues->prefetch(0x8C001000u) && queues->transfer_count() == 2u,
                "Normales PREF ausserhalb des SQ-Fensters loest einen Transfer aus.");
        try {
            static_cast<void>(queues->prefetch(0xE0001002u));
            require(false, "Direktes nicht ausgerichtetes SQ-PREF wurde akzeptiert.");
        } catch (const MemoryAccessError& error) {
            require(error.reason() == MemoryAccessErrorReason::Unmapped &&
                        error.operation() == MemoryAccessOperation::Write &&
                        error.address() == 0xE0001002u,
                    "Direktes nicht ausgerichtetes SQ-PREF verliert den Adressfehler.");
        }
        require(transfers.size() == 2u && queues->transfer_count() == 2u,
                "Direktes nicht ausgerichtetes SQ-PREF mutiert Sink oder Transferzaehler.");

        const auto icbi = queues->maintain(CacheMaintenanceOperation::Icbi, 0xAC001000u);
        require(icbi.invalidated_code && !tracker.valid("ram-code"),
                "ICBI an ausfuehrbarem RAM hinterlaesst einen stale Block.");
        for (const auto operation : {CacheMaintenanceOperation::Ocbi,
                                     CacheMaintenanceOperation::Ocbp,
                                     CacheMaintenanceOperation::Ocbwb}) {
            require_rejected(
                [&] { static_cast<void>(queues->maintain(operation, 0x8C001000u)); },
                "Eine nicht modellierte Operand-Cache-Operation wurde still akzeptiert.");
        }

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
        memory.set_guest_write_observer([&](const auto& event) {
            static_cast<void>(direct_tracker.observe_write(
                event.address, event.size, event.source, event.bytes_changed));
        });
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

        constexpr std::uint32_t sq_source = 0xE2000020u;
        constexpr std::uint32_t ta_target = 0x12000020u;
        std::vector<StoreQueueTransfer> translated_transfers;
        Sh4StoreQueues translated_queues(
            memory, [&](const auto& transfer) { translated_transfers.push_back(transfer); });
        CpuState no_mmu_cpu;
        no_mmu_cpu.address_space = std::make_shared<RuntimeAddressSpace>();
        no_mmu_cpu.write_sr(sr_md_mask);
        translated_queues.set_prefetch_address_translator(
            [&no_mmu_cpu](const std::uint32_t address) {
                return translate_store_queue_prefetch(no_mmu_cpu, address);
            });
        translated_queues.write_qacr(1u, 0x10u);
        for (std::uint32_t offset = 0u; offset < 32u; offset += 4u) {
            translated_queues.write_p4(
                sq_source + offset, 0xA3A2A1A0u + offset * 0x01010101u, MemoryAccessWidth::Word);
        }
        const auto expected_queue_bytes = translated_queues.queue(1u);
        const auto qacr_translation = translate_store_queue_prefetch(no_mmu_cpu, sq_source);
        require(qacr_translation.addressing == StoreQueueAddressingMode::Qacr &&
                    translated_queues.prefetch(sq_source) && translated_transfers.size() == 1u &&
                    translated_transfers.back().target_address == ta_target &&
                    translated_transfers.back().bytes == expected_queue_bytes,
                "AT=0 uebertraegt die Store Queue nicht unveraendert ueber QACR zum TA-FIFO.");

        try {
            static_cast<void>(translate_store_queue_prefetch(no_mmu_cpu, sq_source + 2u));
            require(false, "AT=0-Adressraum akzeptiert ein nicht ausgerichtetes SQ-PREF.");
        } catch (const MemoryAccessError& error) {
            require(error.reason() == MemoryAccessErrorReason::Unmapped &&
                        error.operation() == MemoryAccessOperation::Write &&
                        error.address() == sq_source + 2u,
                    "AT=0-Adressraum verliert den typisierten SQ-Adressfehler.");
        }
        try {
            static_cast<void>(translated_queues.prefetch(sq_source + 2u));
            require(false, "Nicht ausgerichtetes SQ-PREF wurde bei AT=0 akzeptiert.");
        } catch (const MemoryAccessError& error) {
            require(error.reason() == MemoryAccessErrorReason::Unmapped &&
                        error.operation() == MemoryAccessOperation::Write &&
                        error.address() == sq_source + 2u,
                    "Nicht ausgerichtetes AT=0-SQ-PREF verliert den typisierten Adressfehler.");
        }
        require(translated_transfers.size() == 1u &&
                    translated_queues.transfer_count() == 1u,
                "Nicht ausgerichtetes AT=0-SQ-PREF mutiert Sink oder Transferzaehler.");

        no_mmu_cpu.write_sr(0u);
        no_mmu_cpu.address_space->write_mmucr(0x00000200u);
        try {
            static_cast<void>(translated_queues.prefetch(sq_source));
            require(false, "SQMD-geschuetztes User-SQ-PREF wurde bei AT=0 akzeptiert.");
        } catch (const MemoryAccessError& error) {
            require(error.reason() == MemoryAccessErrorReason::Unmapped &&
                        error.operation() == MemoryAccessOperation::Write &&
                        error.address() == sq_source,
                    "SQMD-geschuetztes AT=0-User-PREF verliert den typisierten Adressfehler.");
        }
        require(translated_transfers.size() == 1u &&
                    translated_queues.transfer_count() == 1u,
                "SQMD-geschuetztes AT=0-User-PREF mutiert Sink oder Transferzaehler.");

        auto fallback_cpu = std::make_unique<CpuState>();
        fallback_cpu->write_sr(0u);
        fallback_cpu->mmucr = 0x00000200u;
        std::vector<StoreQueueTransfer> fallback_transfers;
        auto fallback_queues = std::make_unique<Sh4StoreQueues>(
            memory, [&](const auto& transfer) { fallback_transfers.push_back(transfer); });
        fallback_queues->write_qacr(1u, 0x10u);
        fallback_queues->set_prefetch_address_translator(
            [cpu = fallback_cpu.get()](const std::uint32_t address) {
                return translate_store_queue_prefetch(*cpu, address);
            });
        try {
            static_cast<void>(fallback_queues->prefetch(sq_source));
            require(false,
                    "SQMD-geschuetztes User-PREF ohne RuntimeAddressSpace wurde akzeptiert.");
        } catch (const MemoryAccessError& error) {
            require(error.reason() == MemoryAccessErrorReason::Unmapped &&
                        error.operation() == MemoryAccessOperation::Write &&
                        error.address() == sq_source,
                    "AddressSpace-Fallback verliert den SQMD-Adressfehler.");
        }
        require(fallback_transfers.empty() && fallback_queues->transfer_count() == 0u,
                "AddressSpace-Fallback mutiert bei SQMD-Fehler Sink oder Transferzaehler.");
        fallback_cpu->write_sr(sr_md_mask);
        fallback_cpu->mmucr = 1u;
        try {
            static_cast<void>(translate_store_queue_prefetch(*fallback_cpu, sq_source));
            require(false, "AT=1 ohne RuntimeAddressSpace wurde still als QACR-Pfad akzeptiert.");
        } catch (const MemoryAccessError& error) {
            require(error.reason() == MemoryAccessErrorReason::TlbMiss &&
                        error.operation() == MemoryAccessOperation::Write &&
                        error.address() == sq_source,
                    "AT=1-AddressSpace-Fallback liefert keinen kontrollierten TLB-Miss.");
        }

        CpuState mmu_cpu;
        mmu_cpu.address_space = std::make_shared<RuntimeAddressSpace>();
        mmu_cpu.write_sr(sr_md_mask);
        mmu_cpu.address_space->set_mode(AddressTranslationMode::Mmu);
        mmu_cpu.address_space->write_mmucr(1u);
        mmu_cpu.address_space->ldtlb({0xE2000000u,
                                     0x12000000u,
                                     1048576u,
                                     0u,
                                     0u,
                                     true,
                                     true,
                                     true,
                                     true,
                                     true,
                                     true,
                                     false});
        translated_queues.set_prefetch_address_translator(
            [&mmu_cpu](const std::uint32_t address) {
                return translate_store_queue_prefetch(mmu_cpu, address);
            });
        translated_queues.write_qacr(1u, 0x0Cu);
        const auto utlb_translation = translate_store_queue_prefetch(mmu_cpu, sq_source);
        require(utlb_translation.addressing == StoreQueueAddressingMode::Utlb &&
                    utlb_translation.target_address == ta_target &&
                    translated_queues.prefetch(sq_source) && translated_transfers.size() == 2u &&
                    translated_transfers.back().target == StoreQueueTarget::TileAccelerator &&
                    translated_transfers.back().target_address == ta_target &&
                    translated_transfers.front().bytes == translated_transfers.back().bytes &&
                    translated_transfers.back().bytes == expected_queue_bytes,
                "AT=1 uebersetzt SQ-PREF nicht ueber UTLB oder veraendert den 32-Byte-Inhalt.");

        CpuState miss_cpu;
        miss_cpu.address_space = std::make_shared<RuntimeAddressSpace>();
        miss_cpu.write_sr(sr_md_mask);
        miss_cpu.vbr = 0x8C000000u;
        miss_cpu.pteh = 0x5Au;
        miss_cpu.address_space->write_pteh(miss_cpu.pteh);
        miss_cpu.address_space->write_mmucr(1u);
        miss_cpu.address_space->set_mode(AddressTranslationMode::Mmu);
        translated_queues.set_prefetch_address_translator(
            [&miss_cpu](const std::uint32_t address) {
                return translate_store_queue_prefetch(miss_cpu, address);
            });
        try {
            static_cast<void>(translated_queues.prefetch(sq_source));
            require(false, "Fehlende SQ-UTLB-Abbildung wurde akzeptiert.");
        } catch (const MemoryAccessError& error) {
            require(error.reason() == MemoryAccessErrorReason::TlbMiss &&
                        error.operation() == MemoryAccessOperation::Write &&
                        error.address() == sq_source,
                    "SQ-UTLB-Miss verliert den typisierten Schreibfehler.");
            enter_memory_exception(miss_cpu, error, 0x8C010000u);
        }
        require(miss_cpu.last_exception_cause == ExceptionCause::TlbMissWrite &&
                    miss_cpu.expevt == event_tlb_miss_write &&
                    miss_cpu.pc == 0x8C000400u && miss_cpu.tea == sq_source &&
                    miss_cpu.pteh == ((sq_source & 0xFFFFFC00u) | 0x5Au) &&
                    translated_transfers.size() == 2u &&
                    translated_queues.transfer_count() == 2u,
                "SQ-UTLB-Miss schreibt ins FIFO oder verliert PTEH/TEA/Missvektor.");

        CpuState multiple_cpu;
        multiple_cpu.address_space = std::make_shared<RuntimeAddressSpace>();
        multiple_cpu.write_sr(sr_md_mask);
        multiple_cpu.pteh = 0xA5u;
        multiple_cpu.address_space->write_pteh(multiple_cpu.pteh);
        multiple_cpu.address_space->write_mmucr(1u);
        multiple_cpu.address_space->set_mode(AddressTranslationMode::Mmu);
        multiple_cpu.address_space->ldtlb({0xE2000000u,
                                          0x12000000u,
                                          1048576u,
                                          0xA5u,
                                          1u,
                                          true,
                                          true,
                                          true,
                                          true,
                                          true,
                                          true,
                                          false});
        multiple_cpu.address_space->ldtlb({0xE2000000u,
                                          0x10000000u,
                                          1048576u,
                                          0xA5u,
                                          2u,
                                          true,
                                          true,
                                          true,
                                          true,
                                          true,
                                          true,
                                          false});
        translated_queues.set_prefetch_address_translator(
            [&multiple_cpu](const std::uint32_t address) {
                return translate_store_queue_prefetch(multiple_cpu, address);
            });
        try {
            static_cast<void>(translated_queues.prefetch(sq_source));
            require(false, "Mehrere passende SQ-UTLB-Abbildungen wurden akzeptiert.");
        } catch (const MemoryAccessError& error) {
            require(error.reason() == MemoryAccessErrorReason::TlbMultipleHit &&
                        error.operation() == MemoryAccessOperation::Write &&
                        error.address() == sq_source,
                    "SQ-UTLB-Multiple-Hit verliert den typisierten Schreibfehler.");
            enter_memory_exception(multiple_cpu, error, 0x8C010000u);
        }
        require(multiple_cpu.last_exception_cause == ExceptionCause::TlbMultipleHit &&
                    multiple_cpu.expevt == event_tlb_multiple_hit &&
                    multiple_cpu.pc == tlb_multiple_hit_reset_vector &&
                    multiple_cpu.vbr == 0u && multiple_cpu.tea == sq_source &&
                    multiple_cpu.pteh == ((sq_source & 0xFFFFFC00u) | 0xA5u) &&
                    translated_transfers.size() == 2u &&
                    translated_queues.transfer_count() == 2u,
                "SQ-UTLB-Multiple-Hit schreibt ins FIFO oder verliert Reset/PTEH/TEA.");

        DreamcastRuntimeBootImage boot;
        const std::vector<std::uint8_t> disc_sector(dreamcast_data_sector_size, 0u);
        boot.source = std::make_shared<MemoryDiscSource>(
            std::span<const std::uint8_t>(disc_sector), "synthetic-store-queue-disc");
        boot.system_bootstrap.resize(dreamcast_system_bootstrap_size, 0u);
        boot.boot_file = {0x09u, 0x00u};
        boot.repeated_bootstrap_reads_match = true;
        boot.repeated_reads_match = true;
        auto product_cpu = std::make_unique<CpuState>();
        auto product_runtime = std::make_unique<DreamcastRuntimeState>(
            initialize_dreamcast_runtime(*product_cpu, boot));
        product_runtime->store_queues->write_qacr(1u, 0x0Cu);
        for (std::uint32_t offset = 0u; offset < 32u; offset += 4u) {
            product_runtime->store_queues->write_p4(sq_source + offset,
                                                    offset == 0u ? 0x40000000u : 0u,
                                                    MemoryAccessWidth::Word);
        }
        product_runtime->address_space->ldtlb({0xE2000000u,
                                               0x12000000u,
                                               1048576u,
                                               0u,
                                               0u,
                                               true,
                                               true,
                                               true,
                                               true,
                                               true,
                                               true,
                                               false});
        product_runtime->mmu_control->write(0x10u, 1u);
        require(product_runtime->store_queues->prefetch(sq_source) &&
                    product_runtime->store_queue_transfers->size() == 1u &&
                    product_runtime->store_queue_transfers->front().target ==
                        StoreQueueTarget::TileAccelerator &&
                    product_runtime->store_queue_transfers->front().target_address == ta_target &&
                    product_runtime->pvr_ta_fifo->metrics().packets == 1u,
                "Produktive Dreamcast-Runtime verdrahtet SQ-PREF bei AT=1 nicht mit UTLB und TA.");

        ExecutableCodeTracker movca_tracker;
        static_cast<void>(
            movca_tracker.register_block({"movca-code", 0x0C000040u, 4u, "generated", {}}));
        memory.set_guest_write_observer([&](const auto& event) {
            static_cast<void>(movca_tracker.observe_write(
                event.address, event.size, event.source, event.bytes_changed));
        });
        auto cache_ops = std::make_unique<Sh4StoreQueues>(
            memory, StoreQueueSink{}, &movca_tracker, OperandCacheRamProfile::Modeled);
        const auto movca =
            cache_ops->maintain(CacheMaintenanceOperation::MovcaLong, 0x0C000040u, 0xAABBCCDDu);
        require(movca.wrote_memory && movca.invalidated_code &&
                    memory.read_u32(0x0C000040u) == 0xAABBCCDDu,
                "MOVCA.L umgeht Speichernebenwirkung oder Codeinvalidierung.");
        cache_ops->set_operand_cache_ram_enabled(true);
        cache_ops->write_operand_cache_ram(7u, 0x5Au);
        cache_ops->write_operand_cache_ram(0u, 0x44332211u, MemoryAccessWidth::Word);
        cache_ops->write_operand_cache_ram(8190u, 0xAABBu, MemoryAccessWidth::Halfword);
        require(cache_ops->read_operand_cache_ram(7u) == 0x5Au &&
                    cache_ops->read_operand_cache_ram(0u, MemoryAccessWidth::Word) == 0x44332211u &&
                    cache_ops->read_operand_cache_ram(8190u, MemoryAccessWidth::Halfword) ==
                        0xAABBu,
                "Explizit modellierter Operand-Cache-RAM besitzt keinen getrennten Zustand.");
        require_rejected(
            [&] {
                cache_ops->write_operand_cache_ram(8191u, 0xAABBu, MemoryAccessWidth::Halfword);
            },
            "Operand-Cache-RAM akzeptiert Fehlausrichtung oder Bereichsueberlauf.");
        require_rejected([&] { static_cast<void>(queues->read_operand_cache_ram(0u)); },
                         "Deaktivierter Operand-Cache-RAM wurde gelesen.");
        require_rejected(
            [&] { queues->set_operand_cache_ram_enabled(true); },
            "Nicht modellierter CCR-Operand-Cache-RAM-LLE-Pfad wird nicht sichtbar abgelehnt.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
