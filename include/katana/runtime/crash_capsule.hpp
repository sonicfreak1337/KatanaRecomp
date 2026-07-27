#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace katana::runtime {

inline constexpr std::uint32_t crash_capsule_contract_version = 1u;
inline constexpr std::size_t crash_capsule_event_capacity = 16u;
static_assert((crash_capsule_event_capacity &
               (crash_capsule_event_capacity - 1u)) == 0u);

enum class CrashCapsuleEventKind : std::uint8_t {
    Block = 1u,
    Mmio = 2u,
    Scheduler = 3u,
    Error = 4u,
};

struct CrashCapsuleEvent {
    std::uint64_t guest_cycle = 0u;
    std::uint64_t detail = 0u;
    std::uint32_t pc = 0u;
    std::uint32_t subject = 0u;
    std::uint32_t auxiliary = 0u;
    std::uint16_t code = 0u;
    CrashCapsuleEventKind kind = CrashCapsuleEventKind::Block;
    std::uint8_t flags = 0u;
};

// Always-on product fault context. The complete state is fixed-size POD: recording performs
// no allocation, string formatting, locking, map lookup, or callback construction.
struct CrashCapsule {
    std::uint64_t observed_guest_cycle = 0u;
    std::uint64_t last_scheduler_cycle = 0u;
    std::uint64_t last_scheduler_event_id = 0u;
    std::uint32_t last_pc = 0u;
    std::uint32_t last_block = 0u;
    std::uint32_t last_mmio_address = 0u;
    std::uint32_t last_mmio_value = 0u;
    std::uint32_t last_scheduler_event_kind = 0u;
    std::uint32_t first_error_code = 0u;
    std::uint32_t first_error_pc = 0u;
    std::uint32_t first_error_target = 0u;
    std::uint32_t next_event = 0u;
    std::uint32_t event_count = 0u;
    std::uint8_t last_mmio_width = 0u;
    std::uint8_t last_mmio_operation = 0u;
    std::uint8_t first_error_latched = 0u;
    std::uint8_t reserved = 0u;
    std::array<CrashCapsuleEvent, crash_capsule_event_capacity> events{};

    void note_block(const std::uint32_t pc,
                    const std::uint32_t block,
                    const std::uint64_t guest_cycle) noexcept {
        observed_guest_cycle = guest_cycle;
        last_pc = pc;
        last_block = block;
        push({guest_cycle, 0u, pc, block, 0u, 0u, CrashCapsuleEventKind::Block, 0u});
    }

    void note_mmio(const std::uint8_t operation,
                   const std::uint8_t width,
                   const std::uint32_t address,
                   const std::uint32_t value) noexcept {
        last_mmio_operation = operation;
        last_mmio_width = width;
        last_mmio_address = address;
        last_mmio_value = value;
        push({observed_guest_cycle,
              value,
              last_pc,
              address,
              0u,
              width,
              CrashCapsuleEventKind::Mmio,
              operation});
    }

    void note_scheduler(const std::uint64_t guest_cycle,
                        const std::uint64_t event_id,
                        const std::uint32_t event_kind) noexcept {
        observed_guest_cycle = guest_cycle;
        last_scheduler_cycle = guest_cycle;
        last_scheduler_event_id = event_id;
        last_scheduler_event_kind = event_kind;
        push({guest_cycle,
              event_id,
              last_pc,
              event_kind,
              0u,
              0u,
              CrashCapsuleEventKind::Scheduler,
              0u});
    }

    void note_first_error(const std::uint32_t error_code,
                          const std::uint32_t pc,
                          const std::uint32_t target) noexcept {
        if (first_error_latched != 0u) return;
        first_error_latched = 1u;
        first_error_code = error_code;
        first_error_pc = pc;
        first_error_target = target;
        push({observed_guest_cycle,
              target,
              pc,
              error_code,
              0u,
              0u,
              CrashCapsuleEventKind::Error,
              0u});
    }

  private:
    void push(const CrashCapsuleEvent event) noexcept {
        events[next_event] = event;
        next_event = (next_event + 1u) &
                     static_cast<std::uint32_t>(crash_capsule_event_capacity - 1u);
        if (event_count < crash_capsule_event_capacity) ++event_count;
    }
};

static_assert(std::is_standard_layout_v<CrashCapsuleEvent>);
static_assert(std::is_trivially_copyable_v<CrashCapsuleEvent>);
static_assert(std::is_standard_layout_v<CrashCapsule>);
static_assert(std::is_trivially_copyable_v<CrashCapsule>);

} // namespace katana::runtime
