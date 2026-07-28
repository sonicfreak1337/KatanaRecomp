#include "katana/runtime/game_entry_handoff.hpp"

#include "katana/io/input_provenance.hpp"
#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/cache_control.hpp"
#include "katana/runtime/disc_load_transaction.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/interrupt.hpp"
#include "katana/runtime/io_port.hpp"
#include "katana/runtime/platform_interrupt.hpp"
#include "katana/runtime/store_queue.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace katana::runtime {
namespace {

constexpr std::uint32_t device_payload_field = 1u;
constexpr std::uint32_t device_payload_contract = 1u;
constexpr std::size_t maximum_codec_string_size = 4096u;

class PlatformStateWriter final {
  public:
    void magic(const std::string_view value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void u8(const std::uint8_t value) { bytes_.push_back(value); }
    void boolean(const bool value) { u8(value ? 1u : 0u); }
    void u16(const std::uint16_t value) {
        for (std::size_t byte = 0u; byte < 2u; ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void u32(const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void u64(const std::uint64_t value) {
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    template <typename Enum>
    void enumeration(const Enum value) {
        static_assert(std::is_enum_v<Enum>);
        using Underlying = std::underlying_type_t<Enum>;
        if constexpr (sizeof(Underlying) <= sizeof(std::uint8_t))
            u8(static_cast<std::uint8_t>(value));
        else if constexpr (sizeof(Underlying) <= sizeof(std::uint16_t))
            u16(static_cast<std::uint16_t>(value));
        else
            u32(static_cast<std::uint32_t>(value));
    }
    void string(const std::string_view value) {
        if (value.size() > maximum_codec_string_size)
            throw std::invalid_argument(
                "Platform-Handoff-String ueberschreitet das Limit.");
        u32(static_cast<std::uint32_t>(value.size()));
        magic(value);
    }
    void bytes(const std::span<const std::uint8_t> value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument(
                "Platform-Handoff-Payload ueberschreitet das Limit.");
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

class PlatformStateReader final {
  public:
    explicit PlatformStateReader(
        const std::span<const std::uint8_t> bytes) noexcept
        : bytes_(bytes) {}

    void magic(const std::string_view expected) {
        const auto value = take(expected.size());
        if (!std::equal(value.begin(), value.end(), expected.begin()))
            throw std::invalid_argument(
                "Platform-Handoff besitzt eine falsche Signatur.");
    }
    [[nodiscard]] std::uint8_t u8() {
        return take(1u).front();
    }
    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1u)
            throw std::invalid_argument(
                "Platform-Handoff besitzt einen ungueltigen Boolwert.");
        return value != 0u;
    }
    [[nodiscard]] std::uint16_t u16() {
        const auto value = take(2u);
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(value[0]) |
            (static_cast<std::uint16_t>(value[1]) << 8u));
    }
    [[nodiscard]] std::uint32_t u32() {
        const auto value = take(4u);
        std::uint32_t result = 0u;
        for (std::size_t byte = 0u; byte < value.size(); ++byte)
            result |= static_cast<std::uint32_t>(value[byte]) <<
                      (byte * 8u);
        return result;
    }
    [[nodiscard]] std::uint64_t u64() {
        const auto value = take(8u);
        std::uint64_t result = 0u;
        for (std::size_t byte = 0u; byte < value.size(); ++byte)
            result |= static_cast<std::uint64_t>(value[byte]) <<
                      (byte * 8u);
        return result;
    }
    [[nodiscard]] std::string string() {
        const auto size = u32();
        if (size > maximum_codec_string_size)
            throw std::invalid_argument(
                "Platform-Handoff-String ueberschreitet das Limit.");
        const auto value = take(size);
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }
    [[nodiscard]] std::vector<std::uint8_t> bytes(
        const std::size_t maximum) {
        const auto size = u32();
        if (size > maximum)
            throw std::invalid_argument(
                "Platform-Handoff-Payload ueberschreitet das Limit.");
        const auto value = take(size);
        return {value.begin(), value.end()};
    }
    void finish() const {
        if (offset_ != bytes_.size())
            throw std::invalid_argument(
                "Platform-Handoff-Payload besitzt nachlaufende Bytes.");
    }

  private:
    [[nodiscard]] std::span<const std::uint8_t> take(
        const std::size_t size) {
        if (offset_ > bytes_.size() || size > bytes_.size() - offset_)
            throw std::invalid_argument(
                "Platform-Handoff-Payload ist abgeschnitten.");
        const auto result = bytes_.subspan(offset_, size);
        offset_ += size;
        return result;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0u;
};

void write_tlb_mapping(
    PlatformStateWriter& writer,
    const TlbMapping& mapping) {
    writer.u32(mapping.virtual_page);
    writer.u32(mapping.physical_page);
    writer.u32(mapping.page_size);
    writer.u8(mapping.asid);
    writer.u8(mapping.slot);
    writer.boolean(mapping.valid);
    writer.boolean(mapping.readable);
    writer.boolean(mapping.writable);
    writer.boolean(mapping.executable);
    writer.boolean(mapping.user_access);
    writer.boolean(mapping.dirty);
    writer.boolean(mapping.shared);
}

TlbMapping read_tlb_mapping(PlatformStateReader& reader) {
    TlbMapping mapping;
    mapping.virtual_page = reader.u32();
    mapping.physical_page = reader.u32();
    mapping.page_size = reader.u32();
    mapping.asid = reader.u8();
    mapping.slot = reader.u8();
    mapping.valid = reader.boolean();
    mapping.readable = reader.boolean();
    mapping.writable = reader.boolean();
    mapping.executable = reader.boolean();
    mapping.user_access = reader.boolean();
    mapping.dirty = reader.boolean();
    mapping.shared = reader.boolean();
    return mapping;
}

std::vector<std::uint8_t> encode_interrupt_controller(
    const InterruptControllerSnapshot& state) {
    PlatformStateWriter writer;
    writer.magic("KATIRQ1\n");
    writer.u32(device_payload_contract);
    writer.u32(static_cast<std::uint32_t>(state.pending.size()));
    for (const auto& pending : state.pending) {
        writer.u32(pending.source);
        writer.u8(pending.level);
        writer.u32(pending.event_code);
    }
    return std::move(writer).finish();
}

InterruptControllerSnapshot decode_interrupt_controller(
    const std::span<const std::uint8_t> bytes) {
    PlatformStateReader reader(bytes);
    reader.magic("KATIRQ1\n");
    if (reader.u32() != device_payload_contract)
        throw std::invalid_argument(
            "Interrupt-Handoff-Vertrag ist inkompatibel.");
    const auto count = reader.u32();
    if (count > 256u)
        throw std::invalid_argument(
            "Interrupt-Handoff besitzt zu viele Quellen.");
    InterruptControllerSnapshot state;
    state.pending.reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index)
        state.pending.push_back(
            {reader.u32(), reader.u8(), reader.u32()});
    reader.finish();
    return state;
}

std::vector<std::uint8_t> encode_interrupt_router(
    const PlatformInterruptRouterSnapshot& state) {
    PlatformStateWriter writer;
    writer.magic("KATIRR1\n");
    writer.u32(device_payload_contract);
    for (const auto value : state.tmu_levels) writer.u8(value);
    writer.u8(state.rtc_level);
    writer.u8(state.dma_level);
    writer.u8(state.scif_level);
    for (const auto value : state.scif_pending) writer.boolean(value);
    for (const auto value : state.external_pending) writer.boolean(value);
    return std::move(writer).finish();
}

PlatformInterruptRouterSnapshot decode_interrupt_router(
    const std::span<const std::uint8_t> bytes) {
    PlatformStateReader reader(bytes);
    reader.magic("KATIRR1\n");
    if (reader.u32() != device_payload_contract)
        throw std::invalid_argument(
            "Interrupt-Router-Handoff-Vertrag ist inkompatibel.");
    PlatformInterruptRouterSnapshot state;
    for (auto& value : state.tmu_levels) value = reader.u8();
    state.rtc_level = reader.u8();
    state.dma_level = reader.u8();
    state.scif_level = reader.u8();
    for (auto& value : state.scif_pending) value = reader.boolean();
    for (auto& value : state.external_pending) value = reader.boolean();
    reader.finish();
    return state;
}

std::vector<std::uint8_t> encode_interrupt_registers(
    const Sh4InterruptRegistersSnapshot& state) {
    PlatformStateWriter writer;
    writer.magic("KATINT1\n");
    writer.u32(device_payload_contract);
    writer.u16(state.interrupt_control);
    writer.u16(state.priority_a);
    writer.u16(state.priority_b);
    writer.u16(state.priority_c);
    writer.u16(state.priority_d);
    return std::move(writer).finish();
}

Sh4InterruptRegistersSnapshot decode_interrupt_registers(
    const std::span<const std::uint8_t> bytes) {
    PlatformStateReader reader(bytes);
    reader.magic("KATINT1\n");
    if (reader.u32() != device_payload_contract)
        throw std::invalid_argument(
            "INTC-Handoff-Vertrag ist inkompatibel.");
    Sh4InterruptRegistersSnapshot state{
        reader.u16(),
        reader.u16(),
        reader.u16(),
        reader.u16(),
        reader.u16(),
    };
    reader.finish();
    return state;
}

std::vector<std::uint8_t> encode_mmu(
    const RuntimeAddressSpaceSnapshot& state) {
    PlatformStateWriter writer;
    writer.magic("KATMMU1\n");
    writer.u32(device_payload_contract);
    writer.enumeration(state.mode);
    writer.u32(state.mmucr);
    writer.u8(state.asid);
    auto mappings = state.mappings;
    std::sort(
        mappings.begin(), mappings.end(), [](const auto& left, const auto& right) {
            return left.slot < right.slot;
        });
    writer.u32(static_cast<std::uint32_t>(mappings.size()));
    for (const auto& mapping : mappings)
        write_tlb_mapping(writer, mapping);
    for (const auto& mapping : state.itlb)
        write_tlb_mapping(writer, mapping);
    for (const auto value : state.itlb_valid) writer.boolean(value);
    for (const auto value : state.itlb_lru) writer.u8(value);
    for (const auto value : state.itlb_source_slots) writer.u8(value);
    return std::move(writer).finish();
}

RuntimeAddressSpaceSnapshot decode_mmu(
    const std::span<const std::uint8_t> bytes) {
    PlatformStateReader reader(bytes);
    reader.magic("KATMMU1\n");
    if (reader.u32() != device_payload_contract)
        throw std::invalid_argument(
            "MMU-Handoff-Vertrag ist inkompatibel.");
    RuntimeAddressSpaceSnapshot state;
    const auto mode = reader.u8();
    if (mode > static_cast<std::uint8_t>(AddressTranslationMode::Mmu))
        throw std::invalid_argument(
            "MMU-Handoff besitzt einen ungueltigen Modus.");
    state.mode = static_cast<AddressTranslationMode>(mode);
    state.mmucr = reader.u32();
    state.asid = reader.u8();
    state.address_space_generation = 0u;
    state.mmu_generation = 0u;
    state.watchpoint_generation = 0u;
    const auto count = reader.u32();
    if (count > 64u)
        throw std::invalid_argument(
            "MMU-Handoff besitzt zu viele UTLB-Abbildungen.");
    state.mappings.reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index)
        state.mappings.push_back(read_tlb_mapping(reader));
    for (auto& mapping : state.itlb)
        mapping = read_tlb_mapping(reader);
    for (auto& value : state.itlb_valid) value = reader.boolean();
    for (auto& value : state.itlb_lru) value = reader.u8();
    for (auto& value : state.itlb_source_slots) value = reader.u8();
    reader.finish();
    return state;
}

std::vector<std::uint8_t> encode_cache(
    const Sh4CacheControlSnapshot& state) {
    PlatformStateWriter writer;
    writer.magic("KATCAC1\n");
    writer.u32(device_payload_contract);
    writer.u32(state.value);
    writer.u64(state.instruction_invalidations);
    for (const auto value : state.instruction_addresses)
        writer.u32(value);
    for (const auto value : state.operand_addresses)
        writer.u32(value);
    writer.bytes(state.instruction_data);
    writer.bytes(state.operand_data);
    writer.bytes(state.on_chip_ram);
    return std::move(writer).finish();
}

Sh4CacheControlSnapshot decode_cache(
    const std::span<const std::uint8_t> bytes) {
    PlatformStateReader reader(bytes);
    reader.magic("KATCAC1\n");
    if (reader.u32() != device_payload_contract)
        throw std::invalid_argument(
            "Cache-Handoff-Vertrag ist inkompatibel.");
    Sh4CacheControlSnapshot state;
    state.value = reader.u32();
    state.instruction_invalidations = reader.u64();
    for (auto& value : state.instruction_addresses)
        value = reader.u32();
    for (auto& value : state.operand_addresses)
        value = reader.u32();
    const auto instruction_data =
        reader.bytes(state.instruction_data.size());
    const auto operand_data = reader.bytes(state.operand_data.size());
    const auto on_chip_ram = reader.bytes(state.on_chip_ram.size());
    if (instruction_data.size() != state.instruction_data.size() ||
        operand_data.size() != state.operand_data.size() ||
        on_chip_ram.size() != state.on_chip_ram.size())
        throw std::invalid_argument(
            "Cache-Handoff besitzt falsche Arraygroessen.");
    std::copy(
        instruction_data.begin(),
        instruction_data.end(),
        state.instruction_data.begin());
    std::copy(
        operand_data.begin(), operand_data.end(), state.operand_data.begin());
    std::copy(
        on_chip_ram.begin(), on_chip_ram.end(), state.on_chip_ram.begin());
    reader.finish();
    return state;
}

std::vector<std::uint8_t> encode_store_queues(
    const Sh4StoreQueueSnapshot& state) {
    PlatformStateWriter writer;
    writer.magic("KATSQ01\n");
    writer.u32(device_payload_contract);
    for (const auto& queue : state.queues) writer.bytes(queue);
    for (const auto value : state.qacr) writer.u32(value);
    writer.bytes(state.operand_cache_ram);
    writer.enumeration(state.operand_cache_ram_profile);
    writer.boolean(state.operand_cache_ram_enabled);
    writer.boolean(state.external_sink_bound);
    writer.boolean(state.address_translator_bound);
    writer.boolean(state.code_tracker_bound);
    writer.u64(state.transfer_count);
    writer.u64(state.rejected_transfer_count);
    writer.boolean(state.last_sink_fault.has_value());
    if (state.last_sink_fault) {
        const auto& fault = *state.last_sink_fault;
        writer.enumeration(fault.reason);
        writer.u32(fault.source_address);
        writer.u32(fault.target_address);
        writer.string(fault.detail);
        writer.string(fault.packet_class);
        writer.u32(fault.instruction.source_pc);
        writer.u32(fault.instruction.runtime_pc);
        writer.boolean(fault.instruction.valid);
    }
    return std::move(writer).finish();
}

Sh4StoreQueueSnapshot decode_store_queues(
    const std::span<const std::uint8_t> bytes) {
    PlatformStateReader reader(bytes);
    reader.magic("KATSQ01\n");
    if (reader.u32() != device_payload_contract)
        throw std::invalid_argument(
            "Store-Queue-Handoff-Vertrag ist inkompatibel.");
    Sh4StoreQueueSnapshot state;
    for (auto& queue : state.queues) {
        const auto payload = reader.bytes(queue.size());
        if (payload.size() != queue.size())
            throw std::invalid_argument(
                "Store-Queue-Handoff besitzt eine falsche Queuegroesse.");
        std::copy(payload.begin(), payload.end(), queue.begin());
    }
    for (auto& value : state.qacr) value = reader.u32();
    const auto ocram = reader.bytes(state.operand_cache_ram.size());
    if (ocram.size() != state.operand_cache_ram.size())
        throw std::invalid_argument(
            "Store-Queue-Handoff besitzt eine falsche OCRAM-Groesse.");
    std::copy(
        ocram.begin(), ocram.end(), state.operand_cache_ram.begin());
    const auto profile = reader.u8();
    if (profile > static_cast<std::uint8_t>(
                      OperandCacheRamProfile::Modeled))
        throw std::invalid_argument(
            "Store-Queue-Handoff besitzt ein ungueltiges OCRAM-Profil.");
    state.operand_cache_ram_profile =
        static_cast<OperandCacheRamProfile>(profile);
    state.operand_cache_ram_enabled = reader.boolean();
    state.external_sink_bound = reader.boolean();
    state.address_translator_bound = reader.boolean();
    state.code_tracker_bound = reader.boolean();
    state.transfer_count = reader.u64();
    state.rejected_transfer_count = reader.u64();
    if (reader.boolean()) {
        const auto reason = reader.u8();
        if (reason >
            static_cast<std::uint8_t>(
                StoreQueueSinkErrorReason::UnsupportedInput))
            throw std::invalid_argument(
                "Store-Queue-Handoff besitzt einen ungueltigen Fehler.");
        StoreQueueSinkFault fault;
        fault.reason = static_cast<StoreQueueSinkErrorReason>(reason);
        fault.source_address = reader.u32();
        fault.target_address = reader.u32();
        fault.detail = reader.string();
        fault.packet_class = reader.string();
        fault.instruction.source_pc = reader.u32();
        fault.instruction.runtime_pc = reader.u32();
        fault.instruction.valid = reader.boolean();
        state.last_sink_fault = std::move(fault);
    }
    reader.finish();
    return state;
}

std::vector<std::uint8_t> encode_io_ports(
    const Sh4IoPortSnapshot& state) {
    PlatformStateWriter writer;
    writer.magic("KATIOP1\n");
    writer.u32(device_payload_contract);
    writer.u16(state.inputs.port_a);
    writer.u16(state.inputs.port_b);
    writer.u32(state.control_a);
    writer.u16(state.data_a_latch);
    writer.u16(state.effective_data_a);
    writer.u32(state.control_b);
    writer.u16(state.data_b_latch);
    writer.u16(state.effective_data_b);
    writer.u16(state.gpio_interrupt_control);
    return std::move(writer).finish();
}

Sh4IoPortSnapshot decode_io_ports(
    const std::span<const std::uint8_t> bytes) {
    PlatformStateReader reader(bytes);
    reader.magic("KATIOP1\n");
    if (reader.u32() != device_payload_contract)
        throw std::invalid_argument(
            "I/O-Port-Handoff-Vertrag ist inkompatibel.");
    Sh4IoPortSnapshot state;
    state.inputs.port_a = reader.u16();
    state.inputs.port_b = reader.u16();
    state.control_a = reader.u32();
    state.data_a_latch = reader.u16();
    state.effective_data_a = reader.u16();
    state.control_b = reader.u32();
    state.data_b_latch = reader.u16();
    state.effective_data_b = reader.u16();
    state.gpio_interrupt_control = reader.u16();
    reader.finish();
    return state;
}

std::vector<std::uint8_t> encode_flash(
    const FlashMemorySnapshot& state) {
    PlatformStateWriter writer;
    writer.magic("KATFLS1\n");
    writer.u32(device_payload_contract);
    if (state.size > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument(
            "Flash-Handoff besitzt eine ungueltige Groesse.");
    writer.u32(static_cast<std::uint32_t>(state.size));
    writer.enumeration(state.command_state);
    writer.boolean(state.write_protected);
    writer.boolean(state.working_copy_dirty);
    writer.boolean(state.persistent_working_copy);
    writer.bytes(state.source_bytes);
    writer.bytes(state.working_bytes);
    return std::move(writer).finish();
}

FlashMemorySnapshot decode_flash(
    const std::span<const std::uint8_t> bytes) {
    PlatformStateReader reader(bytes);
    reader.magic("KATFLS1\n");
    if (reader.u32() != device_payload_contract)
        throw std::invalid_argument(
            "Flash-Handoff-Vertrag ist inkompatibel.");
    FlashMemorySnapshot state;
    state.size = reader.u32();
    const auto command = reader.u8();
    if (command >
        static_cast<std::uint8_t>(FlashCommandState::EraseConfirm))
        throw std::invalid_argument(
            "Flash-Handoff besitzt einen ungueltigen Kommandozustand.");
    state.command_state = static_cast<FlashCommandState>(command);
    state.write_protected = reader.boolean();
    state.working_copy_dirty = reader.boolean();
    state.persistent_working_copy = reader.boolean();
    state.source_bytes = reader.bytes(dreamcast_flash_size);
    state.working_bytes = reader.bytes(dreamcast_flash_size);
    reader.finish();
    return state;
}

void add_device_payload(
    CapturedGameEntryPlatformState& capture,
    const GameEntryDeviceKind kind,
    std::string name,
    std::vector<std::uint8_t> bytes) {
    const GameEntryDeviceKey key{kind, 0u};
    GameEntryDeviceState state;
    state.key = key;
    state.state_contract_version = device_payload_contract;
    state.payloads.push_back({device_payload_field, {}});
    capture.devices.push_back(std::move(state));
    capture.payloads.push_back(
        {key, device_payload_field, std::move(name), std::move(bytes)});
}

const ValidatedGameEntryDeviceState& require_device(
    const ValidatedGameEntryHandoff& handoff,
    const GameEntryDeviceKind kind) {
    const GameEntryDeviceKey key{kind, 0u};
    const auto devices = handoff.devices();
    const auto found = std::lower_bound(
        devices.begin(),
        devices.end(),
        key,
        [](const auto& left, const auto& right) {
            return std::tuple{
                       static_cast<std::uint16_t>(left.key.kind),
                       left.key.instance} <
                   std::tuple{
                       static_cast<std::uint16_t>(right.kind),
                       right.instance};
        });
    if (found == devices.end() || found->key != key ||
        found->state_contract_version != device_payload_contract ||
        !found->scalars.empty() || found->payloads.size() != 1u ||
        found->payloads.front().payload.field_id != device_payload_field ||
        found->payloads.front().bytes.empty())
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::DeviceStateInvalid);
    return *found;
}

std::span<const std::uint8_t> device_bytes(
    const ValidatedGameEntryHandoff& handoff,
    const GameEntryDeviceKind kind) {
    return require_device(handoff, kind).payloads.front().bytes;
}

InterruptControllerSnapshot expected_interrupt_controller_snapshot(
    const PlatformInterruptRouterSnapshot& router,
    const Sh4TmuSnapshot& tmu,
    const Sh4RtcSnapshot& rtc,
    const Sh4DmacSnapshot& dmac) {
    InterruptControllerSnapshot result;
    const auto append =
        [&](const PlatformInterruptSource source,
            const bool pending,
            const std::uint8_t level) {
            if (!pending) return;
            const auto code = static_cast<std::uint32_t>(source);
            result.pending.push_back({code, level, code});
        };
    for (std::size_t channel = 0u; channel < tmu.channels.size(); ++channel) {
        const auto control = tmu.channels[channel].control;
        append(
            static_cast<PlatformInterruptSource>(
                static_cast<std::uint32_t>(PlatformInterruptSource::Tmu0) +
                static_cast<std::uint32_t>(channel) * 0x20u),
            (control &
             (Sh4Tmu::underflow_flag |
              Sh4Tmu::underflow_interrupt_enable)) ==
                (Sh4Tmu::underflow_flag |
                 Sh4Tmu::underflow_interrupt_enable),
            router.tmu_levels[channel]);
    }
    append(
        PlatformInterruptSource::RtcAlarm,
        rtc.alarm_pending && rtc.alarm_enabled,
        router.rtc_level);
    append(
        PlatformInterruptSource::RtcPeriodic,
        rtc.periodic_pending,
        router.rtc_level);
    append(
        PlatformInterruptSource::RtcCarry,
        rtc.carry_flag && rtc.carry_enabled,
        router.rtc_level);
    for (std::size_t channel = 0u; channel < dmac.channels.size(); ++channel) {
        append(
            static_cast<PlatformInterruptSource>(
                static_cast<std::uint32_t>(PlatformInterruptSource::Dma0) +
                static_cast<std::uint32_t>(channel) * 0x20u),
            dmac.channels[channel].interrupt_pending,
            router.dma_level);
    }
    append(
        PlatformInterruptSource::DmaError,
        (dmac.operation & Sh4Dmac::address_error_flag) != 0u,
        router.dma_level);
    constexpr std::array scif_sources{
        PlatformInterruptSource::ScifError,
        PlatformInterruptSource::ScifReceive,
        PlatformInterruptSource::ScifBreak,
        PlatformInterruptSource::ScifTransmit};
    for (std::size_t source = 0u; source < scif_sources.size(); ++source)
        append(
            scif_sources[source],
            router.scif_pending[source],
            router.scif_level);
    constexpr std::array external_sources{
        PlatformInterruptSource::ExternalLevel2,
        PlatformInterruptSource::ExternalLevel4,
        PlatformInterruptSource::ExternalLevel6};
    constexpr std::array<std::uint8_t, 3u> external_levels{2u, 4u, 6u};
    for (std::size_t line = 0u; line < external_sources.size(); ++line)
        append(
            external_sources[line],
            router.external_pending[line],
            external_levels[line]);
    std::sort(
        result.pending.begin(),
        result.pending.end(),
        [](const auto& left, const auto& right) {
            return left.source < right.source;
        });
    return result;
}

std::string prepared_identity(
    const std::string_view domain,
    const std::vector<std::uint8_t>& payload) {
    PlatformStateWriter writer;
    writer.magic("KATPREP1\n");
    writer.string(domain);
    writer.bytes(payload);
    const auto material = std::move(writer).finish();
    const std::string_view view(
        reinterpret_cast<const char*>(material.data()),
        material.size());
    return "sha256:" + katana::io::sha256_bytes(view);
}

std::string prepared_cpu_identity(const GameEntryCpuState& state) {
    PlatformStateWriter writer;
    writer.magic("KATCPU1\n");
    writer.u32(device_payload_contract);
    for (const auto value : state.gpr_bank0) writer.u32(value);
    for (const auto value : state.gpr_bank1) writer.u32(value);
    for (const auto value : state.r8_to_r15) writer.u32(value);
    for (const auto value : state.fpr_bank0) writer.u32(value);
    for (const auto value : state.fpr_bank1) writer.u32(value);
    writer.u32(state.pc);
    writer.u32(state.pr);
    writer.u32(state.sr);
    writer.u32(state.fpscr);
    writer.u32(state.gbr);
    writer.u32(state.vbr);
    writer.u32(state.dbr);
    writer.u32(state.ssr);
    writer.u32(state.spc);
    writer.u32(state.sgr);
    writer.u32(state.mach);
    writer.u32(state.macl);
    writer.u32(state.fpul);
    writer.u32(state.tra);
    writer.u32(state.tea);
    writer.u32(state.expevt);
    writer.u32(state.intevt);
    writer.u32(state.mmu.pteh);
    writer.u32(state.mmu.ptel);
    writer.u32(state.mmu.ptea);
    writer.u32(state.mmu.ttb);
    writer.u32(state.mmu.mmucr);
    for (const auto& entry : state.mmu.utlb) {
        writer.u32(entry.pteh);
        writer.u32(entry.ptel);
        writer.u32(entry.ptea);
    }
    writer.boolean(state.exception.trap_pending);
    writer.enumeration(state.exception.last_cause);
    writer.boolean(state.exception.in_delay_slot);
    writer.u32(state.exception.last_instruction_pc);
    writer.u32(state.exception.last_instruction_physical_pc);
    writer.u32(state.exception.last_owner_pc);
    writer.boolean(state.exception.sleeping);
    return prepared_identity(
        "katana.complete-platform.cpu",
        std::move(writer).finish());
}

std::string prepared_memory_identity(
    const std::span<const ValidatedGameEntryMemoryOperation> operations) {
    PlatformStateWriter writer;
    writer.magic("KATMEM1\n");
    writer.u32(device_payload_contract);
    writer.u64(static_cast<std::uint64_t>(operations.size()));
    for (const auto& staged : operations) {
        writer.enumeration(staged.operation.region);
        writer.u32(staged.operation.offset);
        writer.u32(staged.operation.size);
        writer.enumeration(staged.operation.kind);
        writer.u8(staged.operation.fill_value);
        writer.boolean(staged.operation.executable);
        writer.bytes(staged.bytes);
    }
    return prepared_identity(
        "katana.complete-platform.memory-writes",
        std::move(writer).finish());
}

std::string prepared_scheduler_identity(
    const GameEntrySchedulerState& state) {
    PlatformStateWriter writer;
    writer.magic("KATSCH1\n");
    writer.u32(device_payload_contract);
    writer.u64(state.current_cycle);
    writer.u64(static_cast<std::uint64_t>(
        state.pending_events.size()));
    for (const auto& event : state.pending_events) {
        writer.u64(event.guest_cycle);
        writer.enumeration(event.kind);
        writer.enumeration(event.owner.kind);
        writer.u32(event.owner.instance);
        writer.u32(event.channel);
        writer.u64(event.token);
    }
    return prepared_identity(
        "katana.complete-platform.scheduler",
        std::move(writer).finish());
}

std::string prepared_irq_identity(
    const InterruptControllerSnapshot& controller,
    const PlatformInterruptRouterSnapshot& router,
    const Sh4InterruptRegistersSnapshot& registers) {
    PlatformStateWriter writer;
    writer.magic("KATIRQSET1\n");
    writer.u32(device_payload_contract);
    writer.bytes(encode_interrupt_controller(controller));
    writer.bytes(encode_interrupt_router(router));
    writer.bytes(encode_interrupt_registers(registers));
    return prepared_identity(
        "katana.complete-platform.irq",
        std::move(writer).finish());
}

DreamcastMapleStateSnapshot expected_maple_state_after_restore(
    const MapleBus& bus,
    const DreamcastMapleController& controller,
    const DreamcastMapleStateSnapshot& captured,
    const PersistenceHandoffPolicy policy) {
    if (policy == PersistenceHandoffPolicy::DiagnosticLossless)
        return captured;
    if (policy != PersistenceHandoffPolicy::ProductPreserveTarget)
        throw std::invalid_argument(
            "Unbekannte Persistenz-Handoff-Policy.");

    auto expected =
        snapshot_dreamcast_maple_state(bus, controller);
    expected.controller = captured.controller;
    for (const auto& source : captured.bus.peripherals) {
        const auto target = std::find_if(
            expected.bus.peripherals.begin(),
            expected.bus.peripherals.end(),
            [&](const auto& candidate) {
                return candidate.port == source.port &&
                       candidate.unit == source.unit;
            });
        if (target == expected.bus.peripherals.end())
            throw std::invalid_argument(
                "Maple-Handoff passt nicht zur Zieltopologie.");
        if (const auto* controller_state =
                std::get_if<MapleControllerDeviceStateSnapshot>(
                    &source.state)) {
            if (!std::holds_alternative<
                    MapleControllerDeviceStateSnapshot>(
                    target->state))
                throw std::invalid_argument(
                    "Maple-Handoff-Peripherietyp stimmt nicht ueberein.");
            target->state = *controller_state;
        } else if (!std::holds_alternative<MapleVmuStateSnapshot>(
                       target->state)) {
            throw std::invalid_argument(
                "Maple-Handoff-Peripherietyp stimmt nicht ueberein.");
        }
    }
    return expected;
}

template <typename Operation>
void for_each_complete_platform_executable_backing_extent(
    const std::uint32_t address,
    const std::size_t size,
    Operation&& operation) {
    constexpr std::uint32_t area3_begin = 0x0C000000u;
    constexpr std::uint32_t area3_end = 0x10000000u;
    constexpr std::uint32_t backing_mask =
        static_cast<std::uint32_t>(
            dreamcast_main_ram_size - 1u);
    const auto physical = canonical_physical_address(address);
    if (size != 0u && physical >= area3_begin &&
        physical < area3_end &&
        size <= static_cast<std::uint64_t>(area3_end) -
                    physical) {
        std::size_t source_offset = 0u;
        auto cursor = physical;
        while (source_offset < size) {
            const auto backing_offset =
                (cursor - area3_begin) & backing_mask;
            const auto extent_size = std::min<std::size_t>(
                size - source_offset,
                dreamcast_main_ram_size - backing_offset);
            operation(
                area3_begin + backing_offset,
                source_offset,
                extent_size,
                true);
            source_offset += extent_size;
            cursor += static_cast<std::uint32_t>(
                extent_size);
        }
        return;
    }
    operation(physical, 0u, size, false);
}

struct PreparedCompletePlatformGuestWriteEffects final {
    ExecutableModuleCatalog* modules = nullptr;
    ExecutableCodeTracker* tracker = nullptr;
    RuntimeBlockTable* blocks = nullptr;
    std::optional<
        ExecutableModuleCatalog::PreparedDiscLoadCatalog>
        module_plan;
    std::vector<ExecutableCodeTracker::PreparedDiscLoadWrite>
        tracker_plans;
    std::vector<RuntimeBlockTable::PreparedDiscLoadInvalidation>
        block_plans;
    bool committed = false;

    PreparedCompletePlatformGuestWriteEffects() = default;
    PreparedCompletePlatformGuestWriteEffects(
        const PreparedCompletePlatformGuestWriteEffects&) = delete;
    PreparedCompletePlatformGuestWriteEffects& operator=(
        const PreparedCompletePlatformGuestWriteEffects&) = delete;
    PreparedCompletePlatformGuestWriteEffects(
        PreparedCompletePlatformGuestWriteEffects&&) = delete;
    PreparedCompletePlatformGuestWriteEffects& operator=(
        PreparedCompletePlatformGuestWriteEffects&&) = delete;

    ~PreparedCompletePlatformGuestWriteEffects() {
        if (committed) return;
        if (tracker != nullptr)
            for (auto& plan : tracker_plans)
                tracker->cancel_disc_load_write(plan);
        if (modules != nullptr && module_plan)
            modules->cancel_disc_load_catalog(*module_plan);
    }

    void commit() noexcept {
        if (committed) std::terminate();
        if (module_plan) {
            if (modules == nullptr) std::terminate();
            modules->commit_disc_load_catalog(
                std::move(*module_plan));
        }
        if (!tracker_plans.empty() && tracker == nullptr)
            std::terminate();
        for (auto& plan : tracker_plans)
            tracker->commit_disc_load_write(std::move(plan));
        if (!block_plans.empty() && blocks == nullptr)
            std::terminate();
        for (auto& plan : block_plans)
            static_cast<void>(
                blocks->commit_disc_load_invalidation(
                    std::move(plan)));
        committed = true;
    }
};

std::unique_ptr<PreparedCompletePlatformGuestWriteEffects>
prepare_complete_platform_guest_write_effects(
    DreamcastRuntimeState& runtime,
    const std::span<const GuestWriteEvent> events) {
    if (runtime.disc_load_transactions &&
        runtime.disc_load_transactions->transaction_active())
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::RuntimeStateInvalid);

    auto prepared = std::make_unique<
        PreparedCompletePlatformGuestWriteEffects>();
    prepared->modules = runtime.module_catalog.get();
    prepared->tracker = runtime.code_tracker.get();
    prepared->blocks = runtime.runtime_blocks.get();
    prepared->tracker_plans.reserve(events.size() * 2u);
    prepared->block_plans.reserve(events.size() * 2u);
    std::vector<std::pair<std::uint32_t, std::size_t>>
        module_invalidations;
    module_invalidations.reserve(events.size() * 2u);

    for (const auto& event : events) {
        if (event.source != CodeWriteSource::Copy)
            throw GameEntryHandoffError(
                GameEntryHandoffFailure::
                    MemoryOperationInvalid);
        for_each_complete_platform_executable_backing_extent(
            event.address,
            event.size,
            [&](const std::uint32_t backing,
                const std::size_t,
                const std::size_t extent_size,
                const bool main_ram) {
                if (main_ram && event.bytes_changed &&
                    prepared->modules != nullptr)
                    module_invalidations.emplace_back(
                        backing, extent_size);
                if (prepared->tracker == nullptr ||
                    (!main_ram &&
                     !prepared->tracker->tracks_address(
                         backing, extent_size)))
                    return;
                prepared->tracker_plans.push_back(
                    prepared->tracker->
                        prepare_disc_load_write(
                            backing,
                            extent_size,
                            event.source,
                            event.bytes_changed));
                if (event.bytes_changed &&
                    prepared->blocks != nullptr)
                    prepared->block_plans.push_back(
                        prepared->blocks->
                            prepare_disc_load_invalidation(
                                backing, extent_size));
            });
    }
    if (!module_invalidations.empty())
        prepared->module_plan.emplace(
            prepared->modules->prepare_disc_load_catalog(
                {}, module_invalidations));
    return prepared;
}

void validate_core_runtime_bindings(
    const DreamcastRuntimeState& runtime) {
    if (!runtime.pvr_registers || !runtime.pvr_ta_fifo ||
        !runtime.pvr_ta_aperture || !runtime.pvr_yuv_converter ||
        !runtime.pvr_renderer || !runtime.gdrom ||
        !runtime.holly_dma.g1 || !runtime.dmac ||
        !runtime.aica_registers || !runtime.aica_rtc || !runtime.aica ||
        !runtime.maple || !runtime.maple_controller ||
        !runtime.system_bus_control || !runtime.system_asic ||
        !runtime.interrupt_controller || !runtime.interrupt_router ||
        !runtime.interrupt_registers || !runtime.address_space ||
        !runtime.cache_control || !runtime.store_queues ||
        !runtime.io_ports || !runtime.holly_dma.g2 ||
        !runtime.holly_dma.pvr || !runtime.tmu || !runtime.rtc_clock ||
        !runtime.rtc || !runtime.scif || !runtime.flash ||
        !runtime.scheduler)
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::RuntimeStateInvalid);
}

} // namespace

CapturedGameEntryPlatformState
capture_complete_game_entry_platform_state(
    const DreamcastRuntimeState& runtime) {
    validate_core_runtime_bindings(runtime);

    const auto scheduler = runtime.scheduler->snapshot();
    if (scheduler.advance_in_progress)
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::RuntimeStateInvalid);

