#include "katana/runtime/gdrom_controller.hpp"

#include "katana/runtime/block_table.hpp"
#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/holly_dma.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace katana::runtime {
namespace {
constexpr std::uint8_t ata_error = 0x01u;
constexpr std::uint8_t ata_drq = 0x08u;
constexpr std::uint8_t ata_ready = 0x40u;
constexpr std::uint8_t ata_busy = 0x80u;
constexpr std::size_t gdrom_hardware_info_size = 32u;
constexpr std::size_t gdrom_writable_mode_size = 10u;
constexpr std::uint32_t bios_command_pio_read = 16u;
constexpr std::uint32_t bios_command_dma_read = 17u;
constexpr std::uint32_t bios_command_dma_stream = 28u;
constexpr std::uint32_t bios_command_pio_stream = 37u;

std::uint32_t be16(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes.at(offset)) << 8u) | bytes.at(offset + 1u);
}
std::uint32_t be24(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes.at(offset)) << 16u) |
           (static_cast<std::uint32_t>(bytes.at(offset + 1u)) << 8u) | bytes.at(offset + 2u);
}
std::uint32_t be32(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes.at(offset)) << 24u) |
           (static_cast<std::uint32_t>(bytes.at(offset + 1u)) << 16u) |
           (static_cast<std::uint32_t>(bytes.at(offset + 2u)) << 8u) | bytes.at(offset + 3u);
}
void append_be32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24u));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16u));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
    bytes.push_back(static_cast<std::uint8_t>(value));
}
void append_le32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16u));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24u));
}

std::array<std::uint8_t, 3u> packet_sense_for_status(const GdRomStatus status) noexcept {
    switch (status) {
    case GdRomStatus::Good:
        return {0u, 0u, 0u};
    case GdRomStatus::NoMedia:
        return {2u, 0x3Au, 0u};
    case GdRomStatus::InvalidCommand:
        return {5u, 0x20u, 0u};
    case GdRomStatus::InvalidField:
        return {5u, 0x24u, 0u};
    case GdRomStatus::OutOfRange:
        return {5u, 0x21u, 0u};
    case GdRomStatus::Aborted:
        return {0x0Bu, 0u, 0u};
    }
    return {5u, 0x20u, 0u};
}

std::optional<std::uint32_t> resolve_bios_write_destination(CpuState& cpu,
                                                            const std::uint32_t guest_address,
                                                            const std::size_t size) {
    constexpr std::size_t minimum_sh4_page_size = 1024u;
    if (size == 0u || size > 0x1'0000'0000ull - guest_address) return std::nullopt;
    try {
        const auto first = translate_guest_address(
            cpu, guest_address, MemoryAccessOperation::Write, MemoryAccessWidth::Byte);
        auto offset = std::min<std::size_t>(
            size, minimum_sh4_page_size - (guest_address & (minimum_sh4_page_size - 1u)));
        while (offset < size) {
            const auto translated = translate_guest_address(
                cpu,
                guest_address + static_cast<std::uint32_t>(offset),
                MemoryAccessOperation::Write,
                MemoryAccessWidth::Byte);
            if (static_cast<std::uint64_t>(translated) !=
                static_cast<std::uint64_t>(first) + offset)
                return std::nullopt;
            offset += std::min<std::size_t>(minimum_sh4_page_size, size - offset);
        }
        if (static_cast<std::uint64_t>(first) + size > 0x1'0000'0000ull ||
            !cpu.memory.is_writable_linear_range(first, size))
            return std::nullopt;
        return first;
    } catch (const MemoryAccessError&) {
        return std::nullopt;
    }
}
} // namespace

DreamcastGdRomController::DreamcastGdRomController(
    Memory& memory,
    EventScheduler& scheduler,
    GdRomDrive drive,
    std::function<void(std::uint64_t)> completion_observer,
    ModuleLoadObserver module_load_observer,
    std::function<void()> command_ack_observer)
    : memory_(memory), scheduler_(scheduler), drive_(std::move(drive)),
      reader_(scheduler, drive_, GdRomTiming{}, completion_observer),
      module_load_observer_(std::move(module_load_observer)),
      completion_observer_(std::move(completion_observer)),
      command_ack_observer_(std::move(command_ack_observer)),
      scheduler_lifetime_(scheduler.lifetime_token()) {
    sector_mode_ = drive_.sector_size() == 2352u
                       ? std::array<std::uint32_t, 4u>{0u, 0x1000u, 0u, 2352u}
                       : std::array<std::uint32_t, 4u>{
                             0u, 0x2000u, 1024u, drive_.sector_size()};
}

DreamcastGdRomController::~DreamcastGdRomController() {
    if (packet_event_ && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*packet_event_));
}

std::uint32_t DreamcastGdRomController::read(const std::uint32_t offset,
                                             const MemoryAccessWidth width) {
    pump_completions();
    if (offset == 0x80u) {
        if (width != MemoryAccessWidth::Halfword || taskfile_phase_ != TaskfilePhase::DataIn ||
            (status_ & ata_drq) == 0u || taskfile_phase_remaining_ == 0u)
            throw std::runtime_error("GD-ROM-Datenregister braucht aktives 16-Bit-PIO.");
        if (data_cursor_ >= data_.size() ||
            taskfile_phase_remaining_ > data_.size() - data_cursor_)
            throw std::logic_error("GD-ROM-DataIn-Phase liegt ausserhalb des Datenpuffers.");
        const auto low = data_[data_cursor_++];
        --taskfile_phase_remaining_;
        auto high = std::uint8_t{0u};
        if (taskfile_phase_remaining_ != 0u && data_cursor_ < data_.size()) {
            high = data_[data_cursor_++];
            --taskfile_phase_remaining_;
        }
        byte_count_ = taskfile_phase_remaining_ == 65'536u
                          ? 0u
                          : static_cast<std::uint16_t>(taskfile_phase_remaining_);
        if (taskfile_phase_remaining_ == 0u) complete_taskfile_data_phase();
        return static_cast<std::uint32_t>(low) | (static_cast<std::uint32_t>(high) << 8u);
    }
    if (width != MemoryAccessWidth::Byte)
        throw std::runtime_error("GD-ROM-Taskfile-Register erfordern 8-Bit-Zugriffe.");
    switch (offset) {
    case 0x18u:
        return status_;
    case 0x84u:
        return error_;
    case 0x88u:
        return interrupt_reason_;
    case 0x8Cu:
        return sector_number_;
    case 0x90u:
        return byte_count_ & 0xFFu;
    case 0x94u:
        return byte_count_ >> 8u;
    case 0x98u:
        return drive_select_;
    case 0x9Cu:
        acknowledge_command_irq();
        return status_;
    default:
        throw std::runtime_error("Unbekannter GD-ROM-Taskfile-Leseoffset.");
    }
}

