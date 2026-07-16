#pragma once

#include "katana/runtime/pvr.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace katana::runtime {

inline constexpr std::uint32_t native_video_contract_version = 1u;

struct NativeVideoConfig {
    std::uint32_t contract_version = native_video_contract_version;
    std::string title = "KatanaRecomp Port";
    std::uint32_t client_width = 640u;
    std::uint32_t client_height = 480u;
    bool initially_visible = true;
};

class NativeVideoOutput {
  public:
    virtual ~NativeVideoOutput() = default;
    virtual void show() = 0;
    virtual void poll_events() = 0;
    virtual void resize(std::uint32_t client_width, std::uint32_t client_height) = 0;
    virtual void present(const PvrFrame& frame) = 0;
    virtual void request_close() noexcept = 0;
    [[nodiscard]] virtual bool close_requested() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t client_width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t client_height() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t presented_frames() const noexcept = 0;
};

[[nodiscard]] bool native_video_available() noexcept;
[[nodiscard]] std::unique_ptr<NativeVideoOutput>
create_native_video_output(const NativeVideoConfig& config = {});

} // namespace katana::runtime
