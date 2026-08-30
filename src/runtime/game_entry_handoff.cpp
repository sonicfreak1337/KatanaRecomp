#include "katana/runtime/game_entry_handoff.hpp"

#include "katana/io/input_provenance.hpp"
#include "katana/runtime/dreamcast_boot.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace katana::runtime {
namespace {

[[noreturn]] void fail(const GameEntryHandoffFailure failure) {
    throw GameEntryHandoffError(failure);
}

bool lower_hex(const std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

bool valid_boot_file_name(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 255u || value == "." || value == "..")
        return false;
    return std::all_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x21u && byte <= 0x7Eu && character != '/' &&
               character != '\\';
    });
}

bool valid_console_profile(const DreamcastConsoleProfile profile) noexcept {
    switch (profile) {
    case DreamcastConsoleProfile::JapanNtsc:
    case DreamcastConsoleProfile::NorthAmericaNtsc:
    case DreamcastConsoleProfile::EuropePal:
    case DreamcastConsoleProfile::Vga:
        return true;
    }
    return false;
}

bool valid_transfer_kind(const GameEntryTransferKind kind) noexcept {
    switch (kind) {
    case GameEntryTransferKind::JumpPreservingPr:
    case GameEntryTransferKind::CallWithReturnPr:
        return true;
    }
    return false;
}

bool valid_completeness(
    const GameEntryHandoffCompleteness completeness) noexcept {
    switch (completeness) {
    case GameEntryHandoffCompleteness::CompletePlatform:
    case GameEntryHandoffCompleteness::CpuMemoryDiagnostic:
        return true;
    }
    return false;
}

bool valid_exception_cause(const ExceptionCause cause) noexcept {
    switch (cause) {
    case ExceptionCause::None:
    case ExceptionCause::Trap:
    case ExceptionCause::IllegalInstruction:
    case ExceptionCause::SlotIllegalInstruction:
    case ExceptionCause::FpuDisabled:
    case ExceptionCause::SlotFpuDisabled:
    case ExceptionCause::AddressErrorRead:
    case ExceptionCause::AddressErrorWrite:
    case ExceptionCause::TlbMissRead:
    case ExceptionCause::TlbMissWrite:
    case ExceptionCause::InitialPageWrite:
    case ExceptionCause::TlbProtectionRead:
    case ExceptionCause::TlbProtectionWrite:
    case ExceptionCause::TlbMultipleHit:
    case ExceptionCause::BusErrorRead:
    case ExceptionCause::BusErrorWrite:
    case ExceptionCause::Interrupt:
    case ExceptionCause::FpuException:
        return true;
    }
    return false;
}

constexpr std::uint32_t game_entry_mmucr_stored_mask = 0xFCFCFF01u;
constexpr std::uint32_t game_entry_ptel_stored_mask = 0x1FFFFDFFu;
constexpr std::uint32_t game_entry_ptea_stored_mask = 0x0000000Fu;
constexpr std::uint32_t game_entry_mmucr_at_mask = 0x00000001u;

bool zero_or_halfword_aligned(const std::uint32_t value) noexcept {
    return value == 0u || (value & 1u) == 0u;
}

void validate_cpu_state_contract(const GameEntryCpuState& state) {
    if (state.pc == 0u || (state.pc & 1u) != 0u ||
        (state.pr & 1u) != 0u ||
        (state.sr & ~sr_writable_mask) != 0u ||
        (state.fpscr & ~fpscr_writable_mask) != 0u ||
        (state.mmu.ptel & ~game_entry_ptel_stored_mask) != 0u ||
        (state.mmu.ptea & ~game_entry_ptea_stored_mask) != 0u ||
        (state.mmu.mmucr & ~game_entry_mmucr_stored_mask) != 0u ||
        !valid_exception_cause(state.exception.last_cause) ||
        !zero_or_halfword_aligned(
            state.exception.last_instruction_pc) ||
        !zero_or_halfword_aligned(
            state.exception.last_instruction_physical_pc) ||
        !zero_or_halfword_aligned(state.exception.last_owner_pc))
        fail(GameEntryHandoffFailure::CpuStateInvalid);

    for (const auto& entry : state.mmu.utlb) {
        if ((entry.ptel & ~game_entry_ptel_stored_mask) != 0u ||
            (entry.ptea & ~game_entry_ptea_stored_mask) != 0u)
            fail(GameEntryHandoffFailure::CpuStateInvalid);
    }
}

bool valid_memory_region(const GameEntryMemoryRegion region) noexcept {
    switch (region) {
    case GameEntryMemoryRegion::MainRam:
    case GameEntryMemoryRegion::Vram:
    case GameEntryMemoryRegion::AicaRam:
        return true;
    }
    return false;
}

bool valid_memory_operation_kind(
    const GameEntryMemoryOperationKind kind) noexcept {
    switch (kind) {
    case GameEntryMemoryOperationKind::Fill:
    case GameEntryMemoryOperationKind::CopyPrivateSlice:
        return true;
    }
    return false;
}

bool valid_device_kind(const GameEntryDeviceKind kind) noexcept {
    switch (kind) {
    case GameEntryDeviceKind::Pvr:
    case GameEntryDeviceKind::GdRom:
    case GameEntryDeviceKind::G1:
    case GameEntryDeviceKind::Sh4Dmac:
    case GameEntryDeviceKind::Aica:
    case GameEntryDeviceKind::Maple:
    case GameEntryDeviceKind::SystemBus:
    case GameEntryDeviceKind::SystemAsic:
    case GameEntryDeviceKind::InterruptController:
    case GameEntryDeviceKind::InterruptRouter:
    case GameEntryDeviceKind::InterruptRegisters:
    case GameEntryDeviceKind::Mmu:
    case GameEntryDeviceKind::Cache:
    case GameEntryDeviceKind::StoreQueues:
    case GameEntryDeviceKind::IoPorts:
    case GameEntryDeviceKind::HollyG2Dma:
    case GameEntryDeviceKind::HollyPvrDma:
    case GameEntryDeviceKind::Sh4Tmu:
    case GameEntryDeviceKind::Sh4RtcClock:
    case GameEntryDeviceKind::Sh4Rtc:
    case GameEntryDeviceKind::Sh4Scif:
    case GameEntryDeviceKind::Flash:
        return true;
    }
    return false;
}

bool valid_scheduler_event_kind(const SchedulerEventKind kind) noexcept {
    switch (kind) {
    case SchedulerEventKind::DiscRead:
    case SchedulerEventKind::Sh4Dmac:
    case SchedulerEventKind::GdRomPacket:
    case SchedulerEventKind::HollyG1Dma:
    case SchedulerEventKind::HollyG2Dma:
    case SchedulerEventKind::HollyPvrDma:
    case SchedulerEventKind::MapleDma:
    case SchedulerEventKind::PvrRender:
    case SchedulerEventKind::PvrVblankIn:
    case SchedulerEventKind::PvrVblankOut:
    case SchedulerEventKind::PvrHblank:
    case SchedulerEventKind::ScifTransmit:
    case SchedulerEventKind::SystemAsic:
    case SchedulerEventKind::Sh4Rtc:
    case SchedulerEventKind::Sh4Tmu0:
    case SchedulerEventKind::Sh4Tmu1:
    case SchedulerEventKind::Sh4Tmu2:
    case SchedulerEventKind::AicaTick:
        return true;
    case SchedulerEventKind::MediaVideo:
    case SchedulerEventKind::MediaAudio:
    case SchedulerEventKind::Unknown:
        return false;
    }
    return false;
}