void DreamcastGdRomController::write(const std::uint32_t offset,
                                     const std::uint32_t value,
                                     const MemoryAccessWidth width) {
    if (offset == 0x80u) {
        if (width != MemoryAccessWidth::Halfword)
            throw std::runtime_error("GD-ROM-Datenregister braucht eine 16-Bit-Paketphase.");
        if (taskfile_phase_ == TaskfilePhase::PacketIn && expecting_packet_) {
            packet_.push_back(static_cast<std::uint8_t>(value));
            packet_.push_back(static_cast<std::uint8_t>(value >> 8u));
            if (packet_.size() == 12u) schedule_packet();
            return;
        }
        if (taskfile_phase_ == TaskfilePhase::DataOut && taskfile_phase_remaining_ != 0u) {
            if (data_cursor_ >= data_.size() ||
                taskfile_phase_remaining_ > data_.size() - data_cursor_)
                throw std::logic_error("GD-ROM-DataOut-Phase liegt ausserhalb des Datenpuffers.");
            data_[data_cursor_++] = static_cast<std::uint8_t>(value);
            --taskfile_phase_remaining_;
            if (taskfile_phase_remaining_ != 0u && data_cursor_ < data_.size()) {
                data_[data_cursor_++] = static_cast<std::uint8_t>(value >> 8u);
                --taskfile_phase_remaining_;
            }
            byte_count_ = taskfile_phase_remaining_ == 65'536u
                              ? 0u
                              : static_cast<std::uint16_t>(taskfile_phase_remaining_);
            if (taskfile_phase_remaining_ == 0u) complete_taskfile_data_phase();
            return;
        }
        throw std::runtime_error("GD-ROM-Datenregister besitzt keine aktive Schreibphase.");
    }
    if (width != MemoryAccessWidth::Byte)
        throw std::runtime_error("GD-ROM-Taskfile-Register erfordern 8-Bit-Zugriffe.");
    switch (offset) {
    case 0x18u:
        if ((value & 0x04u) != 0u) reset_transport();
        return;
    case 0x84u:
        features_ = static_cast<std::uint8_t>(value);
        return;
    case 0x88u:
        sector_count_register_ = static_cast<std::uint8_t>(value);
        return;
    case 0x8Cu:
        sector_number_ = static_cast<std::uint8_t>(value);
        return;
    case 0x90u:
        byte_count_ = static_cast<std::uint16_t>((byte_count_ & 0xFF00u) | (value & 0xFFu));
        return;
    case 0x94u:
        byte_count_ = static_cast<std::uint16_t>((byte_count_ & 0x00FFu) | ((value & 0xFFu) << 8u));
        return;
    case 0x98u:
        drive_select_ = static_cast<std::uint8_t>(value & 0xF0u);
        return;
    case 0x9Cu:
        if (drive_owner_ == DriveOwner::Bios) {
            status_ = ata_busy;
            interrupt_reason_ = 0u;
            return;
        }
        if (taskfile_phase_ != TaskfilePhase::Idle) return;
        drive_owner_ = DriveOwner::Taskfile;
        taskfile_command_failed_ = false;
        error_ = static_cast<std::uint8_t>(error_ & ~0x04u);
        if ((value & 0xFFu) == 0xEFu) {
            taskfile_phase_ = TaskfilePhase::Executing;
            status_ = ata_busy;
            interrupt_reason_ = 0u;
            if (features_ != 0x13u || sector_count_register_ != 0x22u) {
                fail_taskfile_command(5u, 0x24u, 0u, true);
                return;
            }
            finish_taskfile_command();
            return;
        }
        if ((value & 0xFFu) != 0xA0u) {
            fail_taskfile_command(5u, 0x20u, 0u, true);
            return;
        }
        packet_.clear();
        expecting_packet_ = true;
        taskfile_host_byte_limit_ = byte_count_ == 0u ? 65'536u : byte_count_;
        taskfile_phase_ = TaskfilePhase::PacketIn;
        status_ = ata_drq;
        interrupt_reason_ = 1u;
        return;
    default:
        throw std::runtime_error("Unbekannter GD-ROM-Taskfile-Schreiboffset.");
    }
}

void DreamcastGdRomController::publish_data(std::vector<std::uint8_t> data) {
    data_ = std::move(data);
    data_cursor_ = 0u;
    taskfile_phase_remaining_ = 0u;
    if (data_.empty()) {
        if (clear_sense_after_data_) clear_sense();
        finish_taskfile_command();
        return;
    }
    taskfile_phase_ = TaskfilePhase::DataIn;
    begin_next_taskfile_data_phase();
}

void DreamcastGdRomController::publish_dma_data(std::vector<std::uint8_t> data) {
    data_ = std::move(data);
    data_cursor_ = 0u;
    taskfile_phase_remaining_ = 0u;
    byte_count_ = 0u;
    if (data_.empty()) {
        finish_taskfile_command();
        return;
    }
    taskfile_phase_ = TaskfilePhase::DmaIn;
    status_ = ata_busy;
    interrupt_reason_ = 0u;
}

void DreamcastGdRomController::begin_data_out(const std::size_t size,
                                              const std::uint8_t mode_offset) {
    data_.assign(size, 0u);
    data_cursor_ = 0u;
    taskfile_phase_remaining_ = 0u;
    set_mode_offset_ = mode_offset;
    if (data_.empty()) {
        finish_taskfile_command();
        return;
    }
    taskfile_phase_ = TaskfilePhase::DataOut;
    begin_next_taskfile_data_phase();
}

