#include "katana/runtime/gdrom_controller.hpp"

#include "katana/runtime/block_table.hpp"
#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/guest_buffer.hpp"
#include "katana/runtime/holly_dma.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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
constexpr std::uint32_t bios_command_no_operation = 29u;
constexpr std::uint32_t bios_command_request_mode = 30u;
constexpr std::uint32_t bios_command_set_mode = 31u;
constexpr std::uint32_t bios_command_pio_stream = 37u;
constexpr std::uint32_t bios_command_dma_stream_ex = 38u;
constexpr std::uint32_t bios_command_pio_stream_ex = 39u;

constexpr std::size_t bios_parameter_word_count(const std::uint32_t command) noexcept {
    switch (command) {
    case bios_command_pio_read:
    case bios_command_dma_read:
    case bios_command_dma_stream:
    case bios_command_set_mode:
    case bios_command_pio_stream:
        return 4u;
    case 18u:
    case 19u:
        return 2u;
    case bios_command_request_mode:
        return 1u;
    case 24u:
    case bios_command_no_operation:
    case bios_command_dma_stream_ex:
    case bios_command_pio_stream_ex:
    default:
        // Parameterlose und nicht unterstuetzte Kommandos duerfen das architektonisch
        // irrelevante r5 nicht beruehren.
        return 0u;
    }
}

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

std::uint32_t load_le32(const std::span<const std::uint8_t, 4u> bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0u]) |
           (static_cast<std::uint32_t>(bytes[1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[3u]) << 24u);
}

template <typename Submit>
bool admit_scheduled_gdrom_request(std::uint64_t& request_id, Submit&& submit) {
    try {
        request_id = std::forward<Submit>(submit)();
        return true;
    } catch (const std::out_of_range&) {
        return false;
    } catch (const std::overflow_error&) {
        return false;
    }
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

} // namespace

DreamcastGdRomController::DreamcastGdRomController(
    Memory& memory,
    EventScheduler& scheduler,
    GdRomDrive drive,
    std::function<void(std::uint64_t)> completion_observer,
    ModuleLoadObserver module_load_observer,
    std::function<void()> command_ack_observer,
    DiscLoadTransactionExecutor load_transaction_executor,
    std::string content_identity,
    const DiscLoadExecutionPolicy load_execution_policy)
    : memory_(memory), scheduler_(scheduler), drive_(std::move(drive)),
      // Raw media readiness only re-enters the controller. pump_completions() publishes the
      // final BIOS state and commits RAM before it raises the guest-visible observer.
      reader_(scheduler,
              drive_,
              GdRomTiming{},
              [this](const std::uint64_t) { pump_completions(); }),
      module_load_observer_(std::move(module_load_observer)),
      load_transaction_executor_(std::move(load_transaction_executor)),
      content_identity_(std::move(content_identity)),
      load_execution_policy_(load_execution_policy),
      completion_observer_(std::move(completion_observer)),
      command_ack_observer_(std::move(command_ack_observer)),
      scheduler_lifetime_(scheduler.lifetime_token()) {
    if (load_execution_policy_ == DiscLoadExecutionPolicy::RequireAtomicExecutor &&
        !load_transaction_executor_)
        throw std::invalid_argument(
            "Produktiver GD-ROM-Pfad braucht einen atomaren Disc-Load-Executor.");
    if (load_execution_policy_ == DiscLoadExecutionPolicy::RequireAtomicExecutor &&
        content_identity_.empty())
        throw std::invalid_argument(
            "Produktiver GD-ROM-Pfad braucht die verifizierte Contentidentitaet.");
    sector_mode_ = drive_.sector_size() == 2352u
                       ? std::array<std::uint32_t, 4u>{0u, 0x1000u, 0u, 2352u}
                       : std::array<std::uint32_t, 4u>{
                             0u, 0x2000u, 1024u, drive_.sector_size()};
    pending_guest_callbacks_.reserve(gdrom_guest_callback_capacity);
    reset_observer_ =
        scheduler_.add_reset_observer([this] { handle_scheduler_reset(); });
}

DreamcastGdRomController::~DreamcastGdRomController() {
    if (!scheduler_lifetime_.expired()) {
        if (packet_event_)
            static_cast<void>(scheduler_.cancel(*packet_event_));
        static_cast<void>(scheduler_.remove_reset_observer(reset_observer_));
    }
}

DiscLoadCommit DreamcastGdRomController::commit_disc_load(
    const DiscLoadRoute route,
    const std::uint32_t guest_destination,
    const std::uint32_t physical_destination,
    const std::span<const std::uint8_t> bytes,
    const CodeWriteSource source,
    const DiscLoadSourceRange source_range) {
    DiscLoadRequest request;
    request.sequence = claim_disc_load_sequence(next_load_transaction_);
    request.route = route;
    request.guest_destination = guest_destination;
    request.physical_destination = canonical_physical_address(physical_destination);
    request.write_source = source;
    request.content_identity = content_identity_;
    request.byte_identity = disc_load_byte_identity(bytes);
    request.source_range = source_range;
    request.bytes = bytes;
    request.guest_translation_validated = true;

    try {
        DiscLoadCommit commit;
        if (load_transaction_executor_) {
            commit = load_transaction_executor_(request);
        } else {
            if (load_execution_policy_ != DiscLoadExecutionPolicy::StandaloneTestMode)
                throw std::logic_error(
                    "Produktiver GD-ROM-Pfad darf Discbytes nicht direkt schreiben.");
            if (!memory_.is_writable_linear_range(request.physical_destination, bytes.size()))
                throw std::out_of_range(
                    "Disc-Ladetransaktion zielt nicht auf lineares schreibbares RAM.");
            if (!memory_.commit_linear_transaction_bytes(
                    request.physical_destination, bytes, source))
                throw std::out_of_range(
                    "Disc-Ladetransaktion konnte nicht atomar committed werden.");
            if (module_load_observer_)
                module_load_observer_(request.physical_destination, bytes, drive_.identity());
            DiscLoadCommittedRange range;
            range.target_physical_address = request.physical_destination;
            range.backing_physical_address = request.physical_destination;
            range.size = static_cast<std::uint32_t>(bytes.size());
            range.byte_identity = request.byte_identity;
            range.source_range_known = request.source_range.known;
            range.source_byte_offset = request.source_range.byte_offset;
            commit.sequence = request.sequence;
            commit.route = request.route;
            commit.guest_destination = request.guest_destination;
            commit.physical_destination = request.physical_destination;
            commit.write_source = request.write_source;
            commit.content_identity = request.content_identity;
            commit.byte_identity = request.byte_identity;
            commit.source_range = request.source_range;
            commit.committed_bytes = bytes.size();
            commit.ranges.push_back(std::move(range));
        }
        validate_disc_load_commit(request, commit);
        if (committed_load_transactions_ != std::numeric_limits<std::uint64_t>::max())
            ++committed_load_transactions_;
        return commit;
    } catch (...) {
        if (failed_load_transactions_ != std::numeric_limits<std::uint64_t>::max())
            ++failed_load_transactions_;
        throw;
    }
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
    taskfile_data_source_range_ = {};
    taskfile_phase_remaining_ = 0u;
    if (data_.empty()) {
        if (clear_sense_after_data_) clear_sense();
        finish_taskfile_command();
        return;
    }
    taskfile_phase_ = TaskfilePhase::DataIn;
    begin_next_taskfile_data_phase();
}