bool valid_scheduler_event_owner(
    const GameEntryScheduledEvent& event) noexcept {
    const auto owner_is = [&](const GameEntryDeviceKind kind) noexcept {
        return event.owner == GameEntryDeviceKey{kind, 0u};
    };
    switch (event.kind) {
    case SchedulerEventKind::DiscRead:
        return owner_is(GameEntryDeviceKind::GdRom) &&
               event.channel == 1u && event.token != 0u;
    case SchedulerEventKind::Sh4Dmac:
        return owner_is(GameEntryDeviceKind::Sh4Dmac) &&
               event.channel < 4u && event.token == 0u;
    case SchedulerEventKind::GdRomPacket:
        return owner_is(GameEntryDeviceKind::GdRom) &&
               event.channel == 0u && event.token == 0u;
    case SchedulerEventKind::HollyG1Dma:
        return owner_is(GameEntryDeviceKind::G1) &&
               event.channel == 0u && event.token == 0u;
    case SchedulerEventKind::HollyG2Dma:
        return owner_is(GameEntryDeviceKind::HollyG2Dma) &&
               event.channel < 4u && event.token == 0u;
    case SchedulerEventKind::HollyPvrDma:
        return owner_is(GameEntryDeviceKind::HollyPvrDma) &&
               event.channel == 0u && event.token == 0u;
    case SchedulerEventKind::MapleDma:
        return owner_is(GameEntryDeviceKind::Maple) &&
               event.channel == 0u && event.token == 0u;
    case SchedulerEventKind::PvrVblankIn:
        return owner_is(GameEntryDeviceKind::Pvr) &&
               event.channel == 1u && event.token == 1u;
    case SchedulerEventKind::PvrVblankOut:
        return owner_is(GameEntryDeviceKind::Pvr) &&
               event.channel == 2u && event.token == 1u;
    case SchedulerEventKind::PvrHblank:
        return owner_is(GameEntryDeviceKind::Pvr) &&
               event.channel == 3u && event.token == 1u;
    case SchedulerEventKind::ScifTransmit:
        return owner_is(GameEntryDeviceKind::Sh4Scif) &&
               event.channel == 0u && event.token == 1u;
    case SchedulerEventKind::SystemAsic:
        return owner_is(GameEntryDeviceKind::SystemAsic) &&
               event.channel == 0u &&
               event.token <= std::numeric_limits<std::uint16_t>::max();
    case SchedulerEventKind::Sh4Rtc:
        return owner_is(GameEntryDeviceKind::Sh4Rtc) &&
               event.channel == 0u && event.token == 1u;
    case SchedulerEventKind::Sh4Tmu0:
    case SchedulerEventKind::Sh4Tmu1:
    case SchedulerEventKind::Sh4Tmu2:
        return owner_is(GameEntryDeviceKind::Sh4Tmu) &&
               event.channel ==
                   static_cast<std::uint32_t>(event.kind) -
                       static_cast<std::uint32_t>(
                           SchedulerEventKind::Sh4Tmu0) &&
               event.token == 1u;
    case SchedulerEventKind::AicaTick:
        return owner_is(GameEntryDeviceKind::Aica) &&
               event.channel == 1u && event.token == 1u;
    case SchedulerEventKind::PvrRender:
    case SchedulerEventKind::MediaVideo:
    case SchedulerEventKind::MediaAudio:
    case SchedulerEventKind::Unknown:
        return false;
    }
    return false;
}

auto device_key_tuple(const GameEntryDeviceKey& key) noexcept {
    return std::tuple{static_cast<std::uint16_t>(key.kind), key.instance};
}

std::uint32_t memory_region_size(const GameEntryMemoryLayout& layout,
                                 const GameEntryMemoryRegion region) {
    switch (region) {
    case GameEntryMemoryRegion::MainRam:
        return layout.main_ram_size;
    case GameEntryMemoryRegion::Vram:
        return layout.vram_size;
    case GameEntryMemoryRegion::AicaRam:
        return layout.aica_ram_size;
    }
    fail(GameEntryHandoffFailure::MemoryOperationInvalid);
}

bool empty_private_slice(
    const GameEntryPrivateSliceReference& reference) noexcept {
    return reference.artifact_identity.empty() && reference.artifact_offset == 0u &&
           reference.size == 0u && reference.byte_identity.empty();
}

void validate_binding(const GameEntryHandoffBinding& binding) {
    if (binding.schema_version != game_entry_handoff_schema_version ||
        binding.required_runtime_abi != abi_version ||
        binding.required_platform_state_contract !=
            game_entry_platform_state_contract_version ||
        !valid_game_entry_content_identity(
            binding.executable.content_identity) ||
        !valid_boot_file_name(binding.executable.boot_file_name) ||
        !valid_game_entry_sha256_identity(
            binding.executable.boot_byte_identity) ||
        !valid_console_profile(binding.console_profile) ||
        !valid_game_entry_sha256_identity(binding.descriptor_identity))
        fail(GameEntryHandoffFailure::BindingInvalid);
}

void validate_private_slice(
    const GameEntryPrivateSliceReference& reference) {
    if (!valid_game_entry_sha256_identity(reference.artifact_identity) ||
        !valid_game_entry_sha256_identity(reference.byte_identity) ||
        reference.size == 0u ||
        reference.artifact_offset >
            std::numeric_limits<std::uint64_t>::max() - reference.size)
        fail(GameEntryHandoffFailure::PrivateSliceInvalid);
}

template <typename Value>
void append_unsigned(std::string& material, const Value value) {
    static_assert(std::is_unsigned_v<Value>);
    for (std::size_t index = 0u; index < sizeof(Value); ++index) {
        material.push_back(static_cast<char>(
            (value >> (index * 8u)) & static_cast<Value>(0xFFu)));
    }
}

template <typename Enum>
void append_enum(std::string& material, const Enum value) {
    using Underlying = std::underlying_type_t<Enum>;
    using Unsigned = std::make_unsigned_t<Underlying>;
    append_unsigned(material, static_cast<Unsigned>(value));
}

void append_bool(std::string& material, const bool value) {
    append_unsigned(material, static_cast<std::uint8_t>(value ? 1u : 0u));
}

void append_string(std::string& material, const std::string_view value) {
    append_unsigned(material, static_cast<std::uint64_t>(value.size()));
    material.append(value);
}

template <typename Value, std::size_t Size>
void append_u32_array(std::string& material,
                      const std::array<Value, Size>& values) {
    static_assert(std::is_same_v<Value, std::uint32_t>);
    for (const auto value : values)
        append_unsigned(material, value);
}

void append_private_slice(std::string& material,
                          const GameEntryPrivateSliceReference& reference) {
    append_string(material, reference.artifact_identity);
    append_unsigned(material, reference.artifact_offset);
    append_unsigned(material, reference.size);
    append_string(material, reference.byte_identity);
}

void append_binding(std::string& material,
                    const GameEntryHandoffBinding& binding) {
    append_unsigned(material, binding.schema_version);
    append_unsigned(material, binding.required_runtime_abi);
    append_unsigned(material, binding.required_platform_state_contract);
    append_string(material, binding.executable.content_identity);
    append_string(material, binding.executable.boot_file_name);
    append_string(material, binding.executable.boot_byte_identity);
    append_enum(material, binding.console_profile);
}

void append_cpu(std::string& material, const GameEntryCpuState& cpu) {
    append_u32_array(material, cpu.gpr_bank0);
    append_u32_array(material, cpu.gpr_bank1);
    append_u32_array(material, cpu.r8_to_r15);
    append_u32_array(material, cpu.fpr_bank0);
    append_u32_array(material, cpu.fpr_bank1);
    append_unsigned(material, cpu.pc);
    append_unsigned(material, cpu.pr);
    append_unsigned(material, cpu.sr);
    append_unsigned(material, cpu.fpscr);
    append_unsigned(material, cpu.gbr);
    append_unsigned(material, cpu.vbr);
    append_unsigned(material, cpu.dbr);
    append_unsigned(material, cpu.ssr);
    append_unsigned(material, cpu.spc);
    append_unsigned(material, cpu.sgr);
    append_unsigned(material, cpu.mach);
    append_unsigned(material, cpu.macl);
    append_unsigned(material, cpu.fpul);
    append_unsigned(material, cpu.tra);
    append_unsigned(material, cpu.tea);
    append_unsigned(material, cpu.expevt);
    append_unsigned(material, cpu.intevt);
    append_unsigned(material, cpu.mmu.pteh);
    append_unsigned(material, cpu.mmu.ptel);
    append_unsigned(material, cpu.mmu.ptea);
    append_unsigned(material, cpu.mmu.ttb);
    append_unsigned(material, cpu.mmu.mmucr);
    for (const auto& entry : cpu.mmu.utlb) {
        append_unsigned(material, entry.pteh);
        append_unsigned(material, entry.ptel);
        append_unsigned(material, entry.ptea);
    }
    append_bool(material, cpu.exception.trap_pending);
    append_enum(material, cpu.exception.last_cause);
    append_bool(material, cpu.exception.in_delay_slot);
    append_unsigned(material, cpu.exception.last_instruction_pc);
    append_unsigned(material, cpu.exception.last_instruction_physical_pc);
    append_unsigned(material, cpu.exception.last_owner_pc);
    append_bool(material, cpu.exception.sleeping);
}