void DreamcastGdRomController::begin_next_taskfile_data_phase() {
    if (data_cursor_ > data_.size())
        throw std::logic_error("GD-ROM-PIO-Cursor liegt hinter dem Datenpuffer.");
    const auto remaining = data_.size() - data_cursor_;
    if (remaining == 0u) {
        complete_taskfile_data_phase();
        return;
    }
    const auto phase_size = std::min<std::size_t>(remaining, taskfile_host_byte_limit_);
    if (phase_size == 0u || phase_size > 65'536u)
        throw std::logic_error("GD-ROM-PIO-Phasengroesse ist ungueltig.");
    taskfile_phase_remaining_ = static_cast<std::uint32_t>(phase_size);
    byte_count_ = phase_size == 65'536u ? 0u : static_cast<std::uint16_t>(phase_size);
    status_ = ata_drq;
    interrupt_reason_ = taskfile_phase_ == TaskfilePhase::DataIn ? 2u : 0u;
    raise_command_irq(scheduler_.current_cycle());
}

void DreamcastGdRomController::complete_taskfile_data_phase() {
    if (data_cursor_ < data_.size()) {
        status_ = ata_busy;
        interrupt_reason_ = 0u;
        begin_next_taskfile_data_phase();
        return;
    }
    if (taskfile_phase_ == TaskfilePhase::DataOut) {
        if (static_cast<std::size_t>(set_mode_offset_) + data_.size() > drive_mode_.size()) {
            fail_taskfile_command(5u, 0x24u, 0u, false);
            return;
        }
        std::copy(data_.begin(),
                  data_.end(),
                  drive_mode_.begin() + static_cast<std::ptrdiff_t>(set_mode_offset_));
    }
    if (clear_sense_after_data_) clear_sense();
    finish_taskfile_command();
}

void DreamcastGdRomController::finish_taskfile_command() {
    expecting_packet_ = false;
    taskfile_phase_remaining_ = 0u;
    byte_count_ = 0u;
    taskfile_phase_ = TaskfilePhase::Idle;
    if (drive_owner_ == DriveOwner::Taskfile) drive_owner_ = DriveOwner::None;
    status_ = static_cast<std::uint8_t>(ata_ready |
                                        (taskfile_command_failed_ ? ata_error : 0u));
    interrupt_reason_ = 3u;
    clear_sense_after_data_ = false;
    ++completed_commands_;
    raise_command_irq(scheduler_.current_cycle());
}

void DreamcastGdRomController::latch_sense(const std::uint8_t sense_key,
                                           const std::uint8_t asc,
                                           const std::uint8_t ascq,
                                           const bool ata_abort) noexcept {
    sense_key_ = static_cast<std::uint8_t>(sense_key & 0x0Fu);
    sense_asc_ = asc;
    sense_ascq_ = ascq;
    error_ = static_cast<std::uint8_t>((sense_key_ << 4u) | (ata_abort ? 0x04u : 0u));
    status_ = drive_owner_ == DriveOwner::Bios
                  ? ata_busy
                  : static_cast<std::uint8_t>((status_ & ~ata_busy) | ata_ready | ata_error);
}

void DreamcastGdRomController::clear_sense() noexcept {
    sense_key_ = 0u;
    sense_asc_ = 0u;
    sense_ascq_ = 0u;
    error_ = 0u;
    status_ = static_cast<std::uint8_t>(status_ & ~ata_error);
}

void DreamcastGdRomController::fail_taskfile_command(const std::uint8_t sense_key,
                                                     const std::uint8_t asc,
                                                     const std::uint8_t ascq,
                                                     const bool ata_abort) {
    data_.clear();
    data_cursor_ = 0u;
    taskfile_phase_remaining_ = 0u;
    clear_sense_after_data_ = false;
    taskfile_command_failed_ = true;
    latch_sense(sense_key, asc, ascq, ata_abort);
    finish_taskfile_command();
}

void DreamcastGdRomController::raise_command_irq(const std::uint64_t cycle) {
    if (command_irq_asserted_) {
        command_irq_reassert_pending_ = true;
        return;
    }
    command_irq_asserted_ = true;
    if (completion_observer_) completion_observer_(cycle);
}

void DreamcastGdRomController::acknowledge_command_irq() {
    if (!command_irq_asserted_) return;
    command_irq_asserted_ = false;
    if (command_ack_observer_) {
        try {
            command_ack_observer_();
        } catch (...) {
        }
    }
    if (command_irq_reassert_pending_) {
        command_irq_reassert_pending_ = false;
        raise_command_irq(scheduler_.current_cycle());
    }
}

bool DreamcastGdRomController::taskfile_blocks_bios() const noexcept {
    return drive_owner_ == DriveOwner::Taskfile || taskfile_phase_ != TaskfilePhase::Idle;
}

void DreamcastGdRomController::release_bios_owner_if_idle() noexcept {
    if (bios_requests_.empty() && drive_owner_ == DriveOwner::Bios) {
        drive_owner_ = DriveOwner::None;
        status_ = ata_ready;
        interrupt_reason_ = 3u;
    }
}

void DreamcastGdRomController::execute_packet() {
    expecting_packet_ = false;
    if (packet_.size() != 12u) throw std::logic_error("GD-ROM-Paket ist nicht vollstaendig.");
    try {
        switch (packet_[0]) {
        case 0x00u: {
            const auto response = drive_.execute({GdRomCommand::TestUnitReady});
            if (response.status != GdRomStatus::Good) {
                const auto sense = packet_sense_for_status(response.status);
                fail_taskfile_command(sense[0], sense[1], sense[2], false);
                return;
            }
            publish_data({});
            return;
        }
        case 0x10u: {
            const auto offset = static_cast<std::size_t>(packet_[2]);
            const auto count = static_cast<std::size_t>(packet_[4]);
            constexpr std::size_t response_size = 10u;
            if (offset > response_size || count > response_size - offset)
                throw std::invalid_argument("GD-ROM REQ_STAT liegt ausserhalb des Statuspuffers.");

            const auto has_media = drive_.sector_count() != 0u;
            const auto& layout = drive_.layout();
            const auto gd_rom = std::any_of(layout.begin(), layout.end(), [](const auto& track) {
                return track.session > 1u;
            });
            const auto has_data = std::any_of(layout.begin(), layout.end(), [](const auto& track) {
                return track.kind == DiscTrackKind::Data;
            });
            const auto disc_format = !has_media ? 0u : gd_rom ? 8u : has_data ? 1u : 0u;
            const auto current_lba = current_fad_ >= 150u ? current_fad_ - 150u : 0u;
            const DiscTrackLayout* active_track = nullptr;
            for (const auto& track : layout) {
                if (track.lba > current_lba) continue;
                if (active_track == nullptr || track.lba >= active_track->lba)
                    active_track = &track;
            }
            if (active_track == nullptr && !layout.empty()) active_track = &layout.front();
            const auto track_number = has_media && active_track != nullptr
                                          ? std::min<std::uint32_t>(active_track->number, 0xFFu)
                                          : 0u;
            const auto control = active_track != nullptr &&
                                         active_track->kind == DiscTrackKind::Data
                                     ? 4u
                                     : 0u;
            const std::array<std::uint8_t, response_size> response{
                static_cast<std::uint8_t>(has_media ? 1u : 7u),
                static_cast<std::uint8_t>(disc_format << 4u),
                static_cast<std::uint8_t>((control << 4u) | 1u),
                static_cast<std::uint8_t>(track_number),
                static_cast<std::uint8_t>(has_media ? 1u : 0u),
                static_cast<std::uint8_t>(current_fad_ >> 16u),
                static_cast<std::uint8_t>(current_fad_ >> 8u),
                static_cast<std::uint8_t>(current_fad_),
                0u,
                0u};
            publish_data(std::vector<std::uint8_t>(
                response.begin() + static_cast<std::ptrdiff_t>(offset),
                response.begin() + static_cast<std::ptrdiff_t>(offset + count)));
            return;
        }
        case 0x11u: {
            const auto offset = static_cast<std::size_t>(packet_[2]);
            const auto count = static_cast<std::size_t>(packet_[4]);
            if ((offset & 1u) != 0u || offset > gdrom_hardware_info_size ||
                count > gdrom_hardware_info_size - offset)
                throw std::invalid_argument("GD-ROM REQ_MODE liegt ausserhalb des Modepuffers.");
            publish_data(std::vector<std::uint8_t>(drive_mode_.begin() +
                                                       static_cast<std::ptrdiff_t>(offset),
                                                   drive_mode_.begin() +
                                                       static_cast<std::ptrdiff_t>(offset + count)));
            return;
        }
        case 0x12u: {
            const auto offset = static_cast<std::size_t>(packet_[2]);
            const auto count = static_cast<std::size_t>(packet_[4]);
            if ((offset & 1u) != 0u || offset > gdrom_writable_mode_size ||
                count > gdrom_writable_mode_size - offset)
                throw std::invalid_argument("GD-ROM SET_MODE liegt ausserhalb des Modepuffers.");
            begin_data_out(count, static_cast<std::uint8_t>(offset));
            return;
        }
        case 0x13u: {
            std::array<std::uint8_t, 10u> response{0xF0u,
                                                   0u,
                                                   sense_key_,
                                                   0u,
                                                   0u,
                                                   0u,
                                                   0u,
                                                   0u,
                                                   sense_asc_,
                                                   sense_ascq_};
            const auto count = std::min<std::size_t>(packet_[4], response.size());
            clear_sense_after_data_ = true;
            publish_data(std::vector<std::uint8_t>(response.begin(),
                                                   response.begin() +
                                                       static_cast<std::ptrdiff_t>(count)));
            return;
        }
        case 0x14u: {
            std::vector<std::uint8_t> toc_bytes;
            toc_bytes.reserve(408u);
            for (const auto word : build_bios_toc(packet_[1] & 1u))
                append_le32(toc_bytes, word);
            const auto allocation = be16(packet_, 3u);
            toc_bytes.resize(std::min<std::size_t>(toc_bytes.size(), allocation));
            publish_data(std::move(toc_bytes));
            return;
        }
        case 0x28u: {
            const auto lba = be32(packet_, 2u);
            const auto sector_count = be16(packet_, 7u);
            auto response = drive_.execute(
                {GdRomCommand::ReadSectors, lba, sector_count});
            if (response.status != GdRomStatus::Good) {
                const auto sense = packet_sense_for_status(response.status);
                fail_taskfile_command(sense[0], sense[1], sense[2], false);
                return;
            }
            const auto last_fad = static_cast<std::uint64_t>(lba) + 150u + sector_count - 1u;
            if (last_fad > 0x00FFFFFFu)
                throw std::out_of_range("GD-ROM READ(10)-End-FAD ist nicht darstellbar.");
            current_fad_ = static_cast<std::uint32_t>(last_fad);
            publish_data(std::move(response.data));
            return;
        }
        case 0x30u: {
            if ((features_ & 0xFEu) != 0u)
                throw std::invalid_argument("GD-ROM CD_READ besitzt reservierte Featurebits.");
            const auto fad = be24(packet_, 2u);
            const auto sector_count = be24(packet_, 8u);
            if (fad < 150u || sector_count == 0u)
                throw std::invalid_argument("GD-ROM CD_READ besitzt ungueltige FAD-/Laengenfelder.");
            const auto last_fad = static_cast<std::uint64_t>(fad) + sector_count - 1u;
            if (last_fad > 0x00FFFFFFu)
                throw std::out_of_range("GD-ROM CD_READ-End-FAD ist nicht darstellbar.");
            auto response = drive_.execute({GdRomCommand::ReadSectors,
                                            fad_to_lba(fad),
                                            sector_count});
            if (response.status != GdRomStatus::Good) {
                const auto sense = packet_sense_for_status(response.status);
                fail_taskfile_command(sense[0], sense[1], sense[2], false);
                return;
            }
            current_fad_ = static_cast<std::uint32_t>(last_fad);
            if ((features_ & 1u) != 0u)
                publish_dma_data(std::move(response.data));
            else
                publish_data(std::move(response.data));
            return;
        }
        case 0x43u:
            publish_data(build_packet_toc(0u));
            return;
        default:
            throw std::runtime_error("Unbekannter GD-ROM-Paketopcode.");
        }
    } catch (const std::out_of_range&) {
        fail_taskfile_command(5u, 0x21u, 0u, false);
    } catch (const std::invalid_argument&) {
        fail_taskfile_command(5u, 0x24u, 0u, false);
    } catch (const std::exception&) {
        fail_taskfile_command(5u, 0x20u, 0u, false);
    }
}

void DreamcastGdRomController::schedule_packet() {
    if (packet_event_) throw std::logic_error("GD-ROM-Paketkommando ist bereits aktiv.");
    expecting_packet_ = false;
    taskfile_phase_ = TaskfilePhase::Executing;
    status_ = ata_busy;
    interrupt_reason_ = 0u;
    packet_event_ = scheduler_.schedule_after(
        1'000u,
        [this](const auto event_id, const auto cycle) { complete_packet(event_id, cycle); });
}

void DreamcastGdRomController::complete_packet(const SchedulerEventId event_id,
                                               const std::uint64_t cycle) {
    if (!packet_event_ || *packet_event_ != event_id) return;
    packet_event_.reset();
    try {
        execute_packet();
    } catch (...) {
        fail_taskfile_command(5u, 0x20u, 0u, false);
    }
    static_cast<void>(cycle);
}

std::uint32_t DreamcastGdRomController::fad_to_lba(const std::uint32_t fad) noexcept {
    return fad >= 150u ? fad - 150u : fad;
}

std::vector<std::uint8_t>
DreamcastGdRomController::build_packet_toc(const std::uint32_t session) const {
    std::vector<std::uint8_t> result;
    const auto& layout = drive_.layout();
    for (const auto& track : layout) {
        if (session != 0u && track.session != session) continue;
        const auto control = track.kind == DiscTrackKind::Data ? 4u : 0u;
        append_be32(result, (control << 28u) | (1u << 24u) | (track.lba + 150u));
    }
    if (!layout.empty()) {
        const auto& last = layout.back();
        append_be32(result, static_cast<std::uint32_t>(last.lba + last.sector_count + 150u));
    }
    return result;
}

std::array<std::uint32_t, 102u>
DreamcastGdRomController::build_bios_toc(const std::uint32_t area) const {
    std::array<std::uint32_t, 102u> result{};
    result.fill(0xFFFFFFFFu);
    if (area > 1u) return result;
    const auto& layout = drive_.layout();
    if (layout.empty()) return result;
    const auto [minimum_session, maximum_session] = std::minmax_element(
        layout.begin(), layout.end(), [](const auto& left, const auto& right) {
            return left.session < right.session;
        });
    const auto multi_area = minimum_session->session != maximum_session->session;
    const auto selected_session = area == 0u ? minimum_session->session : maximum_session->session;
    std::vector<const DiscTrackLayout*> selected;
    for (const auto& track : layout) {
        if ((!multi_area && area == 0u) || (multi_area && track.session == selected_session))
            selected.push_back(&track);
    }
    if (selected.empty()) return result;
    const auto encode = [](const DiscTrackLayout& track, const std::uint32_t fad) {
        const auto control = track.kind == DiscTrackKind::Data ? 4u : 0u;
        return (control << 28u) | (1u << 24u) | (fad & 0x00FFFFFFu);
    };
    for (const auto* track : selected) {
        if (track->number == 0u || track->number > 99u) continue;
        result[track->number - 1u] = encode(*track, track->lba + 150u);
    }
    const auto* first = selected.front();
    const auto* last = selected.back();
    result[99] = encode(*first, first->number << 16u);
    result[100] = encode(*last, last->number << 16u);
    const auto leadout = static_cast<std::uint64_t>(last->lba) + last->sector_count + 150u;
    if (leadout <= 0x00FFFFFFu)
        result[101] = encode(*last, static_cast<std::uint32_t>(leadout));
    return result;
}

void DreamcastGdRomController::submit_bios_read(CpuState& cpu, BiosRequest& request) {
    const auto byte_count = static_cast<std::uint64_t>(request.parameters[1]) *
                            drive_.sector_size();
    if (byte_count == 0u || byte_count > std::numeric_limits<std::size_t>::max()) {
        request.response.status = GdRomStatus::InvalidField;
        request.status = {5u, static_cast<std::uint32_t>(GdRomStatus::InvalidField), 0u, 0u};
        request.state = GdRomBiosRequestState::Error;
        latch_sense(5u, 0x24u, 0u);
        remember_bios_request(request);
        return;
    }
    const auto destination = resolve_bios_write_destination(
        cpu, request.parameters[2], static_cast<std::size_t>(byte_count));
    if (!destination) {
        request.response.status = GdRomStatus::InvalidField;
        request.status = {5u, static_cast<std::uint32_t>(GdRomStatus::InvalidField), 0u, 0u};
        request.state = GdRomBiosRequestState::Error;
        latch_sense(5u, 0x24u, 0u);
        remember_bios_request(request);
        return;
    }
    request.destination = *destination;
    request.write_source = request.command == 17u ? CodeWriteSource::Dma : CodeWriteSource::Copy;
    request.async_id = reader_.submit({GdRomCommand::ReadSectors,
                                       fad_to_lba(request.parameters[0]),
                                       request.parameters[1]});
    request.state = GdRomBiosRequestState::Processing;
    request.status[3] = 4u;
    status_ = ata_busy;
    interrupt_reason_ = 0u;
    remember_bios_request(request);
}

void DreamcastGdRomController::submit_bios_stream(BiosRequest& request) {
    const auto lba = static_cast<std::uint64_t>(fad_to_lba(request.parameters[0]));
    const auto available = drive_.sector_count();
    if (lba >= available || request.parameters[1] == 0u) {
        request.response.status = GdRomStatus::OutOfRange;
        request.status = {5u, static_cast<std::uint32_t>(GdRomStatus::OutOfRange), 0u, 0u};
        request.state = GdRomBiosRequestState::Error;
        latch_sense(5u, 0x21u, 0u);
        remember_bios_request(request);
        return;
    }
    const auto sectors = request.parameters[1] == 0x1FFu
                             ? available - lba
                             : static_cast<std::uint64_t>(request.parameters[1]);
    if (sectors == 0u || sectors > available - lba ||
        sectors > std::numeric_limits<std::uint32_t>::max() ||
        sectors > std::numeric_limits<std::uint32_t>::max() / drive_.sector_size()) {
        request.response.status = GdRomStatus::OutOfRange;
        request.status = {5u, static_cast<std::uint32_t>(GdRomStatus::OutOfRange), 0u, 0u};
        request.state = GdRomBiosRequestState::Error;
        latch_sense(5u, 0x21u, 0u);
        remember_bios_request(request);
        return;
    }
    request.streaming_dma = request.command == bios_command_dma_stream;
    request.stream_lba = static_cast<std::uint32_t>(lba);
    request.stream_sector_count = static_cast<std::uint32_t>(sectors);
    request.stream_total_bytes = sectors * drive_.sector_size();
    request.stream_consumed_bytes = 0u;
    request.cached_stream_sector = std::numeric_limits<std::uint32_t>::max();
    request.stream_sector_cache.clear();
    request.async_id = reader_.submit({GdRomCommand::TestUnitReady});
    request.state = GdRomBiosRequestState::Processing;
    request.status = {0u, 0u, 0u, 4u};
    status_ = ata_busy;
    interrupt_reason_ = 0u;
    remember_bios_request(request);
}

std::vector<std::uint8_t>
DreamcastGdRomController::preview_stream_bytes(BiosRequest& request,
                                               const std::uint32_t length) {
    if (length == 0u || request.stream_consumed_bytes > request.stream_total_bytes ||
        length > request.stream_total_bytes - request.stream_consumed_bytes)
        throw std::out_of_range("GD-ROM-Streaming fordert mehr Bytes als verbleiben.");
    std::vector<std::uint8_t> result;
    result.reserve(length);
    auto position = request.stream_consumed_bytes;
    while (result.size() != length) {
        const auto relative_sector = position / drive_.sector_size();
        const auto within_sector = static_cast<std::size_t>(position % drive_.sector_size());
        if (relative_sector >= request.stream_sector_count)
            throw std::out_of_range("GD-ROM-Streamingcursor liegt hinter dem Request.");
        const auto sector = request.stream_lba + static_cast<std::uint32_t>(relative_sector);
        if (request.cached_stream_sector != sector) {
            auto response = drive_.execute({GdRomCommand::ReadSectors, sector, 1u});
            if (response.status != GdRomStatus::Good ||
                response.data.size() != drive_.sector_size())
                throw std::runtime_error("GD-ROM-Streamingsektor konnte nicht gelesen werden.");
            request.cached_stream_sector = sector;
            request.stream_sector_cache = std::move(response.data);
        }
        const auto take = std::min<std::size_t>(
            length - result.size(),
            within_sector <= request.stream_sector_cache.size()
                ? request.stream_sector_cache.size() - within_sector
                : 0u);
        if (take == 0u)
            throw std::logic_error("GD-ROM-Streamingsektor passt nicht zum Sektorcursor.");
        result.insert(result.end(),
                      request.stream_sector_cache.begin() +
                          static_cast<std::ptrdiff_t>(within_sector),
                      request.stream_sector_cache.begin() +
                          static_cast<std::ptrdiff_t>(within_sector + take));
        position += take;
    }
    return result;
}

void DreamcastGdRomController::commit_stream_bytes(BiosRequest& request,
                                                   const std::uint32_t length) {
    if (!request.transfer_active || request.transfer_transferred > request.transfer_size ||
        request.stream_consumed_bytes > request.stream_total_bytes ||
        length > request.transfer_size - request.transfer_transferred ||
        length > request.stream_total_bytes - request.stream_consumed_bytes)
        throw std::logic_error("GD-ROM-Streamingcommit passt nicht zum aktiven Transfer.");
    request.transfer_transferred += length;
    request.stream_consumed_bytes += length;
    request.status[2] = static_cast<std::uint32_t>(request.stream_consumed_bytes);
    if (request.transfer_transferred == request.transfer_size) finish_stream_transfer(request);
    remember_bios_request(request);
}

void DreamcastGdRomController::queue_stream_callback(const std::uint32_t request_id,
                                                     const GdRomBiosTransferKind kind) {
    const auto address = kind == GdRomBiosTransferKind::Dma ? dma_callback_ : pio_callback_;
    const auto argument = kind == GdRomBiosTransferKind::Dma ? dma_callback_argument_
                                                             : pio_callback_argument_;
    if (address == 0u) return;
    pending_guest_callbacks_.push_back({kind, address, argument, request_id});
    if (kind == GdRomBiosTransferKind::Dma) {
        dma_completion_pending_ = false;
        dma_completion_request_ = 0u;
    } else {
        pio_completion_pending_ = false;
        pio_completion_request_ = 0u;
    }
}

void DreamcastGdRomController::finish_stream_transfer(BiosRequest& request) {
    const auto kind = request.transfer_kind;
    request.transfer_active = false;
    request.status[3] = 0u;
    status_ = drive_owner_ == DriveOwner::Bios ? ata_busy : ata_ready;
    interrupt_reason_ = 3u;
    if (request.stream_consumed_bytes == request.stream_total_bytes) {
        request.state = GdRomBiosRequestState::Complete;
        ++completed_commands_;
    } else {
        request.state = GdRomBiosRequestState::Streaming;
    }
    if (kind == GdRomBiosTransferKind::Dma) {
        ++completed_dma_;
        dma_completion_pending_ = true;
        dma_completion_request_ = request.id;
    } else {
        pio_completion_pending_ = true;
        pio_completion_request_ = request.id;
        queue_stream_callback(request.id, kind);
    }
}

DreamcastGdRomController::BiosRequest*
DreamcastGdRomController::active_stream_transfer(const GdRomBiosTransferKind kind) noexcept {
    const auto found = std::find_if(bios_requests_.begin(), bios_requests_.end(), [&](auto& entry) {
        return entry.second.transfer_active && entry.second.transfer_kind == kind;
    });
    return found == bios_requests_.end() ? nullptr : &found->second;
}

void DreamcastGdRomController::execute_bios_request(CpuState& cpu, BiosRequest& request) {
    if (request.state != GdRomBiosRequestState::Queued) return;
    if (request.command == bios_command_pio_read || request.command == bios_command_dma_read) {
        submit_bios_read(cpu, request);
        return;
    }
    if (request.command == bios_command_dma_stream ||
        request.command == bios_command_pio_stream) {
        submit_bios_stream(request);
        return;
    }
    if (request.command == 18u || request.command == 19u) {
        if (request.parameters[0] > 1u || request.parameters[1] == 0u) {
            request.response.status = GdRomStatus::InvalidField;
            request.status = {5u, static_cast<std::uint32_t>(GdRomStatus::InvalidField), 0u, 0u};
            request.state = GdRomBiosRequestState::Error;
            latch_sense(5u, 0x24u, 0u);
            remember_bios_request(request);
            return;
        }
        const auto toc = build_bios_toc(request.parameters[0]);
        std::vector<std::uint8_t> toc_bytes;
        toc_bytes.reserve(toc.size() * sizeof(std::uint32_t));
        for (const auto word : toc) append_le32(toc_bytes, word);
        const auto destination =
            resolve_bios_write_destination(cpu, request.parameters[1], toc_bytes.size());
        if (!destination) {
            request.response.status = GdRomStatus::InvalidField;
            request.status =
                {5u, static_cast<std::uint32_t>(GdRomStatus::InvalidField), 0u, 0u};
            request.state = GdRomBiosRequestState::Error;
            latch_sense(5u, 0x24u, 0u);
            remember_bios_request(request);
            return;
        }
        memory_.write_bytes(*destination, toc_bytes, CodeWriteSource::Copy);
        request.status = {0u, 0u, static_cast<std::uint32_t>(toc.size() * 4u), 0u};
        request.state = GdRomBiosRequestState::Complete;
        remember_bios_request(request);
        ++completed_commands_;
        return;
    }
    if (request.command == 24u) {
        request.status = {};
        request.state = GdRomBiosRequestState::Complete;
        remember_bios_request(request);
        ++completed_commands_;
        return;
    }
    request.response.status = GdRomStatus::InvalidCommand;
    request.status = {5u, static_cast<std::uint32_t>(GdRomStatus::InvalidCommand), 0u, 0u};
    request.state = GdRomBiosRequestState::Error;
    latch_sense(5u, 0x20u, 0u);
    remember_bios_request(request);
}

void DreamcastGdRomController::pump_completions() {
    while (auto completion = reader_.take_completed()) {
        const auto found = std::find_if(bios_requests_.begin(), bios_requests_.end(),
                                        [&](const auto& entry) {
                                            return entry.second.async_id == completion->request_id;
                                        });
        if (found == bios_requests_.end()) continue;
        found->second.response = std::move(completion->response);
        if (found->second.stream_total_bytes != 0u) {
            if (found->second.response.status == GdRomStatus::Good) {
                found->second.status = {0u, 0u, 0u, 0u};
                found->second.state = GdRomBiosRequestState::Streaming;
                status_ = drive_owner_ == DriveOwner::Bios ? ata_busy : ata_ready;
                interrupt_reason_ = 3u;
            } else {
                const auto sense = packet_sense_for_status(found->second.response.status);
                found->second.status = {sense[0],
                                        static_cast<std::uint32_t>(
                                            found->second.response.status),
                                        0u,
                                        0u};
                found->second.state = GdRomBiosRequestState::Error;
                latch_sense(sense[0], sense[1], sense[2]);
            }
            remember_bios_request(found->second);
            continue;
        }
        if (found->second.response.status == GdRomStatus::Good &&
            !found->second.response.data.empty()) {
            try {
                memory_.write_bytes(found->second.destination,
                                    found->second.response.data,
                                    found->second.write_source);
                if (module_load_observer_)
                    module_load_observer_(canonical_physical_address(found->second.destination),
                                          found->second.response.data,
                                          drive_.identity());
            } catch (const MemoryAccessError&) {
                found->second.response.status = GdRomStatus::InvalidField;
                found->second.response.data.clear();
                found->second.response.transferred_sectors = 0u;
            }
        }
        const auto transferred_bytes = found->second.response.data.empty()
                                           ? static_cast<std::uint64_t>(
                                                 found->second.response.transferred_sectors) *
                                                 drive_.sector_size()
                                           : found->second.response.data.size();
        if (found->second.response.status == GdRomStatus::Good &&
            transferred_bytes <= std::numeric_limits<std::uint32_t>::max()) {
            found->second.status =
                {0u, 0u, static_cast<std::uint32_t>(transferred_bytes), 0u};
            found->second.state = GdRomBiosRequestState::Complete;
            if ((found->second.command == bios_command_pio_read ||
                 found->second.command == bios_command_dma_read) &&
                found->second.parameters[0] >= 150u &&
                found->second.response.transferred_sectors != 0u) {
                const auto last_fad = static_cast<std::uint64_t>(found->second.parameters[0]) +
                                      found->second.response.transferred_sectors - 1u;
                if (last_fad <= 0x00FFFFFFu)
                    current_fad_ = static_cast<std::uint32_t>(last_fad);
            }
            status_ = drive_owner_ == DriveOwner::Bios ? ata_busy : ata_ready;
            interrupt_reason_ = 3u;
        } else {
            const auto sense = packet_sense_for_status(found->second.response.status);
            found->second.status = {sense[0],
                                    static_cast<std::uint32_t>(found->second.response.status),
                                    0u,
                                    0u};
            found->second.state = GdRomBiosRequestState::Error;
            latch_sense(sense[0], sense[1], sense[2]);
        }
        remember_bios_request(found->second);
        ++completed_commands_;
    }
}

bool DreamcastGdRomController::reload_system_bootstrap(CpuState& cpu) {
    constexpr std::uint32_t destination = 0x8C008100u;
    constexpr std::uint32_t sector_count = 7u;
    const auto& layout = drive_.layout();
    const auto track = std::max_element(
        layout.begin(), layout.end(), [](const auto& left, const auto& right) {
            if (left.kind != right.kind) return left.kind == DiscTrackKind::Audio;
            if (left.session != right.session) return left.session < right.session;
            return left.lba < right.lba;
        });
    if (track == layout.end() || track->kind != DiscTrackKind::Data ||
        track->sector_count < sector_count)
        return false;
    const auto response = drive_.execute({GdRomCommand::ReadSectors, track->lba, sector_count});
    if (response.status != GdRomStatus::Good ||
        response.data.size() != static_cast<std::size_t>(sector_count) * 2048u ||
        !cpu.memory.contains(destination, response.data.size()))
        return false;
    cpu.memory.write_bytes(destination, response.data, CodeWriteSource::Copy);
    if (module_load_observer_)
        module_load_observer_(
            canonical_physical_address(destination), response.data, drive_.identity());
    return true;
}

const DreamcastGdRomController::BiosRequest*
DreamcastGdRomController::find_bios_request(const std::uint32_t id) const noexcept {
    const auto found = bios_requests_.find(id);
    return found == bios_requests_.end() ? nullptr : &found->second;
}

std::uint32_t DreamcastGdRomController::finish_bios_call(GdRomBiosCallEvent event,
                                                         const std::uint32_t result) {
    event.result = result;
    if (event.selector == 0u && event.super_selector == 0u && result != 0u &&
        result != 0xFFFFFFFFu)
        event.request_id = result;
    if (const auto* request = find_bios_request(event.request_id)) {
        event.state_after = request->state;
        event.status = request->status;
    } else if (last_bios_request_.id == event.request_id) {
        event.state_after = last_bios_request_.state;
        event.status = last_bios_request_.status;
    }
    constexpr std::size_t event_capacity = 256u;
    if (bios_call_events_.size() == event_capacity) {
        bios_call_events_.erase(bios_call_events_.begin());
        ++dropped_bios_call_events_;
    }
    bios_call_events_.push_back(std::move(event));
    return result;
}

std::uint32_t DreamcastGdRomController::bios_call(CpuState& cpu,
                                                  const std::uint32_t selector,
                                                  const std::uint32_t super_selector) {
    GdRomBiosCallEvent event;
    event.sequence = next_bios_call_sequence_++;
    event.guest_cycle = scheduler_.current_cycle();
    event.callsite = cpu.pc;
    event.return_address = cpu.pr;
    event.selector = selector;
    event.super_selector = super_selector;
    event.arguments = {cpu.r[4], cpu.r[5], cpu.r[6], cpu.r[7]};
    if (selector == 1u || selector == 6u || selector == 7u || selector == 8u ||
        selector == 12u || selector == 13u) {
        event.request_id = cpu.r[4];
    } else if (selector == 2u) {
        const auto queued = std::find_if(bios_requests_.begin(), bios_requests_.end(),
                                         [](const auto& entry) {
                                             return entry.second.state ==
                                                    GdRomBiosRequestState::Queued;
                                         });
        if (queued != bios_requests_.end()) event.request_id = queued->second.id;
    }
    if (const auto* request = find_bios_request(event.request_id))
        event.state_before = request->state;
    else if (last_bios_request_.id == event.request_id)
        event.state_before = last_bios_request_.state;

    const auto finish = [&](const std::uint32_t result) {
        return finish_bios_call(std::move(event), result);
    };
    if (super_selector == 0xFFFFFFFFu && selector == 0u) {
        reset();
        return finish(0u);
    }
    if (super_selector != 0u) return finish(0xFFFFFFFFu);
    pump_completions();
    if (selector == 0u) {
        if (taskfile_blocks_bios() || !bios_requests_.empty()) return finish(0u);
        if (next_bios_request_ == 0u) next_bios_request_ = 1u;
        BiosRequest request;
        request.id = next_bios_request_++;
        request.command = cpu.r[4];
        if (cpu.r[5] != 0u) {
            try {
                for (std::size_t index = 0u; index < request.parameters.size(); ++index)
                    request.parameters[index] = guest_read_u32(
                        cpu, cpu.r[5] + static_cast<std::uint32_t>(index * 4u));
            } catch (const MemoryAccessError&) {
                return finish(0u);
            }
        }
        const auto id = request.id;
        bios_requests_.emplace(id, std::move(request));
        drive_owner_ = DriveOwner::Bios;
        taskfile_command_failed_ = false;
        status_ = ata_busy;
        interrupt_reason_ = 0u;
        remember_bios_request(bios_requests_.at(id));
        return finish(id);
    }
    if (selector == 1u) {
        const auto found = bios_requests_.find(cpu.r[4]);
        if (found == bios_requests_.end()) return finish(0u);
        if (cpu.r[5] != 0u) {
            std::vector<std::uint8_t> status_bytes;
            status_bytes.reserve(found->second.status.size() * sizeof(std::uint32_t));
            for (const auto word : found->second.status) append_le32(status_bytes, word);
            const auto destination =
                resolve_bios_write_destination(cpu, cpu.r[5], status_bytes.size());
            if (!destination) return finish(0xFFFFFFFFu);
            memory_.write_bytes(*destination, status_bytes, CodeWriteSource::Copy);
        }
        switch (found->second.state) {
        case GdRomBiosRequestState::None:
            return finish(0u);
        case GdRomBiosRequestState::Queued:
        case GdRomBiosRequestState::Processing:
            remember_bios_request(found->second);
            return finish(1u);
        case GdRomBiosRequestState::Streaming:
            remember_bios_request(found->second);
            return finish(3u);
        case GdRomBiosRequestState::Complete:
            remember_bios_request(found->second);
            bios_requests_.erase(found);
            release_bios_owner_if_idle();
            return finish(2u);
        case GdRomBiosRequestState::Error:
            remember_bios_request(found->second);
            bios_requests_.erase(found);
            release_bios_owner_if_idle();
            return finish(0xFFFFFFFFu);
        case GdRomBiosRequestState::Aborted:
            remember_bios_request(found->second);
            bios_requests_.erase(found);
            release_bios_owner_if_idle();
            return finish(0u);
        }
        return finish(0xFFFFFFFFu);
    }
    if (selector == 2u) {
        const auto queued = std::find_if(bios_requests_.begin(), bios_requests_.end(),
                                         [](const auto& entry) {
                                             return entry.second.state ==
                                                    GdRomBiosRequestState::Queued;
                                         });
        if (queued != bios_requests_.end()) execute_bios_request(cpu, queued->second);
        pump_completions();
        return finish(0u);
    }
    if (selector == 8u) {
        const auto found = bios_requests_.find(cpu.r[4]);
        if (found == bios_requests_.end())
            return finish(0xFFFFFFFFu);
        if (found->second.state == GdRomBiosRequestState::Complete) return finish(0u);
        if (found->second.state != GdRomBiosRequestState::Queued &&
            found->second.state != GdRomBiosRequestState::Processing &&
            found->second.state != GdRomBiosRequestState::Streaming)
            return finish(0xFFFFFFFFu);
        auto aborted = std::move(found->second);
        if (aborted.async_id != 0u) static_cast<void>(reader_.cancel(aborted.async_id));
        if (aborted.transfer_active && aborted.transfer_kind == GdRomBiosTransferKind::Dma &&
            g1_bus_ != nullptr)
            g1_bus_->abort_transfer();
        pending_guest_callbacks_.erase(
            std::remove_if(pending_guest_callbacks_.begin(),
                           pending_guest_callbacks_.end(),
                           [&](const auto& callback) {
                               return callback.request_id == aborted.id;
                           }),
            pending_guest_callbacks_.end());
        if (dma_completion_request_ == aborted.id) {
            dma_completion_pending_ = false;
            dma_completion_request_ = 0u;
        }
        if (pio_completion_request_ == aborted.id) {
            pio_completion_pending_ = false;
            pio_completion_request_ = 0u;
        }
        aborted.status = {0u,
                          static_cast<std::uint32_t>(GdRomStatus::Aborted),
                          aborted.status[2],
                          0u};
        aborted.response.status = GdRomStatus::Aborted;
        aborted.state = GdRomBiosRequestState::Aborted;
        bios_requests_.erase(found);
        remember_bios_request(aborted);
        release_bios_owner_if_idle();
        return finish(0u);
    }
    if (selector == 3u) {
        reset();
        return finish(0u);
    }
    if (selector == 9u) {
        reset_transport();
        return finish(0u);
    }
    if (selector == 4u) {
        if (cpu.r[4] != 0u) {
            const auto bios_busy = std::any_of(
                bios_requests_.begin(), bios_requests_.end(), [](const auto& request) {
                    return request.second.state == GdRomBiosRequestState::Queued ||
                           request.second.state == GdRomBiosRequestState::Processing ||
                           request.second.transfer_active;
                });
            const auto busy = bios_busy || taskfile_blocks_bios();
            guest_write_u32(cpu, cpu.r[4], busy ? 0u : 1u, CodeWriteSource::Copy);
            guest_write_u32(cpu, cpu.r[4] + 4u, busy ? 0u : 0x80u, CodeWriteSource::Copy);
        }
        return finish(0u);
    }
    if (selector == 5u) {
        if (!dma_completion_pending_) return finish(0xFFFFFFFFu);
        dma_callback_ = cpu.r[4];
        dma_callback_argument_ = cpu.r[5];
        const auto request_id = dma_completion_request_;
        dma_completion_pending_ = false;
        dma_completion_request_ = 0u;
        if (dma_callback_ != 0u)
            pending_guest_callbacks_.push_back({GdRomBiosTransferKind::Dma,
                                                dma_callback_,
                                                dma_callback_argument_,
                                                request_id});
        return finish(0u);
    }
    if (selector == 11u) {
        pio_callback_ = cpu.r[4];
        pio_callback_argument_ = cpu.r[5];
        if (pio_completion_pending_ && pio_callback_ != 0u)
            queue_stream_callback(pio_completion_request_, GdRomBiosTransferKind::Pio);
        return finish(0u);
    }
    if (selector == 6u || selector == 12u) {
        const auto found = bios_requests_.find(cpu.r[4]);
        if (found == bios_requests_.end() || cpu.r[5] == 0u ||
            found->second.state != GdRomBiosRequestState::Streaming ||
            found->second.transfer_active)
            return finish(0xFFFFFFFFu);
        const auto kind = selector == 6u ? GdRomBiosTransferKind::Dma
                                         : GdRomBiosTransferKind::Pio;
        if (found->second.streaming_dma != (kind == GdRomBiosTransferKind::Dma))
            return finish(0xFFFFFFFFu);
        std::uint32_t destination = 0u;
        std::uint32_t length = 0u;
        try {
            destination = guest_read_u32(cpu, cpu.r[5]);
            length = guest_read_u32(cpu, cpu.r[5] + 4u);
        } catch (const MemoryAccessError&) {
            return finish(0xFFFFFFFFu);
        }
        const auto alignment = kind == GdRomBiosTransferKind::Dma ? 32u : 2u;
        const auto stream_remaining =
            found->second.stream_total_bytes - found->second.stream_consumed_bytes;
        if (length == 0u || (destination & (alignment - 1u)) != 0u ||
            (length & (alignment - 1u)) != 0u || length > stream_remaining)
            return finish(0xFFFFFFFFu);
        const auto pio_physical_destination =
            kind == GdRomBiosTransferKind::Pio
                ? resolve_bios_write_destination(cpu, destination, length)
                : std::optional<std::uint32_t>{destination};
        if (!pio_physical_destination) {
            latch_sense(5u, 0x21u, 0u);
            return finish(0xFFFFFFFFu);
        }
        auto& request = found->second;
        request.transfer_kind = kind;
        request.transfer_destination = destination;
        request.transfer_size = length;
        request.transfer_transferred = 0u;
        request.transfer_active = true;
        request.status[3] = 4u;
        status_ = ata_busy;
        interrupt_reason_ = 0u;
        if (kind == GdRomBiosTransferKind::Dma) {
            if (g1_bus_ == nullptr || !g1_bus_->begin_transfer(destination, length, 1u)) {
                request.transfer_kind = GdRomBiosTransferKind::None;
                request.transfer_size = 0u;
                request.transfer_active = false;
                request.status[3] = 0u;
                status_ = ata_busy;
                interrupt_reason_ = 0u;
                return finish(0xFFFFFFFFu);
            }
            remember_bios_request(request);
            return finish(0u);
        }
        try {
            const auto bytes = preview_stream_bytes(request, length);
            for (std::uint32_t offset = 0u; offset < length; offset += 2u) {
                const auto value = static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(bytes[offset]) |
                    (static_cast<std::uint16_t>(bytes[offset + 1u]) << 8u));
                memory_.write_u16(*pio_physical_destination + offset,
                                  value,
                                  CodeWriteSource::Copy);
            }
            if (module_load_observer_)
                module_load_observer_(*pio_physical_destination, bytes, drive_.identity());
            commit_stream_bytes(request, length);
            return finish(0u);
        } catch (const std::exception&) {
            request.transfer_kind = GdRomBiosTransferKind::None;
            request.transfer_size = 0u;
            request.transfer_active = false;
            request.status[3] = 0u;
            latch_sense(5u, 0x21u, 0u);
            return finish(0xFFFFFFFFu);
        }
    }
    if (selector == 7u || selector == 13u) {
        const auto found = bios_requests_.find(cpu.r[4]);
        if (found == bios_requests_.end() || cpu.r[5] == 0u)
            return finish(0xFFFFFFFFu);
        const auto kind = selector == 7u ? GdRomBiosTransferKind::Dma
                                         : GdRomBiosTransferKind::Pio;
        if (found->second.streaming_dma != (kind == GdRomBiosTransferKind::Dma) ||
            (found->second.state != GdRomBiosRequestState::Streaming &&
             found->second.state != GdRomBiosRequestState::Complete))
            return finish(0xFFFFFFFFu);
        if (found->second.transfer_transferred > found->second.transfer_size ||
            found->second.stream_consumed_bytes > found->second.stream_total_bytes)
            return finish(0xFFFFFFFFu);
        const auto transfer_active = found->second.transfer_active;
        const auto progress_or_stream_remaining =
            transfer_active
                ? found->second.transfer_transferred
                : static_cast<std::uint32_t>(found->second.stream_total_bytes -
                                             found->second.stream_consumed_bytes);
        try {
            guest_write_u32(
                cpu, cpu.r[5], progress_or_stream_remaining, CodeWriteSource::Copy);
        } catch (const MemoryAccessError&) {
            return finish(0xFFFFFFFFu);
        }
        return finish(transfer_active ? 1u : 0u);
    }
    if (selector == 10u) {
        if (cpu.r[4] == 0u) return finish(0xFFFFFFFFu);
        try {
            const auto operation = guest_read_u32(cpu, cpu.r[4]);
            if (operation == 1u) {
                for (std::size_t index = 0u; index < sector_mode_.size(); ++index)
                    guest_write_u32(cpu,
                                    cpu.r[4] + static_cast<std::uint32_t>(index * 4u),
                                    index == 0u ? 1u : sector_mode_[index],
                                    CodeWriteSource::Copy);
                return finish(0u);
            }
            if (operation != 0u) return finish(0xFFFFFFFFu);
            std::array<std::uint32_t, 4u> requested{};
            for (std::size_t index = 0u; index < requested.size(); ++index)
                requested[index] = guest_read_u32(
                    cpu, cpu.r[4] + static_cast<std::uint32_t>(index * 4u));
            const auto valid_track_type = requested[2] == 0u || requested[2] == 1024u ||
                                          requested[2] == 2048u;
            const auto supported_data_view =
                requested[3] == drive_.sector_size() &&
                ((drive_.sector_size() == 2048u && requested[1] == 0x2000u) ||
                 (drive_.sector_size() == 2352u && requested[1] == 0x1000u));
            if (!supported_data_view || !valid_track_type)
                return finish(0xFFFFFFFFu);
            sector_mode_ = requested;
            return finish(0u);
        } catch (const MemoryAccessError&) {
            return finish(0xFFFFFFFFu);
        }
    }
    return finish(0xFFFFFFFFu);
}