    const auto pvr_events = runtime.pvr_registers->snapshot();
    const auto pvr = snapshot_dreamcast_pvr_state(
        *runtime.pvr_registers,
        *runtime.pvr_ta_fifo,
        *runtime.pvr_ta_aperture,
        *runtime.pvr_yuv_converter,
        *runtime.pvr_renderer);
    const auto gdrom = runtime.gdrom->snapshot();
    const auto g1 = runtime.holly_dma.g1->snapshot();
    const auto dmac = runtime.dmac->snapshot();
    const auto aica_events = runtime.aica->snapshot();
    const auto aica = snapshot_dreamcast_aica_state(
        *runtime.aica_registers, *runtime.aica_rtc, *runtime.aica);
    const auto maple = snapshot_dreamcast_maple_state(
        *runtime.maple, *runtime.maple_controller);
    const auto system_bus = runtime.system_bus_control->snapshot();
    const auto system_asic = runtime.system_asic->snapshot();
    const auto interrupt_router = runtime.interrupt_router->snapshot();
    const auto interrupt_registers =
        runtime.interrupt_registers->snapshot();
    const auto mmu = runtime.address_space->snapshot();
    const auto cache = runtime.cache_control->snapshot();
    const auto store_queues = runtime.store_queues->snapshot();
    const auto io_ports = runtime.io_ports->snapshot();
    const auto g2 = runtime.holly_dma.g2->snapshot();
    const auto pvr_dma = runtime.holly_dma.pvr->snapshot();
    const auto tmu = runtime.tmu->snapshot();
    const auto rtc_clock = runtime.rtc_clock->snapshot();
    const auto rtc = runtime.rtc->snapshot();
    const auto scif = runtime.scif->snapshot();
    const auto flash = runtime.flash->snapshot();
    // InterruptController is a lazy routing cache in the product hot path.
    // The portable contract records the deterministic architectural pending
    // set implied by the captured devices/router instead of serializing a
    // potentially stale host cache.
    const auto interrupt_controller =
        expected_interrupt_controller_snapshot(
            interrupt_router, tmu, rtc, dmac);

