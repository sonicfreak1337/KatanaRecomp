#include "katana/runtime/gdrom_controller.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

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
} // namespace

DreamcastGdRomController::DreamcastGdRomController(
    Memory& memory,
    EventScheduler& scheduler,
    GdRomDrive drive,
    std::function<void(std::uint64_t)> completion_observer,
    ModuleLoadObserver module_load_observer)
    : memory_(memory), scheduler_(scheduler), drive_(std::move(drive)),
      reader_(scheduler, drive_, GdRomTiming{}, completion_observer),
      module_load_observer_(std::move(module_load_observer)),
      completion_observer_(std::move(completion_observer)),
      scheduler_lifetime_(scheduler.lifetime_token()) {
    sector_mode_[3] = drive_.sector_size();
}

DreamcastGdRomController::~DreamcastGdRomController() {
    if (packet_event_ && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*packet_event_));
}

std::uint32_t DreamcastGdRomController::read(const std::uint32_t offset,
                                             const MemoryAccessWidth width) {
    pump_completions();
    if (offset == 0x80u) {
        if (width != MemoryAccessWidth::Halfword || (status_ & ata_drq) == 0u)
            throw std::runtime_error("GD-ROM-Datenregister braucht aktives 16-Bit-PIO.");
        const auto low = data_cursor_ < data_.size() ? data_[data_cursor_++] : 0u;
        const auto high = data_cursor_ < data_.size() ? data_[data_cursor_++] : 0u;
        if (data_cursor_ == data_.size()) {
            status_ = ata_ready;
            interrupt_reason_ = 3u;
        }
        return static_cast<std::uint32_t>(low) | (static_cast<std::uint32_t>(high) << 8u);
    }
    if (width != MemoryAccessWidth::Byte)
        throw std::runtime_error("GD-ROM-Taskfile-Register erfordern 8-Bit-Zugriffe.");
    switch (offset) {
    case 0x84u:
        return error_;
    case 0x88u:
        return interrupt_reason_;
    case 0x90u:
        return byte_count_ & 0xFFu;
    case 0x94u:
        return byte_count_ >> 8u;
    case 0x9Cu:
    case 0xA0u:
        return status_;
    default:
        throw std::runtime_error("Unbekannter GD-ROM-Taskfile-Leseoffset.");
    }
}

void DreamcastGdRomController::write(const std::uint32_t offset,
                                     const std::uint32_t value,
                                     const MemoryAccessWidth width) {
    if (offset == 0x80u) {
        if (width != MemoryAccessWidth::Halfword || !expecting_packet_)
            throw std::runtime_error("GD-ROM-Datenregister braucht eine 16-Bit-Paketphase.");
        packet_.push_back(static_cast<std::uint8_t>(value));
        packet_.push_back(static_cast<std::uint8_t>(value >> 8u));
        if (packet_.size() == 12u) schedule_packet();
        return;
    }
    if (width != MemoryAccessWidth::Byte)
        throw std::runtime_error("GD-ROM-Taskfile-Register erfordern 8-Bit-Zugriffe.");
    switch (offset) {
    case 0x90u:
        byte_count_ = static_cast<std::uint16_t>((byte_count_ & 0xFF00u) | (value & 0xFFu));
        return;
    case 0x94u:
        byte_count_ = static_cast<std::uint16_t>((byte_count_ & 0x00FFu) | ((value & 0xFFu) << 8u));
        return;
    case 0x9Cu:
        if ((value & 0xFFu) != 0xA0u)
            throw std::runtime_error("GD-ROM akzeptiert im Produktpfad nur ATA PACKET.");
        packet_.clear();
        expecting_packet_ = true;
        status_ = ata_ready | ata_drq;
        interrupt_reason_ = 1u;
        return;
    case 0xA0u:
        if ((value & 0x04u) != 0u) reset_transport();
        return;
    default:
        throw std::runtime_error("Unbekannter GD-ROM-Taskfile-Schreiboffset.");
    }
}

void DreamcastGdRomController::publish_data(std::vector<std::uint8_t> data) {
    data_ = std::move(data);
    data_cursor_ = 0u;
    byte_count_ = static_cast<std::uint16_t>(std::min<std::size_t>(data_.size(), 0xFFFFu));
    status_ = data_.empty() ? ata_ready : static_cast<std::uint8_t>(ata_ready | ata_drq);
    interrupt_reason_ = data_.empty() ? 3u : 2u;
    ++completed_commands_;
}

