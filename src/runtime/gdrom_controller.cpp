#include "katana/runtime/gdrom_controller.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <algorithm>
#include <array>
#include <limits>
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
    : memory_(memory), drive_(std::move(drive)),
      reader_(scheduler, drive_, GdRomTiming{}, std::move(completion_observer)),
      module_load_observer_(std::move(module_load_observer)) {}

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
        if (packet_.size() == 12u) execute_packet();
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
        if ((value & 0x04u) != 0u) reset();
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
            publish_data(build_toc(0u));
            return;
        default:
            throw std::runtime_error("Unbekannter GD-ROM-Paketopcode.");
        }
    } catch (...) {
        status_ = ata_ready | ata_error;
        error_ = 0x04u;
        interrupt_reason_ = 3u;
        throw;
    }
}

std::uint32_t DreamcastGdRomController::fad_to_lba(const std::uint32_t fad) noexcept {
    return fad >= 150u ? fad - 150u : fad;
}

std::vector<std::uint8_t> DreamcastGdRomController::build_toc(const std::uint32_t session) const {
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

std::uint64_t DreamcastGdRomController::submit_bios_read(CpuState& cpu,
                                                         const std::uint32_t command,
                                                         const std::uint32_t parameters) {
    const auto fad = guest_read_u32(cpu, parameters);
    const auto count = guest_read_u32(cpu, parameters + 4u);
    const auto destination = guest_read_u32(cpu, parameters + 8u);
    const auto async_id = reader_.submit({GdRomCommand::ReadSectors, fad_to_lba(fad), count});
    bios_requests_.emplace(async_id,
                           BiosRequest{async_id,
                                       destination,
                                       command == 17u ? CodeWriteSource::Dma
                                                      : CodeWriteSource::Copy,
                                       false,
                                       {}});
    return async_id;
}

void DreamcastGdRomController::pump_completions() {
    while (auto completion = reader_.take_completed()) {
        const auto found = bios_requests_.find(completion->request_id);
        if (found == bios_requests_.end()) continue;
        found->second.response = std::move(completion->response);
        found->second.completed = true;
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
        ++completed_commands_;
    }
}

std::uint32_t DreamcastGdRomController::bios_call(CpuState& cpu,
                                                  const std::uint32_t selector,
                                                  const std::uint32_t super_selector) {
    if (super_selector == 0xFFFFFFFFu && selector == 0u) {
        reset();
        return 0u;
    }
    if (super_selector != 0u) return 0xFFFFFFFFu;
    pump_completions();
    if (selector == 0u) {
        const auto command = cpu.r[4];
        const auto parameters = cpu.r[5];
        if (command == 16u || command == 17u || command == 28u)
            return static_cast<std::uint32_t>(submit_bios_read(cpu, command, parameters));
        const auto id = next_immediate_request_++;
        BiosRequest request;
        request.completed = true;
        if (command == 18u || command == 19u) {
            const auto session = guest_read_u32(cpu, parameters);
            request.destination = guest_read_u32(cpu, parameters + 4u);
            request.response = {GdRomStatus::Good, build_toc(session), 0u};
            memory_.write_bytes(request.destination, request.response.data, CodeWriteSource::Copy);
        } else {
            request.response.status = GdRomStatus::Good;
        }
        bios_requests_.emplace(id, std::move(request));
        return static_cast<std::uint32_t>(id);
    }
    if (selector == 1u) {
        const auto found = bios_requests_.find(cpu.r[4]);
        if (found == bios_requests_.end()) return 0xFFFFFFFFu;
        if (!found->second.completed) return 0u;
        if (cpu.r[5] != 0u) {
            guest_write_u32(cpu, cpu.r[5], found->second.response.transferred_sectors,
                            CodeWriteSource::Copy);
            guest_write_u32(cpu,
                            cpu.r[5] + 4u,
                            static_cast<std::uint32_t>(found->second.response.status),
                            CodeWriteSource::Copy);
        }
        const auto result = found->second.response.status == GdRomStatus::Good ? 1u : 0xFFFFFFFFu;
        bios_requests_.erase(found);
        return result;
    }
    if (selector == 2u) {
        pump_completions();
        return 0u;
    }
    if (selector == 3u || selector == 8u || selector == 9u || selector == 10u) {
        if (selector == 9u) reset();
        return 0u;
    }
    if (selector == 4u) {
        if (cpu.r[4] != 0u) {
            guest_write_u32(cpu, cpu.r[4], 2u, CodeWriteSource::Copy);
            guest_write_u32(cpu, cpu.r[4] + 4u, 0x80u, CodeWriteSource::Copy);
        }
        return 0u;
    }
    if (selector == 5u || selector == 6u || selector == 7u) return 0u;
    return 0xFFFFFFFFu;
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
            completed_dma_};
}

void DreamcastGdRomController::reset() noexcept {
    packet_.clear();
    data_.clear();
    data_cursor_ = 0u;
    status_ = ata_ready;
    error_ = 0u;
    interrupt_reason_ = 0u;
    byte_count_ = 0u;
    expecting_packet_ = false;
    bios_requests_.clear();
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