std::string byte_identity(const std::span<const std::uint8_t> bytes) {
    const std::string_view view(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return "sha256:" + katana::io::sha256_bytes(view);
}

void add_staged_bytes(std::uint64_t& total,
                      const std::uint64_t size,
                      const GameEntryHandoffLimits& limits) {
    if (size > limits.maximum_staged_bytes ||
        total > limits.maximum_staged_bytes - size)
        fail(GameEntryHandoffFailure::LimitExceeded);
    total += size;
}

bool entry_in_allowed_range(
    const std::uint32_t entry,
    const std::span<const GameEntryCodeRange> ranges) noexcept {
    return std::any_of(ranges.begin(), ranges.end(), [entry](const auto& range) {
        return entry >= range.start &&
               static_cast<std::uint64_t>(entry) - range.start < range.size;
    });
}

void validate_request(const GameEntryHandoffRequest& request) {
    validate_binding(request.expected_binding);
    if (request.allowed_entry_ranges.empty() ||
        request.memory_layout.main_ram_size == 0u ||
        request.memory_layout.vram_size == 0u ||
        request.memory_layout.aica_ram_size == 0u ||
        !valid_completeness(request.required_completeness) ||
        (request.required_completeness ==
             GameEntryHandoffCompleteness::CompletePlatform &&
         request.required_devices.empty()) ||
        request.limits.maximum_memory_operations == 0u ||
        request.limits.maximum_device_states == 0u ||
        request.limits.maximum_device_scalars == 0u ||
        request.limits.maximum_device_payloads == 0u ||
        request.limits.maximum_scheduler_events == 0u ||
        request.limits.maximum_staged_bytes == 0u)
        fail(GameEntryHandoffFailure::MemoryLayoutInvalid);

    std::uint64_t previous_end = 0u;
    for (std::size_t index = 0u; index < request.allowed_entry_ranges.size();
         ++index) {
        const auto& range = request.allowed_entry_ranges[index];
        const auto end = static_cast<std::uint64_t>(range.start) + range.size;
        if (range.start == 0u || (range.start & 1u) != 0u ||
            range.size == 0u || (range.size & 1u) != 0u ||
            end > static_cast<std::uint64_t>(
                      std::numeric_limits<std::uint32_t>::max()) +
                      1u ||
            (index != 0u && range.start < previous_end))
            fail(GameEntryHandoffFailure::CpuStateInvalid);
        previous_end = end;
    }

    GameEntryDeviceKey previous_key{};
    bool have_previous = false;
    for (const auto& requirement : request.required_devices) {
        if (!valid_device_kind(requirement.key.kind) ||
            requirement.state_contract_version == 0u ||
            (have_previous &&
             device_key_tuple(previous_key) >=
                 device_key_tuple(requirement.key)))
            fail(GameEntryHandoffFailure::DeviceStateInvalid);
        previous_key = requirement.key;
        have_previous = true;
    }
    if (request.required_completeness ==
        GameEntryHandoffCompleteness::CompletePlatform) {
        if (request.required_devices.size() !=
                dreamcast_game_entry_required_devices_v2.size() ||
            !std::equal(
                request.required_devices.begin(),
                request.required_devices.end(),
                dreamcast_game_entry_required_devices_v2.begin()))
            fail(GameEntryHandoffFailure::DeviceStateInvalid);
    }
}

void validate_cpu(const GameEntryHandoff& handoff,
                  const GameEntryHandoffRequest& request) {
    const auto& cpu = handoff.cpu;
    validate_cpu_state_contract(cpu);
    if (!valid_transfer_kind(handoff.transfer.kind) ||
        handoff.transfer.entry_pc != cpu.pc ||
        handoff.transfer.exact_pr != cpu.pr ||
        !entry_in_allowed_range(cpu.pc, request.allowed_entry_ranges))
        fail(GameEntryHandoffFailure::CpuStateInvalid);
    if (handoff.transfer.kind == GameEntryTransferKind::CallWithReturnPr &&
        cpu.pr == 0u)
        fail(GameEntryHandoffFailure::CpuStateInvalid);
}

void validate_memory_operations(const GameEntryHandoff& handoff,
                                const GameEntryHandoffRequest& request,
                                std::uint64_t& total_staged_bytes,
                                bool& requires_private_reader) {
    if (handoff.memory_operations.size() >
        request.limits.maximum_memory_operations)
        fail(GameEntryHandoffFailure::LimitExceeded);

    GameEntryMemoryRegion previous_region = GameEntryMemoryRegion::MainRam;
    std::uint64_t previous_end = 0u;
    bool have_previous = false;
    for (const auto& operation : handoff.memory_operations) {
        if (!valid_memory_region(operation.region) ||
            !valid_memory_operation_kind(operation.kind) ||
            operation.size == 0u ||
            !valid_game_entry_sha256_identity(
                operation.expected_before_identity) ||
            !valid_game_entry_sha256_identity(
                operation.expected_after_identity))
            fail(GameEntryHandoffFailure::MemoryOperationInvalid);

        const auto end =
            static_cast<std::uint64_t>(operation.offset) + operation.size;
        if (end > memory_region_size(request.memory_layout, operation.region))
            fail(GameEntryHandoffFailure::MemoryOperationInvalid);
        if (have_previous) {
            const auto previous_value =
                static_cast<std::uint8_t>(previous_region);
            const auto current_value =
                static_cast<std::uint8_t>(operation.region);
            if (current_value < previous_value ||
                (current_value == previous_value &&
                 operation.offset < previous_end))
                fail(GameEntryHandoffFailure::MemoryOperationInvalid);
        }
        previous_region = operation.region;
        previous_end = end;
        have_previous = true;

        switch (operation.kind) {
        case GameEntryMemoryOperationKind::Fill:
            if (!empty_private_slice(operation.private_slice))
                fail(GameEntryHandoffFailure::MemoryOperationInvalid);
            break;
        case GameEntryMemoryOperationKind::CopyPrivateSlice:
            validate_private_slice(operation.private_slice);
            if (operation.private_slice.size != operation.size ||
                operation.private_slice.byte_identity !=
                    operation.expected_after_identity)
                fail(GameEntryHandoffFailure::MemoryOperationInvalid);
            requires_private_reader = true;
            break;
        }
        add_staged_bytes(
            total_staged_bytes, operation.size, request.limits);
    }
}

const GameEntryDeviceState*
find_device(const std::vector<GameEntryDeviceState>& devices,
            const GameEntryDeviceKey& key) noexcept {
    const auto found = std::lower_bound(
        devices.begin(), devices.end(), key, [](const auto& left, const auto& right) {
            return device_key_tuple(left.key) < device_key_tuple(right);
        });
    return found != devices.end() && found->key == key ? &*found : nullptr;
}

void validate_devices(const GameEntryHandoff& handoff,
                      const GameEntryHandoffRequest& request,
                      std::uint64_t& total_staged_bytes,
                      bool& requires_private_reader) {
    if (handoff.devices.size() > request.limits.maximum_device_states)
        fail(GameEntryHandoffFailure::LimitExceeded);

    std::size_t scalar_count = 0u;
    std::size_t payload_count = 0u;
    GameEntryDeviceKey previous_key{};
    bool have_previous = false;
    for (const auto& device : handoff.devices) {
        if (!valid_device_kind(device.key.kind) ||
            device.state_contract_version == 0u ||
            (have_previous &&
             device_key_tuple(previous_key) >= device_key_tuple(device.key)))
            fail(GameEntryHandoffFailure::DeviceStateInvalid);
        previous_key = device.key;
        have_previous = true;

        // Platform-state contract v2 gives every required device one stable,
        // lossless binary payload in field 1. Scalars remain available to
        // future per-device contract versions, but accepting an empty v1
        // device would falsely advertise CompletePlatform.
        if (device.state_contract_version == 1u &&
            (!device.scalars.empty() || device.payloads.size() != 1u ||
             device.payloads.front().field_id != 1u))
            fail(GameEntryHandoffFailure::DeviceStateInvalid);

        if (device.scalars.size() >
                request.limits.maximum_device_scalars - scalar_count ||
            device.payloads.size() >
                request.limits.maximum_device_payloads - payload_count)
            fail(GameEntryHandoffFailure::LimitExceeded);
        scalar_count += device.scalars.size();
        payload_count += device.payloads.size();

        for (std::size_t index = 1u; index < device.scalars.size(); ++index) {
            if (device.scalars[index - 1u].field_id >=
                device.scalars[index].field_id)
                fail(GameEntryHandoffFailure::DeviceStateInvalid);
        }
        for (std::size_t index = 1u; index < device.payloads.size(); ++index) {
            if (device.payloads[index - 1u].field_id >=
                device.payloads[index].field_id)
                fail(GameEntryHandoffFailure::DeviceStateInvalid);
        }
        for (const auto& payload : device.payloads) {
            if (std::binary_search(
                    device.scalars.begin(),
                    device.scalars.end(),
                    payload.field_id,
                    [](const auto& left, const auto& right) {
                        if constexpr (std::is_same_v<
                                          std::remove_cvref_t<decltype(left)>,
                                          GameEntryDeviceScalar>)
                            return left.field_id < right;
                        else
                            return left < right.field_id;
                    }))
                fail(GameEntryHandoffFailure::DeviceStateInvalid);
            validate_private_slice(payload.private_slice);
            add_staged_bytes(total_staged_bytes,
                             payload.private_slice.size,
                             request.limits);
            requires_private_reader = true;
        }
    }

    for (const auto& requirement : request.required_devices) {
        const auto* device = find_device(handoff.devices, requirement.key);
        if (device == nullptr ||
            device->state_contract_version !=
                requirement.state_contract_version)
            fail(GameEntryHandoffFailure::DeviceStateInvalid);
    }
    if (handoff.completeness ==
            GameEntryHandoffCompleteness::CompletePlatform &&
        handoff.devices.size() !=
            dreamcast_game_entry_required_devices_v2.size())
        fail(GameEntryHandoffFailure::DeviceStateInvalid);
}

void validate_scheduler(const GameEntryHandoff& handoff,
                        const GameEntryHandoffRequest& request) {
    if (handoff.scheduler.pending_events.size() >
        request.limits.maximum_scheduler_events)
        fail(GameEntryHandoffFailure::LimitExceeded);

    using EventKey =
        std::tuple<std::uint64_t,
                   std::uint32_t,
                   std::uint16_t,
                   std::uint16_t,
                   std::uint32_t,
                   std::uint64_t>;
    EventKey previous{};
    bool have_previous = false;
    for (const auto& event : handoff.scheduler.pending_events) {
        if (!valid_scheduler_event_kind(event.kind) ||
            !valid_device_kind(event.owner.kind) ||
            !valid_scheduler_event_owner(event) ||
            event.guest_cycle < handoff.scheduler.current_cycle ||
            find_device(handoff.devices, event.owner) == nullptr)
            fail(GameEntryHandoffFailure::SchedulerStateInvalid);
        const EventKey current{event.guest_cycle,
                               static_cast<std::uint32_t>(event.kind),
                               static_cast<std::uint16_t>(event.owner.kind),
                               event.owner.instance,
                               event.channel,
                               event.token};
        if (have_previous && previous >= current)
            fail(GameEntryHandoffFailure::SchedulerStateInvalid);
        previous = current;
        have_previous = true;
    }
}

std::vector<std::uint8_t> read_private_slice(
    const GameEntryHandoffProvider& provider,
    const GameEntryPrivateSliceReference& reference) {
    std::vector<std::uint8_t> bytes(reference.size);
    if (provider.read_private_slice == nullptr ||
        !provider.read_private_slice(provider.context, reference, bytes))
        fail(GameEntryHandoffFailure::PrivateSliceReadFailed);
    if (byte_identity(bytes) != reference.byte_identity)
        fail(GameEntryHandoffFailure::PrivateSliceIdentityMismatch);
    return bytes;
}

TlbMapping game_entry_tlb_mapping(const GameEntryTlbEntry& entry,
                                  const std::size_t index) noexcept {
    const auto size_code =
        ((entry.ptel >> 6u) & 2u) | ((entry.ptel >> 4u) & 1u);
    constexpr std::array<std::uint32_t, 4u> page_sizes{
        1024u, 4096u, 65536u, 1048576u};
    const auto protection =
        static_cast<std::uint8_t>((entry.ptel >> 5u) & 3u);
    return {entry.pteh & 0xFFFFFC00u,
            entry.ptel & 0x1FFFFC00u,
            page_sizes[size_code],
            static_cast<std::uint8_t>(entry.pteh & 0xFFu),
            static_cast<std::uint8_t>(index),
            (entry.ptel & 0x00000100u) != 0u,
            true,
            (protection & 1u) != 0u,
            true,
            protection >= 2u,
            (entry.ptel & 0x00000004u) != 0u,
            (entry.ptel & 0x00000002u) != 0u};
}

struct PreparedGameEntryCpuApplication {
    GameEntryCpuState state;
    std::shared_ptr<RuntimeAddressSpace> target_address_space;
    RuntimeAddressSpace address_space;
};

PreparedGameEntryCpuApplication prepare_cpu_application(
    const CpuState& cpu,
    const GameEntryCpuState& state,
    std::shared_ptr<RuntimeAddressSpace> target_address_space = {}) {
    validate_cpu_state_contract(state);
    if (!target_address_space)
        target_address_space = cpu.address_space;
    if (!target_address_space)
        target_address_space = std::make_shared<RuntimeAddressSpace>();

    RuntimeAddressSpace prepared = *target_address_space;
    // Rebuild every translation from the physical UTLB contract. Starting
    // from the live object preserves monotonic guard generations and
    // watchpoint state; clear_tlb() invalidates every cached translation.
    prepared.clear_tlb();
    prepared.write_pteh(state.mmu.pteh);
    prepared.write_mmucr(state.mmu.mmucr);
    for (std::size_t index = 0u; index < state.mmu.utlb.size(); ++index) {
        const auto& entry = state.mmu.utlb[index];
        if (entry.pteh == 0u && entry.ptel == 0u && entry.ptea == 0u)
            continue;
        prepared.ldtlb(game_entry_tlb_mapping(entry, index));
    }
    prepared.set_mode(
        (state.mmu.mmucr & game_entry_mmucr_at_mask) != 0u
            ? AddressTranslationMode::Mmu
            : AddressTranslationMode::NoMmu);

    return {state, std::move(target_address_space), std::move(prepared)};
}

void commit_cpu_application(
    CpuState& cpu,
    PreparedGameEntryCpuApplication prepared) noexcept {
    static_assert(
        std::is_nothrow_move_assignable_v<RuntimeAddressSpace>);

    *prepared.target_address_space = std::move(prepared.address_space);
    cpu.address_space = std::move(prepared.target_address_space);

    const auto gpr_bank_one =
        (prepared.state.sr & (sr_md_mask | sr_rb_mask)) ==
        (sr_md_mask | sr_rb_mask);
    const auto& active_gpr =
        gpr_bank_one ? prepared.state.gpr_bank1
                     : prepared.state.gpr_bank0;
    const auto& inactive_gpr =
        gpr_bank_one ? prepared.state.gpr_bank0
                     : prepared.state.gpr_bank1;
    std::copy(active_gpr.begin(), active_gpr.end(), cpu.r.begin());
    std::copy(inactive_gpr.begin(),
              inactive_gpr.end(),
              cpu.r_bank.begin());
    std::copy(prepared.state.r8_to_r15.begin(),
              prepared.state.r8_to_r15.end(),
              cpu.r.begin() +
                  static_cast<std::ptrdiff_t>(banked_register_count));

    const auto fpu_bank_one =
        (prepared.state.fpscr & fpscr_fr_mask) != 0u;
    cpu.fr = fpu_bank_one ? prepared.state.fpr_bank1
                          : prepared.state.fpr_bank0;
    cpu.xf = fpu_bank_one ? prepared.state.fpr_bank0
                          : prepared.state.fpr_bank1;

    cpu.gbr = prepared.state.gbr;
    cpu.vbr = prepared.state.vbr;
    cpu.dbr = prepared.state.dbr;
    cpu.ssr = prepared.state.ssr;
    cpu.spc = prepared.state.spc;
    cpu.sgr = prepared.state.sgr;
    cpu.mach = prepared.state.mach;
    cpu.macl = prepared.state.macl;
    cpu.fpul = prepared.state.fpul;
    cpu.tra = prepared.state.tra;
    cpu.tea = prepared.state.tea;
    cpu.expevt = prepared.state.expevt;
    cpu.intevt = prepared.state.intevt;

    cpu.pteh = prepared.state.mmu.pteh;
    cpu.ptel = prepared.state.mmu.ptel;
    cpu.ptea = prepared.state.mmu.ptea;
    cpu.ttb = prepared.state.mmu.ttb;
    cpu.mmucr = prepared.state.mmu.mmucr;
    for (std::size_t index = 0u; index < cpu.utlb.size(); ++index) {
        const auto& source = prepared.state.mmu.utlb[index];
        cpu.utlb[index] = {source.pteh, source.ptel, source.ptea};
    }

    cpu.sr = prepared.state.sr;
    cpu.t = (prepared.state.sr & sr_t_mask) != 0u;
    cpu.s = (prepared.state.sr & sr_s_mask) != 0u;
    cpu.q = (prepared.state.sr & sr_q_mask) != 0u;
    cpu.m = (prepared.state.sr & sr_m_mask) != 0u;
    cpu.fpscr = prepared.state.fpscr;

    cpu.trap_pending = prepared.state.exception.trap_pending;
    cpu.last_exception_cause =
        prepared.state.exception.last_cause;
    cpu.exception_in_delay_slot =
        prepared.state.exception.in_delay_slot;
    cpu.last_exception_instruction_pc =
        prepared.state.exception.last_instruction_pc;
    cpu.last_exception_instruction_physical_pc =
        prepared.state.exception.last_instruction_physical_pc;
    cpu.last_exception_owner_pc =
        prepared.state.exception.last_owner_pc;
    cpu.sleeping = prepared.state.exception.sleeping;
    // The contract restores exception level/history, not a newly raised host
    // edge. Keeping the monotonic generation while clearing the edge marker
    // prevents the first imported block from misclassifying old state.
    cpu.last_exception_generation = 0u;

    // The handoff is an architectural control-transfer boundary. Execution
    // bookkeeping is re-established by the first dispatched native block.
    cpu.active_instruction_pc = 0u;
    cpu.active_instruction_physical_pc = 0u;
    cpu.active_block_virtual_start = 0u;
    cpu.active_block_physical_start = 0u;
    cpu.active_block_size = 0u;

    // Publish the architectural control transfer only after every other CPU
    // field and address-space guard is complete.
    cpu.pr = prepared.state.pr;
    cpu.pc = prepared.state.pc;
}

const LinearMemoryDevice& memory_region_device(
    const DreamcastRuntimeState& runtime,
    const GameEntryMemoryRegion region) {
    const std::shared_ptr<LinearMemoryDevice>* device = nullptr;
    switch (region) {
    case GameEntryMemoryRegion::MainRam:
        device = &runtime.main_ram;
        break;
    case GameEntryMemoryRegion::Vram:
        device = &runtime.vram;
        break;
    case GameEntryMemoryRegion::AicaRam:
        device = &runtime.aica_ram;
        break;
    }
    if (device == nullptr || !*device)
        fail(GameEntryHandoffFailure::RuntimeStateInvalid);
    return **device;
}

std::size_t expected_memory_region_size(
    const GameEntryMemoryRegion region) {
    switch (region) {
    case GameEntryMemoryRegion::MainRam:
        return dreamcast_main_ram_size;
    case GameEntryMemoryRegion::Vram:
        return dreamcast_vram_size;
    case GameEntryMemoryRegion::AicaRam:
        return dreamcast_aica_ram_size;
    }
    fail(GameEntryHandoffFailure::MemoryOperationInvalid);
}

std::uint32_t memory_region_physical_base(
    const GameEntryMemoryRegion region) {
    switch (region) {
    case GameEntryMemoryRegion::MainRam:
        return dreamcast_main_ram_area_bases.front();
    case GameEntryMemoryRegion::Vram:
        return dreamcast_vram_64bit_physical_bases.front();
    case GameEntryMemoryRegion::AicaRam:
        return dreamcast_aica_ram_physical_bases.front();
    }
    fail(GameEntryHandoffFailure::MemoryOperationInvalid);
}

void validate_runtime_memory_bindings(
    const CpuState& cpu,
    const DreamcastRuntimeState& runtime) {
    if (!runtime.main_ram || !runtime.vram || !runtime.aica_ram ||
        !runtime.address_space || !runtime.mmu_control ||
        cpu.address_space != runtime.address_space ||
        runtime.main_ram->size() != dreamcast_main_ram_size ||
        runtime.vram->size() != dreamcast_vram_size ||
        runtime.aica_ram->size() != dreamcast_aica_ram_size ||
        !cpu.memory.maps_device(
            dreamcast_main_ram_area_bases.front(),
            dreamcast_main_ram_size,
            runtime.main_ram.get(),
            false) ||
        !cpu.memory.maps_device(
            dreamcast_vram_64bit_physical_bases.front(),
            dreamcast_vram_size,
            runtime.vram.get(),
            false) ||
        !cpu.memory.maps_device(
            dreamcast_aica_ram_physical_bases.front(),
            dreamcast_aica_ram_size,
            runtime.aica_ram.get(),
            false))
        fail(GameEntryHandoffFailure::RuntimeStateInvalid);
}

} // namespace