void DreamcastGdRomController::execute_packet() {
    expecting_packet_ = false;
    if (packet_.size() != 12u) throw std::logic_error("GD-ROM-Paket ist nicht vollstaendig.");
    try {
        switch (packet_[0]) {
        case 0x00u:
            publish_data({});
            return;
        case 0x03u:
            publish_data(std::vector<std::uint8_t>(18u, 0u));
            return;
        case 0x12u: {
            std::vector<std::uint8_t> inquiry(36u, 0u);
            inquiry[0] = 0x05u;
            inquiry[1] = 0x80u;
            inquiry[2] = 0u;
            inquiry[4] = 31u;
            const std::string vendor = "SEGA    GD-ROM DRIVE    ";
            std::copy(vendor.begin(), vendor.end(), inquiry.begin() + 8u);
            publish_data(std::move(inquiry));
            return;
        }
        case 0x28u: {
            const auto response = drive_.execute(
                {GdRomCommand::ReadSectors, be32(packet_, 2u), be16(packet_, 7u)});
            if (response.status != GdRomStatus::Good)
                throw std::out_of_range("GD-ROM READ(10) liegt ausserhalb der Disc.");
            publish_data(response.data);
            return;
        }
        case 0x30u: {
            const auto response = drive_.execute({GdRomCommand::ReadSectors,
                                                  fad_to_lba(be24(packet_, 2u)),
                                                  be24(packet_, 8u)});
            if (response.status != GdRomStatus::Good)
                throw std::out_of_range("GD-ROM READ CD liegt ausserhalb der Disc.");
            publish_data(response.data);
            return;
        }
        case 0x43u:
            publish_data(build_packet_toc(0u));
            return;
        default:
            throw std::runtime_error("Unbekannter GD-ROM-Paketopcode.");
        }
    } catch (const std::exception&) {
        status_ = ata_ready | ata_error;
        error_ = 0x04u;
        interrupt_reason_ = 3u;
    }
}

void DreamcastGdRomController::schedule_packet() {
    if (packet_event_) throw std::logic_error("GD-ROM-Paketkommando ist bereits aktiv.");
    expecting_packet_ = false;
    status_ = 0x80u;
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
        status_ = ata_ready | ata_error;
        error_ = 0x04u;
        interrupt_reason_ = 3u;
    }
    if (completion_observer_) completion_observer_(cycle);
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
    request.destination = request.parameters[2];
    request.write_source = request.command == 17u ? CodeWriteSource::Dma : CodeWriteSource::Copy;
    request.async_id = reader_.submit({GdRomCommand::ReadSectors,
                                       fad_to_lba(request.parameters[0]),
                                       request.parameters[1]});
    request.state = GdRomBiosRequestState::Processing;
    request.status[3] = 1u;
    remember_bios_request(request);
}