void DreamcastGdRomController::dma_to_memory(const std::uint32_t address,
                                             const std::uint32_t length,
                                             const std::uint32_t direction) {
    if (direction != 1u)
        throw std::runtime_error("GD-ROM-G1-DMA unterstuetzt nur Laufwerk-zu-Systemspeicher.");
    if (auto* request = active_stream_transfer(GdRomBiosTransferKind::Dma)) {
        if (length == 0u || address != request->transfer_destination + request->transfer_transferred)
            throw std::out_of_range("GD-ROM-G1-DMA passt nicht zum BIOS-Streamingtransfer.");
        try {
            const auto bytes = preview_stream_bytes(*request, length);
            memory_.write_bytes(address, bytes, CodeWriteSource::Dma);
            if (module_load_observer_)
                module_load_observer_(
                    canonical_physical_address(address), bytes, drive_.identity());
            commit_stream_bytes(*request, length);
        } catch (...) {
            request->response.status = GdRomStatus::OutOfRange;
            request->status = {5u,
                               static_cast<std::uint32_t>(request->response.status),
                               request->status[2],
                               0u};
            request->state = GdRomBiosRequestState::Error;
            request->transfer_active = false;
            latch_sense(5u, 0x21u, 0u);
            remember_bios_request(*request);
            throw;
        }
        return;
    }
    if (taskfile_phase_ != TaskfilePhase::DmaIn || features_ != 1u ||
        (status_ & ata_drq) != 0u)
        throw std::runtime_error("GD-ROM-G1-DMA braucht einen CD_READ-DMA-Vertrag.");
    if (length == 0u || data_cursor_ > data_.size() || length > data_.size() - data_cursor_)
        throw std::out_of_range("GD-ROM-G1-DMA ueberschreitet den aktiven DMA-Puffer.");
    memory_.write_bytes(address,
                        std::span<const std::uint8_t>(data_).subspan(data_cursor_, length),
                        CodeWriteSource::Dma);
    if (module_load_observer_)
        module_load_observer_(canonical_physical_address(address),
                              std::span<const std::uint8_t>(data_).subspan(data_cursor_, length),
                              drive_.identity());
    data_cursor_ += length;
    const auto command_data_complete = data_cursor_ == data_.size();
    if (command_data_complete) {
        finish_taskfile_command();
        ++completed_dma_;
    }
}

