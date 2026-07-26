#pragma once

#include "katana/runtime/pvr.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t native_video_contract_version = 2u;

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
    std::optional<PvrGuestFrameProofSource> proof_source;
    std::uint64_t render_generation = 0u;
    std::uint64_t write_generation_first = 0u;
    std::uint64_t write_generation_last = 0u;
};

enum class GuestFrameEvidenceMarker : std::uint8_t {
    None = 0u,
    FirstGuestScanout = 1u << 0u,
    FirstTaFrame = 1u << 1u,
    FirstPostBootstrapTaFrame = 1u << 2u,
};

using GuestFrameEvidenceMarkers = std::uint8_t;

[[nodiscard]] constexpr GuestFrameEvidenceMarkers
guest_frame_evidence_marker(const GuestFrameEvidenceMarker marker) noexcept {
    return static_cast<GuestFrameEvidenceMarkers>(marker);
}

[[nodiscard]] constexpr bool
has_guest_frame_evidence_marker(const GuestFrameEvidenceMarkers markers,
                                const GuestFrameEvidenceMarker marker) noexcept {
    return (markers & guest_frame_evidence_marker(marker)) != 0u;
}

struct GuestFrameEvidenceObservation {
    GuestFrameEvidenceMarkers markers = 0u;
    std::optional<PvrGuestFrameProofSource> proof_source;
    std::uint64_t render_generation = 0u;
    std::uint64_t write_generation_first = 0u;
    std::uint64_t write_generation_last = 0u;
};

class GuestFrameEvidenceTracker {
  public:
    // Post-bootstrap TA evidence remains title-independent: a TA frame must follow either a
    // bootstrap scanout or an earlier proven guest scanout once guest program progress has
    // been observed. This deliberately does not claim gameplay.
    [[nodiscard]] GuestFrameEvidenceObservation observe(const GuestFramePumpResult& frame,
                                                        bool guest_program_progressed) noexcept;

    [[nodiscard]] bool guest_scanout_seen() const noexcept;
    [[nodiscard]] bool ta_frame_seen() const noexcept;
    [[nodiscard]] bool post_bootstrap_ta_frame_seen() const noexcept;
    [[nodiscard]] bool bootstrap_scanout_seen() const noexcept;

  private:
    bool guest_scanout_seen_ = false;
    bool ta_frame_seen_ = false;
    bool post_bootstrap_ta_frame_seen_ = false;
    bool bootstrap_scanout_seen_ = false;
};

[[nodiscard]] GuestFramePumpResult pump_guest_frame_proof(PvrSoftwareRenderer& renderer,
                                                          NativeVideoOutput* output);

} // namespace katana::runtime
