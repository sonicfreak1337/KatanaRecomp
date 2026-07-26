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

class MapleDmaFault final : public std::runtime_error {
  public:
    MapleDmaFault(const MapleDmaError error,
                  const std::uint32_t address,
                  const std::string& message)
        : std::runtime_error(message), error_(error), address_(address) {}

    [[nodiscard]] MapleDmaError error() const noexcept { return error_; }
    [[nodiscard]] std::uint32_t address() const noexcept { return address_; }

  private:
    MapleDmaError error_ = MapleDmaError::InternalLifecycle;
    std::uint32_t address_ = 0u;
};

std::uint32_t swap_word(const std::uint32_t value) noexcept {
    return ((value & 0x000000FFu) << 24u) | ((value & 0x0000FF00u) << 8u) |
           ((value & 0x00FF0000u) >> 8u) | ((value & 0xFF000000u) >> 24u);
}

std::uint32_t checked_address_add(const std::uint32_t address, const std::size_t bytes) {
    if (bytes > std::numeric_limits<std::uint32_t>::max() - address)
        throw MapleDmaFault(
            MapleDmaError::InvalidDescriptor, address, "Maple-DMA-Adresse laeuft ueber.");
    return address + static_cast<std::uint32_t>(bytes);
}

bool guest_caused_dma_error(const MapleDmaError error) noexcept {
    switch (error) {
    case MapleDmaError::InvalidConfiguration:
    case MapleDmaError::ProtectedRange:
    case MapleDmaError::InvalidDescriptor:
    case MapleDmaError::UnsupportedDescriptor:
    case MapleDmaError::ResponseRange:
    case MapleDmaError::AtomicCommitFailure:
        return true;
    case MapleDmaError::None:
    case MapleDmaError::SchedulerFailure:
    case MapleDmaError::InternalLifecycle:
        return false;
    }
    return false;
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
        return (error_ != MapleDmaError::None || hard_trigger_failed_ ? 1u : 0u) |
               (static_cast<std::uint32_t>(error_) << 8u);
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
        if (enabled_ == 0u) {
            cancel_pending();
            state_ = MapleDmaState::Disabled;
            error_ = MapleDmaError::None;
            error_address_.reset();
            clear_event_publication();
            hard_trigger_failed_ = false;
        } else if (state_ == MapleDmaState::Disabled) {
            state_ = MapleDmaState::Completed;
        }
        return;
    case DmaStart:
        if ((value & 1u) == 0u || enabled_ == 0u) return;
        if (state_ == MapleDmaState::Active || completion_event_) {
            fail(MapleDmaError::InvalidConfiguration, command_table_);
            return;
        }
        if (trigger_select_ != 0u) {
            pending_responses_.clear();
            active_ = 1u;
            state_ = MapleDmaState::Armed;
            error_ = MapleDmaError::None;
            error_address_.reset();
            clear_event_publication();
            hard_trigger_failed_ = false;
        } else {
            start_dma();
        }
        return;
    case SystemControl:
        system_control_ = value & 0xFFFF130Fu;
        return;
    case HardTriggerClear:
        if ((value & 1u) != 0u) {
            hard_trigger_failed_ = false;
            error_ = MapleDmaError::None;
            error_address_.reset();
            clear_event_publication();
            if (state_ == MapleDmaState::Failed)
                state_ = enabled_ != 0u ? MapleDmaState::Completed
                                        : MapleDmaState::Disabled;
        }
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
    cancel_pending();
    command_table_ = 0u;
    trigger_select_ = 0u;
    enabled_ = 0u;
    active_ = 0u;
    state_ = MapleDmaState::Disabled;
    error_ = MapleDmaError::None;
    error_address_.reset();
    clear_event_publication();
    system_control_ = 0x3A980000u;
    address_protect_ = 0x00007F00u;
    msb_select_ = 1u;
    tx_address_ = 0u;
    rx_address_ = 0u;
    rx_base_ = 0u;
    hard_trigger_failed_ = false;
}

std::uint64_t DreamcastMapleController::completed_dma_count() const noexcept {
    return completed_dma_count_;
}

std::uint64_t DreamcastMapleController::transferred_word_count() const noexcept {
    return transferred_word_count_;
}

void DreamcastMapleController::hardware_trigger() noexcept {
    if (trigger_select_ == 0u || enabled_ == 0u || state_ != MapleDmaState::Armed)
        return;
    try {
        start_dma();
    } catch (...) {
        hard_trigger_failed_ = true;
    }
}