    struct StableEvent {
        SchedulerEventKind kind = SchedulerEventKind::Unknown;
        GameEntryDeviceKey owner;
        std::uint32_t channel = 0u;
        std::uint64_t token = 0u;
    };
    std::map<SchedulerEventId, StableEvent> stable_events;
    const auto register_event =
        [&](const SchedulerEventId event_id,
            const SchedulerEventKind kind,
            const GameEntryDeviceKind owner,
            const std::uint32_t channel,
            const std::uint64_t token) {
            if (event_id == 0u ||
                !stable_events
                     .emplace(
                         event_id,
                         StableEvent{
                             kind, {owner, 0u}, channel, token})
                     .second)
                throw GameEntryHandoffError(
                    GameEntryHandoffFailure::SchedulerStateInvalid);
        };
    const auto register_optional =
        [&](const std::optional<SchedulerEventId>& event_id,
            const SchedulerEventKind kind,
            const GameEntryDeviceKind owner,
            const std::uint32_t channel,
            const std::uint64_t token) {
            if (event_id)
                register_event(
                    *event_id, kind, owner, channel, token);
        };

    for (const auto& pending : gdrom.reader.pending)
        register_event(
            pending.event_id,
            SchedulerEventKind::DiscRead,
            GameEntryDeviceKind::GdRom,
            dreamcast_gdrom_async_read_event_channel,
            pending.request_id);
    register_optional(
        gdrom.packet_event,
        SchedulerEventKind::GdRomPacket,
        GameEntryDeviceKind::GdRom,
        dreamcast_gdrom_packet_event_channel,
        dreamcast_gdrom_packet_event_token_v1);
    if (dmac.event_id) {
        if (!dmac.scheduled_channel ||
            *dmac.scheduled_channel >= Sh4Dmac::channel_count)
            throw GameEntryHandoffError(
                GameEntryHandoffFailure::SchedulerStateInvalid);
        register_event(
            *dmac.event_id,
            SchedulerEventKind::Sh4Dmac,
            GameEntryDeviceKind::Sh4Dmac,
            static_cast<std::uint32_t>(*dmac.scheduled_channel),
            sh4_dmac_event_token_v1);
    }
    for (std::size_t channel = 0u; channel < g2.channels.size(); ++channel)
        register_optional(
            g2.channels[channel].completion_event,
            SchedulerEventKind::HollyG2Dma,
            GameEntryDeviceKind::HollyG2Dma,
            static_cast<std::uint32_t>(channel),
            dreamcast_holly_dma_event_token_v1);
    register_optional(
        g1.channel.completion_event,
        SchedulerEventKind::HollyG1Dma,
        GameEntryDeviceKind::G1,
        0u,
        dreamcast_holly_dma_event_token_v1);
    register_optional(
        pvr_dma.channel.completion_event,
        SchedulerEventKind::HollyPvrDma,
        GameEntryDeviceKind::HollyPvrDma,
        0u,
        dreamcast_holly_dma_event_token_v1);
    register_optional(
        maple.controller.completion_event,
        SchedulerEventKind::MapleDma,
        GameEntryDeviceKind::Maple,
        dreamcast_maple_dma_event_channel,
        dreamcast_maple_dma_event_token_v1);
    if (!pvr_events.render_event_ids.empty())
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::DeviceStateInvalid);
    register_optional(
        pvr_events.vblank_in_event,
        SchedulerEventKind::PvrVblankIn,
        GameEntryDeviceKind::Pvr,
        dreamcast_pvr_vblank_in_event_channel,
        dreamcast_pvr_scan_event_token_v1);
    register_optional(
        pvr_events.vblank_out_event,
        SchedulerEventKind::PvrVblankOut,
        GameEntryDeviceKind::Pvr,
        dreamcast_pvr_vblank_out_event_channel,
        dreamcast_pvr_scan_event_token_v1);
    register_optional(
        pvr_events.hblank_event,
        SchedulerEventKind::PvrHblank,
        GameEntryDeviceKind::Pvr,
        dreamcast_pvr_hblank_event_channel,
        dreamcast_pvr_scan_event_token_v1);
    register_optional(
        aica_events.tick_event,
        SchedulerEventKind::AicaTick,
        GameEntryDeviceKind::Aica,
        dreamcast_aica_tick_event_channel,
        dreamcast_aica_tick_event_token_v1);
    for (const auto& pending : system_asic.scheduled_events)
        register_optional(
            pending.event_id,
            SchedulerEventKind::SystemAsic,
            GameEntryDeviceKind::SystemAsic,
            dreamcast_system_asic_event_channel,
            static_cast<std::uint64_t>(pending.event));
    for (std::size_t channel = 0u; channel < tmu.channels.size(); ++channel)
        register_optional(
            tmu.channels[channel].event,
            static_cast<SchedulerEventKind>(
                static_cast<std::uint32_t>(SchedulerEventKind::Sh4Tmu0) +
                static_cast<std::uint32_t>(channel)),
            GameEntryDeviceKind::Sh4Tmu,
            static_cast<std::uint32_t>(channel),
            sh4_tmu_event_token_v1);
    register_optional(
        rtc.event,
        SchedulerEventKind::Sh4Rtc,
        GameEntryDeviceKind::Sh4Rtc,
        sh4_rtc_event_channel,
        sh4_rtc_event_token_v1);
    register_optional(
        scif.transmit_event,
        SchedulerEventKind::ScifTransmit,
        GameEntryDeviceKind::Sh4Scif,
        sh4_scif_transmit_event_channel,
        sh4_scif_transmit_event_token_v1);

    CapturedGameEntryPlatformState capture;
    capture.scheduler.current_cycle = scheduler.current_cycle;
    capture.scheduler.pending_events.reserve(
        scheduler.pending_events.size());
    for (const auto& pending : scheduler.pending_events) {
        // The host media clock is constructed after a product handoff is
        // applied. Its video/audio callbacks are host-lifecycle state, not
        // portable Dreamcast device events, and are therefore recreated by
        // the target session instead of serialized here.
        if (pending.kind == SchedulerEventKind::MediaVideo ||
            pending.kind == SchedulerEventKind::MediaAudio)
            continue;
        const auto found = stable_events.find(pending.event_id);
        if (found == stable_events.end() ||
            found->second.kind != pending.kind ||
            pending.guest_cycle < scheduler.current_cycle)
            throw GameEntryHandoffError(
                GameEntryHandoffFailure::SchedulerStateInvalid);
        capture.scheduler.pending_events.push_back(
            {pending.guest_cycle,
             pending.kind,
             found->second.owner,
             found->second.channel,
             found->second.token});
        stable_events.erase(found);
    }
    if (!stable_events.empty())
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::SchedulerStateInvalid);
    std::sort(
        capture.scheduler.pending_events.begin(),
        capture.scheduler.pending_events.end(),
        [](const auto& left, const auto& right) {
            return std::tuple{
                       left.guest_cycle,
                       static_cast<std::uint32_t>(left.kind),
                       static_cast<std::uint16_t>(left.owner.kind),
                       left.owner.instance,
                       left.channel,
                       left.token} <
                   std::tuple{
                       right.guest_cycle,
                       static_cast<std::uint32_t>(right.kind),
                       static_cast<std::uint16_t>(right.owner.kind),
                       right.owner.instance,
                       right.channel,
                       right.token};
        });
    for (std::size_t index = 1u;
         index < capture.scheduler.pending_events.size();
         ++index) {
        if (capture.scheduler.pending_events[index - 1u] ==
            capture.scheduler.pending_events[index])
            throw GameEntryHandoffError(
                GameEntryHandoffFailure::SchedulerStateInvalid);
    }

    capture.devices.reserve(
        dreamcast_game_entry_required_devices_v2.size());
    capture.payloads.reserve(
        dreamcast_game_entry_required_devices_v2.size());
    add_device_payload(
        capture,
        GameEntryDeviceKind::Pvr,
        "pvr-v1",
        encode_dreamcast_pvr_state(pvr));
    add_device_payload(
        capture,
        GameEntryDeviceKind::GdRom,
        "gdrom-v1",
        encode_dreamcast_gdrom_state(gdrom));
    add_device_payload(
        capture,
        GameEntryDeviceKind::G1,
        "g1-v1",
        encode_dreamcast_g1_dma_state(g1));
    add_device_payload(
        capture,
        GameEntryDeviceKind::Sh4Dmac,
        "sh4-dmac-v1",
        encode_sh4_dmac_state(dmac));
    add_device_payload(
        capture,
        GameEntryDeviceKind::Aica,
        "aica-v1",
        encode_dreamcast_aica_state(aica));
    add_device_payload(
        capture,
        GameEntryDeviceKind::Maple,
        "maple-v1",
        encode_dreamcast_maple_state(maple));
    add_device_payload(
        capture,
        GameEntryDeviceKind::SystemBus,
        "system-bus-v1",
        encode_dreamcast_system_bus_state(system_bus));
    add_device_payload(
        capture,
        GameEntryDeviceKind::SystemAsic,
        "system-asic-v1",
        encode_dreamcast_system_asic_state(system_asic));
    add_device_payload(
        capture,
        GameEntryDeviceKind::InterruptController,
        "interrupt-controller-v1",
        encode_interrupt_controller(interrupt_controller));
    add_device_payload(
        capture,
        GameEntryDeviceKind::InterruptRouter,
        "interrupt-router-v1",
        encode_interrupt_router(interrupt_router));
    add_device_payload(
        capture,
        GameEntryDeviceKind::InterruptRegisters,
        "interrupt-registers-v1",
        encode_interrupt_registers(interrupt_registers));
    add_device_payload(
        capture,
        GameEntryDeviceKind::Mmu,
        "mmu-v1",
        encode_mmu(mmu));
    add_device_payload(
        capture,
        GameEntryDeviceKind::Cache,
        "cache-v1",
        encode_cache(cache));
    add_device_payload(
        capture,
        GameEntryDeviceKind::StoreQueues,
        "store-queues-v1",
        encode_store_queues(store_queues));
    add_device_payload(
        capture,
        GameEntryDeviceKind::IoPorts,
        "io-ports-v1",
        encode_io_ports(io_ports));
    add_device_payload(
        capture,
        GameEntryDeviceKind::HollyG2Dma,
        "g2-dma-v1",
        encode_dreamcast_g2_dma_state(g2));
    add_device_payload(
        capture,
        GameEntryDeviceKind::HollyPvrDma,
        "pvr-dma-v1",
        encode_dreamcast_pvr_dma_state(pvr_dma));
    add_device_payload(
        capture,
        GameEntryDeviceKind::Sh4Tmu,
        "sh4-tmu-v1",
        encode_sh4_tmu_state(tmu));
    add_device_payload(
        capture,
        GameEntryDeviceKind::Sh4RtcClock,
        "sh4-rtc-clock-v1",
        encode_sh4_rtc_clock_state(rtc_clock));
    add_device_payload(
        capture,
        GameEntryDeviceKind::Sh4Rtc,
        "sh4-rtc-v1",
        encode_sh4_rtc_state(rtc));
    add_device_payload(
        capture,
        GameEntryDeviceKind::Sh4Scif,
        "sh4-scif-v1",
        encode_sh4_scif_state(scif));
    add_device_payload(
        capture,
        GameEntryDeviceKind::Flash,
        "flash-v1",
        encode_flash(flash));
    return capture;
}