GdRomProductStatus DreamcastGdRomController::status() const noexcept {
    std::uint64_t stream_remaining = 0u;
    std::uint32_t transfer_remaining = 0u;
    for (const auto& [id, request] : bios_requests_) {
        static_cast<void>(id);
        if (request.stream_consumed_bytes <= request.stream_total_bytes)
            stream_remaining = std::max(
                stream_remaining, request.stream_total_bytes - request.stream_consumed_bytes);
        if (request.transfer_active && request.transfer_transferred <= request.transfer_size)
            transfer_remaining =
                std::max(transfer_remaining, request.transfer_size - request.transfer_transferred);
    }
    const auto pio_bytes_available =
        taskfile_phase_ == TaskfilePhase::DataIn && data_cursor_ <= data_.size()
            ? data_.size() - data_cursor_
            : 0u;
    return {status_,
            interrupt_reason_,
            pio_bytes_available,
            bios_requests_.size(),
            completed_commands_,
            completed_dma_,
            sector_mode_,
            dma_callback_,
            dma_callback_argument_,
            pio_callback_,
            pio_callback_argument_,
            stream_remaining,
            transfer_remaining,
            pending_guest_callbacks_.size()};
}

const GdRomBiosRequestStatus& DreamcastGdRomController::last_bios_request() const noexcept {
    return last_bios_request_;
}

