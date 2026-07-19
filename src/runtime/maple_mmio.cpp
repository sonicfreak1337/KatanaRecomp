#include "katana/runtime/maple_mmio.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace katana::runtime {
namespace {
constexpr std::size_t maximum_dma_descriptors = 1'024u;
constexpr std::size_t maximum_maple_frame_words = 256u;

std::uint32_t swap_word(const std::uint32_t value) noexcept {
    return ((value & 0x000000FFu) << 24u) | ((value & 0x0000FF00u) << 8u) |
           ((value & 0x00FF0000u) >> 8u) | ((value & 0xFF000000u) >> 24u);
}

std::uint32_t checked_address_add(const std::uint32_t address, const std::size_t bytes) {
    if (bytes > std::numeric_limits<std::uint32_t>::max() - address)
        throw std::out_of_range("Maple-DMA-Adresse laeuft ueber.");
    return address + static_cast<std::uint32_t>(bytes);
}
} // namespace

DreamcastMapleController::DreamcastMapleController(Memory& memory,
                                                   EventScheduler& scheduler,
                                                   std::shared_ptr<MapleBus> bus,
                                                   const MapleDmaTiming timing,
                                                   std::function<void()> completion_observer)
    : memory_(memory), scheduler_(scheduler), bus_(std::move(bus)), timing_(timing),
      completion_observer_(std::move(completion_observer)),
      scheduler_lifetime_(scheduler.lifetime_token()) {
    if (!bus_) throw std::invalid_argument("Maple-MMIO braucht einen Maple-Bus.");
    if (timing_.cycles_per_word == 0u)
        throw std::invalid_argument("Maple-DMA-Timing braucht positive Zyklen pro Wort.");
    reset_observer_ = scheduler_.add_reset_observer([this] { handle_scheduler_reset(); });
    reset();
}

DreamcastMapleController::~DreamcastMapleController() {
    if (scheduler_lifetime_.expired()) return;
    if (completion_event_) static_cast<void>(scheduler_.cancel(*completion_event_));
    static_cast<void>(scheduler_.remove_reset_observer(reset_observer_));
}

std::uint32_t DreamcastMapleController::read(const std::uint32_t offset) const {
    using namespace maple_register;
    switch (offset) {
    case DmaCommandTable:
        return command_table_;
    case DmaTriggerSelect:
        return trigger_select_;
    case DmaEnable:
        return enabled_;
    case DmaStart:
        return active_;
    case SystemControl:
        return system_control_;
    case Status:
        return 0u;
    case MsbSelect:
        return msb_select_;
    case TxAddressCounter:
        return tx_address_;
    case RxAddressCounter:
        return rx_address_;
    case RxBaseAddress:
        return rx_base_;
    default:
        throw std::runtime_error("Unbekannter oder nicht lesbarer Maple-MMIO-Offset.");
    }
}

void DreamcastMapleController::write(const std::uint32_t offset, const std::uint32_t value) {
    using namespace maple_register;
    switch (offset) {
    case DmaCommandTable:
        command_table_ = value & 0x1FFFFFE0u;
        return;
    case DmaTriggerSelect:
        trigger_select_ = value & 1u;
        return;
    case DmaEnable:
        enabled_ = value & 1u;
        if (enabled_ == 0u && completion_event_) {
            static_cast<void>(scheduler_.cancel(*completion_event_));
            completion_event_.reset();
            pending_responses_.clear();
            active_ = 0u;
        }
        return;
    case DmaStart:
        if ((value & 1u) != 0u && enabled_ != 0u) start_dma();
        return;
    case SystemControl:
        system_control_ = value & 0xFFFF130Fu;
        return;
    case HardTriggerClear:
        return;
    case DmaAddressProtect:
        if ((value >> 16u) == 0x6155u) address_protect_ = value & 0x00007F7Fu;
        return;
    case MsbSelect:
        msb_select_ = value & 1u;
        return;
    default:
        throw std::runtime_error("Unbekannter oder nicht schreibbarer Maple-MMIO-Offset.");
    }
}

void DreamcastMapleController::reset() noexcept {
    if (completion_event_ && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*completion_event_));
    completion_event_.reset();
    pending_responses_.clear();
    command_table_ = 0u;
    trigger_select_ = 0u;
    enabled_ = 0u;
    active_ = 0u;
    system_control_ = 0x3A980000u;
    address_protect_ = 0x00007F00u;
    msb_select_ = 1u;
    tx_address_ = 0u;
    rx_address_ = 0u;
    rx_base_ = 0u;
}

