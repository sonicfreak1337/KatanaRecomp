#pragma once

#include "katana/runtime/aica.hpp"

#include <cstdint>
#include <memory>

namespace katana::runtime {

class AicaArm7Core final {
  public:
    AicaArm7Core();
    ~AicaArm7Core();
    AicaArm7Core(const AicaArm7Core&) = delete;
    AicaArm7Core& operator=(const AicaArm7Core&) = delete;

    void bind_bus(const std::shared_ptr<AicaRegisterFile>& registers,
                  const std::shared_ptr<LinearMemoryDevice>& ram);
    void reset(bool enabled) noexcept;
    void set_enabled(bool enabled) noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool bus_bound() const noexcept;
    [[nodiscard]] bool faulted() const noexcept;
    void mark_faulted() noexcept;
    void set_fiq_line(bool asserted) noexcept;
    void run_cycles(std::uint64_t cycles) noexcept;

    [[nodiscard]] AicaArm7Snapshot snapshot() const noexcept;
    void validate_state_restore(const AicaArm7Snapshot& state) const;
    void commit_validated_state_restore(AicaArm7Snapshot state) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace katana::runtime