void DreamcastGdRomController::bind_g1_bus(DreamcastG1BusController* const g1_bus) noexcept {
    g1_bus_ = g1_bus;
}

std::optional<GdRomGuestCallback> DreamcastGdRomController::take_pending_guest_callback() {
    if (pending_guest_callbacks_.empty()) return std::nullopt;
    auto callback = pending_guest_callbacks_.front();
    pending_guest_callbacks_.erase(pending_guest_callbacks_.begin());
    return callback;
}

std::span<const GdRomBiosCallEvent>
DreamcastGdRomController::bios_call_events() const noexcept {
    return bios_call_events_;
}

std::string DreamcastGdRomController::format_bios_call_events_json() const {
    std::ostringstream output;
    output << "{\"schema\":\"katana-gdrom-bios-events\",\"version\":1,"
              "\"dropped_events\":"
           << dropped_bios_call_events_ << ",\"events\":[";
    for (std::size_t index = 0u; index < bios_call_events_.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& event = bios_call_events_[index];
        output << "{\"sequence\":" << event.sequence << ",\"guest_cycle\":"
               << event.guest_cycle << ",\"callsite\":" << event.callsite
               << ",\"return_address\":" << event.return_address << ",\"selector\":"
               << event.selector << ",\"super_selector\":" << event.super_selector
               << ",\"arguments\":[";
        for (std::size_t argument = 0u; argument < event.arguments.size(); ++argument) {
            if (argument != 0u) output << ',';
            output << event.arguments[argument];
        }
        output << "],\"request_id\":" << event.request_id << ",\"state_before\":"
               << static_cast<std::uint32_t>(event.state_before) << ",\"state_after\":"
               << static_cast<std::uint32_t>(event.state_after) << ",\"result\":"
               << event.result << ",\"status\":[";
        for (std::size_t word = 0u; word < event.status.size(); ++word) {
            if (word != 0u) output << ',';
            output << event.status[word];
        }
        output << "]}";
    }
    output << "]}";
    return output.str();
}

