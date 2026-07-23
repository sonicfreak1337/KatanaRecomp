#pragma once

#include "katana/runtime/memory.hpp"

#include <cstdint>
#include <memory>

namespace katana::runtime {

inline constexpr std::uint32_t sh4_port_control_a_address = 0xFF80002Cu;
inline constexpr std::uint32_t sh4_port_data_a_address = 0xFF800030u;
inline constexpr std::uint32_t sh4_port_control_b_address = 0xFF800040u;
inline constexpr std::uint32_t sh4_port_data_b_address = 0xFF800044u;
inline constexpr std::uint32_t sh4_gpio_interrupt_control_address = 0xFF800048u;

struct Sh4IoPortInputs {
    std::uint16_t port_a = 0u;
    std::uint16_t port_b = 0u;
};

struct Sh4IoPortSnapshot {
    Sh4IoPortInputs inputs{};
    std::uint32_t control_a = 0u;
    std::uint16_t data_a_latch = 0u;
    std::uint16_t effective_data_a = 0u;
    std::uint32_t control_b = 0u;
    std::uint16_t data_b_latch = 0u;
    std::uint16_t effective_data_b = 0u;
    std::uint16_t gpio_interrupt_control = 0u;
};

class Sh4IoPort final {
  public:
    explicit Sh4IoPort(Sh4IoPortInputs inputs = {});

    [[nodiscard]] std::uint32_t control_a() const noexcept;
    [[nodiscard]] std::uint16_t data_a() const noexcept;
    [[nodiscard]] std::uint32_t control_b() const noexcept;
    [[nodiscard]] std::uint16_t data_b() const noexcept;
    [[nodiscard]] std::uint16_t gpio_interrupt_control() const noexcept;
    void write_control_a(std::uint32_t value) noexcept;
    void write_data_a(std::uint16_t value) noexcept;
    void write_control_b(std::uint32_t value) noexcept;
    void write_data_b(std::uint16_t value) noexcept;
    void write_gpio_interrupt_control(std::uint16_t value) noexcept;
    void set_inputs(Sh4IoPortInputs inputs) noexcept;
    [[nodiscard]] Sh4IoPortSnapshot snapshot() const noexcept;
    void reset() noexcept;

  private:
    [[nodiscard]] static std::uint16_t output_mask(std::uint32_t control,
                                                   std::uint32_t bits) noexcept;

    Sh4IoPortInputs inputs_{};
    std::uint32_t control_a_ = 0u;
    std::uint16_t data_a_latch_ = 0u;
    std::uint32_t control_b_ = 0u;
    std::uint16_t data_b_latch_ = 0u;
    std::uint16_t gpio_interrupt_control_ = 0u;
};

[[nodiscard]] std::shared_ptr<Sh4IoPort> map_sh4_io_ports(Memory& memory,
                                                          Sh4IoPortInputs inputs = {});

} // namespace katana::runtime