std::uint64_t DreamcastMapleController::completed_dma_count() const noexcept {
    return completed_dma_count_;
}

std::uint64_t DreamcastMapleController::transferred_word_count() const noexcept {
    return transferred_word_count_;
}

bool DreamcastMapleController::protected_address(const std::uint32_t address,
                                                 const std::size_t size) const noexcept {
    if (size == 0u) return false;
    const auto bottom = ((address_protect_ & 0x7Fu) << 20u) | 0x08000000u;
    const auto top = (((address_protect_ >> 8u) & 0x7Fu) << 20u) | 0x080FFFE0u;
    const auto physical = address & 0x1FFFFFFFu;
    if (size - 1u > std::numeric_limits<std::uint32_t>::max() - physical) return false;
    const auto end = physical + static_cast<std::uint32_t>(size - 1u);
    return physical >= bottom && end <= top && memory_.contains(address, size);
}

std::pair<std::uint8_t, std::uint8_t>
DreamcastMapleController::decode_recipient(const std::uint8_t bus,
                                           const std::uint8_t recipient) const {
    if (bus >= maple_port_count) throw std::out_of_range("Maple-DMA-Bus liegt ausserhalb 0..3.");
    if ((recipient & 0x20u) != 0u) return {bus, std::uint8_t{0u}};
    for (std::uint8_t bit = 0u; bit < 5u; ++bit)
        if ((recipient & (std::uint8_t{1u} << bit)) != 0u)
            return {bus, static_cast<std::uint8_t>(bit + 1u)};
    throw std::invalid_argument("Maple-DMA-Empfaenger besitzt keine Geraeteadresse.");
}

void DreamcastMapleController::start_dma() {
    if (active_ != 0u || completion_event_)
        throw std::logic_error(
            "Maple-DMA wurde waehrend eines aktiven Transfers erneut gestartet.");
    if (!protected_address(command_table_, sizeof(std::uint32_t)))
        throw std::out_of_range("Maple-DMA-Kommandotabelle liegt ausserhalb des Schutzfensters.");

    active_ = 1u;
    pending_responses_.clear();
    tx_address_ = command_table_;
    std::uint64_t transfer_words = 0u;
    bool last = false;
    try {
        for (std::size_t descriptor_index = 0u; descriptor_index < maximum_dma_descriptors && !last;
             ++descriptor_index) {
            if (!protected_address(tx_address_, 2u * sizeof(std::uint32_t)))
                throw std::out_of_range(
                    "Maple-DMA-Deskriptor liegt ausserhalb des Schutzfensters.");
            const auto descriptor = memory_.read_u32(tx_address_);
            const auto destination = memory_.read_u32(tx_address_ + 4u) & 0x1FFFFFE0u;
            last = (descriptor & 0x80000000u) != 0u;
            const auto pattern = (descriptor >> 8u) & 7u;
            const auto bus = static_cast<std::uint8_t>((descriptor >> 16u) & 3u);
            const auto frame_words = static_cast<std::size_t>(descriptor & 0xFFu) + 1u;

            if (pattern == 0u) {
                if (frame_words > maximum_maple_frame_words)
                    throw std::out_of_range("Maple-DMA-Frame ist groesser als 256 Woerter.");
                const auto descriptor_bytes = (2u + frame_words) * sizeof(std::uint32_t);
                if (!protected_address(tx_address_, descriptor_bytes))
                    throw std::out_of_range("Maple-DMA-Frame verlaesst das Schutzfenster.");
                std::vector<std::uint32_t> frame(frame_words);
                for (std::size_t word = 0u; word < frame_words; ++word) {
                    auto value =
                        memory_.read_u32(tx_address_ + 8u + static_cast<std::uint32_t>(word * 4u));
                    if (msb_select_ == 0u) value = swap_word(value);
                    frame[word] = value;
                }
                const auto frame_header = frame.front();
                const auto payload_words = static_cast<std::size_t>(frame_header >> 24u);
                if (payload_words + 1u != frame_words)
                    throw std::invalid_argument(
                        "Maple-Frame-Laenge stimmt nicht mit dem Deskriptor ueberein.");
                const auto recipient = static_cast<std::uint8_t>((frame_header >> 8u) & 0xFFu);
                const auto sender = static_cast<std::uint8_t>((frame_header >> 16u) & 0xFFu);
                const auto [port, unit] = decode_recipient(bus, recipient);
                std::vector<std::uint32_t> output;
                if (!bus_->attached(port, unit)) {
                    output.push_back(0xFFFFFFFFu);
                } else {
                    MapleRequest request;
                    request.command = static_cast<MapleCommand>(frame_header & 0xFFu);
                    request.payload.assign(frame.begin() + 1, frame.end());
                    auto response = bus_->exchange_without_completion(port, unit, request);
                    if (response.payload.size() > 0xFFu)
                        throw std::out_of_range("Maple-Antwort ueberschreitet 255 Payloadwoerter.");
                    const auto response_header =
                        static_cast<std::uint32_t>(response.code) |
                        (static_cast<std::uint32_t>(sender) << 8u) |
                        (static_cast<std::uint32_t>(recipient) << 16u) |
                        (static_cast<std::uint32_t>(response.payload.size()) << 24u);
                    output.reserve(response.payload.size() + 1u);
                    output.push_back(response_header);
                    output.insert(output.end(), response.payload.begin(), response.payload.end());
                }
                if (!protected_address(destination, output.size() * sizeof(std::uint32_t)))
                    throw std::out_of_range(
                        "Maple-DMA-Antwort liegt ausserhalb des Schutzfensters.");
                if (msb_select_ == 0u)
                    for (auto& word : output)
                        word = swap_word(word);
                pending_responses_.push_back({destination, std::move(output)});
                rx_base_ = destination;
                transfer_words += frame_words + pending_responses_.back().words.size();
                tx_address_ = checked_address_add(tx_address_, descriptor_bytes);
            } else if (pattern == 2u || pattern == 3u || pattern == 4u || pattern == 7u) {
                transfer_words += 1u;
                tx_address_ = checked_address_add(tx_address_, sizeof(std::uint32_t));
            } else {
                throw std::invalid_argument("Unbekanntes Maple-DMA-Deskriptormuster.");
            }
        }
        if (!last)
            throw std::runtime_error("Maple-DMA-Kommandotabelle besitzt keinen Enddeskriptor.");
        if (transfer_words == 0u ||
            transfer_words > std::numeric_limits<std::uint64_t>::max() / timing_.cycles_per_word)
            throw std::overflow_error("Maple-DMA-Zeitbudget ist ungueltig oder laeuft ueber.");
        const auto latency = transfer_words * timing_.cycles_per_word;
        completion_event_ = scheduler_.schedule_after(
            latency, [this](const auto event_id, const auto) { complete_dma(event_id); });
        transferred_word_count_ += transfer_words;
    } catch (...) {
        active_ = 0u;
        pending_responses_.clear();
        throw;
    }
}

