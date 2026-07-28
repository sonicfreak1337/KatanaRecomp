#include "katana/runtime/disc.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace katana::runtime {

std::vector<DiscTrackLayout> DiscSource::layout() const {
    return {{1u, 0u, DiscTrackKind::Data, 2048u, size() / 2048u, 1u}};
}
namespace {
void validate_identity(const std::string& identity) {
    if (identity.empty()) {
        throw std::invalid_argument(
            "Eine Disc-Quelle braucht eine nichtleere semantische Identitaet.");
    }
}

void validate_range(const std::uint64_t source_size,
                    const std::uint64_t offset,
                    const std::size_t length) {
    if (offset > source_size || static_cast<std::uint64_t>(length) > source_size - offset) {
        throw std::out_of_range("Disc-Lesezugriff liegt ausserhalb der Quelle.");
    }
}
} // namespace

std::vector<std::uint8_t> DiscSource::read(const std::uint64_t offset,
                                           const std::size_t length) const {
    std::vector<std::uint8_t> result(length);
    read(offset, result);
    return result;
}

MemoryDiscSource::MemoryDiscSource(const std::span<const std::uint8_t> bytes, std::string identity)
    : bytes_(bytes.begin(), bytes.end()), identity_(std::move(identity)) {
    validate_identity(identity_);
}

std::uint64_t MemoryDiscSource::size() const noexcept {
    return bytes_.size();
}
const std::string& MemoryDiscSource::identity() const noexcept {
    return identity_;
}

void MemoryDiscSource::read(const std::uint64_t offset,
                            const std::span<std::uint8_t> destination) const {
    validate_range(size(), offset, destination.size());
    const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    std::copy(begin, begin + static_cast<std::ptrdiff_t>(destination.size()), destination.begin());
}

FileDiscSource::FileDiscSource(std::filesystem::path path, std::string identity)
    : path_(std::move(path)), identity_(std::move(identity)) {
    validate_identity(identity_);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path_, error) || error) {
        throw std::invalid_argument("Disc-Dateiquelle ist keine lesbare regulaere Datei.");
    }
    size_ = std::filesystem::file_size(path_, error);
    if (error) {
        throw std::runtime_error("Groesse der Disc-Dateiquelle konnte nicht gelesen werden.");
    }
    stream_.open(path_, std::ios::binary);
    if (!stream_) {
        throw std::runtime_error(
            "Disc-Dateiquelle konnte nicht dauerhaft read-only geoeffnet werden.");
    }
}

std::uint64_t FileDiscSource::size() const noexcept {
    return size_;
}
const std::string& FileDiscSource::identity() const noexcept {
    return identity_;
}

void FileDiscSource::read(const std::uint64_t offset,
                          const std::span<std::uint8_t> destination) const {
    validate_range(size_, offset, destination.size());
    if (destination.empty()) {
        return;
    }
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        destination.size() >
            static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::out_of_range("Disc-Dateilesebereich ist fuer den Hoststream zu gross.");
    }
    const std::lock_guard lock(stream_mutex_);
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset));
    stream_.read(reinterpret_cast<char*>(destination.data()),
                 static_cast<std::streamsize>(destination.size()));
    if (!stream_ || stream_.gcount() != static_cast<std::streamsize>(destination.size())) {
        throw std::runtime_error("Disc-Dateiquelle lieferte einen unvollstaendigen Read.");
    }
    read_operations_.fetch_add(1u, std::memory_order_relaxed);
    bytes_read_.fetch_add(destination.size(), std::memory_order_relaxed);
}

std::uint64_t FileDiscSource::read_operations() const noexcept {
    return read_operations_.load(std::memory_order_relaxed);
}

std::uint64_t FileDiscSource::bytes_read() const noexcept {
    return bytes_read_.load(std::memory_order_relaxed);
}

std::uint64_t FileDiscSource::open_operations() const noexcept {
    return 1u;
}

