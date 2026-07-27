#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/exception.hpp"
#include "katana/runtime/store_queue.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace katana::runtime;
namespace {
BlockExit static_icbi_block(CpuState&, BlockExecutionContext&) {
    return {};
}

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

template <std::size_t Capacity> struct GuestMemoryAccessCapture {
    std::array<GuestMemoryAccessEvent, Capacity> events{};
    std::size_t count = 0u;
    std::size_t dropped = 0u;

    static void callback(void* const context,
                         const GuestMemoryAccessEvent& event) noexcept {
        auto& capture = *static_cast<GuestMemoryAccessCapture*>(context);
        if (capture.count < capture.events.size()) {
            capture.events[capture.count++] = event;
        } else {
            ++capture.dropped;
        }
    }

    [[nodiscard]] GuestMemoryAccessSink sink() noexcept {
        return GuestMemoryAccessSink{this, &GuestMemoryAccessCapture::callback};
    }
};

void require_store_queue_range_write(const GuestMemoryAccessEvent& event,
                                     const std::uint32_t target_address,
                                     const std::size_t expected_size,
                                     const bool expected_changed,
                                     const GuestInstructionOrigin instruction,
                                     const std::uint64_t retired_guest_instructions,
                                     const std::uint64_t attempted_guest_instructions,
                                     const char* const message) {
    require(event.operation == MemoryAccessOperation::Write &&
                event.access_origin == GuestMemoryAccessOrigin::Memory &&
                event.instruction.source_pc == instruction.source_pc &&
                event.instruction.runtime_pc == instruction.runtime_pc &&
                event.instruction.valid == instruction.valid &&
                event.virtual_address == target_address &&
                event.physical_address == target_address &&
                event.width == MemoryAccessWidth::Byte && event.size == expected_size &&
                event.write_source == CodeWriteSource::StoreQueue &&
                !event.scalar_value_valid && event.bytes_changed == expected_changed &&
                event.retired_guest_instructions == retired_guest_instructions &&
                event.attempted_guest_instructions == attempted_guest_instructions &&
                event.linear_backing != nullptr && event.linear_contiguous &&
                event.linear_size == expected_size && event.linear_byte_count == 1u,
            message);
}
} // namespace

