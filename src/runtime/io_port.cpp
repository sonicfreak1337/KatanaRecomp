#include "katana/runtime/io_port.hpp"

#include <stdexcept>
#include <utility>

namespace katana::runtime {
namespace {

std::shared_ptr<MmioMemoryDevice> make_register(const std::shared_ptr<Sh4IoPort>& state,
                                                const MemoryAccessWidth required_width,
                                                MmioReadHandler read,
                                                MmioWriteHandler write) {
    return std::make_shared<MmioMemoryDevice>(
        static_cast<std::size_t>(required_width),
        [state, required_width, read = std::move(read)](const std::uint32_t offset,
                                                        const MemoryAccessWidth width) {
            if (offset != 0u || width != required_width)
                throw std::invalid_argument("SH-4-I/O-Portzugriff besitzt die falsche Breite.");
            return read(offset, width);
        },
        [state, required_width, write = std::move(write)](
            const std::uint32_t offset, const std::uint32_t value, const MemoryAccessWidth width) {
            if (offset != 0u || width != required_width)
                throw std::invalid_argument("SH-4-I/O-Portzugriff besitzt die falsche Breite.");
            write(offset, value, width);
        });
}

} // namespace

Sh4IoPort::Sh4IoPort(const Sh4IoPortInputs inputs) : inputs_(inputs) {}

std::uint16_t Sh4IoPort::output_mask(const std::uint32_t control,
                                     const std::uint32_t bits) noexcept {
    std::uint16_t mask = 0u;
    for (std::uint32_t bit = 0u; bit < bits; ++bit)
        mask |= static_cast<std::uint16_t>(
            (((control >> (bit * 2u)) & 3u) == 1u ? 1u : 0u) << bit);
    return mask;
}

std::uint32_t Sh4IoPort::control_a() const noexcept {
    return control_a_;
}

std::uint16_t Sh4IoPort::data_a() const noexcept {
    const auto outputs = output_mask(control_a_, 16u);
    return static_cast<std::uint16_t>((data_a_latch_ & outputs) | (inputs_.port_a & ~outputs));
}

std::uint32_t Sh4IoPort::control_b() const noexcept {
    return control_b_;
}

std::uint16_t Sh4IoPort::data_b() const noexcept {
    const auto outputs = output_mask(control_b_, 4u);
    return static_cast<std::uint16_t>(((data_b_latch_ & outputs) | (inputs_.port_b & ~outputs)) &
                                      0x000Fu);
}

std::uint16_t Sh4IoPort::gpio_interrupt_control() const noexcept {
    return gpio_interrupt_control_;
}

void Sh4IoPort::write_control_a(const std::uint32_t value) noexcept {
    control_a_ = value;
}
void Sh4IoPort::write_data_a(const std::uint16_t value) noexcept {
    data_a_latch_ = value;
}
void Sh4IoPort::write_control_b(const std::uint32_t value) noexcept {
    control_b_ = value & 0xFFu;
}
void Sh4IoPort::write_data_b(const std::uint16_t value) noexcept {
    data_b_latch_ = value & 0xFu;
}
void Sh4IoPort::write_gpio_interrupt_control(const std::uint16_t value) noexcept {
    gpio_interrupt_control_ = value;
}
void Sh4IoPort::set_inputs(const Sh4IoPortInputs inputs) noexcept {
    inputs_ = inputs;
}
void Sh4IoPort::reset() noexcept {
    control_a_ = 0u;
    data_a_latch_ = 0u;
    control_b_ = 0u;
    data_b_latch_ = 0u;
    gpio_interrupt_control_ = 0u;
}

std::shared_ptr<Sh4IoPort> map_sh4_io_ports(Memory& memory, const Sh4IoPortInputs inputs) {
    auto state = std::make_shared<Sh4IoPort>(inputs);
    memory.map_region("sh4-pctra",
                      sh4_port_control_a_address,
                      make_register(
                          state,
                          MemoryAccessWidth::Word,
                          [state](std::uint32_t, MemoryAccessWidth) { return state->control_a(); },
                          [state](std::uint32_t, std::uint32_t value, MemoryAccessWidth) {
                              state->write_control_a(value);
                          }));
    memory.map_region("sh4-pdtra",
                      sh4_port_data_a_address,
                      make_register(
                          state,
                          MemoryAccessWidth::Halfword,
                          [state](std::uint32_t, MemoryAccessWidth) { return state->data_a(); },
                          [state](std::uint32_t, std::uint32_t value, MemoryAccessWidth) {
                              state->write_data_a(static_cast<std::uint16_t>(value));
                          }));
    memory.map_region("sh4-pctrb",
                      sh4_port_control_b_address,
                      make_register(
                          state,
                          MemoryAccessWidth::Word,
                          [state](std::uint32_t, MemoryAccessWidth) { return state->control_b(); },
                          [state](std::uint32_t, std::uint32_t value, MemoryAccessWidth) {
                              state->write_control_b(value);
                          }));
    memory.map_region("sh4-pdtrb",
                      sh4_port_data_b_address,
                      make_register(
                          state,
                          MemoryAccessWidth::Halfword,
                          [state](std::uint32_t, MemoryAccessWidth) { return state->data_b(); },
                          [state](std::uint32_t, std::uint32_t value, MemoryAccessWidth) {
                              state->write_data_b(static_cast<std::uint16_t>(value));
                          }));
    memory.map_region(
        "sh4-gpioic",
        sh4_gpio_interrupt_control_address,
        make_register(
            state,
            MemoryAccessWidth::Halfword,
            [state](std::uint32_t, MemoryAccessWidth) { return state->gpio_interrupt_control(); },
            [state](std::uint32_t, std::uint32_t value, MemoryAccessWidth) {
                state->write_gpio_interrupt_control(static_cast<std::uint16_t>(value));
            }));
    return state;
}

} // namespace katana::runtime