GdRomDrive::GdRomDrive(std::shared_ptr<const DiscSource> source, const std::uint32_t sector_size)
    : source_(std::move(source)), sector_size_(sector_size) {
    if (!source_) {
        throw std::invalid_argument("GD-ROM-Laufwerk braucht eine Disc-Quelle.");
    }
    if (sector_size_ == 0u) {
        throw std::invalid_argument("GD-ROM-Sektorgroesse darf nicht null sein.");
    }
    layout_ = source_->layout();
    if (layout_.empty())
        throw std::invalid_argument("GD-ROM-Laufwerk braucht mindestens einen Disc-Track.");
}

namespace {
void append_be32(std::vector<std::uint8_t>& output, const std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24u));
    output.push_back(static_cast<std::uint8_t>(value >> 16u));
    output.push_back(static_cast<std::uint8_t>(value >> 8u));
    output.push_back(static_cast<std::uint8_t>(value));
}
} // namespace

GdRomResponse GdRomDrive::execute(const GdRomRequest& request) const {
    const auto sectors = source_->size() / sector_size_;
    if (request.command == GdRomCommand::TestUnitReady) {
        return {sectors == 0u ? GdRomStatus::NoMedia : GdRomStatus::Good, {}, 0u};
    }
    if (request.command == GdRomCommand::GetStatus) {
        return {
            GdRomStatus::Good,
            {static_cast<std::uint8_t>(sectors == 0u ? GdRomStatus::NoMedia : GdRomStatus::Good),
             0u,
             0u,
             0u},
            0u};
    }
    if (request.command == GdRomCommand::GetCapacity) {
        if (sectors == 0u) {
            return {GdRomStatus::NoMedia, {}, 0u};
        }
        if (sectors - 1u > std::numeric_limits<std::uint32_t>::max()) {
            return {GdRomStatus::OutOfRange, {}, 0u};
        }
        GdRomResponse response;
        append_be32(response.data, static_cast<std::uint32_t>(sectors - 1u));
        append_be32(response.data, sector_size_);
        return response;
    }
    if (request.command != GdRomCommand::ReadSectors) {
        return {GdRomStatus::InvalidCommand, {}, 0u};
    }
    if (request.sector_count == 0u) {
        return {GdRomStatus::InvalidField, {}, 0u};
    }
    const auto lba = static_cast<std::uint64_t>(request.lba);
    const auto count = static_cast<std::uint64_t>(request.sector_count);
    if (lba >= sectors || count > sectors - lba) {
        return {GdRomStatus::OutOfRange, {}, 0u};
    }
    const auto byte_count = count * sector_size_;
    const auto byte_offset = lba * sector_size_;
    if (byte_count > std::numeric_limits<std::size_t>::max()) {
        return {GdRomStatus::OutOfRange, {}, 0u};
    }
    try {
        return {GdRomStatus::Good,
                source_->read(byte_offset, static_cast<std::size_t>(byte_count)),
                request.sector_count};
    } catch (const std::out_of_range&) {
        return {GdRomStatus::OutOfRange, {}, 0u};
    }
}

std::uint32_t GdRomDrive::sector_size() const noexcept {
    return sector_size_;
}

std::uint64_t GdRomDrive::sector_count() const noexcept {
    return source_->size() / sector_size_;
}

const std::vector<DiscTrackLayout>& GdRomDrive::layout() const noexcept {
    return layout_;
}

const std::string& GdRomDrive::identity() const noexcept {
    return source_->identity();
}


GdRomAsyncReader::GdRomAsyncReader(EventScheduler& scheduler,
                                   GdRomDrive drive,
                                   const GdRomTiming timing,
                                   std::function<void(std::uint64_t)> completion_observer)
    : scheduler_(scheduler), scheduler_lifetime_(scheduler.lifetime_token()),
      drive_(std::move(drive)), timing_(timing),
      completion_observer_(std::move(completion_observer)) {
    reset_observer_ = scheduler_.add_reset_observer([this] { handle_scheduler_reset(); });
}