struct PreparedGameEntryCpuRestore::Data {
    PreparedGameEntryCpuApplication prepared;
};

struct PreparedGameEntryCpuMemoryHandoff::Data {
    PreparedGameEntryCpuApplication cpu;
    std::optional<Memory::PreparedLinearTransactionBatch> memory;
    std::size_t memory_operation_count = 0u;
    std::uint64_t memory_byte_count = 0u;
    bool memory_committed = false;
};

PreparedGameEntryCpuRestore::PreparedGameEntryCpuRestore() = default;

PreparedGameEntryCpuRestore::PreparedGameEntryCpuRestore(
    PreparedGameEntryCpuRestore&&) noexcept = default;

PreparedGameEntryCpuRestore&
PreparedGameEntryCpuRestore::operator=(
    PreparedGameEntryCpuRestore&&) noexcept = default;

PreparedGameEntryCpuRestore::~PreparedGameEntryCpuRestore() = default;

PreparedGameEntryCpuMemoryHandoff::
    PreparedGameEntryCpuMemoryHandoff() = default;

PreparedGameEntryCpuMemoryHandoff::
    PreparedGameEntryCpuMemoryHandoff(
        PreparedGameEntryCpuMemoryHandoff&&) noexcept = default;

PreparedGameEntryCpuMemoryHandoff&
PreparedGameEntryCpuMemoryHandoff::operator=(
    PreparedGameEntryCpuMemoryHandoff&&) noexcept = default;

