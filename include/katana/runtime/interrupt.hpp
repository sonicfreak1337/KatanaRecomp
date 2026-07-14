#pragma once

#include "katana/runtime/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace katana::runtime {

using InterruptSource = std::uint32_t;

struct PendingInterrupt {
    InterruptSource source = 0u;
    std::uint8_t level = 0u;
    std::uint32_t event_code = 0u;
};

class InterruptController {
public:
    void request(
        InterruptSource source,
        std::uint8_t level,
        std::uint32_t event_code
    );
    [[nodiscard]] bool cancel(InterruptSource source) noexcept;
    void clear() noexcept;
    [[nodiscard]] bool pending(InterruptSource source) const noexcept;
    [[nodiscard]] std::size_t pending_count() const noexcept;

private:
    friend bool accept_pending_interrupt(
        CpuState& cpu,
        InterruptController& controller
    ) noexcept;

    std::vector<PendingInterrupt> pending_;
};

[[nodiscard]] bool accept_pending_interrupt(
    CpuState& cpu,
    InterruptController& controller
) noexcept;

} // namespace katana::runtime