GdRomAsyncReader::~GdRomAsyncReader() {
    if (scheduler_lifetime_.expired()) return;
    for (const auto& request : pending_)
        static_cast<void>(scheduler_.cancel(request.event_id));
    static_cast<void>(scheduler_.remove_reset_observer(reset_observer_));
}

std::uint64_t GdRomAsyncReader::submit(const GdRomRequest& request) {
    const auto sectors = request.command == GdRomCommand::ReadSectors
                             ? static_cast<std::uint64_t>(request.sector_count)
                             : 0u;
    if (sectors != 0u &&
        timing_.cycles_per_sector >
            (std::numeric_limits<std::uint64_t>::max() - timing_.command_latency) / sectors) {
        throw std::out_of_range("GD-ROM-Requestlatenz laeuft ueber.");
    }
    const auto duration = timing_.command_latency + sectors * timing_.cycles_per_sector;
    if (scheduler_.current_cycle() > std::numeric_limits<std::uint64_t>::max() - duration) {
        throw std::out_of_range("GD-ROM-Fertigstellungszyklus laeuft ueber.");
    }
    if (next_request_id_ == 0u) {
        throw std::overflow_error("GD-ROM-Request-ID ist erschoepft.");
    }
    const auto id = next_request_id_;
    const auto ready_cycle = scheduler_.current_cycle() + duration;
    if (pending_.size() == std::numeric_limits<std::size_t>::max() ||
        completed_.size() >
            std::numeric_limits<std::size_t>::max() - pending_.size() - 1u)
        throw std::length_error("GD-ROM-Requestqueue ist erschoepft.");
    pending_.reserve(pending_.size() + 1u);
    completed_.reserve(completed_.size() + pending_.size() + 1u);
    pending_.push_back({id, ready_cycle, request, 0u});
    try {
        pending_.back().event_id = scheduler_.schedule_at(
            ready_cycle,
            [this, id](const auto, const auto cycle) { complete(id, cycle); },
            SchedulerEventKind::DiscRead);
    } catch (...) {
        pending_.pop_back();
        throw;
    }
    ++next_request_id_;
    return id;
}

bool GdRomAsyncReader::cancel(const std::uint64_t request_id) noexcept {
    const auto pending = std::find_if(pending_.begin(), pending_.end(), [&](const auto& value) {
        return value.request_id == request_id;
    });
    if (pending != pending_.end()) {
        if (!scheduler_lifetime_.expired())
            static_cast<void>(scheduler_.cancel(pending->event_id));
        pending_.erase(pending);
        return true;
    }
    const auto completed =
        std::find_if(completed_.begin(), completed_.end(), [&](const auto& value) {
            return value.request_id == request_id;
        });
    if (completed == completed_.end()) return false;
    completed_.erase(completed);
    return true;
}

void GdRomAsyncReader::reset() noexcept {
    if (!scheduler_lifetime_.expired()) {
        for (const auto& request : pending_)
            static_cast<void>(scheduler_.cancel(request.event_id));
    }
    pending_.clear();
    completed_.clear();
    next_request_id_ = 1u;
}

void GdRomAsyncReader::complete(const std::uint64_t request_id,
                                const std::uint64_t cycle) noexcept {
    const auto request = std::find_if(pending_.begin(), pending_.end(), [&](const auto& value) {
        return value.request_id == request_id;
    });
    if (request == pending_.end() || request->ready_cycle != cycle) return;

    const auto admitted_request = request->request;
    const auto admitted_id = request->request_id;
    pending_.erase(request);
    GdRomResponse response;
    try {
        response = drive_.execute(admitted_request);
    } catch (...) {
        response.status = GdRomStatus::Aborted;
        response.data.clear();
        response.transferred_sectors = 0u;
    }
    // submit() reserves one completion slot for every admitted pending request before it creates
    // the scheduler event, so this move cannot allocate in the callback.
    completed_.push_back({admitted_id, cycle, std::move(response)});
    std::sort(completed_.begin(), completed_.end(), [](const auto& left, const auto& right) {
        if (left.ready_cycle != right.ready_cycle) {
            return left.ready_cycle < right.ready_cycle;
        }
        return left.request_id < right.request_id;
    });
    if (completion_observer_) {
        try {
            completion_observer_(cycle);
        } catch (...) {
            // The completion remains queued. Host observers cannot unwind through the scheduler.
        }
    }
}