int main() {
    static_assert(!noexcept(std::declval<const Sh4StoreQueues&>().snapshot()));
    try {
        Memory memory(0u);
        memory.map_region("ram", 0x0C000000u, std::make_shared<LinearMemoryDevice>(0x20000u));
        ExecutableCodeTracker tracker;
        static_cast<void>(tracker.register_block({"ram-code", 0x0C001000u, 32u, "generated", {}}));
        RuntimeBlockTable runtime_blocks;
        runtime_blocks.bind_code_tracker(
            &tracker, StaticAotInvalidationContract::Coordinated);
        RuntimeBlock static_block{0x8C001000u,
                                  0x0C001000u,
                                  32u,
                                  BlockEndKind::Fallthrough,
                                  {},
                                  static_icbi_block,
                                  "generated",
                                  false};
        static_block.static_variant_policy =
            StaticVariantPolicy::DirectP1P2RuntimeStateAgnostic;
        static_cast<void>(runtime_blocks.register_static(std::move(static_block)));
        runtime_blocks.seal_static();
        std::vector<StoreQueueTransfer> transfers;
        auto queues = std::make_unique<Sh4StoreQueues>(
            memory, [&](const auto& transfer) { transfers.push_back(transfer); }, &tracker);
        queues->bind_runtime_block_table(&runtime_blocks);
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
        Sh4StoreQueues result_queues(
            memory,
            [](const StoreQueueTransfer&) {
                throw StoreQueueSinkError(StoreQueueSinkErrorReason::UnsupportedInput,
                                          "synthetic-ta-rejection",
                                          "reserved-parameter-6");
            });
        result_queues.write_qacr(0u, 0x0Cu);
        constexpr GuestInstructionOrigin result_origin{0x8C001234u, 0x8C005678u, true};
        require(result_queues.prefetch_result(0xE0001000u, result_origin) ==
                        StoreQueuePrefetchResult::Rejected &&
                    result_queues.rejected_transfer_count() == 1u &&
                    result_queues.last_sink_fault() &&
                    result_queues.last_sink_fault()->packet_class ==
                        "reserved-parameter-6" &&
                    result_queues.last_sink_fault()->instruction.source_pc ==
                        result_origin.source_pc,
                "Typisiertes SQ-PREF-Ergebnis verliert Rejection, Paketklasse oder Gast-PC.");
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
            require(error.reason() == MemoryAccessErrorReason::Misaligned &&
                        error.operation() == MemoryAccessOperation::Write &&
                        error.address() == 0xE0001002u,
                    "Direktes nicht ausgerichtetes SQ-PREF verliert den Adressfehler.");
        }
        require(transfers.size() == 2u && queues->transfer_count() == 2u,
                "Direktes nicht ausgerichtetes SQ-PREF mutiert Sink oder Transferzaehler.");

        const auto icbi = queues->maintain(CacheMaintenanceOperation::Icbi, 0xAC001000u);
        require(icbi.invalidated_code && !tracker.valid("ram-code") &&
                    !runtime_blocks.lookup_static_aot(
                        0x0C001000u, 0x8C001000u, {}),
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
        constexpr GuestInstructionOrigin prefetch_origin{
            0x8C012340u, 0x8D012340u, true};
        constexpr std::uint64_t prefetch_retired = 0x123456789ull;
        constexpr std::uint64_t prefetch_attempted = 0x987654321ull;
        GuestMemoryAccessCapture<4u> direct_accesses;
        memory.set_guest_memory_access_sink(direct_accesses.sink());
        require(direct->prefetch(0xE0000040u,
                                 prefetch_origin,
                                 prefetch_retired,
                                 prefetch_attempted) &&
                    !direct_tracker.valid("sq-code"),
                "RAM-SQ-Ziel umgeht Speichernebenwirkung oder Codeinvalidierung.");
        require(direct_accesses.count == 2u && direct_accesses.dropped == 0u,
                "Direktes SQ-PREF trennt geaenderte und unveraenderte Bytes nicht exakt.");
        require_store_queue_range_write(
            direct_accesses.events.front(),
            0x0C000040u,
            4u,
            true,
            prefetch_origin,
            prefetch_retired,
            prefetch_attempted,
            "Direktes SQ-PREF verliert Writer-PC, Runtime-PC, Retired-Zahl oder StoreQueue-Quelle.");
        require_store_queue_range_write(
            direct_accesses.events[1u],
            0x0C000044u,
            28u,
            false,
            prefetch_origin,
            prefetch_retired,
            prefetch_attempted,
            "Direktes SQ-PREF markiert unveraenderte Restbytes als Writer.");
        require(!direct->prefetch(0x8C000040u,
                                  prefetch_origin,
                                  prefetch_retired,
                                  prefetch_attempted) &&
                    direct_accesses.count == 2u,
                "Normales PREF ausserhalb des SQ-Fensters emittiert einen Zielwrite.");
        memory.clear_guest_memory_access_sink();
        require(memory.read_u32(0x0C000040u) == 0x44332211u,
                "RAM-SQ-Ziel enthaelt nach PREF nicht den Queue-Inhalt.");
        direct->write_qacr(1u, 0x10u);
        direct->write_p4(0xE2000020u, 0x88776655u, MemoryAccessWidth::Word);
        require(direct->prefetch(0xE2000020u) && ta_offsets.size() == 32u &&
                    ta_offsets.front() == 0x20u && ta_offsets.back() == 0x3Fu,
                "TA-SQ-Ziel durchlaeuft seine MMIO-Nebenwirkungen nicht exakt.");

        constexpr std::uint32_t sq_source = 0xE2000020u;
        constexpr std::uint32_t ta_target = 0x12000020u;
        std::vector<StoreQueueTransfer> translated_transfers;
        auto translated_queues = std::make_unique<Sh4StoreQueues>(
            memory, [&](const auto& transfer) { translated_transfers.push_back(transfer); });
        CpuState no_mmu_cpu;
        no_mmu_cpu.address_space = std::make_shared<RuntimeAddressSpace>();
        no_mmu_cpu.write_sr(sr_md_mask);
        translated_queues->set_prefetch_address_translator(
            [&no_mmu_cpu](const std::uint32_t address) {
                return translate_store_queue_prefetch(no_mmu_cpu, address);
            });
        translated_queues->write_qacr(1u, 0x10u);
        for (std::uint32_t offset = 0u; offset < 32u; offset += 4u) {
            translated_queues->write_p4(
                sq_source + offset, 0xA3A2A1A0u + offset * 0x01010101u, MemoryAccessWidth::Word);
        }
        const auto expected_queue_bytes = translated_queues->queue(1u);
        const auto qacr_translation = translate_store_queue_prefetch(no_mmu_cpu, sq_source);
        require(qacr_translation.addressing == StoreQueueAddressingMode::Qacr &&
                    translated_queues->prefetch(sq_source) && translated_transfers.size() == 1u &&
                    translated_transfers.back().target_address == ta_target &&
                    translated_transfers.back().bytes == expected_queue_bytes,
                "AT=0 uebertraegt die Store Queue nicht unveraendert ueber QACR zum TA-FIFO.");

        try {
            static_cast<void>(translate_store_queue_prefetch(no_mmu_cpu, sq_source + 2u));
            require(false, "AT=0-Adressraum akzeptiert ein nicht ausgerichtetes SQ-PREF.");
        } catch (const MemoryAccessError& error) {
            require(error.reason() == MemoryAccessErrorReason::Misaligned &&
                        error.operation() == MemoryAccessOperation::Write &&
                        error.address() == sq_source + 2u,
                     "AT=0-Adressraum verliert den typisierten SQ-Adressfehler.");
        }
        try {
            static_cast<void>(translated_queues->prefetch(sq_source + 2u));
            require(false, "Nicht ausgerichtetes SQ-PREF wurde bei AT=0 akzeptiert.");
        } catch (const MemoryAccessError& error) {
            require(error.reason() == MemoryAccessErrorReason::Misaligned &&
                        error.operation() == MemoryAccessOperation::Write &&
                        error.address() == sq_source + 2u,
                     "Nicht ausgerichtetes AT=0-SQ-PREF verliert den typisierten Adressfehler.");
        }
        require(translated_transfers.size() == 1u &&
                    translated_queues->transfer_count() == 1u,
                "Nicht ausgerichtetes AT=0-SQ-PREF mutiert Sink oder Transferzaehler.");

        no_mmu_cpu.write_sr(0u);
        no_mmu_cpu.address_space->write_mmucr(0x00000200u);
        try {
            static_cast<void>(translated_queues->prefetch(sq_source));
            require(false, "SQMD-geschuetztes User-SQ-PREF wurde bei AT=0 akzeptiert.");
        } catch (const MemoryAccessError& error) {
            require(error.reason() == MemoryAccessErrorReason::Unmapped &&
                        error.operation() == MemoryAccessOperation::Write &&
                        error.address() == sq_source,
                    "SQMD-geschuetztes AT=0-User-PREF verliert den typisierten Adressfehler.");
        }
        require(translated_transfers.size() == 1u &&
                    translated_queues->transfer_count() == 1u,
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
        translated_queues->set_prefetch_address_translator(
            [&mmu_cpu](const std::uint32_t address) {
                return translate_store_queue_prefetch(mmu_cpu, address);
            });
        translated_queues->write_qacr(1u, 0x0Cu);
        const auto utlb_translation = translate_store_queue_prefetch(mmu_cpu, sq_source);
        require(utlb_translation.addressing == StoreQueueAddressingMode::Utlb &&
                    utlb_translation.target_address == ta_target &&
                    translated_queues->prefetch(sq_source) && translated_transfers.size() == 2u &&
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
        translated_queues->set_prefetch_address_translator(
            [&miss_cpu](const std::uint32_t address) {
                return translate_store_queue_prefetch(miss_cpu, address);
            });
        try {
            static_cast<void>(translated_queues->prefetch(sq_source));
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
                    translated_queues->transfer_count() == 2u,
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
        translated_queues->set_prefetch_address_translator(
            [&multiple_cpu](const std::uint32_t address) {
                return translate_store_queue_prefetch(multiple_cpu, address);
            });
        try {
            static_cast<void>(translated_queues->prefetch(sq_source));
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
                    translated_queues->transfer_count() == 2u,
                "SQ-UTLB-Multiple-Hit schreibt ins FIFO oder verliert Reset/PTEH/TEA.");

        DreamcastRuntimeBootImage boot;
        const std::vector<std::uint8_t> disc_sector(dreamcast_data_sector_size, 0u);
        boot.source = std::make_shared<MemoryDiscSource>(
            std::span<const std::uint8_t>(disc_sector), "synthetic-store-queue-disc");
        boot.system_bootstrap.resize(dreamcast_system_bootstrap_size, 0u);
        boot.boot_file = {0x09u, 0x00u};
        boot.content_identity = "sha256:synthetic-store-queue-content";
        boot.repeated_bootstrap_reads_match = true;
        boot.repeated_reads_match = true;
        auto product_cpu = std::make_unique<CpuState>();
        auto product_runtime = std::make_unique<DreamcastRuntimeState>(
            initialize_dreamcast_runtime(*product_cpu, boot));
        GuestMemoryAccessCapture<4u> product_accesses;
        product_cpu->memory.set_guest_memory_access_sink(product_accesses.sink());
        product_runtime->store_queues->write_qacr(0u, 0x0Cu);
        product_runtime->store_queues->write_p4(
            0xE0000040u, 0x44332211u, MemoryAccessWidth::Word);
        require(product_runtime->store_queues->prefetch(
                    0xE0000040u,
                    prefetch_origin,
                    prefetch_retired,
                    prefetch_attempted) &&
                    product_runtime->store_queue_transfers->size() == 1u &&
                    product_runtime->store_queue_transfers->front().instruction.source_pc ==
                        prefetch_origin.source_pc &&
                    product_runtime->store_queue_transfers->front().instruction.runtime_pc ==
                        prefetch_origin.runtime_pc &&
                    product_runtime->store_queue_transfers->front()
                            .retired_guest_instructions == prefetch_retired &&
                    product_runtime->store_queue_transfers->front()
                            .attempted_guest_instructions == prefetch_attempted &&
                    product_accesses.count == 2u && product_accesses.dropped == 0u,
                "Produktiver externer SQ-Sink verliert PREF-Provenienz vor write_bytes_at.");
        require_store_queue_range_write(
            product_accesses.events.front(),
            0x0C000040u,
            4u,
            true,
            prefetch_origin,
            prefetch_retired,
            prefetch_attempted,
            "Produktiver SQ-write_bytes_at-Pfad verliert Writer-Provenienz oder Quelle.");
        require_store_queue_range_write(
            product_accesses.events[1u],
            0x0C000044u,
            28u,
            false,
            prefetch_origin,
            prefetch_retired,
            prefetch_attempted,
            "Produktiver SQ-write_bytes_at-Pfad markiert No-op-Bytes als Writer.");
        product_cpu->memory.clear_guest_memory_access_sink();
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
                    product_runtime->store_queue_transfers->size() == 2u &&
                    product_runtime->store_queue_transfers->back().target ==
                        StoreQueueTarget::TileAccelerator &&
                    product_runtime->store_queue_transfers->back().target_address == ta_target &&
                    product_runtime->pvr_ta_fifo->metrics().packets == 1u,
                "Produktive Dreamcast-Runtime verdrahtet SQ-PREF bei AT=1 nicht mit UTLB und TA.");

        const auto accepted_sq_transfers =
            product_runtime->store_queues->transfer_count();
        const auto accepted_ta_packets =
            product_runtime->pvr_ta_fifo->metrics().packets;
        product_runtime->store_queues->write_p4(
            sq_source, 0x60000000u, MemoryAccessWidth::Word);
        bool typed_prefetch_rejection = false;
        try {
            static_cast<void>(product_runtime->store_queues->prefetch(
                sq_source, prefetch_origin, prefetch_retired, prefetch_attempted));
        } catch (const StoreQueuePrefetchRejected& error) {
            typed_prefetch_rejection =
                error.fault().reason == StoreQueueSinkErrorReason::UnsupportedInput &&
                error.fault().packet_class == "reserved-parameter-3" &&
                error.fault().target_address == ta_target &&
                error.fault().instruction.source_pc == prefetch_origin.source_pc &&
                error.fault().instruction.runtime_pc == prefetch_origin.runtime_pc;
        }
        require(typed_prefetch_rejection &&
                    product_runtime->store_queues->transfer_count() ==
                        accepted_sq_transfers &&
                    product_runtime->store_queues->rejected_transfer_count() == 1u &&
                    product_runtime->store_queues->last_sink_fault() &&
                    product_runtime->store_queues->last_sink_fault()->reason ==
                        StoreQueueSinkErrorReason::UnsupportedInput &&
                    product_runtime->pvr_ta_fifo->metrics().packets ==
                        accepted_ta_packets &&
                    product_runtime->pvr_ta_fifo->metrics().rejected_packets == 1u &&
                    product_runtime->pvr_ta_fifo->snapshot().first_input_error &&
                    product_runtime->pvr_ta_fifo->snapshot().first_input_error->reason ==
                        PvrTaInputErrorReason::UnsupportedPacket,
                "Ungueltiges TA-Paket aus echtem SQ-PREF endet nicht typisiert "
                "oder committed Teilzustand.");

        constexpr std::uint32_t reset_sentinel_address = 0x8C001FF0u;
        product_cpu->memory.write_u32(
            reset_sentinel_address, 0xA55AA55Au, CodeWriteSource::Copy);
        product_cpu->r[7u] = 0x12345678u;
        product_runtime->system_asic->raise(
            SystemAsicEvent::PvrVblank,
            product_runtime->scheduler->current_cycle());
        product_runtime->pvr_registers->write(
            pvr_register::BorderColor, 0x00112233u);
        product_runtime->pvr_yuv_converter->write_u8(0u, 0x44u);
        product_runtime->holly_dma.g1->write(0xA0u, 0x55667788u);
        product_runtime->holly_dma.g2->write(0x00u, 0x00801000u);
        product_runtime->holly_dma.pvr->write(0x00u, 0x04002000u);
        product_cpu->memory.write_u32(0xA05F6890u, 0x7611u);
        require(product_runtime->system_bus_control->system_reset_requests() == 1u &&
                    product_runtime->system_asic->read(0x00u) == 0u &&
                    product_runtime->pvr_registers->read(
                        pvr_register::BorderColor) == 0u &&
                    product_runtime->pvr_ta_fifo->metrics().packets == 0u &&
                    product_runtime->pvr_ta_fifo->metrics().rejected_packets == 0u &&
                    !product_runtime->pvr_ta_aperture->snapshot().packet_active &&
                    product_runtime->pvr_yuv_converter->snapshot().input.empty() &&
                    product_runtime->holly_dma.g1->gdrom_read_access_timing() == 0u &&
                    product_runtime->holly_dma.g2->channel_state(0u).peripheral_address ==
                        0u &&
                    product_runtime->holly_dma.pvr->state().peripheral_address == 0u &&
                    product_cpu->r[7u] == 0x12345678u &&
                    product_cpu->memory.read_u32(reset_sentinel_address) == 0xA55AA55Au,
                "SB_SFRES erreicht Holly/PVR/DMA nicht oder simuliert einen CPU-/RAM-Power-on.");

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
        const auto store_queue_snapshot = cache_ops->snapshot();
        require(store_queue_snapshot.operand_cache_ram_enabled &&
                    store_queue_snapshot.operand_cache_ram_profile ==
                        OperandCacheRamProfile::Modeled &&
                    store_queue_snapshot.operand_cache_ram[0u] == 0x11u &&
                    store_queue_snapshot.operand_cache_ram[3u] == 0x44u &&
                    store_queue_snapshot.operand_cache_ram[7u] == 0x5Au &&
                    store_queue_snapshot.code_tracker_bound,
                "Store-Queue-Snapshot verliert OCRAM-Bytes, Profil oder Trackerbindung.");
        cache_ops->write_qacr(0u, 0x0Cu);
        cache_ops->reset();
        const auto reset_store_queue = cache_ops->snapshot();
        require(reset_store_queue.queues ==
                        std::array<std::array<std::uint8_t, 32u>, 2u>{} &&
                    reset_store_queue.qacr == std::array<std::uint32_t, 2u>{} &&
                    reset_store_queue.operand_cache_ram ==
                        std::array<std::uint8_t, 8192u>{} &&
                    !reset_store_queue.operand_cache_ram_enabled &&
                    reset_store_queue.transfer_count == 0u &&
                    reset_store_queue.rejected_transfer_count == 0u &&
                    !reset_store_queue.last_sink_fault,
                "Store-Queue-Reset laesst QACR-, Queue-, OCRAM- oder Fehlerzustand stehen.");
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
