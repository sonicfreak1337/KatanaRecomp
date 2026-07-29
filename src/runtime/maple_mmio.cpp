#include "katana/runtime/maple_mmio.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace katana::runtime {
namespace {
constexpr std::size_t maximum_dma_descriptors = 1'024u;
constexpr std::size_t maximum_maple_frame_words = 256u;
constexpr std::size_t maximum_maple_history_records = 1'000'000u;
constexpr std::array<std::uint8_t, 8u> maple_state_magic{
    'K', 'A', 'T', 'M', 'A', 'P', '1', '\n'};

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

bool valid_dma_state(const MapleDmaState state) noexcept {
    switch (state) {
    case MapleDmaState::Disabled:
    case MapleDmaState::Armed:
    case MapleDmaState::Active:
    case MapleDmaState::Completed:
    case MapleDmaState::Failed:
        return true;
    }
    return false;
}

bool valid_dma_error(const MapleDmaError error) noexcept {
    switch (error) {
    case MapleDmaError::None:
    case MapleDmaError::InvalidConfiguration:
    case MapleDmaError::ProtectedRange:
    case MapleDmaError::InvalidDescriptor:
    case MapleDmaError::UnsupportedDescriptor:
    case MapleDmaError::ResponseRange:
    case MapleDmaError::SchedulerFailure:
    case MapleDmaError::AtomicCommitFailure:
    case MapleDmaError::InternalLifecycle:
        return true;
    }
    return false;
}

bool valid_publication_state(
    const MapleDmaEventPublicationState state) noexcept {
    switch (state) {
    case MapleDmaEventPublicationState::NotRequested:
    case MapleDmaEventPublicationState::Publishing:
    case MapleDmaEventPublicationState::Published:
    case MapleDmaEventPublicationState::Failed:
        return true;
    }
    return false;
}

bool valid_publication_error(
    const MapleDmaEventPublicationError error) noexcept {
    switch (error) {
    case MapleDmaEventPublicationError::None:
    case MapleDmaEventPublicationError::ObserverException:
        return true;
    }
    return false;
}

void validate_controller_snapshot_shape(
    const DreamcastMapleControllerSnapshot& state,
    const bool require_live_completion_event) {
    if (state.timing.cycles_per_word == 0u ||
        !valid_dma_state(state.state) || !valid_dma_error(state.error) ||
        !valid_publication_state(state.event_publication_state) ||
        !valid_publication_error(state.event_publication_error))
        throw std::invalid_argument(
            "Maple-DMA-Handoff besitzt ungueltige Enum- oder Timingwerte.");
    if ((state.command_table & ~0x1FFFFFE0u) != 0u ||
        state.trigger_select > 1u || state.enabled > 1u ||
        state.active > 1u ||
        (state.system_control & ~0xFFFF130Fu) != 0u ||
        (state.address_protect & ~0x00007F7Fu) != 0u ||
        state.msb_select > 1u)
        throw std::invalid_argument(
            "Maple-DMA-Handoff besitzt ungueltige Registerbits.");
    if (state.completion_event &&
        state.completion_event_rehydration_pending)
        throw std::invalid_argument(
            "Maple-DMA-Handoff darf kein gebundenes und ausstehendes "
            "Rehydrationsevent zugleich besitzen.");
    if (state.event_publication_state ==
        MapleDmaEventPublicationState::Publishing)
        throw std::invalid_argument(
            "Maple-DMA-Handoff darf nicht waehrend eines Hostcallbacks "
            "erfasst werden.");
    if ((state.event_publication_state ==
             MapleDmaEventPublicationState::Failed) !=
        (state.event_publication_error ==
             MapleDmaEventPublicationError::ObserverException))
        throw std::invalid_argument(
            "Maple-DMA-Handoff besitzt inkonsistente "
            "Eventpublikationsdaten.");

    const auto active_transfer = state.state == MapleDmaState::Active;
    if (active_transfer) {
        if (state.enabled == 0u || state.active == 0u ||
            state.error != MapleDmaError::None ||
            (require_live_completion_event &&
             (!state.completion_event ||
              state.completion_event_rehydration_pending)))
            throw std::invalid_argument(
                "Aktiver Maple-DMA-Handoff besitzt keinen gueltigen "
                "Completionvertrag.");
    } else if (state.completion_event ||
               state.completion_event_rehydration_pending ||
               !state.pending_responses.empty()) {
        throw std::invalid_argument(
            "Inaktiver Maple-DMA-Handoff besitzt aktive Transferdaten.");
    }

    switch (state.state) {
    case MapleDmaState::Disabled:
        if (state.enabled != 0u || state.active != 0u ||
            state.error != MapleDmaError::None)
            throw std::invalid_argument(
                "Deaktivierter Maple-DMA-Handoff ist inkonsistent.");
        break;
    case MapleDmaState::Armed:
        if (state.enabled == 0u || state.active == 0u ||
            state.trigger_select == 0u ||
            state.error != MapleDmaError::None)
            throw std::invalid_argument(
                "Bewaffneter Maple-DMA-Handoff ist inkonsistent.");
        break;
    case MapleDmaState::Active:
        break;
    case MapleDmaState::Completed:
        if (state.enabled == 0u || state.active != 0u ||
            state.error != MapleDmaError::None)
            throw std::invalid_argument(
                "Abgeschlossener Maple-DMA-Handoff ist inkonsistent.");
        break;
    case MapleDmaState::Failed:
        if (state.active != 0u || state.error == MapleDmaError::None)
            throw std::invalid_argument(
                "Fehlgeschlagener Maple-DMA-Handoff ist inkonsistent.");
        break;
    }

    if (state.pending_responses.size() > maximum_dma_descriptors)
        throw std::invalid_argument(
            "Maple-DMA-Handoff besitzt zu viele ausstehende Antworten.");
    std::size_t total_words = 0u;
    for (const auto& response : state.pending_responses) {
        if (response.words.empty() ||
            response.words.size() > maximum_maple_frame_words ||
            response.destination > 0x1FFFFFE0u ||
            (response.destination & 3u) != 0u ||
            response.words.size() >
                maximum_dma_descriptors * maximum_maple_frame_words -
                    total_words)
            throw std::invalid_argument(
                "Maple-DMA-Handoff besitzt eine ungueltige ausstehende "
                "Antwort.");
        total_words += response.words.size();
    }
}

void validate_bus_snapshot_shape(const MapleBusStateSnapshot& state) {
    if (state.next_sequence == 0u ||
        state.history.size() > maximum_maple_history_records)
        throw std::invalid_argument(
            "Maple-Bus-Handoff besitzt ungueltige Sequenzdaten.");

    std::array<bool, maple_port_count * maple_units_per_port>
        described{};
    std::size_t previous_slot = 0u;
    bool have_previous = false;
    for (const auto& peripheral : state.peripherals) {
        if (peripheral.port >= maple_port_count ||
            peripheral.unit >= maple_units_per_port)
            throw std::invalid_argument(
                "Maple-Bus-Handoff besitzt eine ungueltige Adresse.");
        const auto slot =
            static_cast<std::size_t>(peripheral.port) *
                maple_units_per_port +
            peripheral.unit;
        if ((have_previous && previous_slot >= slot) ||
            !state.attached[slot] || described[slot])
            throw std::invalid_argument(
                "Maple-Bus-Handoff besitzt ungeordnete oder doppelte "
                "Peripherie.");
        previous_slot = slot;
        have_previous = true;
        described[slot] = true;

        if (const auto* vmu =
                std::get_if<MapleVmuStateSnapshot>(&peripheral.state)) {
            if (vmu->source_image.size() != vmu_storage_size ||
                vmu->working_image.size() != vmu_storage_size ||
                (!vmu->persistent_working_copy &&
                 vmu->working_copy_dirty) ||
                (vmu->pending_write &&
                 (vmu->write_protected ||
                  vmu->pending_write->block >= vmu_block_count ||
                  vmu->pending_write->partition != 0u ||
                  vmu->pending_write->next_phase == 0u ||
                  vmu->pending_write->next_phase > 4u)))
                throw std::invalid_argument(
                    "Maple-Bus-Handoff besitzt einen ungueltigen VMU-Zustand.");
        }
    }
    for (std::size_t slot = 0u; slot < described.size(); ++slot) {
        if (described[slot] != state.attached[slot])
            throw std::invalid_argument(
                "Maple-Bus-Handoff fehlt Zustand fuer angeschlossene "
                "Peripherie.");
    }

    std::uint64_t previous_sequence = 0u;
    for (const auto& record : state.history) {
        if (record.sequence == 0u ||
            record.sequence <= previous_sequence ||
            record.sequence >= state.next_sequence ||
            record.port >= maple_port_count ||
            record.unit >= maple_units_per_port)
            throw std::invalid_argument(
                "Maple-Bus-Handoff besitzt eine ungueltige Historie.");
        previous_sequence = record.sequence;
    }
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
        if (state_ == MapleDmaState::Active || completion_event_ ||
            completion_event_rehydration_pending_) {
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
    completion_event_rehydration_pending_ = false;
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
    result.completion_event_rehydration_pending =
        completion_event_rehydration_pending_;
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

void DreamcastMapleController::validate_state_restore(
    const DreamcastMapleControllerSnapshot& state) const {
    validate_controller_snapshot_shape(state, false);
    if (state.timing.cycles_per_word != timing_.cycles_per_word)
        throw std::invalid_argument(
            "Maple-DMA-Handoff passt nicht zum Runtime-Timingvertrag.");

    const auto bottom =
        ((state.address_protect & 0x7Fu) << 20u) | 0x08000000u;
    const auto top =
        (((state.address_protect >> 8u) & 0x7Fu) << 20u) |
        0x080FFFFFu;
    for (const auto& response : state.pending_responses) {
        const auto byte_count =
            response.words.size() * sizeof(std::uint32_t);
        const auto physical =
            response.destination & 0x1FFFFFFFu;
        if (byte_count == 0u ||
            byte_count - 1u >
                std::numeric_limits<std::uint32_t>::max() - physical)
            throw std::invalid_argument(
                "Maple-DMA-Handoff-Antwortbereich laeuft ueber.");
        const auto end =
            physical + static_cast<std::uint32_t>(byte_count - 1u);
        if (physical < bottom || end > top ||
            !memory_.contains(response.destination, byte_count) ||
            !memory_.is_writable_linear_range(
                response.destination, byte_count))
            throw std::invalid_argument(
                "Maple-DMA-Handoff-Antwort passt nicht zum "
                "Runtime-Speichervertrag.");
    }
}

PreparedDreamcastMapleControllerRestore
DreamcastMapleController::prepare_state_restore(
    const DreamcastMapleControllerSnapshot& state) const {
    validate_state_restore(state);
    PreparedDreamcastMapleControllerRestore prepared;
    prepared.owner_ = this;
    prepared.state_ = state;
    // Captured IDs are process-local and are never published by commit.
    prepared.state_.completion_event.reset();
    return prepared;
}

void DreamcastMapleController::commit_prepared_state_restore(
    PreparedDreamcastMapleControllerRestore prepared) noexcept {
    assert(prepared.owner_ == this);
    auto& state = prepared.state_;
    if (completion_event_ && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*completion_event_));
    completion_event_.reset();
    pending_responses_.swap(state.pending_responses);
    command_table_ = state.command_table;
    trigger_select_ = state.trigger_select;
    enabled_ = state.enabled;
    active_ = state.active;
    state_ = state.state;
    error_ = state.error;
    error_address_ = state.error_address;
    event_publication_state_ = state.event_publication_state;
    event_publication_error_ = state.event_publication_error;
    event_publication_failure_count_ =
        state.event_publication_failure_count;
    system_control_ = state.system_control;
    address_protect_ = state.address_protect;
    msb_select_ = state.msb_select;
    tx_address_ = state.tx_address;
    rx_address_ = state.rx_address;
    rx_base_ = state.rx_base;
    completed_dma_count_ = state.completed_dma_count;
    transferred_word_count_ = state.transferred_word_count;
    failed_dma_count_ = state.failed_dma_count;
    hard_trigger_failed_ = state.hard_trigger_failed;
    completion_event_rehydration_pending_ =
        state_ == MapleDmaState::Active;
}

void DreamcastMapleController::restore_state_passive(
    const DreamcastMapleControllerSnapshot& state) {
    auto prepared = prepare_state_restore(state);
    commit_prepared_state_restore(std::move(prepared));
}

SchedulerEventId DreamcastMapleController::rehydrate_scheduled_event(
    const std::uint64_t guest_cycle,
    const std::uint32_t channel,
    const std::uint64_t token) {
    if (channel != dreamcast_maple_dma_event_channel ||
        token != dreamcast_maple_dma_event_token_v1)
        throw std::invalid_argument(
            "Maple-DMA-Handoff besitzt einen unbekannten Eventkanal oder "
            "Token.");
    if (!completion_event_rehydration_pending_ ||
        completion_event_ || state_ != MapleDmaState::Active ||
        active_ == 0u || enabled_ == 0u)
        throw std::logic_error(
            "Maple-DMA-Handoff erwartet kein Completionevent.");
    if (guest_cycle < scheduler_.current_cycle())
        throw std::invalid_argument(
            "Maple-DMA-Completion darf nicht in der Vergangenheit liegen.");

    const auto event_id = scheduler_.schedule_at(
        guest_cycle,
        make_rehydrated_scheduled_event_callback(channel, token),
        SchedulerEventKind::MapleDma);
    commit_rehydrated_scheduled_event(event_id, channel, token);
    return event_id;
}

SchedulerCallback
DreamcastMapleController::make_rehydrated_scheduled_event_callback(
    const std::uint32_t channel,
    const std::uint64_t token) {
    if (channel != dreamcast_maple_dma_event_channel ||
        token != dreamcast_maple_dma_event_token_v1)
        throw std::invalid_argument(
            "Maple-DMA-Handoff besitzt einen unbekannten Eventkanal oder "
            "Token.");
    return [this](const auto restored_event_id, const auto) {
        complete_dma(restored_event_id);
    };
}

void DreamcastMapleController::commit_rehydrated_scheduled_event(
    const SchedulerEventId event_id,
    const std::uint32_t channel,
    const std::uint64_t token) noexcept {
    if (channel != dreamcast_maple_dma_event_channel ||
        token != dreamcast_maple_dma_event_token_v1 ||
        !completion_event_rehydration_pending_ || completion_event_ ||
        state_ != MapleDmaState::Active || active_ == 0u ||
        enabled_ == 0u)
        std::terminate();
    completion_event_ = event_id;
    completion_event_rehydration_pending_ = false;
}

bool DreamcastMapleController::event_rehydration_pending() const noexcept {
    return completion_event_rehydration_pending_;
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
        if (state_ == MapleDmaState::Active || completion_event_ ||
            completion_event_rehydration_pending_)
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
                        (static_cast<std::uint32_t>(
                             bus_->response_sender_address(port, unit))
                         << 16u) |
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
    if (completion_event_rehydration_pending_ || !completion_event_ ||
        *completion_event_ != event_id)
        return;
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
    completion_event_rehydration_pending_ = false;
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
    completion_event_rehydration_pending_ = false;
    pending_responses_.clear();
    active_ = 0u;
    state_ = enabled_ != 0u ? MapleDmaState::Completed : MapleDmaState::Disabled;
    error_ = MapleDmaError::None;
    error_address_.reset();
    clear_event_publication();
    hard_trigger_failed_ = false;
}

DreamcastMapleStateSnapshot
snapshot_dreamcast_maple_state(
    const MapleBus& bus,
    const DreamcastMapleController& controller) {
    DreamcastMapleStateSnapshot state;
    state.bus = bus.state_snapshot();
    state.controller = controller.snapshot();
    validate_bus_snapshot_shape(state.bus);
    validate_controller_snapshot_shape(state.controller, true);
    return state;
}

void validate_dreamcast_maple_state_restore(
    const MapleBus& bus,
    const DreamcastMapleController& controller,
    const DreamcastMapleStateSnapshot& state) {
    validate_dreamcast_maple_state_restore(
        bus,
        controller,
        state,
        PersistenceHandoffPolicy::DiagnosticLossless);
}

void validate_dreamcast_maple_state_restore(
    const MapleBus& bus,
    const DreamcastMapleController& controller,
    const DreamcastMapleStateSnapshot& state,
    const PersistenceHandoffPolicy policy) {
    static_cast<void>(prepare_dreamcast_maple_state_restore(
        bus, controller, state, policy));
}

PreparedDreamcastMapleStateRestore
prepare_dreamcast_maple_state_restore(
    const MapleBus& bus,
    const DreamcastMapleController& controller,
    const DreamcastMapleStateSnapshot& state,
    const PersistenceHandoffPolicy policy) {
    validate_bus_snapshot_shape(state.bus);
    validate_controller_snapshot_shape(state.controller, false);
    if (policy == PersistenceHandoffPolicy::ProductPreserveTarget &&
        !state.controller.pending_responses.empty()) {
        const auto has_vmu = std::any_of(
            state.bus.peripherals.begin(),
            state.bus.peripherals.end(),
            [](const auto& peripheral) {
                return std::holds_alternative<MapleVmuStateSnapshot>(
                    peripheral.state);
            });
        if (has_vmu)
            throw std::invalid_argument(
                "Produkt-Handoff lehnt ausstehende Maple-Antworten bei "
                "angeschlossener VMU ab: die Antwortprovenienz bindet "
                "Capture-Savebytes nicht sicher aus.");
    }
    auto prepared_bus = bus.prepare_state_restore(state.bus, policy);
    auto prepared_controller =
        controller.prepare_state_restore(state.controller);
    return {
        std::move(prepared_bus),
        std::move(prepared_controller),
    };
}

void commit_dreamcast_maple_state_restore(
    MapleBus& bus,
    DreamcastMapleController& controller,
    PreparedDreamcastMapleStateRestore prepared) noexcept {
    bus.commit_prepared_state_restore(std::move(prepared.bus_));
    controller.commit_prepared_state_restore(
        std::move(prepared.controller_));
}

void restore_dreamcast_maple_state_passive(
    MapleBus& bus,
    DreamcastMapleController& controller,
    const DreamcastMapleStateSnapshot& state) {
    auto prepared = prepare_dreamcast_maple_state_restore(
        bus,
        controller,
        state,
        PersistenceHandoffPolicy::DiagnosticLossless);
    commit_dreamcast_maple_state_restore(
        bus, controller, std::move(prepared));
}

namespace {

class MapleStateWriter final {
  public:
    void u8(const std::uint8_t value) { bytes_.push_back(value); }

    void boolean(const bool value) { u8(value ? 1u : 0u); }

    void u32(const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void u64(const std::uint64_t value) {
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }

    void raw(const std::span<const std::uint8_t> bytes) {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

class MapleStateReader final {
  public:
    explicit MapleStateReader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {}

    [[nodiscard]] std::uint8_t u8() {
        require(1u);
        return bytes_[offset_++];
    }

    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1u)
            throw std::invalid_argument(
                "Maple-Handoff-Payload besitzt ein ungueltiges Boolean.");
        return value != 0u;
    }

    [[nodiscard]] std::uint32_t u32() {
        require(4u);
        std::uint32_t value = 0u;
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            value |= static_cast<std::uint32_t>(
                         bytes_[offset_ + byte])
                     << (byte * 8u);
        offset_ += 4u;
        return value;
    }

    [[nodiscard]] std::uint64_t u64() {
        require(8u);
        std::uint64_t value = 0u;
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            value |= static_cast<std::uint64_t>(
                         bytes_[offset_ + byte])
                     << (byte * 8u);
        offset_ += 8u;
        return value;
    }

    [[nodiscard]] std::vector<std::uint8_t> raw(
        const std::size_t size) {
        require(size);
        std::vector<std::uint8_t> result(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            bytes_.begin() +
                static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += size;
        return result;
    }

    void expect_end() const {
        if (offset_ != bytes_.size())
            throw std::invalid_argument(
                "Maple-Handoff-Payload besitzt nachlaufende Bytes.");
    }

  private:
    void require(const std::size_t size) const {
        if (offset_ > bytes_.size() ||
            size > bytes_.size() - offset_)
            throw std::invalid_argument(
                "Maple-Handoff-Payload ist abgeschnitten.");
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0u;
};

template <typename Enum>
void write_enum(MapleStateWriter& writer, const Enum value) {
    static_assert(std::is_enum_v<Enum>);
    writer.u32(static_cast<std::uint32_t>(value));
}

template <typename Enum>
Enum read_enum(MapleStateReader& reader) {
    static_assert(std::is_enum_v<Enum>);
    return static_cast<Enum>(reader.u32());
}

std::uint32_t checked_u32_size(const std::size_t size,
                               const char* const message) {
    if (size > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument(message);
    return static_cast<std::uint32_t>(size);
}

void encode_bus_state(MapleStateWriter& writer,
                      const MapleBusStateSnapshot& state) {
    std::uint32_t attached_mask = 0u;
    for (std::size_t slot = 0u; slot < state.attached.size(); ++slot) {
        if (state.attached[slot])
            attached_mask |= std::uint32_t{1u} << slot;
    }
    writer.u32(attached_mask);
    writer.u64(state.next_sequence);
    writer.u32(checked_u32_size(
        state.peripherals.size(),
        "Maple-Handoff besitzt zu viele Peripherieeintraege."));
    for (const auto& peripheral : state.peripherals) {
        writer.u8(peripheral.port);
        writer.u8(peripheral.unit);
        if (const auto* controller =
                std::get_if<MapleControllerDeviceStateSnapshot>(
                    &peripheral.state)) {
            writer.u8(1u);
            writer.u64(controller->next_frame);
        } else {
            const auto& vmu =
                std::get<MapleVmuStateSnapshot>(peripheral.state);
            writer.u8(2u);
            std::uint8_t flags = 0u;
            if (vmu.write_protected) flags |= 1u;
            if (vmu.working_copy_dirty) flags |= 2u;
            if (vmu.persistent_working_copy) flags |= 4u;
            if (vmu.pending_write) flags |= 8u;
            writer.u8(flags);
            writer.u32(checked_u32_size(
                vmu.source_image.size(),
                "Maple-Handoff-VMU-Quellabbild ist zu gross."));
            writer.raw(vmu.source_image);
            writer.u32(checked_u32_size(
                vmu.working_image.size(),
                "Maple-Handoff-VMU-Arbeitsabbild ist zu gross."));
            writer.raw(vmu.working_image);
            if (vmu.pending_write) {
                writer.u32(vmu.pending_write->block);
                writer.u8(vmu.pending_write->partition);
                writer.u8(vmu.pending_write->next_phase);
                writer.raw(vmu.pending_write->bytes);
            }
        }
    }

    writer.u32(checked_u32_size(
        state.history.size(),
        "Maple-Handoff besitzt zu viele Historieneintraege."));
    for (const auto& record : state.history) {
        writer.u64(record.sequence);
        writer.u8(record.port);
        writer.u8(record.unit);
        writer.u8(static_cast<std::uint8_t>(record.command));
        writer.u8(static_cast<std::uint8_t>(record.response));
    }
}

MapleBusStateSnapshot decode_bus_state(MapleStateReader& reader) {
    MapleBusStateSnapshot state;
    const auto attached_mask = reader.u32();
    if ((attached_mask >> state.attached.size()) != 0u)
        throw std::invalid_argument(
            "Maple-Handoff besitzt ungueltige Topologiebits.");
    for (std::size_t slot = 0u; slot < state.attached.size(); ++slot)
        state.attached[slot] =
            (attached_mask & (std::uint32_t{1u} << slot)) != 0u;
    state.next_sequence = reader.u64();

    const auto peripheral_count = reader.u32();
    if (peripheral_count > state.attached.size())
        throw std::invalid_argument(
            "Maple-Handoff besitzt zu viele Peripherieeintraege.");
    state.peripherals.reserve(peripheral_count);
    for (std::uint32_t index = 0u; index < peripheral_count; ++index) {
        MapleAttachedPeripheralStateSnapshot peripheral;
        peripheral.port = reader.u8();
        peripheral.unit = reader.u8();
        const auto kind = reader.u8();
        if (kind == 1u) {
            peripheral.state =
                MapleControllerDeviceStateSnapshot{reader.u64()};
        } else if (kind == 2u) {
            MapleVmuStateSnapshot vmu;
            const auto flags = reader.u8();
            if ((flags & ~std::uint8_t{15u}) != 0u)
                throw std::invalid_argument(
                    "Maple-Handoff-VMU besitzt ungueltige Flags.");
            vmu.write_protected = (flags & 1u) != 0u;
            vmu.working_copy_dirty = (flags & 2u) != 0u;
            vmu.persistent_working_copy = (flags & 4u) != 0u;
            const auto source_size = reader.u32();
            if (source_size != vmu_storage_size)
                throw std::invalid_argument(
                    "Maple-Handoff-VMU-Quellabbild besitzt nicht 128 KiB.");
            vmu.source_image = reader.raw(source_size);
            const auto working_size = reader.u32();
            if (working_size != vmu_storage_size)
                throw std::invalid_argument(
                    "Maple-Handoff-VMU-Arbeitsabbild besitzt nicht 128 KiB.");
            vmu.working_image = reader.raw(working_size);
            if ((flags & 8u) != 0u) {
                MapleVmuPendingWrite pending;
                const auto block = reader.u32();
                if (block >= vmu_block_count)
                    throw std::invalid_argument(
                        "Maple-Handoff-VMU-Schreibphase besitzt einen "
                        "ungueltigen Block.");
                pending.block = static_cast<std::uint16_t>(block);
                pending.partition = reader.u8();
                pending.next_phase = reader.u8();
                const auto bytes = reader.raw(pending.bytes.size());
                std::copy(bytes.begin(), bytes.end(), pending.bytes.begin());
                vmu.pending_write = std::move(pending);
            }
            peripheral.state = std::move(vmu);
        } else {
            throw std::invalid_argument(
                "Maple-Handoff besitzt einen unbekannten Peripherietyp.");
        }
        state.peripherals.push_back(std::move(peripheral));
    }

    const auto history_count = reader.u32();
    if (history_count > maximum_maple_history_records)
        throw std::invalid_argument(
            "Maple-Handoff besitzt zu viele Historieneintraege.");
    state.history.reserve(history_count);
    for (std::uint32_t index = 0u; index < history_count; ++index) {
        state.history.push_back(
            {reader.u64(),
             reader.u8(),
             reader.u8(),
             static_cast<MapleCommand>(reader.u8()),
             static_cast<MapleResponseCode>(reader.u8())});
    }
    return state;
}

void encode_controller_state(
    MapleStateWriter& writer,
    const DreamcastMapleControllerSnapshot& state) {
    writer.u64(state.timing.cycles_per_word);
    writer.boolean(state.completion_event.has_value());
    writer.u32(checked_u32_size(
        state.pending_responses.size(),
        "Maple-Handoff besitzt zu viele DMA-Antworten."));
    for (const auto& response : state.pending_responses) {
        writer.u32(response.destination);
        writer.u32(checked_u32_size(
            response.words.size(),
            "Maple-Handoff-DMA-Antwort ist zu gross."));
        for (const auto word : response.words) writer.u32(word);
    }
    writer.u32(state.command_table);
    writer.u32(state.trigger_select);
    writer.u32(state.enabled);
    writer.u32(state.active);
    write_enum(writer, state.state);
    write_enum(writer, state.error);
    writer.boolean(state.error_address.has_value());
    if (state.error_address) writer.u32(*state.error_address);
    write_enum(writer, state.event_publication_state);
    write_enum(writer, state.event_publication_error);
    writer.u64(state.event_publication_failure_count);
    writer.u32(state.system_control);
    writer.u32(state.address_protect);
    writer.u32(state.msb_select);
    writer.u32(state.tx_address);
    writer.u32(state.rx_address);
    writer.u32(state.rx_base);
    writer.u64(state.completed_dma_count);
    writer.u64(state.transferred_word_count);
    writer.u64(state.failed_dma_count);
    writer.boolean(state.hard_trigger_failed);
}

DreamcastMapleControllerSnapshot decode_controller_state(
    MapleStateReader& reader) {
    DreamcastMapleControllerSnapshot state;
    state.timing.cycles_per_word = reader.u64();
    const auto completion_required = reader.boolean();
    const auto response_count = reader.u32();
    if (response_count > maximum_dma_descriptors)
        throw std::invalid_argument(
            "Maple-Handoff besitzt zu viele DMA-Antworten.");
    state.pending_responses.reserve(response_count);
    for (std::uint32_t index = 0u; index < response_count; ++index) {
        DreamcastMaplePendingResponseSnapshot response;
        response.destination = reader.u32();
        const auto word_count = reader.u32();
        if (word_count == 0u ||
            word_count > maximum_maple_frame_words)
            throw std::invalid_argument(
                "Maple-Handoff-DMA-Antwort besitzt eine ungueltige "
                "Wortzahl.");
        response.words.reserve(word_count);
        for (std::uint32_t word = 0u; word < word_count; ++word)
            response.words.push_back(reader.u32());
        state.pending_responses.push_back(std::move(response));
    }
    state.command_table = reader.u32();
    state.trigger_select = reader.u32();
    state.enabled = reader.u32();
    state.active = reader.u32();
    state.state = read_enum<MapleDmaState>(reader);
    state.error = read_enum<MapleDmaError>(reader);
    if (reader.boolean()) state.error_address = reader.u32();
    state.event_publication_state =
        read_enum<MapleDmaEventPublicationState>(reader);
    state.event_publication_error =
        read_enum<MapleDmaEventPublicationError>(reader);
    state.event_publication_failure_count = reader.u64();
    state.system_control = reader.u32();
    state.address_protect = reader.u32();
    state.msb_select = reader.u32();
    state.tx_address = reader.u32();
    state.rx_address = reader.u32();
    state.rx_base = reader.u32();
    state.completed_dma_count = reader.u64();
    state.transferred_word_count = reader.u64();
    state.failed_dma_count = reader.u64();
    state.hard_trigger_failed = reader.boolean();
    if (completion_required !=
        (state.state == MapleDmaState::Active))
        throw std::invalid_argument(
            "Maple-Handoff-DMA-Eventpraesenz passt nicht zum Transferzustand.");
    // A scheduler ID is never portable. restore_state_passive() marks an
    // active decoded transfer as awaiting its typed completion event.
    state.completion_event.reset();
    state.completion_event_rehydration_pending = false;
    return state;
}

} // namespace

std::vector<std::uint8_t> encode_dreamcast_maple_state(
    const DreamcastMapleStateSnapshot& state) {
    validate_bus_snapshot_shape(state.bus);
    validate_controller_snapshot_shape(state.controller, true);

    MapleStateWriter writer;
    writer.raw(maple_state_magic);
    writer.u32(dreamcast_maple_state_contract_version);
    encode_bus_state(writer, state.bus);
    encode_controller_state(writer, state.controller);
    return std::move(writer).finish();
}

DreamcastMapleStateSnapshot decode_dreamcast_maple_state(
    const std::span<const std::uint8_t> bytes) {
    MapleStateReader reader(bytes);
    if (reader.raw(maple_state_magic.size()) !=
        std::vector<std::uint8_t>(
            maple_state_magic.begin(), maple_state_magic.end()))
        throw std::invalid_argument(
            "Maple-Handoff-Payload besitzt keine gueltige Signatur.");
    if (reader.u32() != dreamcast_maple_state_contract_version)
        throw std::invalid_argument(
            "Maple-Handoff-Payload besitzt einen inkompatiblen Vertrag.");

    DreamcastMapleStateSnapshot state;
    state.bus = decode_bus_state(reader);
    state.controller = decode_controller_state(reader);
    reader.expect_end();
    validate_bus_snapshot_shape(state.bus);
    validate_controller_snapshot_shape(state.controller, false);
    return state;
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