PreparedGameEntryCpuMemoryHandoff::
    ~PreparedGameEntryCpuMemoryHandoff() = default;

std::size_t
PreparedGameEntryCpuMemoryHandoff::memory_operation_count() const noexcept {
    return data_ ? data_->memory_operation_count : 0u;
}

std::uint64_t
PreparedGameEntryCpuMemoryHandoff::memory_byte_count() const noexcept {
    return data_ ? data_->memory_byte_count : 0u;
}

std::span<const GuestWriteEvent>
PreparedGameEntryCpuMemoryHandoff::
    memory_guest_write_events() const noexcept {
    return data_ && data_->memory
               ? data_->memory->guest_write_events()
               : std::span<const GuestWriteEvent>{};
}

void PreparedGameEntryCpuMemoryHandoff::
    suppress_memory_guest_write_observer() noexcept {
    if (!data_ || !data_->memory || data_->memory_committed)
        std::terminate();
    data_->memory->suppress_guest_write_observer();
}

PreparedGameEntryCpuRestore prepare_game_entry_cpu_restore(
    const CpuState& cpu,
    const GameEntryCpuState& state,
    std::shared_ptr<RuntimeAddressSpace> target_address_space) {
    PreparedGameEntryCpuRestore result;
    result.data_ = std::make_unique<PreparedGameEntryCpuRestore::Data>(
        PreparedGameEntryCpuRestore::Data{
            prepare_cpu_application(
                cpu, state, std::move(target_address_space))});
    return result;
}

void validate_prepared_game_entry_cpu_mmu(
    const PreparedGameEntryCpuRestore& prepared,
    const RuntimeAddressSpaceSnapshot& expected) {
    if (!prepared.data_)
        fail(GameEntryHandoffFailure::CpuStateInvalid);
    auto prepared_snapshot =
        prepared.data_->prepared.address_space.snapshot();
    auto prepared_mappings = std::move(prepared_snapshot.mappings);
    auto captured_mappings = expected.mappings;
    const auto by_slot = [](const auto& left, const auto& right) {
        return left.slot < right.slot;
    };
    std::sort(
        prepared_mappings.begin(), prepared_mappings.end(), by_slot);
    std::sort(
        captured_mappings.begin(), captured_mappings.end(), by_slot);
    if (prepared_snapshot.mode != expected.mode ||
        prepared_snapshot.mmucr != expected.mmucr ||
        prepared_snapshot.asid != expected.asid ||
        prepared_mappings != captured_mappings)
        fail(GameEntryHandoffFailure::DeviceStateInvalid);
}

void commit_prepared_game_entry_cpu_restore(
    CpuState& cpu,
    PreparedGameEntryCpuRestore prepared) noexcept {
    if (!prepared.data_) std::terminate();
    commit_cpu_application(
        cpu, std::move(prepared.data_->prepared));
}

GameEntryCpuState capture_game_entry_cpu_state(const CpuState& cpu) {
    GameEntryCpuState captured;
    const auto sr = cpu.read_sr();
    const auto fpscr = cpu.read_fpscr();

    const auto gpr_bank_one =
        (sr & (sr_md_mask | sr_rb_mask)) ==
        (sr_md_mask | sr_rb_mask);
    if (gpr_bank_one) {
        std::copy_n(
            cpu.r.begin(), banked_register_count, captured.gpr_bank1.begin());
        captured.gpr_bank0 = cpu.r_bank;
    } else {
        std::copy_n(
            cpu.r.begin(), banked_register_count, captured.gpr_bank0.begin());
        captured.gpr_bank1 = cpu.r_bank;
    }
    std::copy(cpu.r.begin() +
                  static_cast<std::ptrdiff_t>(banked_register_count),
              cpu.r.end(),
              captured.r8_to_r15.begin());

    if ((fpscr & fpscr_fr_mask) != 0u) {
        captured.fpr_bank1 = cpu.fr;
        captured.fpr_bank0 = cpu.xf;
    } else {
        captured.fpr_bank0 = cpu.fr;
        captured.fpr_bank1 = cpu.xf;
    }

    captured.pc = cpu.pc;
    captured.pr = cpu.pr;
    captured.sr = sr;
    captured.fpscr = fpscr;
    captured.gbr = cpu.gbr;
    captured.vbr = cpu.vbr;
    captured.dbr = cpu.dbr;
    captured.ssr = cpu.ssr;
    captured.spc = cpu.spc;
    captured.sgr = cpu.sgr;
    captured.mach = cpu.mach;
    captured.macl = cpu.macl;
    captured.fpul = cpu.fpul;
    captured.tra = cpu.tra;
    captured.tea = cpu.tea;
    captured.expevt = cpu.expevt;
    captured.intevt = cpu.intevt;
    captured.mmu.pteh = cpu.pteh;
    captured.mmu.ptel = cpu.ptel;
    captured.mmu.ptea = cpu.ptea;
    captured.mmu.ttb = cpu.ttb;
    captured.mmu.mmucr = cpu.mmucr;
    for (std::size_t index = 0u; index < cpu.utlb.size(); ++index) {
        const auto& source = cpu.utlb[index];
        captured.mmu.utlb[index] =
            {source.pteh, source.ptel, source.ptea};
    }
    captured.exception = {
        cpu.trap_pending,
        cpu.last_exception_cause,
        cpu.exception_in_delay_slot,
        cpu.last_exception_instruction_pc,
        cpu.last_exception_instruction_physical_pc,
        cpu.last_exception_owner_pc,
        cpu.sleeping};
    return captured;
}