struct PreparedCompletePlatformHandoff::Data {
    struct EventPublication {
        SchedulerEventKind kind = SchedulerEventKind::Unknown;
        std::uint64_t guest_cycle = 0u;
        std::uint32_t channel = 0u;
        std::uint64_t token = 0u;
        SchedulerEventId event_id = 0u;
    };

    CpuState* cpu_owner = nullptr;
    DreamcastRuntimeState* runtime_owner = nullptr;
    std::unique_ptr<PreparedGameEntryCpuMemoryHandoff> cpu_memory;
    std::unique_ptr<PreparedCompletePlatformGuestWriteEffects>
        guest_write_effects;
    std::unique_ptr<PreparedDreamcastPvrStateRestore> pvr;
    std::unique_ptr<DreamcastGdRomController::PreparedStateRestore> gdrom;
    DreamcastG1DmaSnapshot g1;
    Sh4DmacSnapshot dmac;
    std::unique_ptr<PreparedDreamcastAicaStateRestore> aica;
    std::unique_ptr<PreparedDreamcastMapleStateRestore> maple;
    DreamcastSystemBusSnapshot system_bus;
    std::unique_ptr<DreamcastSystemAsic::PreparedStateRestore> system_asic;
    InterruptControllerSnapshot interrupt_controller;
    PlatformInterruptRouterSnapshot interrupt_router;
    Sh4InterruptRegistersSnapshot interrupt_registers;
    Sh4CacheControlSnapshot cache;
    Sh4StoreQueueSnapshot store_queues;
    Sh4IoPortSnapshot io_ports;
    DreamcastG2DmaSnapshot g2;
    DreamcastPvrDmaSnapshot pvr_dma;
    Sh4TmuSnapshot tmu;
    Sh4RtcClockDomain::Snapshot rtc_clock;
    Sh4RtcSnapshot rtc;
    std::unique_ptr<PreparedSh4ScifStateRestore> scif;
    std::unique_ptr<PreparedFlashMemoryRestore> flash;
    std::unique_ptr<EventScheduler::PreparedPassiveRestore> scheduler;
    std::vector<EventPublication> event_publications;
    std::size_t device_count = 0u;
    GameEntryCompletePlatformExpectedIdentities expected_identities;
};

