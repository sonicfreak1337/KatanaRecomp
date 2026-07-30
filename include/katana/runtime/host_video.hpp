#pragma once

#include "katana/runtime/pvr.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t native_video_contract_version = 3u;

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

enum class NativeVideoBackend : std::uint8_t {
    Unknown,
    Win32Gdi,
    Win32D3d11Hardware,
};

enum class NativeVideoPresentationOutcome : std::uint8_t {
    None,
    Presented,
    Occluded,
    NotPresentable,
    BackendFailure,
};

[[nodiscard]] constexpr const char*
native_video_backend_name(const NativeVideoBackend backend) noexcept {
    switch (backend) {
    case NativeVideoBackend::Unknown:
        return "unknown";
    case NativeVideoBackend::Win32Gdi:
        return "win32-gdi";
    case NativeVideoBackend::Win32D3d11Hardware:
        return "win32-d3d11-hardware";
    }
    return "unknown";
}

[[nodiscard]] constexpr const char*
native_video_presentation_outcome_name(
    const NativeVideoPresentationOutcome outcome) noexcept {
    switch (outcome) {
    case NativeVideoPresentationOutcome::None:
        return "none";
    case NativeVideoPresentationOutcome::Presented:
        return "presented";
    case NativeVideoPresentationOutcome::Occluded:
        return "occluded";
    case NativeVideoPresentationOutcome::NotPresentable:
        return "not-presentable";
    case NativeVideoPresentationOutcome::BackendFailure:
        return "backend-failure";
    }
    return "unknown";
}

struct NativeVideoPresentationTelemetry {
    std::uint64_t submitted_frames = 0u;
    std::uint64_t presented_frames = 0u;
    std::uint64_t occluded_frames = 0u;
    std::uint64_t not_presentable_frames = 0u;
    std::uint64_t backend_failures = 0u;
    NativeVideoPresentationOutcome last_outcome =
        NativeVideoPresentationOutcome::None;
    bool currently_occluded = false;
};

struct NativeVideoViewport {
    std::uint32_t x = 0u;
    std::uint32_t y = 0u;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
};

[[nodiscard]] NativeVideoViewport
calculate_native_video_viewport(std::uint32_t frame_width,
                                std::uint32_t frame_height,
                                std::uint32_t client_width,
                                std::uint32_t client_height) noexcept;

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
    [[nodiscard]] virtual NativeVideoBackend backend() const noexcept {
        return NativeVideoBackend::Unknown;
    }
    [[nodiscard]] virtual bool hardware_accelerated() const noexcept {
        return false;
    }
    [[nodiscard]] virtual std::uint64_t backend_fallbacks() const noexcept {
        return 0u;
    }
    [[nodiscard]] virtual NativeVideoPresentationTelemetry
    presentation_telemetry() const noexcept {
        NativeVideoPresentationTelemetry result;
        result.presented_frames = presented_frames();
        result.submitted_frames = result.presented_frames;
        if (result.presented_frames != 0u)
            result.last_outcome = NativeVideoPresentationOutcome::Presented;
        return result;
    }
};

[[nodiscard]] bool native_video_available() noexcept;
[[nodiscard]] std::unique_ptr<NativeVideoOutput>
create_native_video_output(const NativeVideoConfig& config = {});
[[nodiscard]] bool present_guest_frame_proof(NativeVideoOutput& output,
                                             const PvrGuestFrameProof& proof);

enum class GuestFramePresentedSource : std::uint8_t { None, Scanout, GuestProof };

[[nodiscard]] constexpr const char*
guest_frame_presented_source_name(const GuestFramePresentedSource source) noexcept {
    switch (source) {
    case GuestFramePresentedSource::None:
        return "none";
    case GuestFramePresentedSource::Scanout:
        return "scanout";
    case GuestFramePresentedSource::GuestProof:
        return "guest-proof";
    }
    return "unknown";
}

[[nodiscard]] constexpr const char*
guest_frame_proof_source_name(const PvrGuestFrameProofSource source) noexcept {
    switch (source) {
    case PvrGuestFrameProofSource::TaRender:
        return "ta-render";
    case PvrGuestFrameProofSource::DirectFramebuffer:
        return "direct-framebuffer";
    }
    return "unknown";
}

inline constexpr std::size_t guest_frame_visibility_tile_columns = 8u;
inline constexpr std::size_t guest_frame_visibility_tile_rows = 6u;
inline constexpr std::size_t guest_frame_visibility_tile_count =
    guest_frame_visibility_tile_columns * guest_frame_visibility_tile_rows;
inline constexpr std::uint32_t guest_frame_minimum_changed_tiles = 4u;

struct GuestFramePresentationEvidence {
    bool valid = false;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::uint64_t pixel_count = 0u;
    std::uint64_t digest = 0u;
    std::uint64_t nonblack_pixels = 0u;
    std::uint64_t relevant_color_class_mask = 0u;
    std::uint16_t relevant_luminance_class_mask = 0u;
};

[[nodiscard]] GuestFramePresentationEvidence
describe_guest_frame_presentation(const PvrFrame& frame) noexcept;
[[nodiscard]] bool
has_relevant_guest_frame_content(const GuestFramePresentationEvidence& evidence) noexcept;
[[nodiscard]] std::uint64_t
minimum_visible_guest_frame_changed_pixels(std::uint64_t pixel_count) noexcept;

struct GuestFrameVisibilityObservation {
    bool baseline_available = false;
    bool current_frame_valid = false;
    bool geometry_matches = false;
    bool digest_changed = false;
    bool visible_progress = false;
    std::uint64_t proof_changed_pixels = 0u;
    std::uint64_t changed_pixels = 0u;
    std::uint64_t required_changed_pixels = 0u;
    std::uint64_t required_changed_pixels_per_tile = 0u;
    std::uint64_t required_changed_pixels_per_class = 0u;
    std::uint32_t changed_tiles = 0u;
    std::uint32_t changed_interior_tiles = 0u;
    std::uint32_t relevant_color_classes = 0u;
    std::uint32_t relevant_luminance_classes = 0u;
    std::uint32_t changed_color_classes = 0u;
    std::uint32_t changed_luminance_classes = 0u;
};

class GuestFrameVisibilityClassifier {
  public:
    // Missing or changed baseline geometry is adopted fail-closed: the adopting
    // proof is never progress, while a later sufficiently different proof may be.
    void begin_product_interval(std::optional<PvrFrame> baseline) noexcept;

    [[nodiscard]] GuestFrameVisibilityObservation
    observe(const PvrFrame& frame,
            const GuestFramePresentationEvidence& evidence,
            std::uint64_t proof_changed_pixels);
    [[nodiscard]] bool baseline_available() const noexcept;

  private:
    std::optional<PvrFrame> baseline_;
    GuestFramePresentationEvidence baseline_evidence_;
};

struct GuestFramePumpResult {
    bool guest_frame_proven = false;
    bool frame_presented = false;
    bool proven_frame_presented = false;
    std::optional<PvrGuestFrameProofSource> proof_source;
    GuestFramePresentedSource presented_source = GuestFramePresentedSource::None;
    std::optional<PvrGuestFrameProofSource> presented_proof_source;
    std::optional<PvrFrame> presented_frame;
    GuestFramePresentationEvidence presented_frame_evidence;
    bool presented_frame_evidence_collected = false;
    std::uint64_t presented_changed_pixels = 0u;
    std::uint64_t presented_nonblack_pixels = 0u;
    std::uint64_t presented_pixel_count = 0u;
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
                                                          NativeVideoOutput* output,
                                                          bool collect_detailed_presentation_evidence =
                                                              true);

} // namespace katana::runtime