void GdRomAsyncReader::handle_scheduler_reset() noexcept {
    pending_.clear();
    completed_.clear();
    next_request_id_ = 1u;
}

std::optional<GdRomAsyncCompletion> GdRomAsyncReader::take_completed() {
    if (completed_.empty()) {
        return std::nullopt;
    }
    auto result = std::move(completed_.front());
    completed_.erase(completed_.begin());
    return result;
}

std::size_t GdRomAsyncReader::pending_count() const noexcept {
    return pending_.size();
}
std::uint64_t GdRomAsyncReader::current_cycle() const noexcept {
    return scheduler_.current_cycle();
}

GdRomAsyncReaderSnapshot GdRomAsyncReader::snapshot() const {
    GdRomAsyncReaderSnapshot result;
    result.scheduler_cycle = scheduler_.current_cycle();
    result.timing = timing_;
    result.drive_identity = drive_.identity();
    result.drive_sector_size = drive_.sector_size();
    result.next_request_id = next_request_id_;
    result.pending.reserve(pending_.size());
    for (const auto& request : pending_) {
        result.pending.push_back({request.request_id,
                                  request.ready_cycle,
                                  request.request,
                                  request.event_id,
                                  request.event_rehydration_pending});
    }
    result.completed = completed_;
    return result;
}

void GdRomAsyncReader::validate_state_restore(
    const GdRomAsyncReaderSnapshot& state) const {
    validate_state_restore(state, scheduler_.current_cycle());
}

void GdRomAsyncReader::validate_state_restore(
    const GdRomAsyncReaderSnapshot& state,
    const std::uint64_t expected_scheduler_cycle) const {
    if (state.scheduler_cycle != expected_scheduler_cycle)
        throw std::invalid_argument(
            "GD-ROM-Reader-Handoff passt nicht zur wiederhergestellten Gastzeit.");
    if (state.timing.command_latency != timing_.command_latency ||
        state.timing.cycles_per_sector != timing_.cycles_per_sector)
        throw std::invalid_argument(
            "GD-ROM-Reader-Handoff passt nicht zum Runtime-Timingvertrag.");
    if (state.drive_identity != drive_.identity() ||
        state.drive_sector_size != drive_.sector_size())
        throw std::invalid_argument(
            "GD-ROM-Reader-Handoff passt nicht zur gebundenen Discidentitaet.");
    if (state.next_request_id == 0u)
        throw std::invalid_argument(
            "GD-ROM-Reader-Handoff besitzt keine gueltige naechste Request-ID.");

    std::set<std::uint64_t> request_ids;
    for (const auto& pending : state.pending) {
        if (pending.request_id == 0u ||
            pending.request_id >= state.next_request_id ||
            pending.ready_cycle < state.scheduler_cycle ||
            !request_ids.insert(pending.request_id).second)
            throw std::invalid_argument(
                "GD-ROM-Reader-Handoff besitzt eine ungueltige Pending-Queue.");
        if ((pending.event_id != 0u) ==
            pending.event_rehydration_pending)
            throw std::invalid_argument(
                "GD-ROM-Reader-Handoff besitzt keinen eindeutigen Eventvertrag.");
        if (static_cast<std::uint8_t>(pending.request.command) >
            static_cast<std::uint8_t>(GdRomCommand::ReadSectors))
            throw std::invalid_argument(
                "GD-ROM-Reader-Handoff besitzt ein unbekanntes Kommando.");
    }
    std::uint64_t previous_cycle = 0u;
    std::uint64_t previous_id = 0u;
    bool first = true;
    for (const auto& completed : state.completed) {
        if (completed.request_id == 0u ||
            completed.request_id >= state.next_request_id ||
            completed.ready_cycle > state.scheduler_cycle ||
            !request_ids.insert(completed.request_id).second ||
            static_cast<std::uint8_t>(completed.response.status) >
                static_cast<std::uint8_t>(GdRomStatus::Aborted))
            throw std::invalid_argument(
                "GD-ROM-Reader-Handoff besitzt eine ungueltige Completion-Queue.");
        if (!first &&
            (completed.ready_cycle < previous_cycle ||
             (completed.ready_cycle == previous_cycle &&
              completed.request_id <= previous_id)))
            throw std::invalid_argument(
                "GD-ROM-Reader-Handoff-Completion-Queue ist nicht stabil sortiert.");
        first = false;
        previous_cycle = completed.ready_cycle;
        previous_id = completed.request_id;
    }
}