PreparedCompletePlatformHandoff::
    PreparedCompletePlatformHandoff() = default;

PreparedCompletePlatformHandoff::
    PreparedCompletePlatformHandoff(
        PreparedCompletePlatformHandoff&&) noexcept = default;

PreparedCompletePlatformHandoff&
PreparedCompletePlatformHandoff::operator=(
    PreparedCompletePlatformHandoff&&) noexcept = default;

PreparedCompletePlatformHandoff::
    ~PreparedCompletePlatformHandoff() = default;

const GameEntryCompletePlatformExpectedIdentities&
PreparedCompletePlatformHandoff::expected_identities() const noexcept {
    if (!data_) std::terminate();
    return data_->expected_identities;
}

PreparedCompletePlatformHandoff
prepare_validated_game_entry_complete_platform_handoff(
    CpuState& cpu,
    DreamcastRuntimeState& runtime,
    const ValidatedGameEntryHandoff& handoff,
    const GameEntryCompletePlatformRestoreProfile profile) {
    if (handoff.completeness() !=
            GameEntryHandoffCompleteness::CompletePlatform ||
        cpu.pending_guest_cycles != 0u)
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::CompletenessMismatch);
    validate_core_runtime_bindings(runtime);
    const auto live_scheduler = runtime.scheduler->snapshot();
    if (live_scheduler.advance_in_progress)
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::RuntimeStateInvalid);

    // Decode every owning payload before the first mutation and normalize
    // codec failures into the stable handoff error contract.
    const auto decode_device =
        [&](const GameEntryDeviceKind kind, auto&& decoder) {
            try {
                return decoder(device_bytes(handoff, kind));
            } catch (const GameEntryHandoffError&) {
                throw;
            } catch (...) {
                throw GameEntryHandoffError(
                    GameEntryHandoffFailure::DeviceStateInvalid);
            }
        };
    auto pvr = decode_device(
        GameEntryDeviceKind::Pvr,
        [](const auto bytes) {
            return decode_dreamcast_pvr_state(bytes);
        });
    auto gdrom = decode_device(
        GameEntryDeviceKind::GdRom,
        [](const auto bytes) {
            return decode_dreamcast_gdrom_state(bytes);
        });
    auto g1 = decode_device(
        GameEntryDeviceKind::G1,
        [](const auto bytes) {
            return decode_dreamcast_g1_dma_state(bytes);
        });
    auto dmac = decode_device(
        GameEntryDeviceKind::Sh4Dmac,
        [](const auto bytes) {
            return decode_sh4_dmac_state(bytes);
        });
    auto aica = decode_device(
        GameEntryDeviceKind::Aica,
        [](const auto bytes) {
            return decode_dreamcast_aica_state(bytes);
        });
    auto maple = decode_device(
        GameEntryDeviceKind::Maple,
        [](const auto bytes) {
            return decode_dreamcast_maple_state(bytes);
        });
    auto system_bus = decode_device(
        GameEntryDeviceKind::SystemBus,
        [](const auto bytes) {
            return decode_dreamcast_system_bus_state(bytes);
        });
    auto system_asic = decode_device(
        GameEntryDeviceKind::SystemAsic,
        [](const auto bytes) {
            return decode_dreamcast_system_asic_state(bytes);
        });
    auto interrupt_controller = decode_device(
        GameEntryDeviceKind::InterruptController,
        [](const auto bytes) {
            return decode_interrupt_controller(bytes);
        });
    auto interrupt_router = decode_device(
        GameEntryDeviceKind::InterruptRouter,
        [](const auto bytes) {
            return decode_interrupt_router(bytes);
        });
    auto interrupt_registers = decode_device(
        GameEntryDeviceKind::InterruptRegisters,
        [](const auto bytes) {
            return decode_interrupt_registers(bytes);
        });
    auto mmu = decode_device(
        GameEntryDeviceKind::Mmu,
        [](const auto bytes) {
            return decode_mmu(bytes);
        });
    auto cache = decode_device(
        GameEntryDeviceKind::Cache,
        [](const auto bytes) {
            return decode_cache(bytes);
        });
    auto store_queues = decode_device(
        GameEntryDeviceKind::StoreQueues,
        [](const auto bytes) {
            return decode_store_queues(bytes);
        });
    auto io_ports = decode_device(
        GameEntryDeviceKind::IoPorts,
        [](const auto bytes) {
            return decode_io_ports(bytes);
        });
    auto g2 = decode_device(
        GameEntryDeviceKind::HollyG2Dma,
        [](const auto bytes) {
            return decode_dreamcast_g2_dma_state(bytes);
        });
    auto pvr_dma = decode_device(
        GameEntryDeviceKind::HollyPvrDma,
        [](const auto bytes) {
            return decode_dreamcast_pvr_dma_state(bytes);
        });
    auto tmu = decode_device(
        GameEntryDeviceKind::Sh4Tmu,
        [](const auto bytes) {
            return decode_sh4_tmu_state(bytes);
        });
    auto rtc_clock = decode_device(
        GameEntryDeviceKind::Sh4RtcClock,
        [](const auto bytes) {
            return decode_sh4_rtc_clock_state(bytes);
        });
    auto rtc = decode_device(
        GameEntryDeviceKind::Sh4Rtc,
        [](const auto bytes) {
            return decode_sh4_rtc_state(bytes);
        });
    auto scif = decode_device(
        GameEntryDeviceKind::Sh4Scif,
        [](const auto bytes) {
            return decode_sh4_scif_state(bytes);
        });
    auto flash = decode_device(
        GameEntryDeviceKind::Flash,
        [](const auto bytes) {
            return decode_flash(bytes);
        });

    ObservationRestorePolicy observation_policy{};
    PersistenceHandoffPolicy persistence_policy{};
    switch (profile) {
    case GameEntryCompletePlatformRestoreProfile::DiagnosticLossless:
        observation_policy =
            ObservationRestorePolicy::PreserveCapturedDiagnostics;
        persistence_policy =
            PersistenceHandoffPolicy::DiagnosticLossless;
        break;
    case GameEntryCompletePlatformRestoreProfile::ProductHandoff:
        observation_policy =
            ObservationRestorePolicy::FreshProductEpoch;
        persistence_policy =
            PersistenceHandoffPolicy::ProductPreserveTarget;
        break;
    default:
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::CompletenessMismatch);
    }

    auto prepared_cpu_memory =
        prepare_validated_game_entry_cpu_memory_handoff(
            cpu, runtime, handoff);

    std::vector<std::uint8_t> final_vram;
    if (observation_policy ==
        ObservationRestorePolicy::FreshProductEpoch) {
        const auto current_vram = runtime.vram->bytes();
        final_vram.assign(
            current_vram.begin(), current_vram.end());
        for (const auto& staged : handoff.memory_operations()) {
            if (staged.operation.region !=
                GameEntryMemoryRegion::Vram)
                continue;
            const auto offset =
                static_cast<std::size_t>(
                    staged.operation.offset);
            if (offset > final_vram.size() ||
                staged.bytes.size() >
                    final_vram.size() - offset)
                throw GameEntryHandoffError(
                    GameEntryHandoffFailure::
                        MemoryOperationInvalid);
            std::copy(
                staged.bytes.begin(),
                staged.bytes.end(),
                final_vram.begin() +
                    static_cast<std::ptrdiff_t>(offset));
        }
    }
    try {
        normalize_dreamcast_pvr_observations_for_restore(
            pvr, observation_policy, final_vram);
        normalize_dreamcast_aica_observations_for_restore(
            aica, observation_policy);
    } catch (const GameEntryHandoffError&) {
        throw;
    } catch (...) {
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::DeviceStateInvalid);
    }

    const auto source_cycle = handoff.scheduler().current_cycle;
    if ((live_scheduler.guest_cycle_budget &&
         source_cycle > *live_scheduler.guest_cycle_budget) ||
        static_cast<std::uint64_t>(
            handoff.scheduler().pending_events.size()) >
            std::numeric_limits<SchedulerEventId>::max() -
                live_scheduler.next_event_id ||
        gdrom.reader.scheduler_cycle != source_cycle ||
        tmu.scheduler_cycle != source_cycle ||
        rtc.scheduler_cycle != source_cycle ||
        scif.scheduler_cycle != source_cycle ||
        rtc.clock.guest_cycles_per_second !=
            rtc_clock.guest_cycles_per_second ||
        rtc.clock.epoch_cycle != rtc_clock.epoch_cycle)
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::SchedulerStateInvalid);

    const auto expected_tmu0 = static_cast<std::uint8_t>(
        (interrupt_registers.priority_a >> 12u) & 0xFu);
    const auto expected_tmu1 = static_cast<std::uint8_t>(
        (interrupt_registers.priority_a >> 8u) & 0xFu);
    const auto expected_tmu2 = static_cast<std::uint8_t>(
        (interrupt_registers.priority_a >> 4u) & 0xFu);
    const auto expected_rtc = static_cast<std::uint8_t>(
        interrupt_registers.priority_a & 0xFu);
    const auto expected_dma = static_cast<std::uint8_t>(
        (interrupt_registers.priority_c >> 8u) & 0xFu);
    const auto expected_scif = static_cast<std::uint8_t>(
        (interrupt_registers.priority_c >> 4u) & 0xFu);
    if (interrupt_router.tmu_levels !=
            std::array<std::uint8_t, 3u>{
                expected_tmu0, expected_tmu1, expected_tmu2} ||
        interrupt_router.rtc_level != expected_rtc ||
        interrupt_router.dma_level != expected_dma ||
        interrupt_router.scif_level != expected_scif)
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::DeviceStateInvalid);
    if (dreamcast_system_asic_expected_external_lines(system_asic) !=
            interrupt_router.external_pending ||
        expected_interrupt_controller_snapshot(
            interrupt_router, tmu, rtc, dmac) !=
            interrupt_controller)
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::DeviceStateInvalid);
    validate_dreamcast_gdrom_g1_restore_contract(gdrom, g1);

    const auto& scheduled_events = handoff.scheduler().pending_events;
    const auto require_scheduled_event =
        [&](const SchedulerEventKind kind,
            const GameEntryDeviceKind owner,
            const std::uint32_t channel,
            const std::uint64_t token)
            -> const GameEntryScheduledEvent& {
            const auto found = std::find_if(
                scheduled_events.begin(),
                scheduled_events.end(),
                [&](const auto& event) {
                    return event.kind == kind &&
                           event.owner ==
                               GameEntryDeviceKey{owner, 0u} &&
                           event.channel == channel &&
                           event.token == token;
                });
            if (found == scheduled_events.end())
                throw GameEntryHandoffError(
                    GameEntryHandoffFailure::SchedulerStateInvalid);
            return *found;
        };

    // TMU/RTC/SCIF deadlines live exclusively in the typed scheduler
    // timeline. Their portable device codecs deliberately omit process-local
    // SchedulerEventIds and reconstruct the passive rehydration contract here.
    for (std::size_t channel = 0u; channel < tmu.channels.size(); ++channel) {
        auto& timer = tmu.channels[channel];
        if (!timer.running) continue;
        const auto& event = require_scheduled_event(
            static_cast<SchedulerEventKind>(
                static_cast<std::uint32_t>(SchedulerEventKind::Sh4Tmu0) +
                static_cast<std::uint32_t>(channel)),
            GameEntryDeviceKind::Sh4Tmu,
            static_cast<std::uint32_t>(channel),
            sh4_tmu_event_token_v1);
        timer.event.reset();
        timer.event_deadline = event.guest_cycle;
        timer.event_rehydration_pending = true;
    }
    if (rtc.rtc_enabled) {
        const auto& event = require_scheduled_event(
            SchedulerEventKind::Sh4Rtc,
            GameEntryDeviceKind::Sh4Rtc,
            sh4_rtc_event_channel,
            sh4_rtc_event_token_v1);
        rtc.event.reset();
        rtc.event_deadline = event.guest_cycle;
        rtc.event_rehydration_pending = true;
    }
    const bool scif_transmit_active =
        !scif.transmit_fifo.empty() && (scif.control & 0x20u) != 0u &&
        (scif.fifo_control & 0x04u) == 0u;
    if (scif_transmit_active) {
        const auto& event = require_scheduled_event(
            SchedulerEventKind::ScifTransmit,
            GameEntryDeviceKind::Sh4Scif,
            sh4_scif_transmit_event_channel,
            sh4_scif_transmit_event_token_v1);
        scif.transmit_event.reset();
        scif.transmit_event_deadline = event.guest_cycle;
        scif.transmit_event_rehydration_pending = true;
    }

    struct ExpectedStableEvent {
        SchedulerEventKind kind = SchedulerEventKind::Unknown;
        GameEntryDeviceKey owner;
        std::uint32_t channel = 0u;
        std::uint64_t token = 0u;
        std::optional<std::uint64_t> exact_cycle;
    };
    std::vector<ExpectedStableEvent> expected_events;
    const auto expect_event =
        [&](const SchedulerEventKind kind,
            const GameEntryDeviceKind owner,
            const std::uint32_t channel,
            const std::uint64_t token,
            const std::optional<std::uint64_t> exact_cycle =
                std::nullopt) {
            expected_events.push_back(
                {kind, {owner, 0u}, channel, token, exact_cycle});
        };
    for (const auto& pending : gdrom.reader.pending)
        expect_event(
            SchedulerEventKind::DiscRead,
            GameEntryDeviceKind::GdRom,
            dreamcast_gdrom_async_read_event_channel,
            pending.request_id,
            pending.ready_cycle);
    if (gdrom.packet_event_rehydration_pending)
        expect_event(
            SchedulerEventKind::GdRomPacket,
            GameEntryDeviceKind::GdRom,
            dreamcast_gdrom_packet_event_channel,
            dreamcast_gdrom_packet_event_token_v1);
    if (dmac.event_rehydration_pending) {
        if (!dmac.scheduled_channel)
            throw GameEntryHandoffError(
                GameEntryHandoffFailure::SchedulerStateInvalid);
        expect_event(
            SchedulerEventKind::Sh4Dmac,
            GameEntryDeviceKind::Sh4Dmac,
            static_cast<std::uint32_t>(*dmac.scheduled_channel),
            sh4_dmac_event_token_v1);
    }
    if (g1.channel.completion_event_rehydration_pending)
        expect_event(
            SchedulerEventKind::HollyG1Dma,
            GameEntryDeviceKind::G1,
            0u,
            dreamcast_holly_dma_event_token_v1,
            g1.channel.completion_cycle);
    for (std::size_t channel = 0u; channel < g2.channels.size(); ++channel) {
        if (g2.channels[channel].completion_event_rehydration_pending)
            expect_event(
                SchedulerEventKind::HollyG2Dma,
                GameEntryDeviceKind::HollyG2Dma,
                static_cast<std::uint32_t>(channel),
                dreamcast_holly_dma_event_token_v1,
                g2.channels[channel].completion_cycle);
    }
    if (pvr_dma.channel.completion_event_rehydration_pending)
        expect_event(
            SchedulerEventKind::HollyPvrDma,
            GameEntryDeviceKind::HollyPvrDma,
            0u,
            dreamcast_holly_dma_event_token_v1,
            pvr_dma.channel.completion_cycle);
    if (maple.controller.state == MapleDmaState::Active)
        expect_event(
            SchedulerEventKind::MapleDma,
            GameEntryDeviceKind::Maple,
            dreamcast_maple_dma_event_channel,
            dreamcast_maple_dma_event_token_v1);
    if (pvr.registers.vblank_in_event_rehydration_pending)
        expect_event(
            SchedulerEventKind::PvrVblankIn,
            GameEntryDeviceKind::Pvr,
            dreamcast_pvr_vblank_in_event_channel,
            dreamcast_pvr_scan_event_token_v1);
    if (pvr.registers.vblank_out_event_rehydration_pending)
        expect_event(
            SchedulerEventKind::PvrVblankOut,
            GameEntryDeviceKind::Pvr,
            dreamcast_pvr_vblank_out_event_channel,
            dreamcast_pvr_scan_event_token_v1);
    if (pvr.registers.hblank_event_rehydration_pending)
        expect_event(
            SchedulerEventKind::PvrHblank,
            GameEntryDeviceKind::Pvr,
            dreamcast_pvr_hblank_event_channel,
            dreamcast_pvr_scan_event_token_v1);
    if (aica.execution.tick_event_rehydration_pending)
        expect_event(
            SchedulerEventKind::AicaTick,
            GameEntryDeviceKind::Aica,
            dreamcast_aica_tick_event_channel,
            dreamcast_aica_tick_event_token_v1);
    for (const auto& pending : system_asic.scheduled_events)
        expect_event(
            SchedulerEventKind::SystemAsic,
            GameEntryDeviceKind::SystemAsic,
            dreamcast_system_asic_event_channel,
            static_cast<std::uint64_t>(pending.event),
            pending.guest_cycle);
    for (std::size_t channel = 0u; channel < tmu.channels.size(); ++channel) {
        if (tmu.channels[channel].running)
            expect_event(
                static_cast<SchedulerEventKind>(
                    static_cast<std::uint32_t>(
                        SchedulerEventKind::Sh4Tmu0) +
                    static_cast<std::uint32_t>(channel)),
                GameEntryDeviceKind::Sh4Tmu,
                static_cast<std::uint32_t>(channel),
                sh4_tmu_event_token_v1,
                tmu.channels[channel].event_deadline);
    }
    if (rtc.rtc_enabled)
        expect_event(
            SchedulerEventKind::Sh4Rtc,
            GameEntryDeviceKind::Sh4Rtc,
            sh4_rtc_event_channel,
            sh4_rtc_event_token_v1,
            rtc.event_deadline);
    if (scif_transmit_active)
        expect_event(
            SchedulerEventKind::ScifTransmit,
            GameEntryDeviceKind::Sh4Scif,
            sh4_scif_transmit_event_channel,
            sh4_scif_transmit_event_token_v1,
            scif.transmit_event_deadline);

    if (expected_events.size() != scheduled_events.size())
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::SchedulerStateInvalid);
    std::vector<bool> matched(scheduled_events.size(), false);
    for (const auto& expected : expected_events) {
        const auto found = std::find_if(
            scheduled_events.begin(),
            scheduled_events.end(),
            [&](const auto& candidate) {
                const auto index = static_cast<std::size_t>(
                    &candidate - scheduled_events.data());
                return !matched[index] &&
                       candidate.kind == expected.kind &&
                       candidate.owner == expected.owner &&
                       candidate.channel == expected.channel &&
                       candidate.token == expected.token &&
                       (!expected.exact_cycle ||
                        candidate.guest_cycle ==
                            *expected.exact_cycle);
            });
        if (found == scheduled_events.end())
            throw GameEntryHandoffError(
                GameEntryHandoffFailure::SchedulerStateInvalid);
        matched[static_cast<std::size_t>(
            &*found - scheduled_events.data())] = true;
    }

    // Topology and source identities can be validated before importing time.
    runtime.interrupt_controller->validate_state_restore(
        interrupt_controller);
    runtime.interrupt_router->validate_state_restore(interrupt_router);
    runtime.interrupt_registers->validate_state_restore(
        interrupt_registers);
    runtime.address_space->validate_state_restore(mmu);
    runtime.cache_control->validate_state_restore(cache);
    runtime.store_queues->validate_state_restore(store_queues);
    runtime.io_ports->validate_state_restore(io_ports);
    validate_dreamcast_pvr_state_restore(
        *runtime.pvr_registers,
        *runtime.pvr_ta_fifo,
        *runtime.pvr_ta_aperture,
        *runtime.pvr_yuv_converter,
        *runtime.pvr_renderer,
        pvr);
    runtime.gdrom->validate_state_restore(gdrom, source_cycle);
    runtime.holly_dma.g1->validate_state_restore(g1);
    runtime.dmac->validate_state_restore(dmac);
    validate_dreamcast_aica_state_restore(
        *runtime.aica_registers,
        *runtime.aica_rtc,
        *runtime.aica,
        aica,
        source_cycle);
    runtime.system_bus_control->validate_state_restore(system_bus);
    runtime.system_asic->validate_state_restore(system_asic);
    runtime.holly_dma.g2->validate_state_restore(g2);
    runtime.holly_dma.pvr->validate_state_restore(pvr_dma);
    validate_dreamcast_pvr_dma_dmac_restore_contract(pvr_dma, dmac);
    runtime.rtc_clock->validate_state_restore(rtc_clock);
    runtime.tmu->validate_state_restore(tmu, source_cycle);
    runtime.rtc->validate_state_restore(rtc, source_cycle);
    runtime.scif->validate_state_restore(scif, source_cycle);

    bind_prepared_game_entry_cpu_mmu(
        prepared_cpu_memory, std::move(mmu));

    GameEntryCompletePlatformExpectedIdentities expected_identities;
    expected_identities.cpu =
        prepared_cpu_identity(handoff.cpu());
    expected_identities.memory =
        prepared_memory_identity(handoff.memory_operations());
    expected_identities.pvr = prepared_identity(
        "katana.complete-platform.pvr",
        encode_dreamcast_pvr_state(pvr));
    expected_identities.aica = prepared_identity(
        "katana.complete-platform.aica",
        encode_dreamcast_aica_state(aica));
    const auto expected_maple =
        expected_maple_state_after_restore(
            *runtime.maple,
            *runtime.maple_controller,
            maple,
            persistence_policy);
    expected_identities.maple = prepared_identity(
        "katana.complete-platform.maple",
        encode_dreamcast_maple_state(expected_maple));
    expected_identities.scheduler =
        prepared_scheduler_identity(handoff.scheduler());
    expected_identities.irq = prepared_irq_identity(
        interrupt_controller,
        interrupt_router,
        interrupt_registers);

    const auto pvr_hblank_line =
        pvr.registers.hblank_event_line;
    std::vector<SchedulerPreparedEvent> prepared_events;
    prepared_events.reserve(scheduled_events.size());
    try {
        for (const auto& event : scheduled_events) {
            SchedulerCallback callback;
            switch (event.kind) {
            case SchedulerEventKind::DiscRead:
            case SchedulerEventKind::GdRomPacket:
                callback =
                    runtime.gdrom->
                        make_rehydrated_scheduled_event_callback(
                            event.channel, event.token);
                break;
            case SchedulerEventKind::Sh4Dmac:
                callback =
                    runtime.dmac->
                        make_rehydrated_scheduled_event_callback(
                            event.channel, event.token);
                break;
            case SchedulerEventKind::HollyG1Dma:
                callback =
                    runtime.holly_dma.g1->
                        make_rehydrated_scheduled_event_callback(
                            event.channel, event.token);
                break;
            case SchedulerEventKind::HollyG2Dma:
                callback =
                    runtime.holly_dma.g2->
                        make_rehydrated_scheduled_event_callback(
                            event.channel, event.token);
                break;
            case SchedulerEventKind::HollyPvrDma:
                callback =
                    runtime.holly_dma.pvr->
                        make_rehydrated_scheduled_event_callback(
                            event.channel, event.token);
                break;
            case SchedulerEventKind::MapleDma:
                callback =
                    runtime.maple_controller->
                        make_rehydrated_scheduled_event_callback(
                            event.channel, event.token);
                break;
            case SchedulerEventKind::PvrVblankIn:
            case SchedulerEventKind::PvrVblankOut:
            case SchedulerEventKind::PvrHblank:
                callback =
                    runtime.pvr_registers->
                        make_rehydrated_scheduled_event_callback(
                            event.channel,
                            event.token,
                            pvr_hblank_line);
                break;
            case SchedulerEventKind::ScifTransmit:
                callback =
                    runtime.scif->
                        make_rehydrated_scheduled_event_callback(
                            event.channel, event.token);
                break;
            case SchedulerEventKind::SystemAsic:
                callback =
                    runtime.system_asic->
                        make_rehydrated_scheduled_event_callback(
                            event.channel, event.token);
                break;
            case SchedulerEventKind::Sh4Rtc:
                callback =
                    runtime.rtc->
                        make_rehydrated_scheduled_event_callback(
                            event.channel, event.token);
                break;
            case SchedulerEventKind::Sh4Tmu0:
            case SchedulerEventKind::Sh4Tmu1:
            case SchedulerEventKind::Sh4Tmu2:
                callback =
                    runtime.tmu->
                        make_rehydrated_scheduled_event_callback(
                            event.channel, event.token);
                break;
            case SchedulerEventKind::AicaTick:
                callback =
                    runtime.aica->
                        make_rehydrated_scheduled_event_callback(
                            event.channel, event.token);
                break;
            case SchedulerEventKind::Unknown:
            case SchedulerEventKind::MediaVideo:
            case SchedulerEventKind::MediaAudio:
            case SchedulerEventKind::PvrRender:
                throw GameEntryHandoffError(
                    GameEntryHandoffFailure::SchedulerStateInvalid);
            }
            prepared_events.push_back(
                {event.guest_cycle,
                 std::move(callback),
                 event.kind});
        }
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const GameEntryHandoffError&) {
        throw;
    } catch (...) {
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::SchedulerStateInvalid);
    }

    EventScheduler::PreparedPassiveRestore scheduler_plan =
        [&]() {
            try {
                return runtime.scheduler->prepare_passive_restore(
                    source_cycle, prepared_events);
            } catch (const std::bad_alloc&) {
                throw;
            } catch (...) {
                throw GameEntryHandoffError(
                    GameEntryHandoffFailure::
                        SchedulerStateInvalid);
            }
        }();
    const auto assigned_event_ids =
        scheduler_plan.assigned_event_ids();
    if (assigned_event_ids.size() != scheduled_events.size())
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::SchedulerStateInvalid);
    std::vector<
        PreparedCompletePlatformHandoff::Data::EventPublication>
        event_publications;
    event_publications.reserve(scheduled_events.size());
    for (std::size_t index = 0u;
         index < scheduled_events.size();
         ++index) {
        const auto& event = scheduled_events[index];
        event_publications.push_back(
            {event.kind,
             event.guest_cycle,
             event.channel,
             event.token,
             assigned_event_ids[index]});
    }

    std::unique_ptr<PreparedDreamcastPvrStateRestore>
        pvr_plan;
    std::unique_ptr<
        DreamcastGdRomController::PreparedStateRestore>
        gdrom_plan;
    std::unique_ptr<PreparedDreamcastAicaStateRestore>
        aica_plan;
    std::unique_ptr<PreparedDreamcastMapleStateRestore>
        maple_plan;
    std::unique_ptr<DreamcastSystemAsic::PreparedStateRestore>
        system_asic_plan;
    std::unique_ptr<PreparedSh4ScifStateRestore>
        scif_plan;
    std::unique_ptr<PreparedFlashMemoryRestore>
        flash_plan;
    try {
        pvr_plan =
            std::make_unique<PreparedDreamcastPvrStateRestore>(
                prepare_dreamcast_pvr_state_restore(
                    *runtime.pvr_registers,
                    *runtime.pvr_ta_fifo,
                    *runtime.pvr_ta_aperture,
                    *runtime.pvr_yuv_converter,
                    *runtime.pvr_renderer,
                    std::move(pvr)));
        gdrom_plan = std::make_unique<
            DreamcastGdRomController::PreparedStateRestore>(
            runtime.gdrom->prepare_state_restore(gdrom));
        aica_plan =
            std::make_unique<PreparedDreamcastAicaStateRestore>(
                prepare_dreamcast_aica_state_restore(
                    *runtime.aica_registers,
                    *runtime.aica_rtc,
                    *runtime.aica,
                    std::move(aica),
                    source_cycle));
        maple_plan =
            std::make_unique<PreparedDreamcastMapleStateRestore>(
                prepare_dreamcast_maple_state_restore(
                    *runtime.maple,
                    *runtime.maple_controller,
                    maple,
                    persistence_policy));
        system_asic_plan = std::make_unique<
            DreamcastSystemAsic::PreparedStateRestore>(
            runtime.system_asic->prepare_state_restore(
                *runtime.scheduler,
                system_asic,
                source_cycle));
        scif_plan =
            std::make_unique<PreparedSh4ScifStateRestore>(
                runtime.scif->prepare_state_restore(scif));
        flash_plan =
            std::make_unique<PreparedFlashMemoryRestore>(
                runtime.flash->prepare_state_restore(
                    flash, persistence_policy));
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const GameEntryHandoffError&) {
        throw;
    } catch (...) {
        throw GameEntryHandoffError(
            GameEntryHandoffFailure::DeviceStateInvalid);
    }

    auto guest_write_effects =
        prepare_complete_platform_guest_write_effects(
            runtime,
            prepared_cpu_memory.memory_guest_write_events());
    prepared_cpu_memory.suppress_memory_guest_write_observer();

    auto data =
        std::make_unique<PreparedCompletePlatformHandoff::Data>();
    data->cpu_owner = &cpu;
    data->runtime_owner = &runtime;
    data->cpu_memory =
        std::make_unique<PreparedGameEntryCpuMemoryHandoff>(
            std::move(prepared_cpu_memory));
    data->guest_write_effects =
        std::move(guest_write_effects);
    data->pvr = std::move(pvr_plan);
    data->gdrom = std::move(gdrom_plan);
    data->g1 = std::move(g1);
    data->dmac = std::move(dmac);
    data->aica = std::move(aica_plan);
    data->maple = std::move(maple_plan);
    data->system_bus = std::move(system_bus);
    data->system_asic = std::move(system_asic_plan);
    data->interrupt_controller =
        std::move(interrupt_controller);
    data->interrupt_router = std::move(interrupt_router);
    data->interrupt_registers =
        std::move(interrupt_registers);
    data->cache = std::move(cache);
    data->store_queues = std::move(store_queues);
    data->io_ports = std::move(io_ports);
    data->g2 = std::move(g2);
    data->pvr_dma = std::move(pvr_dma);
    data->tmu = std::move(tmu);
    data->rtc_clock = std::move(rtc_clock);
    data->rtc = std::move(rtc);
    data->scif = std::move(scif_plan);
    data->flash = std::move(flash_plan);
    data->scheduler = std::make_unique<
        EventScheduler::PreparedPassiveRestore>(
        std::move(scheduler_plan));
    data->event_publications =
        std::move(event_publications);
    data->device_count = handoff.devices().size();
    data->expected_identities =
        std::move(expected_identities);

    PreparedCompletePlatformHandoff prepared;
    prepared.data_ = std::move(data);
    return prepared;
}