void DreamcastGdRomController::remember_bios_request(const BiosRequest& request) noexcept {
    last_bios_request_ = {request.id, request.command, request.state, request.status};
}

void DreamcastGdRomController::reset_transport() noexcept {
    if (g1_bus_ != nullptr) g1_bus_->abort_transfer();
    if (packet_event_ && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*packet_event_));
    packet_event_.reset();
    packet_.clear();
    data_.clear();
    data_cursor_ = 0u;
    taskfile_phase_remaining_ = 0u;
    taskfile_host_byte_limit_ = 65'536u;
    taskfile_phase_ = TaskfilePhase::Idle;
    drive_owner_ = DriveOwner::None;
    command_irq_reassert_pending_ = false;
    acknowledge_command_irq();
    command_irq_asserted_ = false;
    command_irq_reassert_pending_ = false;
    taskfile_command_failed_ = false;
    clear_sense_after_data_ = false;
    set_mode_offset_ = 0u;
    status_ = ata_ready;
    clear_sense();
    interrupt_reason_ = 0u;
    features_ = 0u;
    sector_count_register_ = 0u;
    sector_number_ = 0u;
    drive_select_ = 0u;
    byte_count_ = 0u;
    current_fad_ = 150u;
    expecting_packet_ = false;
    reader_.reset();
    bios_requests_.clear();
    next_bios_request_ = 1u;
    last_bios_request_ = {};
    dma_completion_pending_ = false;
    pio_completion_pending_ = false;
    dma_completion_request_ = 0u;
    pio_completion_request_ = 0u;
    pending_guest_callbacks_.clear();
}