void apply_game_entry_cpu_state(CpuState& cpu,
                                const GameEntryCpuState& state) {
    auto prepared = prepare_cpu_application(cpu, state);
    commit_cpu_application(cpu, std::move(prepared));
}

GameEntryMemorySnapshot capture_game_entry_memory_snapshot(
    const DreamcastRuntimeState& runtime) {
    const auto copy_region = [&](const GameEntryMemoryRegion region) {
        const auto bytes = memory_region_device(runtime, region).bytes();
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    };
    return {
        copy_region(GameEntryMemoryRegion::MainRam),
        copy_region(GameEntryMemoryRegion::Vram),
        copy_region(GameEntryMemoryRegion::AicaRam)};
}

GameEntryMemoryDelta capture_game_entry_memory_delta(
    const GameEntryMemorySnapshot& before,
    const DreamcastRuntimeState& after,
    const std::uint32_t page_size) {
    if (page_size == 0u || page_size > 1024u * 1024u ||
        (page_size & (page_size - 1u)) != 0u)
        throw std::invalid_argument(
            "Game-entry memory delta page size is invalid.");

    GameEntryMemoryDelta delta;
    const auto append_region =
        [&](const GameEntryMemoryRegion region,
            const std::vector<std::uint8_t>& previous) {
            const auto current =
                memory_region_device(after, region).bytes();
            if (previous.size() != current.size() ||
                current.size() != expected_memory_region_size(region))
                fail(GameEntryHandoffFailure::RuntimeStateInvalid);

            const auto page_changed =
                [&](const std::size_t offset) {
                    const auto size = std::min<std::size_t>(
                        page_size, current.size() - offset);
                    return !std::equal(
                        current.begin() +
                            static_cast<std::ptrdiff_t>(offset),
                        current.begin() +
                            static_cast<std::ptrdiff_t>(offset + size),
                        previous.begin() +
                            static_cast<std::ptrdiff_t>(offset));
                };

            std::size_t offset = 0u;
            while (offset < current.size()) {
                if (!page_changed(offset)) {
                    offset += std::min<std::size_t>(
                        page_size, current.size() - offset);
                    continue;
                }
                const auto run_start = offset;
                do {
                    offset += std::min<std::size_t>(
                        page_size, current.size() - offset);
                } while (offset < current.size() &&
                         page_changed(offset));
                const auto run_size = offset - run_start;
                if (run_start >
                        std::numeric_limits<std::uint32_t>::max() ||
                    run_size >
                        std::numeric_limits<std::uint32_t>::max())
                    fail(GameEntryHandoffFailure::LimitExceeded);

                const auto before_bytes = std::span(previous).subspan(
                    run_start, run_size);
                const auto after_bytes = current.subspan(
                    run_start, run_size);
                GameEntryMemoryOperation operation;
                operation.region = region;
                operation.offset =
                    static_cast<std::uint32_t>(run_start);
                operation.size =
                    static_cast<std::uint32_t>(run_size);
                operation.expected_before_identity =
                    byte_identity(before_bytes);
                operation.expected_after_identity =
                    byte_identity(after_bytes);
                operation.executable =
                    region == GameEntryMemoryRegion::MainRam;

                const auto fill_value = after_bytes.front();
                const auto is_fill = std::all_of(
                    after_bytes.begin(),
                    after_bytes.end(),
                    [&](const auto value) {
                        return value == fill_value;
                    });
                const auto operation_index =
                    static_cast<std::uint32_t>(
                        delta.operations.size());
                if (is_fill) {
                    operation.kind =
                        GameEntryMemoryOperationKind::Fill;
                    operation.fill_value = fill_value;
                } else {
                    operation.kind =
                        GameEntryMemoryOperationKind::CopyPrivateSlice;
                    operation.private_slice.size = operation.size;
                    operation.private_slice.byte_identity =
                        operation.expected_after_identity;
                    delta.payloads.push_back(
                        {operation_index,
                         std::vector<std::uint8_t>(
                             after_bytes.begin(),
                             after_bytes.end())});
                }
                delta.operations.push_back(std::move(operation));
                for (std::size_t index = run_start; index < offset; ++index)
                    if (previous[index] != current[index])
                        ++delta.changed_bytes;
            }
        };

    append_region(GameEntryMemoryRegion::MainRam, before.main_ram);
    append_region(GameEntryMemoryRegion::Vram, before.vram);
    append_region(GameEntryMemoryRegion::AicaRam, before.aica_ram);
    return delta;
}

GameEntryHandoffError::GameEntryHandoffError(
    const GameEntryHandoffFailure failure)
    : std::runtime_error([failure] {
          switch (failure) {
          case GameEntryHandoffFailure::ProviderUnavailable:
              return "game-entry-handoff-provider-unavailable";
          case GameEntryHandoffFailure::DescriptionUnavailable:
              return "game-entry-handoff-description-unavailable";
          case GameEntryHandoffFailure::BindingInvalid:
              return "game-entry-handoff-binding-invalid";
          case GameEntryHandoffFailure::BindingMismatch:
              return "game-entry-handoff-binding-mismatch";
          case GameEntryHandoffFailure::DescriptorIdentityMismatch:
              return "game-entry-handoff-descriptor-identity-mismatch";
          case GameEntryHandoffFailure::CpuStateInvalid:
              return "game-entry-handoff-cpu-state-invalid";
          case GameEntryHandoffFailure::MemoryLayoutInvalid:
              return "game-entry-handoff-memory-layout-invalid";
          case GameEntryHandoffFailure::MemoryOperationInvalid:
              return "game-entry-handoff-memory-operation-invalid";
          case GameEntryHandoffFailure::DeviceStateInvalid:
              return "game-entry-handoff-device-state-invalid";
          case GameEntryHandoffFailure::SchedulerStateInvalid:
              return "game-entry-handoff-scheduler-state-invalid";
          case GameEntryHandoffFailure::PrivateSliceInvalid:
              return "game-entry-handoff-private-slice-invalid";
          case GameEntryHandoffFailure::PrivateSliceReadFailed:
              return "game-entry-handoff-private-slice-read-failed";
          case GameEntryHandoffFailure::PrivateSliceIdentityMismatch:
              return "game-entry-handoff-private-slice-identity-mismatch";
          case GameEntryHandoffFailure::LimitExceeded:
              return "game-entry-handoff-limit-exceeded";
          case GameEntryHandoffFailure::RuntimeStateInvalid:
              return "game-entry-handoff-runtime-state-invalid";
          case GameEntryHandoffFailure::MemoryBeforeIdentityMismatch:
              return "game-entry-handoff-memory-before-identity-mismatch";
          case GameEntryHandoffFailure::MemoryAfterIdentityMismatch:
              return "game-entry-handoff-memory-after-identity-mismatch";
          case GameEntryHandoffFailure::CpuMemoryApplyFailed:
              return "game-entry-handoff-cpu-memory-apply-failed";
          case GameEntryHandoffFailure::CompletenessMismatch:
              return "game-entry-handoff-completeness-mismatch";
          case GameEntryHandoffFailure::DeviceStateApplyFailed:
              return "game-entry-handoff-device-state-apply-failed";
          case GameEntryHandoffFailure::SchedulerStateApplyFailed:
              return "game-entry-handoff-scheduler-state-apply-failed";
          case GameEntryHandoffFailure::SemanticStateMismatch:
              return "game-entry-handoff-semantic-state-mismatch";
          }
          return "game-entry-handoff-invalid";
      }()),
      failure_(failure) {}

GameEntryHandoffFailure GameEntryHandoffError::failure() const noexcept {
    return failure_;
}

const GameEntryHandoffBinding&
ValidatedGameEntryHandoff::binding() const noexcept {
    return binding_;
}

GameEntryHandoffCompleteness
ValidatedGameEntryHandoff::completeness() const noexcept {
    return completeness_;
}

const GameEntryControlTransfer&
ValidatedGameEntryHandoff::transfer() const noexcept {
    return transfer_;
}

