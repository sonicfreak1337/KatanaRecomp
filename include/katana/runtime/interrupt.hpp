#pragma once

#include "katana/runtime/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace katana::runtime {

using InterruptSource = std::uint32_t;

struct PendingInterrupt {
    InterruptSource source = 0u;
    std::uint8_t level = 0u;
    std::uint32_t event_code = 0u;

    [[nodiscard]] bool operator==(const PendingInterrupt&) const = default;
};

struct InterruptControllerSnapshot {
    std::vector<PendingInterrupt> pending;

    [[nodiscard]] bool operator==(const InterruptControllerSnapshot&) const = default;
};

class InterruptController {
  public:
    void request(InterruptSource source, std::uint8_t level, std::uint32_t event_code);
    [[nodiscard]] bool cancel(InterruptSource source) noexcept;
    void clear() noexcept;
    [[nodiscard]] bool pending(InterruptSource source) const noexcept;
    [[nodiscard]] std::size_t pending_count() const noexcept;
    [[nodiscard]] std::uint64_t interrupt_epoch() const noexcept;
    [[nodiscard]] std::uint8_t highest_pending_level() const noexcept;
    [[nodiscard]] std::uint64_t pending_mask() const noexcept;
    [[nodiscard]] bool can_accept(const CpuState& cpu) const noexcept;
    [[nodiscard]] std::optional<PendingInterrupt> highest_pending() const noexcept;
    [[nodiscard]] InterruptControllerSnapshot snapshot() const;

  private:
    friend bool accept_pending_interrupt(CpuState& cpu, InterruptController& controller) noexcept;

    void update_pending_metadata() noexcept;
    std::vector<PendingInterrupt> pending_;
    std::uint64_t interrupt_epoch_ = 0u;
    std::uint64_t pending_mask_ = 0u;
    std::uint8_t highest_pending_level_ = 0u;
};

[[nodiscard]] bool accept_pending_interrupt(CpuState& cpu,
                                            InterruptController& controller) noexcept;

} // namespace katana::runtime
