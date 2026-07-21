#include "katana/runtime/interrupt.hpp"

#include "katana/runtime/exception.hpp"

#include <algorithm>

namespace katana::runtime {

void InterruptController::request(const InterruptSource source,
                                  const std::uint8_t level,
                                  const std::uint32_t event_code) {
    const auto existing =
        std::find_if(pending_.begin(), pending_.end(), [source](const PendingInterrupt& interrupt) {
            return interrupt.source == source;
        });
    const PendingInterrupt request{
        source, static_cast<std::uint8_t>(level > 15u ? 15u : level), event_code};
    if (existing == pending_.end()) {
        pending_.push_back(request);
    } else {
        *existing = request;
    }
}

bool InterruptController::cancel(const InterruptSource source) noexcept {
    const auto old_size = pending_.size();
    std::erase_if(pending_, [source](const PendingInterrupt& interrupt) {
        return interrupt.source == source;
    });
    return pending_.size() != old_size;
}

void InterruptController::clear() noexcept {
    pending_.clear();
}

bool InterruptController::pending(const InterruptSource source) const noexcept {
    return std::any_of(
        pending_.begin(), pending_.end(), [source](const PendingInterrupt& interrupt) {
            return interrupt.source == source;
        });
}

std::size_t InterruptController::pending_count() const noexcept {
    return pending_.size();
}

std::optional<PendingInterrupt> InterruptController::highest_pending() const noexcept {
    const auto selected = std::max_element(
        pending_.begin(), pending_.end(), [](const auto& left, const auto& right) {
            if (left.level != right.level) return left.level < right.level;
            return left.source > right.source;
        });
    if (selected == pending_.end()) return std::nullopt;
    return *selected;
}

bool accept_pending_interrupt(CpuState& cpu, InterruptController& controller) noexcept {
    if (cpu.interrupts_blocked()) {
        return false;
    }

    const std::uint8_t mask = cpu.interrupt_mask();
    const auto selected =
        std::max_element(controller.pending_.begin(),
                         controller.pending_.end(),
                         [](const PendingInterrupt& left, const PendingInterrupt& right) {
                             if (left.level != right.level) {
                                 return left.level < right.level;
                             }
                             return left.source > right.source;
                         });
    if (selected == controller.pending_.end() || selected->level <= mask) {
        return false;
    }

    const PendingInterrupt accepted = *selected;
    controller.pending_.erase(selected);
    enter_exception(cpu,
                    ExceptionRequest{ExceptionCause::Interrupt,
                                     accepted.event_code,
                                     interrupt_vector,
                                     cpu.pc,
                                     std::nullopt,
                                     true,
                                     false});
    return true;
}

} // namespace katana::runtime