const GameEntryCpuState& ValidatedGameEntryHandoff::cpu() const noexcept {
    return cpu_;
}

std::span<const ValidatedGameEntryMemoryOperation>
ValidatedGameEntryHandoff::memory_operations() const noexcept {
    return memory_operations_;
}

std::span<const ValidatedGameEntryDeviceState>
ValidatedGameEntryHandoff::devices() const noexcept {
    return devices_;
}

const GameEntrySchedulerState&
ValidatedGameEntryHandoff::scheduler() const noexcept {
    return scheduler_;
}

const std::string&
ValidatedGameEntryHandoff::expected_semantic_state_identity() const noexcept {
    return expected_semantic_state_identity_;
}

bool valid_game_entry_content_identity(
    const std::string& identity) noexcept {
    return identity.size() == 64u && lower_hex(identity);
}

bool valid_game_entry_sha256_identity(
    const std::string& identity) noexcept {
    constexpr std::string_view prefix = "sha256:";
    return identity.size() == prefix.size() + 64u &&
           std::string_view(identity).starts_with(prefix) &&
           lower_hex(std::string_view(identity).substr(prefix.size()));
}

void append_game_entry_semantic_state(
    std::string& material,
    const GameEntryHandoff& handoff) {
    append_enum(material, handoff.completeness);
    append_enum(material, handoff.transfer.kind);
    append_unsigned(material, handoff.transfer.entry_pc);
    append_unsigned(material, handoff.transfer.exact_pr);
    append_cpu(material, handoff.cpu);
    append_unsigned(
        material,
        static_cast<std::uint64_t>(handoff.memory_operations.size()));
    for (const auto& operation : handoff.memory_operations) {
        append_enum(material, operation.region);
        append_unsigned(material, operation.offset);
        append_unsigned(material, operation.size);
        append_enum(material, operation.kind);
        append_unsigned(material, operation.fill_value);
        append_string(material, operation.expected_after_identity);
        append_bool(material, operation.executable);
    }
    append_unsigned(material,
                    static_cast<std::uint64_t>(handoff.devices.size()));
    for (const auto& device : handoff.devices) {
        append_enum(material, device.key.kind);
        append_unsigned(material, device.key.instance);
        append_unsigned(material, device.state_contract_version);
        append_unsigned(material,
                        static_cast<std::uint64_t>(device.scalars.size()));
        for (const auto& field : device.scalars) {
            append_unsigned(material, field.field_id);
            append_unsigned(material, field.value);
        }
        append_unsigned(material,
                        static_cast<std::uint64_t>(device.payloads.size()));
        for (const auto& payload : device.payloads) {
            append_unsigned(material, payload.field_id);
            append_unsigned(material, payload.private_slice.size);
            append_string(material, payload.private_slice.byte_identity);
        }
    }
    append_unsigned(material, handoff.scheduler.current_cycle);
    append_unsigned(
        material,
        static_cast<std::uint64_t>(
            handoff.scheduler.pending_events.size()));
    for (const auto& event : handoff.scheduler.pending_events) {
        append_unsigned(material, event.guest_cycle);
        append_enum(material, event.kind);
        append_enum(material, event.owner.kind);
        append_unsigned(material, event.owner.instance);
        append_unsigned(material, event.channel);
        append_unsigned(material, event.token);
    }
}

std::string game_entry_semantic_state_identity(
    const GameEntryHandoff& handoff) {
    std::string material;
    append_string(material, "katana.game-entry-handoff.semantic-state");
    append_game_entry_semantic_state(material, handoff);
    return "sha256:" + katana::io::sha256_bytes(material);
}

std::string game_entry_handoff_descriptor_identity(
    const GameEntryHandoff& handoff) {
    std::string material;
    append_string(material, "katana.game-entry-handoff.descriptor");
    append_binding(material, handoff.binding);
    append_enum(material, handoff.completeness);
    append_enum(material, handoff.transfer.kind);
    append_unsigned(material, handoff.transfer.entry_pc);
    append_unsigned(material, handoff.transfer.exact_pr);
    append_cpu(material, handoff.cpu);
    append_unsigned(
        material,
        static_cast<std::uint64_t>(handoff.memory_operations.size()));
    for (const auto& operation : handoff.memory_operations) {
        append_enum(material, operation.region);
        append_unsigned(material, operation.offset);
        append_unsigned(material, operation.size);
        append_enum(material, operation.kind);
        append_unsigned(material, operation.fill_value);
        append_private_slice(material, operation.private_slice);
        append_string(material, operation.expected_before_identity);
        append_string(material, operation.expected_after_identity);
        append_bool(material, operation.executable);
    }
    append_unsigned(material,
                    static_cast<std::uint64_t>(handoff.devices.size()));
    for (const auto& device : handoff.devices) {
        append_enum(material, device.key.kind);
        append_unsigned(material, device.key.instance);
        append_unsigned(material, device.state_contract_version);
        append_unsigned(material,
                        static_cast<std::uint64_t>(device.scalars.size()));
        for (const auto& field : device.scalars) {
            append_unsigned(material, field.field_id);
            append_unsigned(material, field.value);
        }
        append_unsigned(material,
                        static_cast<std::uint64_t>(device.payloads.size()));
        for (const auto& payload : device.payloads) {
            append_unsigned(material, payload.field_id);
            append_private_slice(material, payload.private_slice);
        }
    }
    append_unsigned(material, handoff.scheduler.current_cycle);
    append_unsigned(
        material,
        static_cast<std::uint64_t>(
            handoff.scheduler.pending_events.size()));
    for (const auto& event : handoff.scheduler.pending_events) {
        append_unsigned(material, event.guest_cycle);
        append_enum(material, event.kind);
        append_enum(material, event.owner.kind);
        append_unsigned(material, event.owner.instance);
        append_unsigned(material, event.channel);
        append_unsigned(material, event.token);
    }
    append_string(material, handoff.expected_semantic_state_identity);
    return "sha256:" + katana::io::sha256_bytes(material);
}

ValidatedGameEntryHandoff validate_and_stage_game_entry_handoff(
    const GameEntryHandoffRequest& request,
    const GameEntryHandoffProvider& provider) {
    validate_request(request);
    if (provider.describe == nullptr)
        fail(GameEntryHandoffFailure::ProviderUnavailable);
    const auto* described = provider.describe(provider.context, request);
    if (described == nullptr)
        fail(GameEntryHandoffFailure::DescriptionUnavailable);

    // Deep-copy the descriptor before invoking the provider again. Its
    // callbacks cannot mutate the structure being validated underneath us.
    GameEntryHandoff handoff = *described;
    validate_binding(handoff.binding);
    if (handoff.binding != request.expected_binding)
        fail(GameEntryHandoffFailure::BindingMismatch);
    if (!valid_completeness(handoff.completeness) ||
        handoff.completeness != request.required_completeness)
        fail(GameEntryHandoffFailure::CompletenessMismatch);
    if (!valid_game_entry_sha256_identity(
            handoff.expected_semantic_state_identity) ||
        game_entry_semantic_state_identity(handoff) !=
            handoff.expected_semantic_state_identity)
        fail(GameEntryHandoffFailure::DescriptorIdentityMismatch);

    validate_cpu(handoff, request);
    std::uint64_t total_staged_bytes = 0u;
    bool requires_private_reader = false;
    validate_memory_operations(
        handoff, request, total_staged_bytes, requires_private_reader);
    validate_devices(
        handoff, request, total_staged_bytes, requires_private_reader);
    validate_scheduler(handoff, request);
    if (requires_private_reader && provider.read_private_slice == nullptr)
        fail(GameEntryHandoffFailure::ProviderUnavailable);
    if (game_entry_handoff_descriptor_identity(handoff) !=
        handoff.binding.descriptor_identity)
        fail(GameEntryHandoffFailure::DescriptorIdentityMismatch);

    ValidatedGameEntryHandoff validated;
    validated.binding_ = handoff.binding;
    validated.completeness_ = handoff.completeness;
    validated.transfer_ = handoff.transfer;
    validated.cpu_ = handoff.cpu;
    validated.memory_operations_.reserve(
        handoff.memory_operations.size());
    for (auto& operation : handoff.memory_operations) {
        std::vector<std::uint8_t> bytes;
        switch (operation.kind) {
        case GameEntryMemoryOperationKind::Fill:
            bytes.assign(operation.size, operation.fill_value);
            break;
        case GameEntryMemoryOperationKind::CopyPrivateSlice:
            bytes = read_private_slice(provider, operation.private_slice);
            break;
        }
        if (byte_identity(bytes) != operation.expected_after_identity)
            fail(GameEntryHandoffFailure::PrivateSliceIdentityMismatch);
        validated.memory_operations_.push_back(
            {std::move(operation), std::move(bytes)});
    }

    validated.devices_.reserve(handoff.devices.size());
    for (auto& device : handoff.devices) {
        ValidatedGameEntryDeviceState staged;
        staged.key = device.key;
        staged.state_contract_version = device.state_contract_version;
        staged.scalars = std::move(device.scalars);
        staged.payloads.reserve(device.payloads.size());
        for (auto& payload : device.payloads) {
            auto bytes =
                read_private_slice(provider, payload.private_slice);
            staged.payloads.push_back(
                {std::move(payload), std::move(bytes)});
        }
        validated.devices_.push_back(std::move(staged));
    }
    validated.scheduler_ = std::move(handoff.scheduler);
    validated.expected_semantic_state_identity_ =
        std::move(handoff.expected_semantic_state_identity);
    return validated;
}