void GdRomAsyncReader::restore_state_passive(
    const GdRomAsyncReaderSnapshot& state) {
    validate_state_restore(state);

    std::vector<Pending> restored_pending;
    restored_pending.reserve(state.pending.size());
    for (const auto& pending : state.pending)
        restored_pending.push_back({pending.request_id,
                                    pending.ready_cycle,
                                    pending.request,
                                    0u,
                                    true});
    auto restored_completed = state.completed;

    if (!scheduler_lifetime_.expired()) {
        for (const auto& pending : pending_)
            if (pending.event_id != 0u)
                static_cast<void>(scheduler_.cancel(pending.event_id));
    }
    pending_ = std::move(restored_pending);
    completed_ = std::move(restored_completed);
    next_request_id_ = state.next_request_id;
}

SchedulerEventId GdRomAsyncReader::rehydrate_scheduled_event(
    const std::uint64_t guest_cycle,
    const std::uint32_t channel,
    const std::uint64_t token) {
    if (channel != gdrom_async_read_event_channel || token == 0u)
        throw std::invalid_argument(
            "GD-ROM-Reader-Handoff besitzt einen unbekannten Eventkanal oder Token.");
    const auto pending =
        std::find_if(pending_.begin(), pending_.end(), [token](const auto& value) {
            return value.request_id == token;
        });
    if (pending == pending_.end() || !pending->event_rehydration_pending ||
        pending->event_id != 0u)
        throw std::logic_error(
            "GD-ROM-Reader-Handoff erwartet dieses Completionevent nicht.");
    if (guest_cycle != pending->ready_cycle ||
        guest_cycle < scheduler_.current_cycle())
        throw std::invalid_argument(
            "GD-ROM-Reader-Completion passt nicht zur gespeicherten Gastzeit.");
    const auto request_id = pending->request_id;
    const auto event_id = scheduler_.schedule_at(
        guest_cycle,
        [this, request_id](const auto, const auto cycle) {
            complete(request_id, cycle);
        },
        SchedulerEventKind::DiscRead);
    pending->event_id = event_id;
    pending->event_rehydration_pending = false;
    return event_id;
}

bool GdRomAsyncReader::event_rehydration_pending() const noexcept {
    return std::any_of(pending_.begin(), pending_.end(), [](const auto& value) {
        return value.event_rehydration_pending;
    });
}

namespace {

class DiscStateWriter final {
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
            throw std::length_error("GD-ROM-Reader-State-String ist zu gross.");
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void raw(const std::span<const std::uint8_t> value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("GD-ROM-Reader-State-Payload ist zu gross.");
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

class DiscStateReader final {
  public:
    explicit DiscStateReader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {}
    [[nodiscard]] std::uint8_t u8() {
        require(1u);
        return bytes_[cursor_++];
    }
    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1u)
            throw std::invalid_argument(
                "GD-ROM-Reader-State besitzt ein ungueltiges Boolean.");
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
                "GD-ROM-Reader-State besitzt nachlaufende Daten.");
    }