void DreamcastGdRomController::reset() noexcept {
    reset_transport();
    drive_mode_ = {0u, 0u, 0u, 0u, 0u, 0xB4u, 0x19u, 0u, 0u, 0x08u};
    sector_mode_ = drive_.sector_size() == 2352u
                       ? std::array<std::uint32_t, 4u>{0u, 0x1000u, 0u, 2352u}
                       : std::array<std::uint32_t, 4u>{
                             0u, 0x2000u, 1024u, drive_.sector_size()};
    dma_callback_ = 0u;
    dma_callback_argument_ = 0u;
    pio_callback_ = 0u;
    pio_callback_argument_ = 0u;
}

std::shared_ptr<DreamcastGdRomController>
map_dreamcast_gdrom(Memory& memory,
                    EventScheduler& scheduler,
                    GdRomDrive drive,
                    std::function<void(std::uint64_t)> completion_observer,
                    DreamcastGdRomController::ModuleLoadObserver module_load_observer,
                    std::function<void()> command_ack_observer) {
    auto controller = std::make_shared<DreamcastGdRomController>(
        memory,
        scheduler,
        std::move(drive),
        std::move(completion_observer),
        std::move(module_load_observer),
        std::move(command_ack_observer));
    auto device = std::make_shared<MmioMemoryDevice>(
        gdrom_register_size,
        [controller](const auto offset, const auto width) { return controller->read(offset, width); },
        [controller](const auto offset, const auto value, const auto width) {
            controller->write(offset, value, width);
        });
    for (const auto segment : dreamcast_direct_segment_bases) {
        const auto base = segment + gdrom_register_physical_base;
        memory.map_region("dreamcast-gdrom-" + std::to_string(base), base, device);
    }
    return controller;
}

} // namespace katana::runtime