bool DreamcastMapleController::hard_trigger_failed() const noexcept {
    return hard_trigger_failed_;
}

MapleDmaState DreamcastMapleController::state() const noexcept {
    return state_;
}

MapleDmaError DreamcastMapleController::error() const noexcept {
    return error_;
}

std::optional<std::uint32_t> DreamcastMapleController::error_address() const noexcept {
    return error_address_;
}

MapleDmaEventPublicationState
DreamcastMapleController::event_publication_state() const noexcept {
    return event_publication_state_;
}

MapleDmaEventPublicationError
DreamcastMapleController::event_publication_error() const noexcept {
    return event_publication_error_;
}

std::uint64_t
DreamcastMapleController::event_publication_failure_count() const noexcept {
    return event_publication_failure_count_;
}

DreamcastMapleControllerSnapshot DreamcastMapleController::snapshot() const {
    DreamcastMapleControllerSnapshot result;
    result.timing = timing_;
    result.completion_event = completion_event_;
    result.pending_responses.reserve(pending_responses_.size());
    for (const auto& response : pending_responses_)
        result.pending_responses.push_back({response.destination, response.words});
    result.command_table = command_table_;
    result.trigger_select = trigger_select_;
    result.enabled = enabled_;
    result.active = active_;
    result.state = state_;
    result.error = error_;
    result.error_address = error_address_;
    result.event_publication_state = event_publication_state_;
    result.event_publication_error = event_publication_error_;
    result.event_publication_failure_count = event_publication_failure_count_;
    result.system_control = system_control_;
    result.address_protect = address_protect_;
    result.msb_select = msb_select_;
    result.tx_address = tx_address_;
    result.rx_address = rx_address_;
    result.rx_base = rx_base_;
    result.completed_dma_count = completed_dma_count_;
    result.transferred_word_count = transferred_word_count_;
    result.failed_dma_count = failed_dma_count_;
    result.hard_trigger_failed = hard_trigger_failed_;
    return result;
}

bool DreamcastMapleController::protected_address(const std::uint32_t address,
                                                 const std::size_t size) const noexcept {
    if (size == 0u) return false;
    const auto bottom = ((address_protect_ & 0x7Fu) << 20u) | 0x08000000u;
    const auto top = (((address_protect_ >> 8u) & 0x7Fu) << 20u) | 0x080FFFFFu;
    const auto physical = address & 0x1FFFFFFFu;
    if (size - 1u > std::numeric_limits<std::uint32_t>::max() - physical) return false;
    const auto end = physical + static_cast<std::uint32_t>(size - 1u);
    return physical >= bottom && end <= top && memory_.contains(address, size);
}

std::pair<std::uint8_t, std::uint8_t>
DreamcastMapleController::decode_recipient(const std::uint8_t bus,
                                           const std::uint8_t recipient) const {
    if (bus >= maple_port_count)
        throw MapleDmaFault(MapleDmaError::InvalidDescriptor,
                            tx_address_,
                            "Maple-DMA-Bus liegt ausserhalb 0..3.");
    if ((recipient & 0x20u) != 0u) return {bus, std::uint8_t{0u}};
    for (std::uint8_t bit = 0u; bit < 5u; ++bit)
        if ((recipient & (std::uint8_t{1u} << bit)) != 0u)
            return {bus, static_cast<std::uint8_t>(bit + 1u)};
    throw MapleDmaFault(MapleDmaError::InvalidDescriptor,
                        tx_address_,
                        "Maple-DMA-Empfaenger besitzt keine Geraeteadresse.");
}