GameEntryCompletePlatformApplyResult
commit_prepared_game_entry_complete_platform_handoff(
    CpuState& cpu,
    DreamcastRuntimeState& runtime,
    PreparedCompletePlatformHandoff prepared) noexcept {
    if (!prepared.data_ ||
        prepared.data_->cpu_owner != &cpu ||
        prepared.data_->runtime_owner != &runtime ||
        !prepared.data_->cpu_memory ||
        !prepared.data_->guest_write_effects ||
        !prepared.data_->pvr ||
        !prepared.data_->gdrom ||
        !prepared.data_->aica ||
        !prepared.data_->maple ||
        !prepared.data_->system_asic ||
        !prepared.data_->scif ||
        !prepared.data_->flash ||
        !prepared.data_->scheduler)
        std::terminate();
    auto& data = *prepared.data_;
    const auto memory_operation_count =
        data.cpu_memory->memory_operation_count();
    const auto memory_byte_count =
        data.cpu_memory->memory_byte_count();
    const auto scheduler_event_count =
        data.event_publications.size();

    // Memory and all guest-visible device state are published before the CPU.
    // Every operation below is a prevalidated, allocation-free noexcept
    // commit. PR/PC are the final architectural fields published below.
    commit_prepared_game_entry_memory_handoff(
        cpu, *data.cpu_memory);
    data.guest_write_effects->commit();
    runtime.flash->commit_prepared_state_restore(
        std::move(*data.flash));
    runtime.cache_control->commit_validated_state_restore(
        std::move(data.cache));
    runtime.store_queues->commit_validated_state_restore(
        std::move(data.store_queues));
    runtime.io_ports->commit_validated_state_restore(
        std::move(data.io_ports));
    commit_dreamcast_pvr_state_restore(
        *runtime.pvr_registers,
        *runtime.pvr_ta_fifo,
        *runtime.pvr_ta_aperture,
        *runtime.pvr_yuv_converter,
        *runtime.pvr_renderer,
        std::move(*data.pvr));
    runtime.gdrom->commit_prepared_state_restore(
        std::move(*data.gdrom));
    runtime.holly_dma.g1->commit_validated_state_restore(
        std::move(data.g1));
    runtime.dmac->commit_validated_state_restore(
        std::move(data.dmac));
    commit_dreamcast_aica_state_restore(
        *runtime.aica_registers,
        *runtime.aica_rtc,
        *runtime.aica,
        std::move(*data.aica));
    commit_dreamcast_maple_state_restore(
        *runtime.maple,
        *runtime.maple_controller,
        std::move(*data.maple));
    runtime.system_bus_control->
        commit_validated_state_restore(
            std::move(data.system_bus));
    runtime.system_asic->commit_prepared_state_restore(
        std::move(*data.system_asic));
    runtime.holly_dma.g2->commit_validated_state_restore(
        std::move(data.g2));
    runtime.holly_dma.pvr->commit_validated_state_restore(
        std::move(data.pvr_dma));
    runtime.rtc_clock->commit_validated_state_restore(
        std::move(data.rtc_clock));
    runtime.tmu->commit_validated_state_restore(
        std::move(data.tmu));
    runtime.rtc->commit_validated_state_restore(
        std::move(data.rtc));
    runtime.scif->commit_prepared_state_restore(
        std::move(*data.scif));
    runtime.interrupt_registers->
        commit_validated_state_restore(
            std::move(data.interrupt_registers));
    runtime.interrupt_router->commit_validated_state_restore(
        std::move(data.interrupt_router));
    runtime.interrupt_controller->
        commit_validated_state_restore(
            std::move(data.interrupt_controller));

    runtime.scheduler->commit_prepared_passive_restore(
        std::move(*data.scheduler));
    for (const auto& publication : data.event_publications) {
        switch (publication.kind) {
        case SchedulerEventKind::DiscRead:
        case SchedulerEventKind::GdRomPacket:
            runtime.gdrom->commit_rehydrated_scheduled_event(
                publication.event_id,
                publication.channel,
                publication.token);
            break;
        case SchedulerEventKind::Sh4Dmac:
            runtime.dmac->commit_rehydrated_scheduled_event(
                publication.event_id,
                publication.channel,
                publication.token);
            break;
        case SchedulerEventKind::HollyG1Dma:
            runtime.holly_dma.g1->
                commit_rehydrated_scheduled_event(
                    publication.event_id,
                    publication.channel,
                    publication.token);
            break;
        case SchedulerEventKind::HollyG2Dma:
            runtime.holly_dma.g2->
                commit_rehydrated_scheduled_event(
                    publication.event_id,
                    publication.channel,
                    publication.token);
            break;
        case SchedulerEventKind::HollyPvrDma:
            runtime.holly_dma.pvr->
                commit_rehydrated_scheduled_event(
                    publication.event_id,
                    publication.channel,
                    publication.token);
            break;
        case SchedulerEventKind::MapleDma:
            runtime.maple_controller->
                commit_rehydrated_scheduled_event(
                    publication.event_id,
                    publication.channel,
                    publication.token);
            break;
        case SchedulerEventKind::PvrVblankIn:
        case SchedulerEventKind::PvrVblankOut:
        case SchedulerEventKind::PvrHblank:
            runtime.pvr_registers->
                commit_rehydrated_scheduled_event(
                    publication.event_id,
                    publication.channel,
                    publication.token);
            break;
        case SchedulerEventKind::ScifTransmit:
            runtime.scif->commit_rehydrated_scheduled_event(
                publication.event_id,
                publication.channel,
                publication.token);
            break;
        case SchedulerEventKind::SystemAsic:
            runtime.system_asic->
                commit_rehydrated_scheduled_event(
                    *runtime.scheduler,
                    publication.event_id,
                    publication.guest_cycle,
                    publication.channel,
                    publication.token);
            break;
        case SchedulerEventKind::Sh4Rtc:
            runtime.rtc->commit_rehydrated_scheduled_event(
                publication.event_id,
                publication.channel,
                publication.token);
            break;
        case SchedulerEventKind::Sh4Tmu0:
        case SchedulerEventKind::Sh4Tmu1:
        case SchedulerEventKind::Sh4Tmu2:
            runtime.tmu->commit_rehydrated_scheduled_event(
                publication.event_id,
                publication.channel,
                publication.token);
            break;
        case SchedulerEventKind::AicaTick:
            runtime.aica->commit_rehydrated_scheduled_event(
                publication.event_id,
                publication.channel,
                publication.token);
            break;
        case SchedulerEventKind::Unknown:
        case SchedulerEventKind::MediaVideo:
        case SchedulerEventKind::MediaAudio:
        case SchedulerEventKind::PvrRender:
            std::terminate();
        }
    }

    commit_prepared_game_entry_cpu_handoff(
        cpu, std::move(*data.cpu_memory));
    return {
        memory_operation_count,
        memory_byte_count,
        data.device_count,
        scheduler_event_count,
        std::move(data.expected_identities)};
}

GameEntryCompletePlatformApplyResult
apply_validated_game_entry_complete_platform_handoff(
    CpuState& cpu,
    DreamcastRuntimeState& runtime,
    const ValidatedGameEntryHandoff& handoff,
    const GameEntryCompletePlatformRestoreProfile profile) {
    auto prepared =
        prepare_validated_game_entry_complete_platform_handoff(
            cpu, runtime, handoff, profile);
    return commit_prepared_game_entry_complete_platform_handoff(
        cpu, runtime, std::move(prepared));
}

} // namespace katana::runtime