void DreamcastMapleController::complete_dma(const SchedulerEventId event_id) {
    if (!completion_event_ || *completion_event_ != event_id || active_ == 0u)
        throw std::logic_error("Maple-DMA-Completion besitzt keinen aktiven Transfer.");
    for (const auto& response : pending_responses_) {
        for (std::size_t word = 0u; word < response.words.size(); ++word) {
            const auto address = response.destination + static_cast<std::uint32_t>(word * 4u);
            memory_.write_u32(address, response.words[word], CodeWriteSource::Dma);
            rx_address_ = address;
        }
    }
    pending_responses_.clear();
    completion_event_.reset();
    active_ = 0u;
    ++completed_dma_count_;
    if (completion_observer_) completion_observer_();
}

void DreamcastMapleController::handle_scheduler_reset() noexcept {
    completion_event_.reset();
    pending_responses_.clear();
    active_ = 0u;
}

std::shared_ptr<DreamcastMapleController>
map_dreamcast_maple_controller(Memory& memory,
                               EventScheduler& scheduler,
                               std::shared_ptr<MapleBus> bus,
                               const MapleDmaTiming timing,
                               std::function<void()> completion_observer) {
    auto controller = std::make_shared<DreamcastMapleController>(
        memory, scheduler, std::move(bus), timing, std::move(completion_observer));
    auto device = std::make_shared<MmioMemoryDevice>(
        maple_mmio_register_size,
        [controller](const auto offset, const auto width) {
            if (width != MemoryAccessWidth::Word)
                throw std::runtime_error("Maple-Steuerregister erfordern 32-Bit-MMIO.");
            return controller->read(offset);
        },
        [controller](const auto offset, const auto value, const auto width) {
            if (width != MemoryAccessWidth::Word)
                throw std::runtime_error("Maple-Steuerregister erfordern 32-Bit-MMIO.");
            controller->write(offset, value);
        });
    for (const auto segment : dreamcast_direct_segment_bases)
        memory.map_region("dreamcast-maple-mmio-" + std::to_string(segment),
                          segment + maple_mmio_physical_base,
                          device);
    return controller;
}

} // namespace katana::runtime