void DreamcastMapleController::start_dma() {
    try {
        if (state_ == MapleDmaState::Active || completion_event_)
            throw MapleDmaFault(MapleDmaError::InvalidConfiguration,
                                command_table_,
                                "Maple-DMA wurde waehrend eines aktiven Transfers erneut "
                                "gestartet.");
        if (enabled_ == 0u)
            throw MapleDmaFault(MapleDmaError::InvalidConfiguration,
                                command_table_,
                                "Maple-DMA wurde im deaktivierten Zustand gestartet.");

        active_ = 1u;
        state_ = MapleDmaState::Active;
        error_ = MapleDmaError::None;
        error_address_.reset();
        clear_event_publication();
        hard_trigger_failed_ = false;
        pending_responses_.clear();
        tx_address_ = command_table_;
        std::uint64_t transfer_words = 0u;
        bool last = false;
        if (!protected_address(command_table_, sizeof(std::uint32_t)) ||
            !memory_.is_readable_linear_range(command_table_, sizeof(std::uint32_t)))
            throw MapleDmaFault(
                MapleDmaError::ProtectedRange,
                command_table_,
                "Maple-DMA-Kommandotabelle liegt ausserhalb des Schutzfensters.");
        for (std::size_t descriptor_index = 0u; descriptor_index < maximum_dma_descriptors && !last;
             ++descriptor_index) {
            if (!protected_address(tx_address_, 2u * sizeof(std::uint32_t)) ||
                !memory_.is_readable_linear_range(
                    tx_address_, 2u * sizeof(std::uint32_t)))
                throw MapleDmaFault(
                    MapleDmaError::ProtectedRange,
                    tx_address_,
                    "Maple-DMA-Deskriptor liegt ausserhalb des Schutzfensters.");
            const auto descriptor = memory_.read_u32(tx_address_);
            const auto destination = memory_.read_u32(tx_address_ + 4u) & 0x1FFFFFE0u;
            last = (descriptor & 0x80000000u) != 0u;
            const auto pattern = (descriptor >> 8u) & 7u;
            const auto bus = static_cast<std::uint8_t>((descriptor >> 16u) & 3u);
            const auto frame_words = static_cast<std::size_t>(descriptor & 0xFFu) + 1u;

            if (pattern == 0u) {
                if (frame_words > maximum_maple_frame_words)
                    throw MapleDmaFault(MapleDmaError::InvalidDescriptor,
                                        tx_address_,
                                        "Maple-DMA-Frame ist groesser als 256 Woerter.");
                const auto descriptor_bytes = (2u + frame_words) * sizeof(std::uint32_t);
                if (!protected_address(tx_address_, descriptor_bytes) ||
                    !memory_.is_readable_linear_range(tx_address_, descriptor_bytes))
                    throw MapleDmaFault(MapleDmaError::ProtectedRange,
                                        tx_address_,
                                        "Maple-DMA-Frame verlaesst das Schutzfenster.");
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
                    throw MapleDmaFault(
                        MapleDmaError::InvalidDescriptor,
                        tx_address_,
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
                    auto response = bus_->exchange_without_completion_at(
                        port, unit, request, scheduler_.current_cycle());
                    if (response.payload.size() > 0xFFu)
                        throw MapleDmaFault(
                            MapleDmaError::ResponseRange,
                            destination,
                            "Maple-Antwort ueberschreitet 255 Payloadwoerter.");
                    const auto response_header =
                        static_cast<std::uint32_t>(response.code) |
                        (static_cast<std::uint32_t>(sender) << 8u) |
                        (static_cast<std::uint32_t>(recipient) << 16u) |
                        (static_cast<std::uint32_t>(response.payload.size()) << 24u);
                    output.reserve(response.payload.size() + 1u);
                    output.push_back(response_header);
                    output.insert(output.end(), response.payload.begin(), response.payload.end());
                }
                const auto response_bytes = output.size() * sizeof(std::uint32_t);
                if (!protected_address(destination, response_bytes) ||
                    !memory_.is_writable_linear_range(destination, response_bytes))
                    throw MapleDmaFault(
                        MapleDmaError::ResponseRange,
                        destination,
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
                throw MapleDmaFault(MapleDmaError::UnsupportedDescriptor,
                                    tx_address_,
                                    "Unbekanntes Maple-DMA-Deskriptormuster.");
            }
        }
        if (!last)
            throw MapleDmaFault(
                MapleDmaError::InvalidDescriptor,
                tx_address_,
                "Maple-DMA-Kommandotabelle besitzt keinen Enddeskriptor.");
        if (transfer_words == 0u ||
            transfer_words > std::numeric_limits<std::uint64_t>::max() / timing_.cycles_per_word)
            throw MapleDmaFault(MapleDmaError::InvalidConfiguration,
                                command_table_,
                                "Maple-DMA-Zeitbudget ist ungueltig oder laeuft ueber.");
        const auto latency = transfer_words * timing_.cycles_per_word;
        try {
            completion_event_ = scheduler_.schedule_after(
                latency,
                [this](const auto event_id, const auto) { complete_dma(event_id); },
                SchedulerEventKind::MapleDma);
        } catch (...) {
            throw MapleDmaFault(MapleDmaError::SchedulerFailure,
                                command_table_,
                                "Maple-DMA-Completion konnte nicht geplant werden.");
        }
        transferred_word_count_ += transfer_words;
    } catch (const MapleDmaFault& fault) {
        fail(fault.error(), fault.address());
        if (!guest_caused_dma_error(fault.error())) throw;
    } catch (...) {
        fail(MapleDmaError::InternalLifecycle, tx_address_);
        throw;
    }
}

void DreamcastMapleController::complete_dma(const SchedulerEventId event_id) noexcept {
    if (!completion_event_ || *completion_event_ != event_id) return;
    if (state_ != MapleDmaState::Active || active_ == 0u) {
        fail(MapleDmaError::InternalLifecycle, tx_address_);
        return;
    }

    struct StagedResponse {
        std::uint32_t destination = 0u;
        std::vector<std::uint8_t> bytes;
    };

    try {
        std::vector<StagedResponse> staged;
        staged.reserve(pending_responses_.size());
        for (const auto& response : pending_responses_) {
            if (response.words.empty() ||
                response.words.size() >
                    std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t)) {
                fail(MapleDmaError::ResponseRange, response.destination);
                return;
            }
            const auto byte_count = response.words.size() * sizeof(std::uint32_t);
            if (!protected_address(response.destination, byte_count) ||
                !memory_.is_writable_linear_range(response.destination, byte_count)) {
                fail(MapleDmaError::ResponseRange, response.destination);
                return;
            }
            StagedResponse item;
            item.destination = response.destination;
            item.bytes.resize(byte_count);
            for (std::size_t word = 0u; word < response.words.size(); ++word) {
                const auto value = response.words[word];
                const auto byte_offset = word * sizeof(value);
                for (std::size_t byte = 0u; byte < sizeof(value); ++byte)
                    item.bytes[byte_offset + byte] =
                        static_cast<std::uint8_t>(value >> (byte * 8u));
            }
            staged.push_back(std::move(item));
        }

        std::vector<LinearMemoryTransactionWrite> writes;
        writes.reserve(staged.size());
        for (const auto& response : staged)
            writes.push_back({response.destination, response.bytes});
        if (!memory_.commit_linear_transaction_batch(writes, CodeWriteSource::Dma)) {
            fail(MapleDmaError::AtomicCommitFailure,
                 staged.empty() ? rx_base_ : staged.front().destination);
            return;
        }
        if (!staged.empty()) {
            const auto& response = staged.back();
            rx_address_ =
                response.destination +
                static_cast<std::uint32_t>(response.bytes.size() -
                                           sizeof(std::uint32_t));
        }
        pending_responses_.clear();
        completion_event_.reset();
        active_ = 0u;
        state_ = MapleDmaState::Completed;
        error_ = MapleDmaError::None;
        error_address_.reset();
        ++completed_dma_count_;
        publish_dma_event();
    } catch (...) {
        fail(MapleDmaError::InternalLifecycle, rx_base_);
    }
}

