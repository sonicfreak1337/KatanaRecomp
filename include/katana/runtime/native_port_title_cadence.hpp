#pragma once

#include <cstdint>

namespace katana::runtime {

// Optional capability, separate from the generated AOT host-services ABI.
// A title may wait on its proven native clock and publish that completed
// update once without paying the generic host simulation deadline again.
class NativePortTitleCadenceHost {
  public:
    virtual ~NativePortTitleCadenceHost() = default;
    [[nodiscard]] virtual bool title_cadence_available() const noexcept = 0;
    virtual void wait_until_title_deadline(std::uint64_t deadline_nanoseconds) = 0;
    virtual void present_frame_after_title_cadence(std::uint64_t frame_index) = 0;
};

} // namespace katana::runtime