PreparedGameEntryCpuMemoryHandoff
prepare_validated_game_entry_cpu_memory_handoff(
    CpuState& cpu,
    DreamcastRuntimeState& runtime,
    const ValidatedGameEntryHandoff& handoff) {
    validate_runtime_memory_bindings(cpu, runtime);
    if (cpu.pending_guest_cycles != 0u)
        fail(GameEntryHandoffFailure::RuntimeStateInvalid);

    const auto& transfer = handoff.transfer();
    const auto& state = handoff.cpu();
    validate_cpu_state_contract(state);
    if (!valid_transfer_kind(transfer.kind) ||
        transfer.entry_pc != state.pc ||
        transfer.exact_pr != state.pr ||
        (transfer.kind == GameEntryTransferKind::CallWithReturnPr &&
         transfer.exact_pr == 0u))
        fail(GameEntryHandoffFailure::CpuStateInvalid);

    // Prepare the complete address-space replacement before any RAM byte is
    // touched. The live object itself is replaced only after the memory batch
    // has been atomically admitted and committed.
    auto prepared_cpu =
        prepare_cpu_application(cpu, state, runtime.address_space);

    std::vector<LinearMemoryTransactionWrite> writes;
    writes.reserve(handoff.memory_operations().size());
    std::uint64_t total_bytes = 0u;
    GameEntryMemoryRegion previous_region =
        GameEntryMemoryRegion::MainRam;
    std::uint64_t previous_end = 0u;
    bool have_previous = false;

    for (const auto& staged : handoff.memory_operations()) {
        const auto& operation = staged.operation;
        if (!valid_memory_region(operation.region) ||
            !valid_memory_operation_kind(operation.kind) ||
            operation.size == 0u ||
            operation.size != staged.bytes.size() ||
            !valid_game_entry_sha256_identity(
                operation.expected_before_identity) ||
            !valid_game_entry_sha256_identity(
                operation.expected_after_identity))
            fail(GameEntryHandoffFailure::MemoryOperationInvalid);

        const auto expected_size =
            expected_memory_region_size(operation.region);
        const auto& device =
            memory_region_device(runtime, operation.region);
        if (device.size() != expected_size)
            fail(GameEntryHandoffFailure::RuntimeStateInvalid);
        const auto end =
            static_cast<std::uint64_t>(operation.offset) +
            operation.size;
        if (end > device.size())
            fail(GameEntryHandoffFailure::MemoryOperationInvalid);

        if (have_previous) {
            const auto previous_value =
                static_cast<std::uint8_t>(previous_region);
            const auto current_value =
                static_cast<std::uint8_t>(operation.region);
            if (current_value < previous_value ||
                (current_value == previous_value &&
                 operation.offset < previous_end))
                fail(GameEntryHandoffFailure::MemoryOperationInvalid);
        }
        previous_region = operation.region;
        previous_end = end;
        have_previous = true;

        switch (operation.kind) {
        case GameEntryMemoryOperationKind::Fill:
            if (!empty_private_slice(operation.private_slice) ||
                !std::all_of(
                    staged.bytes.begin(),
                    staged.bytes.end(),
                    [&](const auto value) {
                        return value == operation.fill_value;
                    }))
                fail(GameEntryHandoffFailure::MemoryOperationInvalid);
            break;
        case GameEntryMemoryOperationKind::CopyPrivateSlice:
            validate_private_slice(operation.private_slice);
            if (operation.private_slice.size != operation.size ||
                operation.private_slice.byte_identity !=
                    operation.expected_after_identity)
                fail(GameEntryHandoffFailure::MemoryOperationInvalid);
            break;
        }

        if (byte_identity(staged.bytes) !=
            operation.expected_after_identity)
            fail(GameEntryHandoffFailure::MemoryAfterIdentityMismatch);
        const auto current = device.bytes().subspan(
            operation.offset, operation.size);
        if (byte_identity(current) !=
            operation.expected_before_identity)
            fail(GameEntryHandoffFailure::MemoryBeforeIdentityMismatch);

        if (total_bytes >
            std::numeric_limits<std::uint64_t>::max() -
                operation.size)
            fail(GameEntryHandoffFailure::LimitExceeded);
        total_bytes += operation.size;
        writes.push_back(
            {memory_region_physical_base(operation.region) +
                 operation.offset,
             staged.bytes});
    }

    auto prepared_memory =
        cpu.memory.prepare_linear_transaction_batch(
            writes, CodeWriteSource::Copy);
    if (!prepared_memory)
        fail(GameEntryHandoffFailure::CpuMemoryApplyFailed);

    PreparedGameEntryCpuMemoryHandoff prepared;
    prepared.data_ =
        std::make_unique<PreparedGameEntryCpuMemoryHandoff::Data>(
            PreparedGameEntryCpuMemoryHandoff::Data{
                std::move(prepared_cpu),
                std::move(prepared_memory),
                handoff.memory_operations().size(),
                total_bytes,
                false});
    return prepared;
}

void commit_prepared_game_entry_memory_handoff(
    CpuState& cpu,
    PreparedGameEntryCpuMemoryHandoff& prepared) noexcept {
    if (!prepared.data_ || !prepared.data_->memory ||
        prepared.data_->memory_committed)
        std::terminate();
    cpu.memory.commit_prepared_linear_transaction_batch(
        std::move(*prepared.data_->memory));
    prepared.data_->memory.reset();
    prepared.data_->memory_committed = true;
}

void bind_prepared_game_entry_cpu_mmu(
    PreparedGameEntryCpuMemoryHandoff& prepared,
    RuntimeAddressSpaceSnapshot expected) {
    if (!prepared.data_ || prepared.data_->memory_committed)
        fail(GameEntryHandoffFailure::CpuStateInvalid);
    auto actual =
        prepared.data_->cpu.address_space.snapshot();
    auto actual_mappings = actual.mappings;
    auto expected_mappings = expected.mappings;
    const auto by_slot = [](const auto& left, const auto& right) {
        return left.slot < right.slot;
    };
    std::sort(
        actual_mappings.begin(), actual_mappings.end(), by_slot);
    std::sort(
        expected_mappings.begin(), expected_mappings.end(), by_slot);
    if (actual.mode != expected.mode ||
        actual.mmucr != expected.mmucr ||
        actual.asid != expected.asid ||
        actual_mappings != expected_mappings)
        fail(GameEntryHandoffFailure::DeviceStateInvalid);
    prepared.data_->cpu.address_space.commit_validated_state_restore(
        std::move(expected));
}

void commit_prepared_game_entry_cpu_handoff(
    CpuState& cpu,
    PreparedGameEntryCpuMemoryHandoff prepared) noexcept {
    if (!prepared.data_ || !prepared.data_->memory_committed ||
        prepared.data_->memory)
        std::terminate();
    commit_cpu_application(
        cpu, std::move(prepared.data_->cpu));
}

GameEntryCpuMemoryApplyResult
apply_validated_game_entry_cpu_memory_handoff(
    CpuState& cpu,
    DreamcastRuntimeState& runtime,
    const ValidatedGameEntryHandoff& handoff) {
    auto prepared =
        prepare_validated_game_entry_cpu_memory_handoff(
            cpu, runtime, handoff);
    const auto memory_operation_count =
        prepared.memory_operation_count();
    const auto memory_byte_count = prepared.memory_byte_count();
    commit_prepared_game_entry_memory_handoff(cpu, prepared);
    commit_prepared_game_entry_cpu_handoff(cpu, std::move(prepared));

    const bool device_state_pending = !handoff.devices().empty();
    const bool scheduler_state_pending =
        handoff.scheduler().current_cycle != 0u ||
        !handoff.scheduler().pending_events.empty();
    const bool incomplete_handoff =
        handoff.completeness() !=
        GameEntryHandoffCompleteness::CompletePlatform;
    const auto status =
        incomplete_handoff || device_state_pending ||
                scheduler_state_pending
            ? GameEntryCpuMemoryApplyStatus::
                  CpuMemoryAndControlTransferAppliedPlatformStatePending
            : GameEntryCpuMemoryApplyStatus::
                  CpuMemoryAndControlTransferApplied;
    return {status,
            memory_operation_count,
            memory_byte_count,
            device_state_pending,
            scheduler_state_pending,
            incomplete_handoff};
}

} // namespace katana::runtime
