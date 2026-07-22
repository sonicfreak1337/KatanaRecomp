#pragma once

#include "katana/runtime/pvr.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t native_video_contract_version = 1u;

struct NativeVideoConfig {
    std::uint32_t contract_version = native_video_contract_version;
    std::string title = "KatanaRecomp Port";
    std::uint32_t client_width = 640u;
    std::uint32_t client_height = 480u;
    bool initially_visible = true;
};

enum class NativeHostEventKind : std::uint8_t { FocusGained, FocusLost, KeyDown, KeyUp, Close };
enum class NativeHostKey : std::uint8_t { Unknown, Start, A, B, X, Y, Up, Down, Left, Right };

struct NativeHostEvent {
    std::uint64_t sequence = 0u;
    NativeHostEventKind kind = NativeHostEventKind::FocusGained;
    NativeHostKey key = NativeHostKey::Unknown;
};

class NativeVideoOutput {
  public:
    virtual ~NativeVideoOutput() = default;
    virtual void show() = 0;
    virtual void poll_events() = 0;
    [[nodiscard]] virtual std::vector<NativeHostEvent> drain_events() = 0;
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
[[nodiscard]] bool present_guest_frame_proof(NativeVideoOutput& output,
                                             const PvrGuestFrameProof& proof);

struct GuestFramePumpResult {
    bool guest_frame_proven = false;
    bool frame_presented = false;
};

[[nodiscard]] GuestFramePumpResult pump_guest_frame_proof(PvrSoftwareRenderer& renderer,
                                                          NativeVideoOutput* output);

} // namespace katana::runtime
