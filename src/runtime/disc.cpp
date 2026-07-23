#include "katana/runtime/disc.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
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
    const auto id = next_request_id_++;
    const auto ready_cycle = scheduler_.current_cycle() + duration;
    const auto event_id = scheduler_.schedule_at(
        ready_cycle,
        [this, id](const auto, const auto cycle) { complete(id, cycle); },
        SchedulerEventKind::DiscRead);
    pending_.push_back({id, ready_cycle, request, event_id});
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

void GdRomAsyncReader::complete(const std::uint64_t request_id, const std::uint64_t cycle) {
    const auto request = std::find_if(pending_.begin(), pending_.end(), [&](const auto& value) {
        return value.request_id == request_id;
    });
    if (request == pending_.end() || request->ready_cycle != cycle) {
        throw std::logic_error("GD-ROM-Schedulercompletion besitzt keinen Request.");
    }
    completed_.push_back({request->request_id, cycle, drive_.execute(request->request)});
    pending_.erase(request);
    std::sort(completed_.begin(), completed_.end(), [](const auto& left, const auto& right) {
        if (left.ready_cycle != right.ready_cycle) {
            return left.ready_cycle < right.ready_cycle;
        }
        return left.request_id < right.request_id;
    });
    if (completion_observer_) completion_observer_(cycle);
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
    result.next_request_id = next_request_id_;
    result.pending.reserve(pending_.size());
    for (const auto& request : pending_) {
        result.pending.push_back(
            {request.request_id, request.ready_cycle, request.request, request.event_id});
    }
    result.completed = completed_;
    return result;
}

} // namespace katana::runtime
