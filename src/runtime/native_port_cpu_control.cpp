#include "katana/runtime/native_port_cpu_control.hpp"

#include "katana/runtime/cache_control.hpp"

#include <atomic>
#include <stdexcept>

namespace katana::runtime {

class NativePortCpuControlDevice final : public MemoryDevice {
  public:
    explicit NativePortCpuControlDevice(
        std::shared_ptr<NativePortCpuControl> state)
        : state_(std::move(state)) {}

    [[nodiscard]] std::size_t size() const noexcept override {
        return sizeof(std::uint32_t);
    }

    [[nodiscard]] std::uint8_t read_u8(std::uint32_t) const override {
        throw std::invalid_argument(
            "Native SH-4 CCR permits only aligned 32-bit accesses.");
    }

    [[nodiscard]] std::uint32_t
    read_u32(const std::uint32_t offset) const override {
        require_word(offset);
        return state_->value();
    }

    void validate_write(const std::uint32_t offset,
                        const std::size_t size,
                        CodeWriteSource) const override {
        if (offset != 0u || size != sizeof(std::uint32_t))
            throw std::invalid_argument(
                "Native SH-4 CCR permits only aligned 32-bit accesses.");
    }

    void write_u8(std::uint32_t, std::uint8_t) override {
        throw std::invalid_argument(
            "Native SH-4 CCR permits only aligned 32-bit accesses.");
    }

    void write_u32(const std::uint32_t offset,
                   const std::uint32_t value) override {
        require_word(offset);
        state_->write(value);
    }

  private:
    static void require_word(const std::uint32_t offset) {
        if (offset != 0u)
            throw std::invalid_argument(
                "Native SH-4 CCR permits only aligned 32-bit accesses.");
    }

    std::shared_ptr<NativePortCpuControl> state_;
};

NativePortCpuControl::NativePortCpuControl(
    const std::uint32_t initial_value)
    : value_(initial_value) {
    if (!valid_initial_value(initial_value))
        throw std::invalid_argument(
            "Native SH-4 CCR bootstrap value is invalid.");
}

std::uint32_t NativePortCpuControl::value() const noexcept {
    return value_;
}

std::uint64_t
NativePortCpuControl::instruction_invalidation_count() const noexcept {
    return instruction_invalidations_;
}

std::uint64_t
NativePortCpuControl::operand_invalidation_count() const noexcept {
    return operand_invalidations_;
}

void NativePortCpuControl::maintain_operand_range(
    const NativePortOperandCacheOperation operation,
    const std::uint32_t address,
    const std::uint32_t size) {
    switch (operation) {
    case NativePortOperandCacheOperation::Invalidate:
    case NativePortOperandCacheOperation::Purge:
        ++operand_invalidations_;
        break;
    case NativePortOperandCacheOperation::WriteBack:
        break;
    default:
        throw std::invalid_argument(
            "Native operand-cache operation is invalid.");
    }

    const auto end = static_cast<std::uint64_t>(address) + size;
    if (end > (std::uint64_t{1u} << 32u))
        throw std::invalid_argument(
            "Native operand-cache range wraps the guest address space.");

    // The address and size bind the title-level provider contract even though
    // coherent host memory needs only a global ordering boundary here.
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void NativePortCpuControl::write(const std::uint32_t value) {
    if ((value & ~supported_write_mask) != 0u)
        throw std::invalid_argument(
            "Native SH-4 CCR write sets reserved bits.");

    const bool invalidate_instruction =
        (value & instruction_invalidate) != 0u;
    const bool invalidate_operand = (value & operand_invalidate) != 0u;
    if (invalidate_instruction) ++instruction_invalidations_;
    if (invalidate_operand) ++operand_invalidations_;

    // Guest caches do not exist in the native product.  The command still
    // forms a full ordering boundary around guest memory and generated AOT
    // execution.  Immutable-code guards reject self-modifying code before an
    // ICI command could otherwise hide it.
    if (invalidate_instruction || invalidate_operand)
        std::atomic_thread_fence(std::memory_order_seq_cst);

    // ICI and OCI are commands and read back as zero.  Only persistent CCR
    // configuration survives the write.
    value_ = value & configuration_mask;
}

std::shared_ptr<NativePortCpuControl>
map_native_port_cpu_control(Memory& memory,
                            const std::uint32_t initial_value) {
    auto state = std::make_shared<NativePortCpuControl>(initial_value);
    auto device = std::make_shared<NativePortCpuControlDevice>(state);
    memory.map_region(
        "native-sh4-cpu-control",
        sh4_cache_control_address,
        std::move(device));
    return state;
}

} // namespace katana::runtime