void DreamcastGdRomController::publish_dma_data(std::vector<std::uint8_t> data,
                                                const DiscLoadSourceRange source_range) {
    data_ = std::move(data);
    data_cursor_ = 0u;
    taskfile_data_source_range_ = source_range;
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

void DreamcastGdRomController::notify_completion(const std::uint64_t cycle) noexcept {
    if (!completion_observer_) return;
    try {
        completion_observer_(cycle);
    } catch (...) {
        // A host interrupt sink is observational. Device state has already been committed and
        // must remain visible even when that sink rejects a notification.
    }
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
    notify_completion(cycle);
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
            if ((features_ & 1u) != 0u) {
                const DiscLoadSourceRange source_range{
                    true,
                    static_cast<std::uint64_t>(fad_to_lba(fad)) * drive_.sector_size(),
                    response.data.size()};
                publish_dma_data(std::move(response.data), source_range);
            } else
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
    try {
        packet_event_ = scheduler_.schedule_after(
            1'000u,
            [this](const auto event_id, const auto cycle) { complete_packet(event_id, cycle); },
            SchedulerEventKind::GdRomPacket);
        packet_event_rehydration_pending_ = false;
    } catch (const std::overflow_error&) {
        packet_event_.reset();
        packet_event_rehydration_pending_ = false;
        fail_taskfile_command(0x0Bu, 0u, 0u, true);
    }
}

void DreamcastGdRomController::complete_packet(const SchedulerEventId event_id,
                                               const std::uint64_t cycle) {
    if (!packet_event_ || *packet_event_ != event_id) return;
    packet_event_.reset();
    packet_event_rehydration_pending_ = false;
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

void DreamcastGdRomController::submit_bios_read(BiosRequest& request) {
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
    const auto destination =
        request.guest_binding
            ? resolve_guest_write_buffer(*request.guest_binding,
                                         memory_,
                                         request.parameters[2],
                                         static_cast<std::size_t>(byte_count))
            : std::nullopt;
    if (!destination) {
        request.response.status = GdRomStatus::InvalidField;
        request.status = {5u, static_cast<std::uint32_t>(GdRomStatus::InvalidField), 0u, 0u};
        request.state = GdRomBiosRequestState::Error;
        latch_sense(5u, 0x24u, 0u);
        remember_bios_request(request);
        return;
    }
    request.destination = destination->physical_address;
    request.write_source = request.command == 17u ? CodeWriteSource::Dma : CodeWriteSource::Copy;
    if (!admit_scheduled_gdrom_request(request.async_id, [&] {
            return reader_.submit({GdRomCommand::ReadSectors,
                                   fad_to_lba(request.parameters[0]),
                                   request.parameters[1]});
        })) {
        request.response.status = GdRomStatus::Aborted;
        request.status = {0x0Bu, static_cast<std::uint32_t>(GdRomStatus::Aborted), 0u, 0u};
        request.state = GdRomBiosRequestState::Error;
        latch_sense(0x0Bu, 0u, 0u, true);
        remember_bios_request(request);
        return;
    }
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
    if (!admit_scheduled_gdrom_request(
            request.async_id,
            [&] { return reader_.submit({GdRomCommand::TestUnitReady}); })) {
        request.response.status = GdRomStatus::Aborted;
        request.status = {0x0Bu, static_cast<std::uint32_t>(GdRomStatus::Aborted), 0u, 0u};
        request.state = GdRomBiosRequestState::Error;
        latch_sense(0x0Bu, 0u, 0u, true);
        remember_bios_request(request);
        return;
    }
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

void DreamcastGdRomController::enqueue_guest_callback(
    const GdRomGuestCallback callback) noexcept {
    const auto duplicate = std::find_if(
        pending_guest_callbacks_.begin(),
        pending_guest_callbacks_.end(),
        [&](const auto& queued) {
            return queued.kind == callback.kind && queued.address == callback.address &&
                   queued.argument == callback.argument &&
                   queued.request_id == callback.request_id;
        });
    if (duplicate != pending_guest_callbacks_.end()) {
        if (coalesced_guest_callbacks_ != std::numeric_limits<std::uint64_t>::max())
            ++coalesced_guest_callbacks_;
        return;
    }
    if (pending_guest_callbacks_.size() == gdrom_guest_callback_capacity) {
        pending_guest_callbacks_.erase(pending_guest_callbacks_.begin());
        if (dropped_guest_callbacks_ != std::numeric_limits<std::uint64_t>::max())
            ++dropped_guest_callbacks_;
    }
    pending_guest_callbacks_.push_back(callback);
}

void DreamcastGdRomController::queue_stream_callback(const std::uint32_t request_id,
                                                     const GdRomBiosTransferKind kind) {
    const auto address = kind == GdRomBiosTransferKind::Dma ? dma_callback_ : pio_callback_;
    const auto argument = kind == GdRomBiosTransferKind::Dma ? dma_callback_argument_
                                                             : pio_callback_argument_;
    if (address == 0u) return;
    enqueue_guest_callback({kind, address, argument, request_id});
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
    request.transfer_buffer.reset();
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

void DreamcastGdRomController::execute_bios_request(BiosRequest& request) {
    if (request.state != GdRomBiosRequestState::Queued) return;
    if (!request.guest_binding) {
        request.response.status = GdRomStatus::InvalidField;
        request.status =
            {5u, static_cast<std::uint32_t>(GdRomStatus::InvalidField), 0u, 0u};
        request.state = GdRomBiosRequestState::Error;
        latch_sense(5u, 0x24u, 0u);
        remember_bios_request(request);
        return;
    }
    if (request.command == bios_command_pio_read || request.command == bios_command_dma_read) {
        submit_bios_read(request);
        return;
    }
    if (request.command == bios_command_dma_stream ||
        request.command == bios_command_pio_stream) {
        submit_bios_stream(request);
        return;
    }
    if (request.command == bios_command_dma_stream_ex ||
        request.command == bios_command_pio_stream_ex) {
        // The public ABI names these extended streaming commands, but their parameter contract is
        // not independently established. Never guess by aliasing them to the non-EX commands: a
        // deterministic Illegal Request leaves all streaming state untouched.
        request.response.status = GdRomStatus::InvalidCommand;
        request.status = {5u, static_cast<std::uint32_t>(GdRomStatus::InvalidCommand), 0u, 0u};
        request.state = GdRomBiosRequestState::Error;
        latch_sense(5u, 0x20u, 0u);
        remember_bios_request(request);
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
        const auto destination = resolve_guest_write_buffer(
            *request.guest_binding, memory_, request.parameters[1], toc_bytes.size(), 4u);
        if (!destination) {
            request.response.status = GdRomStatus::InvalidField;
            request.status =
                {5u, static_cast<std::uint32_t>(GdRomStatus::InvalidField), 0u, 0u};
            request.state = GdRomBiosRequestState::Error;
            latch_sense(5u, 0x24u, 0u);
            remember_bios_request(request);
            return;
        }
        if (!commit_guest_write_buffer(
                *request.guest_binding, memory_, *destination, toc_bytes)) {
            request.response.status = GdRomStatus::InvalidField;
            request.status =
                {5u, static_cast<std::uint32_t>(GdRomStatus::InvalidField), 0u, 0u};
            request.state = GdRomBiosRequestState::Error;
            latch_sense(5u, 0x24u, 0u);
            remember_bios_request(request);
            return;
        }
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
    if (request.command == bios_command_no_operation) {
        request.status = {};
        request.state = GdRomBiosRequestState::Complete;
        remember_bios_request(request);
        ++completed_commands_;
        return;
    }
    if (request.command == bios_command_request_mode) {
        constexpr std::size_t mode_word_count = 4u;
        constexpr std::size_t mode_output_size = mode_word_count * sizeof(std::uint32_t);
        const auto destination =
            resolve_guest_write_buffer(*request.guest_binding,
                                       memory_,
                                       request.parameters[0],
                                       mode_output_size,
                                       4u);
        if (!destination) {
            request.response.status = GdRomStatus::InvalidField;
            request.status =
                {5u, static_cast<std::uint32_t>(GdRomStatus::InvalidField), 0u, 0u};
            request.state = GdRomBiosRequestState::Error;
            latch_sense(5u, 0x24u, 0u);
            remember_bios_request(request);
            return;
        }
        const std::array<std::uint32_t, mode_word_count> mode{
            drive_mode_[2u],
            (static_cast<std::uint32_t>(drive_mode_[4u]) << 8u) | drive_mode_[5u],
            drive_mode_[6u],
            drive_mode_[9u],
        };
        std::vector<std::uint8_t> mode_bytes;
        mode_bytes.reserve(mode_output_size);
        for (const auto word : mode) append_le32(mode_bytes, word);
        if (!commit_guest_write_buffer(
                *request.guest_binding, memory_, *destination, mode_bytes)) {
            request.response.status = GdRomStatus::InvalidField;
            request.status =
                {5u, static_cast<std::uint32_t>(GdRomStatus::InvalidField), 0u, 0u};
            request.state = GdRomBiosRequestState::Error;
            latch_sense(5u, 0x24u, 0u);
            remember_bios_request(request);
            return;
        }
        request.status = {0u, 0u, static_cast<std::uint32_t>(gdrom_writable_mode_size), 0u};
        request.state = GdRomBiosRequestState::Complete;
        remember_bios_request(request);
        ++completed_commands_;
        return;
    }
    if (request.command == bios_command_set_mode) {
        drive_mode_[2u] = static_cast<std::uint8_t>(request.parameters[0]);
        drive_mode_[4u] = static_cast<std::uint8_t>(request.parameters[1] >> 8u);
        drive_mode_[5u] = static_cast<std::uint8_t>(request.parameters[1]);
        drive_mode_[6u] = static_cast<std::uint8_t>(request.parameters[2]);
        drive_mode_[9u] = static_cast<std::uint8_t>(request.parameters[3]);
        request.status = {0u, 0u, static_cast<std::uint32_t>(gdrom_writable_mode_size), 0u};
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
            notify_completion(scheduler_.current_cycle());
            continue;
        }
        if (found->second.response.status == GdRomStatus::Good &&
            !found->second.response.data.empty()) {
            try {
                if (!found->second.guest_binding)
                    throw std::logic_error(
                        "Asynchroner GD-ROM-BIOS-Request besitzt keinen Gastkontext.");
                const auto destination = resolve_guest_write_buffer(
                    *found->second.guest_binding,
                    memory_,
                    found->second.parameters[2],
                    found->second.response.data.size());
                if (!destination ||
                    destination->physical_address != found->second.destination)
                    throw std::out_of_range(
                        "Asynchrones GD-ROM-BIOS-Ziel wurde bis Completion umgebunden.");
                static_cast<void>(commit_disc_load(
                    found->second.command == bios_command_dma_read
                        ? DiscLoadRoute::BiosDma
                        : DiscLoadRoute::BiosPio,
                     found->second.parameters[2],
                     found->second.destination,
                     found->second.response.data,
                     found->second.write_source,
                     {true,
                      static_cast<std::uint64_t>(
                          fad_to_lba(found->second.parameters[0])) *
                          drive_.sector_size(),
                      found->second.response.data.size()}));
            } catch (...) {
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
        notify_completion(scheduler_.current_cycle());
    }
}

bool DreamcastGdRomController::reload_system_bootstrap(CpuState& cpu) {
    constexpr std::uint32_t destination = 0x8C008100u;
    constexpr std::uint32_t sector_count = 7u;
    if (&cpu.memory != &memory_) return false;
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
    try {
        static_cast<void>(commit_disc_load(DiscLoadRoute::SystemBootstrap,
                                           destination,
                                           canonical_physical_address(destination),
                                           response.data,
                                           CodeWriteSource::Copy,
                                           {true,
                                            static_cast<std::uint64_t>(track->lba) *
                                                drive_.sector_size(),
                                            response.data.size()}));
    } catch (...) {
        return false;
    }
    return true;
}

const DreamcastGdRomController::BiosRequest*
DreamcastGdRomController::find_bios_request(const std::uint32_t id) const noexcept {
    const auto found = bios_requests_.find(id);
    return found == bios_requests_.end() ? nullptr : &found->second;
}

bool DreamcastGdRomController::bios_request_id_reserved(const std::uint32_t id) const noexcept {
    if (id == 0u || bios_requests_.contains(id)) return true;
    if ((dma_completion_pending_ && dma_completion_request_ == id) ||
        (pio_completion_pending_ && pio_completion_request_ == id))
        return true;
    return std::any_of(pending_guest_callbacks_.begin(),
                       pending_guest_callbacks_.end(),
                       [&](const auto& callback) { return callback.request_id == id; });
}

std::optional<std::uint32_t>
DreamcastGdRomController::next_available_bios_request_id() const noexcept {
    auto candidate = next_bios_request_ == 0u ? 1u : next_bios_request_;
    const auto maximum_reserved =
        bios_requests_.size() + pending_guest_callbacks_.size() + 2u;
    for (std::size_t attempt = 0u; attempt <= maximum_reserved; ++attempt) {
        if (!bios_request_id_reserved(candidate)) return candidate;
        candidate = candidate == std::numeric_limits<std::uint32_t>::max()
                        ? 1u
                        : candidate + 1u;
    }
    return std::nullopt;
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
    if (&cpu.memory != &memory_)
        return finish(selector == 0u ? 0u : 0xFFFFFFFFu);
    pump_completions();
    if (selector == 0u) {
        if (taskfile_blocks_bios() || !bios_requests_.empty()) return finish(0u);
        BiosRequest request;
        request.guest_binding = bind_guest_address_space(cpu);
        request.command = cpu.r[4];
        const auto parameter_word_count = bios_parameter_word_count(request.command);
        if (cpu.r[5] != 0u && parameter_word_count != 0u) {
            const auto parameter_bytes = parameter_word_count * sizeof(std::uint32_t);
            const auto source = resolve_guest_read_buffer(
                *request.guest_binding, memory_, cpu.r[5], parameter_bytes, 4u);
            std::array<std::uint8_t, 4u * sizeof(std::uint32_t)> bytes{};
            const auto parameter_bytes_view =
                std::span<std::uint8_t>(bytes).first(parameter_bytes);
            if (!source ||
                !read_guest_buffer(
                    *request.guest_binding, memory_, *source, parameter_bytes_view))
                return finish(0u);
            for (std::size_t index = 0u; index < parameter_word_count; ++index)
                request.parameters[index] =
                    load_le32(std::span<const std::uint8_t, 4u>(
                        bytes.data() + index * sizeof(std::uint32_t), 4u));
        }
        const auto id = next_available_bios_request_id();
        if (!id) return finish(0u);
        request.id = *id;
        const auto [inserted, admitted] = bios_requests_.emplace(*id, std::move(request));
        if (!admitted) return finish(0u);
        next_bios_request_ = *id == std::numeric_limits<std::uint32_t>::max()
                                 ? 1u
                                 : *id + 1u;
        drive_owner_ = DriveOwner::Bios;
        taskfile_command_failed_ = false;
        status_ = ata_busy;
        interrupt_reason_ = 0u;
        remember_bios_request(inserted->second);
        return finish(*id);
    }
    if (selector == 1u) {
        const auto found = bios_requests_.find(cpu.r[4]);
        if (found == bios_requests_.end()) return finish(0u);
        if (cpu.r[5] != 0u) {
            std::vector<std::uint8_t> status_bytes;
            status_bytes.reserve(found->second.status.size() * sizeof(std::uint32_t));
            for (const auto word : found->second.status) append_le32(status_bytes, word);
            const auto destination =
                resolve_guest_write_buffer(cpu, cpu.r[5], status_bytes.size(), 4u);
            if (!destination ||
                !commit_guest_write_buffer(cpu, *destination, status_bytes))
                return finish(0xFFFFFFFFu);
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
        if (queued != bios_requests_.end()) execute_bios_request(queued->second);
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
            std::vector<std::uint8_t> status_bytes;
            status_bytes.reserve(2u * sizeof(std::uint32_t));
            append_le32(status_bytes, busy ? 0u : 1u);
            append_le32(status_bytes, busy ? 0u : 0x80u);
            const auto destination =
                resolve_guest_write_buffer(cpu, cpu.r[4], status_bytes.size(), 4u);
            if (!destination ||
                !commit_guest_write_buffer(cpu, *destination, status_bytes))
                return finish(0xFFFFFFFFu);
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
            enqueue_guest_callback({GdRomBiosTransferKind::Dma,
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
        auto& request = found->second;
        if (!request.guest_binding || &cpu.memory != &memory_)
            return finish(0xFFFFFFFFu);
        const auto kind = selector == 6u ? GdRomBiosTransferKind::Dma
                                         : GdRomBiosTransferKind::Pio;
        if (request.streaming_dma != (kind == GdRomBiosTransferKind::Dma))
            return finish(0xFFFFFFFFu);
        constexpr std::size_t transfer_descriptor_size =
            2u * sizeof(std::uint32_t);
        const auto descriptor = resolve_guest_read_buffer(
            *request.guest_binding,
            memory_,
            cpu.r[5],
            transfer_descriptor_size,
            alignof(std::uint32_t));
        std::array<std::uint8_t, transfer_descriptor_size> descriptor_bytes{};
        if (!descriptor ||
            !read_guest_buffer(*request.guest_binding,
                               memory_,
                               *descriptor,
                               descriptor_bytes))
            return finish(0xFFFFFFFFu);
        const auto destination =
            load_le32(std::span<const std::uint8_t, 4u>(
                descriptor_bytes.data(), sizeof(std::uint32_t)));
        const auto length =
            load_le32(std::span<const std::uint8_t, 4u>(
                descriptor_bytes.data() + sizeof(std::uint32_t),
                sizeof(std::uint32_t)));
        const auto alignment = kind == GdRomBiosTransferKind::Dma ? 32u : 2u;
        const auto stream_remaining =
            request.stream_total_bytes - request.stream_consumed_bytes;
        if (length == 0u || (destination & (alignment - 1u)) != 0u ||
            (length & (alignment - 1u)) != 0u || length > stream_remaining)
            return finish(0xFFFFFFFFu);
        const auto transfer_buffer = resolve_guest_write_buffer(
            *request.guest_binding, memory_, destination, length, alignment);
        if (!transfer_buffer) {
            latch_sense(5u, 0x21u, 0u);
            return finish(0xFFFFFFFFu);
        }
        request.transfer_kind = kind;
        request.transfer_destination = destination;
        request.transfer_size = length;
        request.transfer_transferred = 0u;
        request.transfer_active = true;
        request.transfer_buffer = *transfer_buffer;
        request.status[3] = 4u;
        status_ = ata_busy;
        interrupt_reason_ = 0u;
        if (kind == GdRomBiosTransferKind::Dma) {
            if (g1_bus_ == nullptr ||
                !g1_bus_->begin_transfer(
                    transfer_buffer->physical_address, length, 1u)) {
                request.transfer_kind = GdRomBiosTransferKind::None;
                request.transfer_size = 0u;
                request.transfer_active = false;
                request.transfer_buffer.reset();
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
            const auto current_destination = resolve_guest_write_buffer(
                *request.guest_binding, memory_, destination, length, alignment);
            if (!current_destination ||
                current_destination->physical_address !=
                    transfer_buffer->physical_address)
                throw std::out_of_range(
                    "GD-ROM-PIO-Streamingziel wurde vor dem Commit umgebunden.");
            static_cast<void>(commit_disc_load(DiscLoadRoute::BiosPioStream,
                                               destination,
                                               transfer_buffer->physical_address,
                                               bytes,
                                               CodeWriteSource::Copy,
                                               {true,
                                                static_cast<std::uint64_t>(
                                                    request.stream_lba) *
                                                        drive_.sector_size() +
                                                    request.stream_consumed_bytes,
                                                bytes.size()}));
            commit_stream_bytes(request, length);
            return finish(0u);
        } catch (...) {
            request.response.status = GdRomStatus::OutOfRange;
            request.status = {5u,
                              static_cast<std::uint32_t>(request.response.status),
                              request.status[2],
                              0u};
            request.state = GdRomBiosRequestState::Error;
            request.transfer_kind = GdRomBiosTransferKind::None;
            request.transfer_size = 0u;
            request.transfer_transferred = 0u;
            request.transfer_active = false;
            request.transfer_buffer.reset();
            request.status[3] = 0u;
            latch_sense(5u, 0x21u, 0u);
            remember_bios_request(request);
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
        std::vector<std::uint8_t> progress_bytes;
        progress_bytes.reserve(sizeof(std::uint32_t));
        append_le32(progress_bytes, progress_or_stream_remaining);
        const auto destination =
            resolve_guest_write_buffer(cpu, cpu.r[5], progress_bytes.size(), 4u);
        if (!destination ||
            !commit_guest_write_buffer(cpu, *destination, progress_bytes))
            return finish(0xFFFFFFFFu);
        return finish(transfer_active ? 1u : 0u);
    }
    if (selector == 10u) {
        if (cpu.r[4] == 0u) return finish(0xFFFFFFFFu);
        constexpr std::size_t sector_mode_size =
            4u * sizeof(std::uint32_t);
        const auto source =
            resolve_guest_read_buffer(cpu, cpu.r[4], sector_mode_size, 4u);
        std::array<std::uint8_t, sector_mode_size> source_bytes{};
        if (!source || !read_guest_buffer(cpu, *source, source_bytes))
            return finish(0xFFFFFFFFu);
        const auto operation =
            load_le32(std::span<const std::uint8_t, 4u>(source_bytes.data(), 4u));
        if (operation == 1u) {
            std::vector<std::uint8_t> mode_bytes;
            mode_bytes.reserve(sector_mode_size);
            append_le32(mode_bytes, 1u);
            for (std::size_t index = 1u; index < sector_mode_.size(); ++index)
                append_le32(mode_bytes, sector_mode_[index]);
            const auto destination =
                resolve_guest_write_buffer(cpu, cpu.r[4], mode_bytes.size(), 4u);
            if (!destination ||
                !commit_guest_write_buffer(cpu, *destination, mode_bytes))
                return finish(0xFFFFFFFFu);
            return finish(0u);
        }
        if (operation != 0u) return finish(0xFFFFFFFFu);
        std::array<std::uint32_t, 4u> requested{};
        for (std::size_t index = 0u; index < requested.size(); ++index)
            requested[index] = load_le32(std::span<const std::uint8_t, 4u>(
                source_bytes.data() + index * sizeof(std::uint32_t), 4u));
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
    }
    return finish(0xFFFFFFFFu);
}

void DreamcastGdRomController::dma_to_memory(const std::uint32_t address,
                                             const std::uint32_t length,
                                             const std::uint32_t direction) {
    if (direction != 1u)
        throw std::runtime_error("GD-ROM-G1-DMA unterstuetzt nur Laufwerk-zu-Systemspeicher.");
    if (auto* request = active_stream_transfer(GdRomBiosTransferKind::Dma)) {
        if (length == 0u || !request->guest_binding || !request->transfer_buffer)
            throw std::out_of_range("GD-ROM-G1-DMA passt nicht zum BIOS-Streamingtransfer.");
        const auto current_buffer = resolve_guest_write_buffer(
            *request->guest_binding,
            memory_,
            request->transfer_buffer->guest_address,
            request->transfer_buffer->size,
            request->transfer_buffer->alignment);
        const auto expected_address =
            static_cast<std::uint64_t>(request->transfer_buffer->physical_address) +
            request->transfer_transferred;
        if (!current_buffer ||
            current_buffer->physical_address !=
                request->transfer_buffer->physical_address ||
            expected_address > std::numeric_limits<std::uint32_t>::max() ||
            canonical_physical_address(address) !=
                canonical_physical_address(
                    static_cast<std::uint32_t>(expected_address)))
            throw std::out_of_range("GD-ROM-G1-DMA passt nicht zum BIOS-Streamingtransfer.");
        try {
            const auto bytes = preview_stream_bytes(*request, length);
            static_cast<void>(commit_disc_load(DiscLoadRoute::BiosDmaStream,
                                               request->transfer_destination +
                                                   request->transfer_transferred,
                                               address,
                                               bytes,
                                               CodeWriteSource::Dma,
                                               {true,
                                                static_cast<std::uint64_t>(
                                                    request->stream_lba) *
                                                        drive_.sector_size() +
                                                    request->stream_consumed_bytes,
                                                bytes.size()}));
            commit_stream_bytes(*request, length);
        } catch (...) {
            request->response.status = GdRomStatus::OutOfRange;
            request->status = {5u,
                               static_cast<std::uint32_t>(request->response.status),
                               request->status[2],
                               0u};
            request->state = GdRomBiosRequestState::Error;
            request->transfer_active = false;
            request->transfer_buffer.reset();
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
    static_cast<void>(commit_disc_load(
        DiscLoadRoute::TaskfileDma,
        address,
        address,
        std::span<const std::uint8_t>(data_).subspan(data_cursor_, length),
        CodeWriteSource::Dma,
        taskfile_data_source_range_.known
            ? DiscLoadSourceRange{true,
                                  taskfile_data_source_range_.byte_offset + data_cursor_,
                                  length}
            : DiscLoadSourceRange{}));
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
            committed_load_transactions_,
            failed_load_transactions_,
            sector_mode_,
            dma_callback_,
            dma_callback_argument_,
            pio_callback_,
            pio_callback_argument_,
            stream_remaining,
            transfer_remaining,
            pending_guest_callbacks_.size(),
            coalesced_guest_callbacks_,
            dropped_guest_callbacks_};
}

DreamcastGdRomSnapshot DreamcastGdRomController::snapshot() const {
    DreamcastGdRomSnapshot result;
    result.reader = reader_.snapshot();
    result.packet = packet_;
    result.data = data_;
    result.data_cursor = data_cursor_;
    result.taskfile_data_source_range = taskfile_data_source_range_;
    result.taskfile_phase_remaining = taskfile_phase_remaining_;
    result.taskfile_host_byte_limit = taskfile_host_byte_limit_;
    result.taskfile_phase = static_cast<std::uint8_t>(taskfile_phase_);
    result.drive_owner = static_cast<std::uint8_t>(drive_owner_);
    result.command_irq_asserted = command_irq_asserted_;
    result.command_irq_reassert_pending = command_irq_reassert_pending_;
    result.taskfile_command_failed = taskfile_command_failed_;
    result.clear_sense_after_data = clear_sense_after_data_;
    result.set_mode_offset = set_mode_offset_;
    result.drive_mode = drive_mode_;
    result.sense_key = sense_key_;
    result.sense_asc = sense_asc_;
    result.sense_ascq = sense_ascq_;
    result.status = status_;
    result.error = error_;
    result.interrupt_reason = interrupt_reason_;
    result.features = features_;
    result.sector_count_register = sector_count_register_;
    result.sector_number = sector_number_;
    result.drive_select = drive_select_;
    result.byte_count = byte_count_;
    result.current_fad = current_fad_;
    result.expecting_packet = expecting_packet_;
    result.bios_requests.reserve(bios_requests_.size());
    for (const auto& [id, request] : bios_requests_) {
        static_cast<void>(id);
        std::optional<RuntimeAddressSpaceSnapshot> guest_address_space;
        if (request.guest_binding && request.guest_binding->address_space)
            guest_address_space = request.guest_binding->address_space->snapshot();
        result.bios_requests.push_back({
            request.id,
            request.command,
            request.parameters,
            request.async_id,
            request.destination,
            request.write_source,
            request.state,
            request.status,
            request.response,
            request.streaming_dma,
            request.stream_lba,
            request.stream_sector_count,
            request.stream_total_bytes,
            request.stream_consumed_bytes,
            request.cached_stream_sector,
            request.stream_sector_cache,
            request.transfer_kind,
            request.transfer_destination,
            request.transfer_size,
            request.transfer_transferred,
            request.transfer_active,
            request.transfer_buffer,
            request.guest_binding.has_value(),
            request.guest_binding ? request.guest_binding->privileged : false,
            std::move(guest_address_space),
        });
    }
    result.next_bios_request = next_bios_request_;
    result.last_bios_request = last_bios_request_;
    result.bios_call_events = bios_call_events_;
    result.next_bios_call_sequence = next_bios_call_sequence_;
    result.dropped_bios_call_events = dropped_bios_call_events_;
    result.completed_commands = completed_commands_;
    result.completed_dma = completed_dma_;
    result.next_load_transaction = next_load_transaction_;
    result.committed_load_transactions = committed_load_transactions_;
    result.failed_load_transactions = failed_load_transactions_;
    result.sector_mode = sector_mode_;
    result.dma_callback = dma_callback_;
    result.dma_callback_argument = dma_callback_argument_;
    result.pio_callback = pio_callback_;
    result.pio_callback_argument = pio_callback_argument_;
    result.dma_completion_pending = dma_completion_pending_;
    result.pio_completion_pending = pio_completion_pending_;
    result.dma_completion_request = dma_completion_request_;
    result.pio_completion_request = pio_completion_request_;
    result.pending_guest_callbacks = pending_guest_callbacks_;
    result.coalesced_guest_callbacks = coalesced_guest_callbacks_;
    result.dropped_guest_callbacks = dropped_guest_callbacks_;
    result.packet_event = packet_event_;
    result.packet_event_rehydration_pending =
        packet_event_rehydration_pending_;
    result.g1_bus_bound = g1_bus_ != nullptr;
    result.completion_observer_bound =
        static_cast<bool>(completion_observer_);
    result.module_load_observer_bound =
        static_cast<bool>(module_load_observer_);
    result.command_ack_observer_bound =
        static_cast<bool>(command_ack_observer_);
    result.load_transaction_executor_bound =
        static_cast<bool>(load_transaction_executor_);
    result.content_identity = content_identity_;
    result.load_execution_policy = load_execution_policy_;
    return result;
}

void DreamcastGdRomController::validate_state_restore(
    const DreamcastGdRomSnapshot& state) const {
    validate_state_restore(state, scheduler_.current_cycle());
}

void DreamcastGdRomController::validate_state_restore(
    const DreamcastGdRomSnapshot& state,
    const std::uint64_t expected_scheduler_cycle) const {
    reader_.validate_state_restore(state.reader, expected_scheduler_cycle);
    if (state.g1_bus_bound != (g1_bus_ != nullptr) ||
        state.completion_observer_bound !=
            static_cast<bool>(completion_observer_) ||
        state.module_load_observer_bound !=
            static_cast<bool>(module_load_observer_) ||
        state.command_ack_observer_bound !=
            static_cast<bool>(command_ack_observer_) ||
        state.load_transaction_executor_bound !=
            static_cast<bool>(load_transaction_executor_) ||
        state.content_identity != content_identity_ ||
        state.load_execution_policy != load_execution_policy_)
        throw std::invalid_argument(
            "GD-ROM-Handoff passt nicht zum Runtime-Wiring oder zur Discidentitaet.");
    if (state.taskfile_phase >
            static_cast<std::uint8_t>(TaskfilePhase::DataOut) ||
        state.drive_owner >
            static_cast<std::uint8_t>(DriveOwner::Taskfile) ||
        state.data_cursor > state.data.size() ||
        state.packet.size() > 12u ||
        state.taskfile_host_byte_limit == 0u ||
        state.pending_guest_callbacks.size() >
            gdrom_guest_callback_capacity ||
        static_cast<std::uint8_t>(state.load_execution_policy) >
            static_cast<std::uint8_t>(
                DiscLoadExecutionPolicy::StandaloneTestMode))
        throw std::invalid_argument(
            "GD-ROM-Handoff besitzt ungueltige Transportdaten.");
    if ((!state.taskfile_data_source_range.known &&
         (state.taskfile_data_source_range.byte_offset != 0u ||
          state.taskfile_data_source_range.byte_count != 0u)) ||
        (state.taskfile_data_source_range.known &&
         (state.taskfile_data_source_range.byte_count !=
              state.data.size() ||
          state.taskfile_data_source_range.byte_offset >
              std::numeric_limits<std::uint64_t>::max() -
                  state.taskfile_data_source_range.byte_count)))
        throw std::invalid_argument(
            "GD-ROM-Handoff besitzt eine ungueltige Discquellrange.");
    const auto phase = static_cast<TaskfilePhase>(state.taskfile_phase);
    const auto scheduled =
        state.packet_event.has_value() ||
        state.packet_event_rehydration_pending;
    if (state.packet_event &&
        state.packet_event_rehydration_pending)
        throw std::invalid_argument(
            "GD-ROM-Handoff besitzt keinen eindeutigen Paket-Eventvertrag.");
    if ((phase == TaskfilePhase::Executing) != scheduled)
        throw std::invalid_argument(
            "GD-ROM-Handoff besitzt einen inkonsistenten Paketzustand.");
    if (phase == TaskfilePhase::PacketIn &&
        (!state.expecting_packet || state.packet.size() > 12u))
        throw std::invalid_argument(
            "GD-ROM-Handoff besitzt eine ungueltige Paketannahme.");
    if ((phase == TaskfilePhase::DataIn ||
         phase == TaskfilePhase::DataOut) &&
        (state.data.empty() ||
         state.taskfile_phase_remaining == 0u ||
         state.taskfile_phase_remaining >
             state.data.size() - state.data_cursor))
        throw std::invalid_argument(
            "GD-ROM-Handoff besitzt eine ungueltige PIO-Phase.");
    if (phase == TaskfilePhase::DmaIn &&
        (state.data.empty() ||
         state.data_cursor > state.data.size()))
        throw std::invalid_argument(
            "GD-ROM-Handoff besitzt eine ungueltige DMA-Phase.");

    std::set<std::uint32_t> request_ids;
    for (const auto& request : state.bios_requests) {
        if (request.id == 0u ||
            request.id >= state.next_bios_request ||
            !request_ids.insert(request.id).second ||
            static_cast<std::uint8_t>(request.state) >
                static_cast<std::uint8_t>(
                    GdRomBiosRequestState::Aborted) ||
            static_cast<std::uint8_t>(request.response.status) >
                static_cast<std::uint8_t>(GdRomStatus::Aborted) ||
            static_cast<std::uint8_t>(request.write_source) >
                static_cast<std::uint8_t>(CodeWriteSource::Fallback) ||
            static_cast<std::uint8_t>(request.transfer_kind) >
                static_cast<std::uint8_t>(
                    GdRomBiosTransferKind::Pio) ||
            request.stream_consumed_bytes >
                request.stream_total_bytes ||
            request.transfer_transferred > request.transfer_size ||
            (!request.guest_binding_present &&
             request.guest_address_space.has_value()))
            throw std::invalid_argument(
                "GD-ROM-Handoff besitzt eine ungueltige BIOS-Requestqueue.");
        if (request.transfer_active) {
            if (!request.transfer_buffer ||
                !request.guest_binding_present ||
                request.transfer_kind ==
                    GdRomBiosTransferKind::None ||
                request.transfer_buffer->size != request.transfer_size ||
                request.transfer_buffer->access !=
                    GuestBufferAccess::Write ||
                request.transfer_buffer->alignment == 0u ||
                !memory_.is_writable_linear_range(
                    request.transfer_buffer->physical_address,
                    request.transfer_buffer->size))
                throw std::invalid_argument(
                    "GD-ROM-Handoff besitzt keinen fortsetzbaren BIOS-Transfer.");
        } else if (request.transfer_buffer) {
            throw std::invalid_argument(
                "GD-ROM-Handoff besitzt einen Buffer ohne aktiven Transfer.");
        }
        if (request.guest_address_space) {
            auto restored_space =
                std::make_shared<RuntimeAddressSpace>();
            restored_space->validate_state_restore(
                *request.guest_address_space);
            restored_space->restore_state_passive(
                *request.guest_address_space);
            if (request.transfer_buffer) {
                const GuestAddressSpaceBinding binding{
                    std::move(restored_space),
                    request.guest_binding_privileged};
                const auto resolved = resolve_guest_write_buffer(
                    binding,
                    memory_,
                    request.transfer_buffer->guest_address,
                    request.transfer_buffer->size,
                    request.transfer_buffer->alignment);
                if (!resolved ||
                    resolved->physical_address !=
                        request.transfer_buffer->physical_address)
                    throw std::invalid_argument(
                        "GD-ROM-Handoff-BIOS-Buffer passt nicht zur MMU-Abbildung.");
            }
        }
    }
    if (state.next_bios_request == 0u ||
        state.next_bios_call_sequence == 0u ||
        state.next_load_transaction == 0u)
        throw std::invalid_argument(
            "GD-ROM-Handoff besitzt eine erschoepfte Sequenz.");
    std::uint64_t previous_call_sequence = 0u;
    std::uint64_t previous_call_cycle = 0u;
    for (const auto& event : state.bios_call_events) {
        if (event.sequence == 0u ||
            event.sequence >= state.next_bios_call_sequence ||
            event.sequence <= previous_call_sequence ||
            event.guest_cycle < previous_call_cycle ||
            event.guest_cycle > expected_scheduler_cycle ||
            static_cast<std::uint8_t>(event.state_before) >
                static_cast<std::uint8_t>(
                    GdRomBiosRequestState::Aborted) ||
            static_cast<std::uint8_t>(event.state_after) >
                static_cast<std::uint8_t>(
                    GdRomBiosRequestState::Aborted))
            throw std::invalid_argument(
                "GD-ROM-Handoff besitzt eine ungueltige BIOS-Aufrufhistorie.");
        previous_call_sequence = event.sequence;
        previous_call_cycle = event.guest_cycle;
    }
    if (static_cast<std::uint8_t>(state.last_bios_request.state) >
            static_cast<std::uint8_t>(
                GdRomBiosRequestState::Aborted) ||
        (state.last_bios_request.id != 0u &&
         state.last_bios_request.id >= state.next_bios_request) ||
        (state.last_bios_request.id == 0u &&
         state.last_bios_request.state !=
             GdRomBiosRequestState::None))
        throw std::invalid_argument(
            "GD-ROM-Handoff besitzt einen ungueltigen letzten BIOS-Request.");
    const auto valid_completion =
        [&request_ids](const bool pending, const std::uint32_t request) {
            return pending ? request != 0u && request_ids.contains(request)
                           : request == 0u;
        };
    if (!valid_completion(
            state.dma_completion_pending,
            state.dma_completion_request) ||
        !valid_completion(
            state.pio_completion_pending,
            state.pio_completion_request))
        throw std::invalid_argument(
            "GD-ROM-Handoff besitzt einen ungueltigen Completionvertrag.");
    for (const auto& callback : state.pending_guest_callbacks)
        if (callback.kind == GdRomBiosTransferKind::None ||
            static_cast<std::uint8_t>(callback.kind) >
                static_cast<std::uint8_t>(GdRomBiosTransferKind::Pio) ||
            callback.address == 0u ||
            callback.request_id == 0u ||
            !request_ids.contains(callback.request_id))
            throw std::invalid_argument(
                "GD-ROM-Handoff besitzt einen ungueltigen Gastcallback.");
}

void DreamcastGdRomController::restore_state_passive(
    const DreamcastGdRomSnapshot& state) {
    validate_state_restore(state);

    std::map<std::uint64_t, BiosRequest> restored_requests;
    for (const auto& source : state.bios_requests) {
        BiosRequest request;
        request.id = source.id;
        request.command = source.command;
        request.parameters = source.parameters;
        request.async_id = source.async_id;
        request.destination = source.destination;
        request.write_source = source.write_source;
        request.state = source.state;
        request.status = source.status;
        request.response = source.response;
        request.streaming_dma = source.streaming_dma;
        request.stream_lba = source.stream_lba;
        request.stream_sector_count = source.stream_sector_count;
        request.stream_total_bytes = source.stream_total_bytes;
        request.stream_consumed_bytes = source.stream_consumed_bytes;
        request.cached_stream_sector = source.cached_stream_sector;
        request.stream_sector_cache = source.stream_sector_cache;
        request.transfer_kind = source.transfer_kind;
        request.transfer_destination = source.transfer_destination;
        request.transfer_size = source.transfer_size;
        request.transfer_transferred = source.transfer_transferred;
        request.transfer_active = source.transfer_active;
        request.transfer_buffer = source.transfer_buffer;
        if (source.guest_binding_present) {
            GuestAddressSpaceBinding binding;
            binding.privileged = source.guest_binding_privileged;
            if (source.guest_address_space) {
                binding.address_space =
                    std::make_shared<RuntimeAddressSpace>();
                binding.address_space->restore_state_passive(
                    *source.guest_address_space);
            }
            request.guest_binding = std::move(binding);
        }
        restored_requests.emplace(request.id, std::move(request));
    }
    auto restored_packet = state.packet;
    auto restored_data = state.data;
    auto restored_bios_call_events = state.bios_call_events;
    auto restored_callbacks = state.pending_guest_callbacks;

    reader_.restore_state_passive(state.reader);
    if (packet_event_ && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*packet_event_));
    packet_event_.reset();
    packet_ = std::move(restored_packet);
    data_ = std::move(restored_data);
    data_cursor_ = state.data_cursor;
    taskfile_data_source_range_ =
        state.taskfile_data_source_range;
    taskfile_phase_remaining_ = state.taskfile_phase_remaining;
    taskfile_host_byte_limit_ = state.taskfile_host_byte_limit;
    taskfile_phase_ = static_cast<TaskfilePhase>(state.taskfile_phase);
    drive_owner_ = static_cast<DriveOwner>(state.drive_owner);
    command_irq_asserted_ = state.command_irq_asserted;
    command_irq_reassert_pending_ =
        state.command_irq_reassert_pending;
    taskfile_command_failed_ = state.taskfile_command_failed;
    clear_sense_after_data_ = state.clear_sense_after_data;
    set_mode_offset_ = state.set_mode_offset;
    drive_mode_ = state.drive_mode;
    sense_key_ = state.sense_key;
    sense_asc_ = state.sense_asc;
    sense_ascq_ = state.sense_ascq;
    status_ = state.status;
    error_ = state.error;
    interrupt_reason_ = state.interrupt_reason;
    features_ = state.features;
    sector_count_register_ = state.sector_count_register;
    sector_number_ = state.sector_number;
    drive_select_ = state.drive_select;
    byte_count_ = state.byte_count;
    current_fad_ = state.current_fad;
    expecting_packet_ = state.expecting_packet;
    bios_requests_ = std::move(restored_requests);
    next_bios_request_ = state.next_bios_request;
    last_bios_request_ = state.last_bios_request;
    bios_call_events_ = std::move(restored_bios_call_events);
    next_bios_call_sequence_ = state.next_bios_call_sequence;
    dropped_bios_call_events_ = state.dropped_bios_call_events;
    completed_commands_ = state.completed_commands;
    completed_dma_ = state.completed_dma;
    next_load_transaction_ = state.next_load_transaction;
    committed_load_transactions_ =
        state.committed_load_transactions;
    failed_load_transactions_ = state.failed_load_transactions;
    sector_mode_ = state.sector_mode;
    dma_callback_ = state.dma_callback;
    dma_callback_argument_ = state.dma_callback_argument;
    pio_callback_ = state.pio_callback;
    pio_callback_argument_ = state.pio_callback_argument;
    dma_completion_pending_ = state.dma_completion_pending;
    pio_completion_pending_ = state.pio_completion_pending;
    dma_completion_request_ = state.dma_completion_request;
    pio_completion_request_ = state.pio_completion_request;
    pending_guest_callbacks_ = std::move(restored_callbacks);
    coalesced_guest_callbacks_ = state.coalesced_guest_callbacks;
    dropped_guest_callbacks_ = state.dropped_guest_callbacks;
    packet_event_rehydration_pending_ =
        state.packet_event.has_value() ||
        state.packet_event_rehydration_pending;
}

SchedulerEventId DreamcastGdRomController::rehydrate_scheduled_event(
    const std::uint64_t guest_cycle,
    const std::uint32_t channel,
    const std::uint64_t token) {
    if (channel == dreamcast_gdrom_async_read_event_channel)
        return reader_.rehydrate_scheduled_event(
            guest_cycle,
            gdrom_async_read_event_channel,
            token);
    if (channel != dreamcast_gdrom_packet_event_channel ||
        token != dreamcast_gdrom_packet_event_token_v1)
        throw std::invalid_argument(
            "GD-ROM-Handoff besitzt einen unbekannten Eventkanal oder Token.");
    if (!packet_event_rehydration_pending_ || packet_event_ ||
        taskfile_phase_ != TaskfilePhase::Executing)
        throw std::logic_error(
            "GD-ROM-Handoff erwartet kein Paket-Completionevent.");
    if (guest_cycle < scheduler_.current_cycle())
        throw std::invalid_argument(
            "GD-ROM-Paketcompletion darf nicht in der Vergangenheit liegen.");
    const auto event_id = scheduler_.schedule_at(
        guest_cycle,
        [this](const auto restored_event_id, const auto cycle) {
            complete_packet(restored_event_id, cycle);
        },
        SchedulerEventKind::GdRomPacket);
    packet_event_ = event_id;
    packet_event_rehydration_pending_ = false;
    return event_id;
}

bool DreamcastGdRomController::event_rehydration_pending() const noexcept {
    return packet_event_rehydration_pending_ ||
           reader_.event_rehydration_pending();
}

namespace {

class GdStateWriter final {
  public:
    void u8(const std::uint8_t value) { bytes_.push_back(value); }
    void boolean(const bool value) { u8(value ? 1u : 0u); }
    void u32(const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void u64(const std::uint64_t value) {
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void string(const std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("GD-ROM-State-String ist zu gross.");
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void raw(const std::span<const std::uint8_t> value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("GD-ROM-State-Payload ist zu gross.");
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }
  private:
    std::vector<std::uint8_t> bytes_;
};

class GdStateReader final {
  public:
    explicit GdStateReader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {}
    [[nodiscard]] std::uint8_t u8() {
        require(1u);
        return bytes_[cursor_++];
    }
    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1u)
            throw std::invalid_argument(
                "GD-ROM-State besitzt ein ungueltiges Boolean.");
        return value != 0u;
    }
    [[nodiscard]] std::uint32_t u32() {
        std::uint32_t value = 0u;
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            value |= static_cast<std::uint32_t>(u8()) << (byte * 8u);
        return value;
    }
    [[nodiscard]] std::uint64_t u64() {
        std::uint64_t value = 0u;
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            value |= static_cast<std::uint64_t>(u8()) << (byte * 8u);
        return value;
    }
    [[nodiscard]] std::string string() {
        const auto size = u32();
        require(size);
        std::string result(
            reinterpret_cast<const char*>(bytes_.data() + cursor_), size);
        cursor_ += size;
        return result;
    }
    [[nodiscard]] std::vector<std::uint8_t> raw() {
        const auto size = u32();
        require(size);
        std::vector<std::uint8_t> result(
            bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_),
            bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_ + size));
        cursor_ += size;
        return result;
    }
    void finish() const {
        if (cursor_ != bytes_.size())
            throw std::invalid_argument(
                "GD-ROM-State besitzt nachlaufende Daten.");
    }
  private:
    void require(const std::size_t size) const {
        if (size > bytes_.size() - cursor_)
            throw std::invalid_argument("GD-ROM-State ist abgeschnitten.");
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_ = 0u;
};

void write_response(GdStateWriter& writer,
                    const GdRomResponse& response) {
    writer.u8(static_cast<std::uint8_t>(response.status));
    writer.raw(response.data);
    writer.u32(response.transferred_sectors);
}

GdRomResponse read_response(GdStateReader& reader) {
    GdRomResponse response;
    response.status = static_cast<GdRomStatus>(reader.u8());
    response.data = reader.raw();
    response.transferred_sectors = reader.u32();
    return response;
}

void write_tlb_mapping(GdStateWriter& writer,
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

TlbMapping read_tlb_mapping(GdStateReader& reader) {
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

void write_address_space(
    GdStateWriter& writer,
    const RuntimeAddressSpaceSnapshot& state) {
    writer.u8(static_cast<std::uint8_t>(state.mode));
    writer.u32(state.mmucr);
    writer.u8(state.asid);
    if (state.mappings.size() >
        std::numeric_limits<std::uint32_t>::max())
        throw std::length_error(
            "GD-ROM-State-MMU-Tabelle ist zu gross.");
    auto mappings = state.mappings;
    std::sort(
        mappings.begin(),
        mappings.end(),
        [](const auto& left, const auto& right) {
            return left.slot < right.slot;
        });
    writer.u32(static_cast<std::uint32_t>(mappings.size()));
    for (const auto& mapping : mappings)
        write_tlb_mapping(writer, mapping);
    for (const auto& mapping : state.itlb)
        write_tlb_mapping(writer, mapping);
    for (const auto valid : state.itlb_valid) writer.boolean(valid);
    for (const auto lru : state.itlb_lru) writer.u8(lru);
    for (const auto slot : state.itlb_source_slots) writer.u8(slot);
}

RuntimeAddressSpaceSnapshot read_address_space(GdStateReader& reader) {
    RuntimeAddressSpaceSnapshot state;
    state.mode = static_cast<AddressTranslationMode>(reader.u8());
    state.mmucr = reader.u32();
    state.asid = reader.u8();
    // Guard generations are process-local and deliberately not imported.
    state.address_space_generation = 0u;
    state.mmu_generation = 0u;
    state.watchpoint_generation = 0u;
    const auto mapping_count = reader.u32();
    if (mapping_count > 64u)
        throw std::invalid_argument(
            "GD-ROM-State besitzt zu viele UTLB-Abbildungen.");
    state.mappings.reserve(mapping_count);
    for (std::uint32_t index = 0u; index < mapping_count; ++index)
        state.mappings.push_back(read_tlb_mapping(reader));
    for (auto& mapping : state.itlb)
        mapping = read_tlb_mapping(reader);
    for (auto& valid : state.itlb_valid) valid = reader.boolean();
    for (auto& lru : state.itlb_lru) lru = reader.u8();
    for (auto& slot : state.itlb_source_slots) slot = reader.u8();
    return state;
}

void write_guest_buffer(
    GdStateWriter& writer,
    const std::optional<GuestLinearBuffer>& buffer) {
    writer.boolean(buffer.has_value());
    if (!buffer) return;
    writer.u32(buffer->guest_address);
    writer.u32(buffer->physical_address);
    writer.u64(buffer->size);
    writer.u64(buffer->alignment);
    writer.u8(static_cast<std::uint8_t>(buffer->access));
}

std::optional<GuestLinearBuffer>
read_guest_buffer(GdStateReader& reader) {
    if (!reader.boolean()) return std::nullopt;
    GuestLinearBuffer buffer;
    buffer.guest_address = reader.u32();
    buffer.physical_address = reader.u32();
    buffer.size = static_cast<std::size_t>(reader.u64());
    buffer.alignment = static_cast<std::size_t>(reader.u64());
    buffer.access = static_cast<GuestBufferAccess>(reader.u8());
    return buffer;
}

void write_bios_request(
    GdStateWriter& writer,
    const GdRomBiosRequestSnapshot& request) {
    writer.u32(request.id);
    writer.u32(request.command);
    for (const auto value : request.parameters) writer.u32(value);
    writer.u64(request.async_id);
    writer.u32(request.destination);
    writer.u8(static_cast<std::uint8_t>(request.write_source));
    writer.u8(static_cast<std::uint8_t>(request.state));
    for (const auto value : request.status) writer.u32(value);
    write_response(writer, request.response);
    writer.boolean(request.streaming_dma);
    writer.u32(request.stream_lba);
    writer.u32(request.stream_sector_count);
    writer.u64(request.stream_total_bytes);
    writer.u64(request.stream_consumed_bytes);
    writer.u32(request.cached_stream_sector);
    writer.raw(request.stream_sector_cache);
    writer.u8(static_cast<std::uint8_t>(request.transfer_kind));
    writer.u32(request.transfer_destination);
    writer.u32(request.transfer_size);
    writer.u32(request.transfer_transferred);
    writer.boolean(request.transfer_active);
    write_guest_buffer(writer, request.transfer_buffer);
    writer.boolean(request.guest_binding_present);
    writer.boolean(request.guest_binding_privileged);
    writer.boolean(request.guest_address_space.has_value());
    if (request.guest_address_space)
        write_address_space(writer, *request.guest_address_space);
}

GdRomBiosRequestSnapshot read_bios_request(GdStateReader& reader) {
    GdRomBiosRequestSnapshot request;
    request.id = reader.u32();
    request.command = reader.u32();
    for (auto& value : request.parameters) value = reader.u32();
    request.async_id = reader.u64();
    request.destination = reader.u32();
    request.write_source = static_cast<CodeWriteSource>(reader.u8());
    request.state = static_cast<GdRomBiosRequestState>(reader.u8());
    for (auto& value : request.status) value = reader.u32();
    request.response = read_response(reader);
    request.streaming_dma = reader.boolean();
    request.stream_lba = reader.u32();
    request.stream_sector_count = reader.u32();
    request.stream_total_bytes = reader.u64();
    request.stream_consumed_bytes = reader.u64();
    request.cached_stream_sector = reader.u32();
    request.stream_sector_cache = reader.raw();
    request.transfer_kind =
        static_cast<GdRomBiosTransferKind>(reader.u8());
    request.transfer_destination = reader.u32();
    request.transfer_size = reader.u32();
    request.transfer_transferred = reader.u32();
    request.transfer_active = reader.boolean();
    request.transfer_buffer = read_guest_buffer(reader);
    request.guest_binding_present = reader.boolean();
    request.guest_binding_privileged = reader.boolean();
    if (reader.boolean())
        request.guest_address_space = read_address_space(reader);
    return request;
}

void write_bios_status(GdStateWriter& writer,
                       const GdRomBiosRequestStatus& status) {
    writer.u32(status.id);
    writer.u32(status.command);
    writer.u8(static_cast<std::uint8_t>(status.state));
    for (const auto value : status.status) writer.u32(value);
}

GdRomBiosRequestStatus read_bios_status(GdStateReader& reader) {
    GdRomBiosRequestStatus status;
    status.id = reader.u32();
    status.command = reader.u32();
    status.state = static_cast<GdRomBiosRequestState>(reader.u8());
    for (auto& value : status.status) value = reader.u32();
    return status;
}

void write_bios_call(GdStateWriter& writer,
                     const GdRomBiosCallEvent& event) {
    writer.u64(event.sequence);
    writer.u64(event.guest_cycle);
    writer.u32(event.callsite);
    writer.u32(event.return_address);
    writer.u32(event.selector);
    writer.u32(event.super_selector);
    for (const auto value : event.arguments) writer.u32(value);
    writer.u32(event.request_id);
    writer.u8(static_cast<std::uint8_t>(event.state_before));
    writer.u8(static_cast<std::uint8_t>(event.state_after));
    writer.u32(event.result);
    for (const auto value : event.status) writer.u32(value);
}

GdRomBiosCallEvent read_bios_call(GdStateReader& reader) {
    GdRomBiosCallEvent event;
    event.sequence = reader.u64();
    event.guest_cycle = reader.u64();
    event.callsite = reader.u32();
    event.return_address = reader.u32();
    event.selector = reader.u32();
    event.super_selector = reader.u32();
    for (auto& value : event.arguments) value = reader.u32();
    event.request_id = reader.u32();
    event.state_before =
        static_cast<GdRomBiosRequestState>(reader.u8());
    event.state_after =
        static_cast<GdRomBiosRequestState>(reader.u8());
    event.result = reader.u32();
    for (auto& value : event.status) value = reader.u32();
    return event;
}

void write_callback(GdStateWriter& writer,
                    const GdRomGuestCallback& callback) {
    writer.u8(static_cast<std::uint8_t>(callback.kind));
    writer.u32(callback.address);
    writer.u32(callback.argument);
    writer.u32(callback.request_id);
}

GdRomGuestCallback read_callback(GdStateReader& reader) {
    return {static_cast<GdRomBiosTransferKind>(reader.u8()),
            reader.u32(),
            reader.u32(),
            reader.u32()};
}

} // namespace

std::vector<std::uint8_t>
encode_dreamcast_gdrom_state(const DreamcastGdRomSnapshot& state) {
    GdStateWriter writer;
    writer.string("KATGDC1");
    writer.u32(dreamcast_gdrom_state_contract_version);
    writer.raw(encode_gdrom_async_reader_state(state.reader));
    writer.raw(state.packet);
    writer.raw(state.data);
    writer.u64(state.data_cursor);
    writer.boolean(state.taskfile_data_source_range.known);
    writer.u64(state.taskfile_data_source_range.byte_offset);
    writer.u64(state.taskfile_data_source_range.byte_count);
    writer.u32(state.taskfile_phase_remaining);
    writer.u32(state.taskfile_host_byte_limit);
    writer.u8(state.taskfile_phase);
    writer.u8(state.drive_owner);
    writer.boolean(state.command_irq_asserted);
    writer.boolean(state.command_irq_reassert_pending);
    writer.boolean(state.taskfile_command_failed);
    writer.boolean(state.clear_sense_after_data);
    writer.u8(state.set_mode_offset);
    for (const auto value : state.drive_mode) writer.u8(value);
    writer.u8(state.sense_key);
    writer.u8(state.sense_asc);
    writer.u8(state.sense_ascq);
    writer.u8(state.status);
    writer.u8(state.error);
    writer.u8(state.interrupt_reason);
    writer.u8(state.features);
    writer.u8(state.sector_count_register);
    writer.u8(state.sector_number);
    writer.u8(state.drive_select);
    writer.u32(state.byte_count);
    writer.u32(state.current_fad);
    writer.boolean(state.expecting_packet);
    if (state.bios_requests.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        state.bios_call_events.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        state.pending_guest_callbacks.size() >
            std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("GD-ROM-State-Queue ist zu gross.");
    writer.u32(static_cast<std::uint32_t>(state.bios_requests.size()));
    for (const auto& request : state.bios_requests)
        write_bios_request(writer, request);
    writer.u32(state.next_bios_request);
    write_bios_status(writer, state.last_bios_request);
    writer.u32(static_cast<std::uint32_t>(state.bios_call_events.size()));
    for (const auto& event : state.bios_call_events)
        write_bios_call(writer, event);
    writer.u64(state.next_bios_call_sequence);
    writer.u64(state.dropped_bios_call_events);
    writer.u64(state.completed_commands);
    writer.u64(state.completed_dma);
    writer.u64(state.next_load_transaction);
    writer.u64(state.committed_load_transactions);
    writer.u64(state.failed_load_transactions);
    for (const auto value : state.sector_mode) writer.u32(value);
    writer.u32(state.dma_callback);
    writer.u32(state.dma_callback_argument);
    writer.u32(state.pio_callback);
    writer.u32(state.pio_callback_argument);
    writer.boolean(state.dma_completion_pending);
    writer.boolean(state.pio_completion_pending);
    writer.u32(state.dma_completion_request);
    writer.u32(state.pio_completion_request);
    writer.u32(
        static_cast<std::uint32_t>(state.pending_guest_callbacks.size()));
    for (const auto& callback : state.pending_guest_callbacks)
        write_callback(writer, callback);
    writer.u64(state.coalesced_guest_callbacks);
    writer.u64(state.dropped_guest_callbacks);
    // Process-local packet_event is deliberately omitted.
    writer.boolean(state.packet_event.has_value() ||
                   state.packet_event_rehydration_pending);
    writer.boolean(state.g1_bus_bound);
    writer.boolean(state.completion_observer_bound);
    writer.boolean(state.module_load_observer_bound);
    writer.boolean(state.command_ack_observer_bound);
    writer.boolean(state.load_transaction_executor_bound);
    writer.string(state.content_identity);
    writer.u8(static_cast<std::uint8_t>(state.load_execution_policy));
    return std::move(writer).finish();
}

DreamcastGdRomSnapshot
decode_dreamcast_gdrom_state(
    const std::span<const std::uint8_t> bytes) {
    GdStateReader reader(bytes);
    if (reader.string() != "KATGDC1" ||
        reader.u32() != dreamcast_gdrom_state_contract_version)
        throw std::invalid_argument(
            "GD-ROM-State besitzt Magic oder Version nicht.");
    DreamcastGdRomSnapshot state;
    state.reader = decode_gdrom_async_reader_state(reader.raw());
    state.packet = reader.raw();
    state.data = reader.raw();
    state.data_cursor = static_cast<std::size_t>(reader.u64());
    state.taskfile_data_source_range.known = reader.boolean();
    state.taskfile_data_source_range.byte_offset = reader.u64();
    state.taskfile_data_source_range.byte_count = reader.u64();
    state.taskfile_phase_remaining = reader.u32();
    state.taskfile_host_byte_limit = reader.u32();
    state.taskfile_phase = reader.u8();
    state.drive_owner = reader.u8();
    state.command_irq_asserted = reader.boolean();
    state.command_irq_reassert_pending = reader.boolean();
    state.taskfile_command_failed = reader.boolean();
    state.clear_sense_after_data = reader.boolean();
    state.set_mode_offset = reader.u8();
    for (auto& value : state.drive_mode) value = reader.u8();
    state.sense_key = reader.u8();
    state.sense_asc = reader.u8();
    state.sense_ascq = reader.u8();
    state.status = reader.u8();
    state.error = reader.u8();
    state.interrupt_reason = reader.u8();
    state.features = reader.u8();
    state.sector_count_register = reader.u8();
    state.sector_number = reader.u8();
    state.drive_select = reader.u8();
    state.byte_count = static_cast<std::uint16_t>(reader.u32());
    state.current_fad = reader.u32();
    state.expecting_packet = reader.boolean();
    const auto request_count = reader.u32();
    if (request_count > 65'536u)
        throw std::invalid_argument(
            "GD-ROM-State besitzt zu viele BIOS-Requests.");
    state.bios_requests.reserve(request_count);
    for (std::uint32_t index = 0u; index < request_count; ++index)
        state.bios_requests.push_back(read_bios_request(reader));
    state.next_bios_request = reader.u32();
    state.last_bios_request = read_bios_status(reader);
    const auto call_count = reader.u32();
    if (call_count > 65'536u)
        throw std::invalid_argument(
            "GD-ROM-State besitzt zu viele BIOS-Call-Events.");
    state.bios_call_events.reserve(call_count);
    for (std::uint32_t index = 0u; index < call_count; ++index)
        state.bios_call_events.push_back(read_bios_call(reader));
    state.next_bios_call_sequence = reader.u64();
    state.dropped_bios_call_events = reader.u64();
    state.completed_commands = reader.u64();
    state.completed_dma = reader.u64();
    state.next_load_transaction = reader.u64();
    state.committed_load_transactions = reader.u64();
    state.failed_load_transactions = reader.u64();
    for (auto& value : state.sector_mode) value = reader.u32();
    state.dma_callback = reader.u32();
    state.dma_callback_argument = reader.u32();
    state.pio_callback = reader.u32();
    state.pio_callback_argument = reader.u32();
    state.dma_completion_pending = reader.boolean();
    state.pio_completion_pending = reader.boolean();
    state.dma_completion_request = reader.u32();
    state.pio_completion_request = reader.u32();
    const auto callback_count = reader.u32();
    if (callback_count > gdrom_guest_callback_capacity)
        throw std::invalid_argument(
            "GD-ROM-State besitzt zu viele Gastcallbacks.");
    state.pending_guest_callbacks.reserve(callback_count);
    for (std::uint32_t index = 0u; index < callback_count; ++index)
        state.pending_guest_callbacks.push_back(read_callback(reader));
    state.coalesced_guest_callbacks = reader.u64();
    state.dropped_guest_callbacks = reader.u64();
    state.packet_event.reset();
    state.packet_event_rehydration_pending = reader.boolean();
    state.g1_bus_bound = reader.boolean();
    state.completion_observer_bound = reader.boolean();
    state.module_load_observer_bound = reader.boolean();
    state.command_ack_observer_bound = reader.boolean();
    state.load_transaction_executor_bound = reader.boolean();
    state.content_identity = reader.string();
    state.load_execution_policy =
        static_cast<DiscLoadExecutionPolicy>(reader.u8());
    reader.finish();
    return state;
}

void validate_dreamcast_gdrom_g1_restore_contract(
    const DreamcastGdRomSnapshot& gdrom,
    const DreamcastG1DmaSnapshot& g1) {
    if (g1.channel.active == 0u) return;
    if (!gdrom.g1_bus_bound ||
        !g1.transfer_handler_bound ||
        g1.channel.direction != 1u ||
        g1.channel.remaining == 0u)
        throw std::invalid_argument(
            "GD-ROM/G1-Handoff besitzt keinen gebundenen aktiven Transfer.");

    constexpr std::uint8_t taskfile_dma_in_phase = 4u;
    if (gdrom.taskfile_phase == taskfile_dma_in_phase) {
        if (gdrom.features != 1u ||
            (gdrom.status & ata_drq) != 0u ||
            gdrom.data_cursor > gdrom.data.size() ||
            gdrom.data_cursor < g1.channel.peripheral_counter ||
            g1.channel.remaining >
                gdrom.data.size() - gdrom.data_cursor)
            throw std::invalid_argument(
                "GD-ROM/G1-Handoff besitzt einen widerspruechlichen Taskfile-DMA-Prefix.");
        return;
    }

    const GdRomBiosRequestSnapshot* active_request = nullptr;
    for (const auto& request : gdrom.bios_requests) {
        if (!request.transfer_active ||
            request.transfer_kind != GdRomBiosTransferKind::Dma)
            continue;
        if (active_request)
            throw std::invalid_argument(
                "GD-ROM/G1-Handoff besitzt mehrere aktive DMA-Requests.");
        active_request = &request;
    }
    if (!active_request || !active_request->transfer_buffer ||
        active_request->transfer_transferred >
            active_request->transfer_size ||
        g1.channel.remaining !=
            active_request->transfer_size -
                active_request->transfer_transferred)
        throw std::invalid_argument(
            "GD-ROM/G1-Handoff besitzt keinen passenden BIOS-DMA-Request.");
    const auto expected_address =
        static_cast<std::uint64_t>(
            active_request->transfer_buffer->physical_address) +
        active_request->transfer_transferred;
    if (expected_address > std::numeric_limits<std::uint32_t>::max() ||
        canonical_physical_address(g1.channel.system_counter) !=
            canonical_physical_address(
                static_cast<std::uint32_t>(expected_address)))
        throw std::invalid_argument(
            "GD-ROM/G1-Handoff besitzt einen widerspruechlichen DMA-Zielcursor.");
}

const GdRomBiosRequestStatus& DreamcastGdRomController::last_bios_request() const noexcept {
    return last_bios_request_;
}

void DreamcastGdRomController::bind_g1_bus(DreamcastG1BusController* const g1_bus) noexcept {
    g1_bus_ = g1_bus;
}

void DreamcastGdRomController::handle_g1_dma_fault(const G1DmaFault& fault) noexcept {
    try {
        const auto illegal_address = fault.reason == HollyDmaFaultReason::IllegalAddress;
        const auto invalid_contract = fault.reason == HollyDmaFaultReason::InvalidLength ||
                                      fault.reason == HollyDmaFaultReason::InvalidDirection;
        const auto sense_key = static_cast<std::uint8_t>(
            illegal_address || invalid_contract ? 5u : 0x0Bu);
        const auto asc = static_cast<std::uint8_t>(illegal_address ? 0x21u
                                                   : invalid_contract ? 0x24u
                                                                      : 0u);

        if (taskfile_phase_ == TaskfilePhase::DmaIn) {
            fail_taskfile_command(sense_key, asc, 0u, true);
            return;
        }

        auto found = std::find_if(bios_requests_.begin(), bios_requests_.end(), [](auto& entry) {
            const auto& request = entry.second;
            return request.transfer_kind == GdRomBiosTransferKind::Dma &&
                   (request.transfer_active || request.state == GdRomBiosRequestState::Error);
        });
        if (found == bios_requests_.end()) return;

        auto& request = found->second;
        const auto transfer_base =
            request.stream_consumed_bytes >= request.transfer_transferred
                ? request.stream_consumed_bytes - request.transfer_transferred
                : 0u;
        const auto exact_prefix = std::min(fault.transferred_bytes, request.transfer_size);
        request.transfer_transferred = exact_prefix;
        request.stream_consumed_bytes = std::min<std::uint64_t>(
            request.stream_total_bytes, transfer_base + exact_prefix);
        request.status[2] = static_cast<std::uint32_t>(request.stream_consumed_bytes);
        request.status[3] = 0u;
        request.response.status = illegal_address || invalid_contract ? GdRomStatus::OutOfRange
                                                                      : GdRomStatus::Aborted;
        request.status[0] = sense_key;
        request.status[1] = static_cast<std::uint32_t>(request.response.status);
        request.state = GdRomBiosRequestState::Error;
        request.transfer_active = false;
        request.transfer_buffer.reset();
        request.transfer_kind = GdRomBiosTransferKind::None;
        pending_guest_callbacks_.erase(
            std::remove_if(pending_guest_callbacks_.begin(),
                           pending_guest_callbacks_.end(),
                           [&](const auto& callback) { return callback.request_id == request.id; }),
            pending_guest_callbacks_.end());
        if (dma_completion_request_ == request.id) {
            dma_completion_pending_ = false;
            dma_completion_request_ = 0u;
        }
        latch_sense(sense_key, asc, 0u, true);
        interrupt_reason_ = 0u;
        remember_bios_request(request);
    } catch (...) {
        // The DMA engine has already cancelled and quiesced its event before notification. A
        // diagnostic or host callback must never unwind back into the scheduler.
    }
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
    packet_event_rehydration_pending_ = false;
    packet_.clear();
    data_.clear();
    data_cursor_ = 0u;
    taskfile_data_source_range_ = {};
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

void DreamcastGdRomController::handle_scheduler_reset() noexcept {
    reset();
}

std::shared_ptr<DreamcastGdRomController>
map_dreamcast_gdrom(Memory& memory,
                    EventScheduler& scheduler,
                    GdRomDrive drive,
                    std::function<void(std::uint64_t)> completion_observer,
                    DreamcastGdRomController::ModuleLoadObserver module_load_observer,
                    std::function<void()> command_ack_observer,
                    DiscLoadTransactionExecutor load_transaction_executor,
                    std::string content_identity,
                    const DiscLoadExecutionPolicy load_execution_policy) {
    auto controller = std::make_shared<DreamcastGdRomController>(
        memory,
        scheduler,
        std::move(drive),
        std::move(completion_observer),
        std::move(module_load_observer),
        std::move(command_ack_observer),
        std::move(load_transaction_executor),
        std::move(content_identity),
        load_execution_policy);
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