void DreamcastGdRomController::execute_bios_request(CpuState& cpu, BiosRequest& request) {
    if (request.state != GdRomBiosRequestState::Queued) return;
    if (request.command == 16u || request.command == 17u || request.command == 28u) {
        submit_bios_read(request);
        return;
    }
    if (request.command == 18u || request.command == 19u) {
        if (request.parameters[0] > 1u || request.parameters[1] == 0u) {
            request.response.status = GdRomStatus::InvalidField;
            request.status = {5u, static_cast<std::uint32_t>(GdRomStatus::InvalidField), 0u, 0u};
            request.state = GdRomBiosRequestState::Error;
            remember_bios_request(request);
            return;
        }
        const auto toc = build_bios_toc(request.parameters[0]);
        for (std::size_t index = 0u; index < toc.size(); ++index)
            guest_write_u32(cpu,
                            request.parameters[1] + static_cast<std::uint32_t>(index * 4u),
                            toc[index],
                            CodeWriteSource::Copy);
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
        if (found->second.response.status == GdRomStatus::Good &&
            !found->second.response.data.empty()) {
            memory_.write_bytes(found->second.destination,
                                found->second.response.data,
                                found->second.write_source);
            if (module_load_observer_)
                module_load_observer_(found->second.destination,
                                      found->second.response.data,
                                      drive_.identity());
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
        } else {
            const auto error = found->second.response.status == GdRomStatus::NoMedia ? 2u : 5u;
            found->second.status = {error,
                                    static_cast<std::uint32_t>(found->second.response.status),
                                    0u,
                                    0u};
            found->second.state = GdRomBiosRequestState::Error;
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
        module_load_observer_(destination, response.data, drive_.identity());
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
        if (!bios_requests_.empty()) return finish(0u);
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
        remember_bios_request(bios_requests_.at(id));
        return finish(id);
    }
    if (selector == 1u) {
        const auto found = bios_requests_.find(cpu.r[4]);
        if (found == bios_requests_.end()) return finish(0u);
        if (cpu.r[5] != 0u) {
            for (std::size_t index = 0u; index < found->second.status.size(); ++index)
                guest_write_u32(cpu,
                                cpu.r[5] + static_cast<std::uint32_t>(index * 4u),
                                found->second.status[index],
                                CodeWriteSource::Copy);
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
            return finish(2u);
        case GdRomBiosRequestState::Error:
            remember_bios_request(found->second);
            bios_requests_.erase(found);
            return finish(0xFFFFFFFFu);
        case GdRomBiosRequestState::Aborted:
            remember_bios_request(found->second);
            bios_requests_.erase(found);
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
        aborted.status = {0u,
                          static_cast<std::uint32_t>(GdRomStatus::Aborted),
                          aborted.status[2],
                          0u};
        aborted.response.status = GdRomStatus::Aborted;
        aborted.state = GdRomBiosRequestState::Aborted;
        bios_requests_.erase(found);
        remember_bios_request(aborted);
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
                    return request.second.state == GdRomBiosRequestState::Processing;
                });
            const auto busy = bios_busy || packet_event_.has_value();
            guest_write_u32(cpu, cpu.r[4], busy ? 0u : 1u, CodeWriteSource::Copy);
            guest_write_u32(cpu, cpu.r[4] + 4u, busy ? 0u : 0x80u, CodeWriteSource::Copy);
        }
        return finish(0u);
    }
    if (selector == 5u || selector == 11u) {
        auto& callback = selector == 5u ? dma_callback_ : pio_callback_;
        auto& argument = selector == 5u ? dma_callback_argument_ : pio_callback_argument_;
        callback = cpu.r[4];
        argument = cpu.r[5];
        return finish(0u);
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
            const auto supported_data_view = requested[1] == 0x2000u &&
                                             requested[3] == drive_.sector_size();
            if (!supported_data_view || !valid_track_type)
                return finish(0xFFFFFFFFu);
            sector_mode_ = requested;
            return finish(0u);
        } catch (const MemoryAccessError&) {
            return finish(0xFFFFFFFFu);
        }
    }
    if (selector == 6u || selector == 7u || selector == 12u || selector == 13u)
        return finish(0xFFFFFFFFu);
    return finish(0xFFFFFFFFu);
}

void DreamcastGdRomController::dma_to_memory(const std::uint32_t address,
                                             const std::uint32_t length,
                                             const std::uint32_t direction) {
    if (direction != 0u)
        throw std::runtime_error("GD-ROM-G1-DMA unterstuetzt nur Laufwerk-zu-Systemspeicher.");
    if (length == 0u || data_cursor_ > data_.size() || length > data_.size() - data_cursor_)
        throw std::out_of_range("GD-ROM-G1-DMA fordert mehr Daten als die aktive PIO-Phase.");
    memory_.write_bytes(address,
                        std::span<const std::uint8_t>(data_).subspan(data_cursor_, length),
                        CodeWriteSource::Dma);
    if (module_load_observer_)
        module_load_observer_(address,
                              std::span<const std::uint8_t>(data_).subspan(data_cursor_, length),
                              drive_.identity());
    data_cursor_ += length;
    if (data_cursor_ == data_.size()) {
        status_ = ata_ready;
        interrupt_reason_ = 3u;
    }
    ++completed_dma_;
}

GdRomProductStatus DreamcastGdRomController::status() const noexcept {
    return {status_,
            interrupt_reason_,
            data_cursor_ <= data_.size() ? data_.size() - data_cursor_ : 0u,
            bios_requests_.size(),
            completed_commands_,
            completed_dma_,
            sector_mode_,
            dma_callback_,
            dma_callback_argument_,
            pio_callback_,
            pio_callback_argument_};
}

const GdRomBiosRequestStatus& DreamcastGdRomController::last_bios_request() const noexcept {
    return last_bios_request_;
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
    if (packet_event_ && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*packet_event_));
    packet_event_.reset();
    packet_.clear();
    data_.clear();
    data_cursor_ = 0u;
    status_ = ata_ready;
    error_ = 0u;
    interrupt_reason_ = 0u;
    byte_count_ = 0u;
    expecting_packet_ = false;
    reader_.reset();
    bios_requests_.clear();
    next_bios_request_ = 1u;
    last_bios_request_ = {};
}

void DreamcastGdRomController::reset() noexcept {
    reset_transport();
    sector_mode_ = {0u, 0x2000u, 1024u, drive_.sector_size()};
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
                    DreamcastGdRomController::ModuleLoadObserver module_load_observer) {
    auto controller = std::make_shared<DreamcastGdRomController>(
        memory,
        scheduler,
        std::move(drive),
        std::move(completion_observer),
        std::move(module_load_observer));
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