void DreamcastMapleController::cancel_pending() noexcept {
    if (completion_event_ && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*completion_event_));
    completion_event_.reset();
    pending_responses_.clear();
    active_ = 0u;
}

void DreamcastMapleController::fail(const MapleDmaError error,
                                    const std::optional<std::uint32_t> address) noexcept {
    cancel_pending();
    state_ = MapleDmaState::Failed;
    error_ = error;
    error_address_ = address;
    ++failed_dma_count_;
    clear_event_publication();
    if (!guest_caused_dma_error(error)) return;
    publish_dma_event();
}

void DreamcastMapleController::publish_dma_event() noexcept {
    clear_event_publication();
    if (!completion_observer_) return;
    event_publication_state_ = MapleDmaEventPublicationState::Publishing;
    try {
        completion_observer_();
        event_publication_state_ = MapleDmaEventPublicationState::Published;
    } catch (...) {
        event_publication_state_ = MapleDmaEventPublicationState::Failed;
        event_publication_error_ =
            MapleDmaEventPublicationError::ObserverException;
        ++event_publication_failure_count_;
    }
}

void DreamcastMapleController::clear_event_publication() noexcept {
    event_publication_state_ =
        MapleDmaEventPublicationState::NotRequested;
    event_publication_error_ = MapleDmaEventPublicationError::None;
}

void DreamcastMapleController::handle_scheduler_reset() noexcept {
    completion_event_.reset();
    pending_responses_.clear();
    active_ = 0u;
    state_ = enabled_ != 0u ? MapleDmaState::Completed : MapleDmaState::Disabled;
    error_ = MapleDmaError::None;
    error_address_.reset();
    clear_event_publication();
    hard_trigger_failed_ = false;
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