  private:
    void require(const std::size_t size) const {
        if (size > bytes_.size() - cursor_)
            throw std::invalid_argument(
                "GD-ROM-Reader-State ist abgeschnitten.");
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_ = 0u;
};

void write_response(DiscStateWriter& writer, const GdRomResponse& response) {
    writer.u8(static_cast<std::uint8_t>(response.status));
    writer.raw(response.data);
    writer.u32(response.transferred_sectors);
}

GdRomResponse read_response(DiscStateReader& reader) {
    GdRomResponse response;
    response.status = static_cast<GdRomStatus>(reader.u8());
    response.data = reader.raw();
    response.transferred_sectors = reader.u32();
    return response;
}

} // namespace

std::vector<std::uint8_t>
encode_gdrom_async_reader_state(const GdRomAsyncReaderSnapshot& state) {
    DiscStateWriter writer;
    writer.string("KATGDR1");
    writer.u32(gdrom_async_reader_state_contract_version);
    writer.u64(state.scheduler_cycle);
    writer.u64(state.timing.command_latency);
    writer.u64(state.timing.cycles_per_sector);
    writer.string(state.drive_identity);
    writer.u32(state.drive_sector_size);
    writer.u64(state.next_request_id);
    if (state.pending.size() > std::numeric_limits<std::uint32_t>::max() ||
        state.completed.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("GD-ROM-Reader-State-Queue ist zu gross.");
    writer.u32(static_cast<std::uint32_t>(state.pending.size()));
    for (const auto& pending : state.pending) {
        writer.u64(pending.request_id);
        writer.u64(pending.ready_cycle);
        writer.u8(static_cast<std::uint8_t>(pending.request.command));
        writer.u32(pending.request.lba);
        writer.u32(pending.request.sector_count);
        // Process-local IDs are deliberately not serialized.
        writer.boolean(true);
    }
    writer.u32(static_cast<std::uint32_t>(state.completed.size()));
    for (const auto& completed : state.completed) {
        writer.u64(completed.request_id);
        writer.u64(completed.ready_cycle);
        write_response(writer, completed.response);
    }
    return std::move(writer).finish();
}

GdRomAsyncReaderSnapshot
decode_gdrom_async_reader_state(const std::span<const std::uint8_t> bytes) {
    DiscStateReader reader(bytes);
    if (reader.string() != "KATGDR1" ||
        reader.u32() != gdrom_async_reader_state_contract_version)
        throw std::invalid_argument(
            "GD-ROM-Reader-State besitzt Magic oder Version nicht.");
    GdRomAsyncReaderSnapshot state;
    state.scheduler_cycle = reader.u64();
    state.timing.command_latency = reader.u64();
    state.timing.cycles_per_sector = reader.u64();
    state.drive_identity = reader.string();
    state.drive_sector_size = reader.u32();
    state.next_request_id = reader.u64();
    const auto pending_count = reader.u32();
    state.pending.reserve(pending_count);
    for (std::uint32_t index = 0u; index < pending_count; ++index) {
        GdRomAsyncPendingSnapshot pending;
        pending.request_id = reader.u64();
        pending.ready_cycle = reader.u64();
        pending.request.command = static_cast<GdRomCommand>(reader.u8());
        pending.request.lba = reader.u32();
        pending.request.sector_count = reader.u32();
        pending.event_id = 0u;
        pending.event_rehydration_pending = reader.boolean();
        state.pending.push_back(std::move(pending));
    }
    const auto completed_count = reader.u32();
    state.completed.reserve(completed_count);
    for (std::uint32_t index = 0u; index < completed_count; ++index)
        state.completed.push_back(
            {reader.u64(), reader.u64(), read_response(reader)});
    reader.finish();
    return state;
}

} // namespace katana::runtime
