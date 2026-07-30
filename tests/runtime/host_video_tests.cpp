#include "katana/runtime/host_video.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#ifdef _WIN32
#include "host_video_d3d11.hpp"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception, typename Callback> bool throws(Callback&& callback) {
    try {
        callback();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

class FakeVideoOutput final : public katana::runtime::NativeVideoOutput {
  public:
    explicit FakeVideoOutput(const bool acknowledge_present = true)
        : acknowledge_present_(acknowledge_present) {}

    void show() override {}
    void poll_events() override {}
    [[nodiscard]] std::vector<katana::runtime::NativeHostEvent> drain_events() override {
        return {};
    }
    void resize(const std::uint32_t width, const std::uint32_t height) override {
        width_ = width;
        height_ = height;
    }
    void present(const katana::runtime::PvrFrame& frame) override {
        last_frame_ = frame;
        ++present_attempts_;
        if (rejected_presentations_ != 0u) {
            --rejected_presentations_;
            return;
        }
        if (acknowledge_present_) ++presented_frames_;
    }
    void request_close() noexcept override { close_requested_ = true; }
    [[nodiscard]] bool close_requested() const noexcept override { return close_requested_; }
    [[nodiscard]] std::uint32_t client_width() const noexcept override { return width_; }
    [[nodiscard]] std::uint32_t client_height() const noexcept override { return height_; }
    [[nodiscard]] std::uint64_t presented_frames() const noexcept override {
        return presented_frames_;
    }
    [[nodiscard]] const katana::runtime::PvrFrame& last_frame() const noexcept {
        return last_frame_;
    }
    void reject_next_presentations(const std::uint32_t count) noexcept {
        rejected_presentations_ = count;
    }
    [[nodiscard]] std::uint64_t present_attempts() const noexcept {
        return present_attempts_;
    }

  private:
    katana::runtime::PvrFrame last_frame_;
    std::uint64_t presented_frames_ = 0u;
    std::uint64_t present_attempts_ = 0u;
    std::uint32_t width_ = 1u;
    std::uint32_t height_ = 1u;
    std::uint32_t rejected_presentations_ = 0u;
    bool acknowledge_present_ = true;
    bool close_requested_ = false;
};

katana::runtime::PvrFrame solid_frame(const std::uint32_t width,
                                      const std::uint32_t height,
                                      const std::array<std::uint8_t, 3u> color) {
    katana::runtime::PvrFrame frame;
    frame.width = width;
    frame.height = height;
    frame.rgba.resize(static_cast<std::size_t>(width) * height * 4u);
    for (std::size_t offset = 0u; offset < frame.rgba.size(); offset += 4u) {
        frame.rgba[offset] = color[0u];
        frame.rgba[offset + 1u] = color[1u];
        frame.rgba[offset + 2u] = color[2u];
        frame.rgba[offset + 3u] = 0xFFu;
    }
    return frame;
}

katana::runtime::PvrFrame structured_frame(const std::uint32_t width,
                                           const std::uint32_t height,
                                           const std::uint32_t phase = 0u) {
    constexpr std::array colors{
        std::array<std::uint8_t, 3u>{0x20u, 0x20u, 0x60u},
        std::array<std::uint8_t, 3u>{0xE0u, 0x40u, 0x40u},
        std::array<std::uint8_t, 3u>{0xF0u, 0xF0u, 0x50u}};
    auto frame = solid_frame(width, height, colors.front());
    for (std::uint32_t y = 0u; y < height; ++y) {
        for (std::uint32_t x = 0u; x < width; ++x) {
            const auto band = std::min<std::size_t>(
                colors.size() - 1u,
                static_cast<std::size_t>(
                    static_cast<std::uint64_t>(x) * colors.size() / width));
            const auto& color = colors[(band + phase) % colors.size()];
            const auto offset =
                (static_cast<std::size_t>(y) * width + x) * 4u;
            frame.rgba[offset] = color[0u];
            frame.rgba[offset + 1u] = color[1u];
            frame.rgba[offset + 2u] = color[2u];
        }
    }
    return frame;
}
} // namespace

int main() {
    using namespace katana::runtime;
    static_assert(native_video_contract_version == 3u);
    static_assert(
        std::string_view(native_video_backend_name(NativeVideoBackend::Win32Gdi)) ==
        "win32-gdi");
#ifdef _WIN32
    static_assert(
        detail::classify_win32_d3d11_present_result(
            detail::Win32D3d11PresentResult::Presented) ==
        detail::Win32D3d11PresentDecision{
            NativeVideoPresentationOutcome::Presented, true});
    static_assert(
        detail::classify_win32_d3d11_present_result(
            detail::Win32D3d11PresentResult::Occluded) ==
        detail::Win32D3d11PresentDecision{
            NativeVideoPresentationOutcome::Occluded, true});
    static_assert(
        detail::classify_win32_d3d11_present_result(
            detail::Win32D3d11PresentResult::NotPresentable) ==
        detail::Win32D3d11PresentDecision{
            NativeVideoPresentationOutcome::NotPresentable, true});
    static_assert(
        detail::classify_win32_d3d11_present_result(
            detail::Win32D3d11PresentResult::Failed) ==
        detail::Win32D3d11PresentDecision{
            NativeVideoPresentationOutcome::BackendFailure, false});
#endif
    FakeVideoOutput backend_default;
    require(backend_default.backend() == NativeVideoBackend::Unknown &&
                !backend_default.hardware_accelerated() &&
                backend_default.backend_fallbacks() == 0u,
            "Generischer Hostvideo-Backendstatus behauptet Hardwarebeschleunigung.");
    const auto wide_viewport =
        calculate_native_video_viewport(640u, 480u, 1280u, 720u);
    const auto tall_viewport =
        calculate_native_video_viewport(1280u, 720u, 640u, 480u);
    require(wide_viewport.x == 160u && wide_viewport.y == 0u &&
                wide_viewport.width == 960u && wide_viewport.height == 720u &&
                tall_viewport.x == 0u && tall_viewport.y == 60u &&
                tall_viewport.width == 640u && tall_viewport.height == 360u &&
                calculate_native_video_viewport(0u, 480u, 640u, 480u).width == 0u,
            "Aspect-Ratio-Viewport erzeugt keine reproduzierbaren Letterbox-Grenzen.");
    PvrGuestFrameProof proof;
    proof.render_generation = 7u;
    proof.changed_pixels = 2u;
    proof.source = PvrGuestFrameProofSource::DirectFramebuffer;
    proof.frame = {2u,
                   1u,
                   {0x10u, 0x20u, 0x30u, 0xFFu, 0x40u, 0x50u, 0x60u, 0xFFu}};
    FakeVideoOutput fake_video;
    require(present_guest_frame_proof(fake_video, proof) &&
                fake_video.presented_frames() == 1u &&
                fake_video.last_frame().rgba == proof.frame.rgba,
            "VBlank-Gastframenachweis erreicht Fake-Hostvideo nicht pixelgenau.");
    FakeVideoOutput non_acknowledging_video(false);
    require(!present_guest_frame_proof(non_acknowledging_video, proof),
            "Host-Presentmarker wird ohne bestaetigten Presentfortschritt gesetzt.");

    PvrSoftwareRenderer retry_renderer;
    require(retry_renderer.retain_unpresented_guest_frame_proof(proof),
            "Synthetischer Praesentationsproof kann nicht als bounded Kandidat gesetzt werden.");
    FakeVideoOutput retry_video;
    retry_video.reject_next_presentations(2u);
    const auto rejected_proof_once =
        pump_guest_frame_proof(retry_renderer, &retry_video);
    require(rejected_proof_once.guest_frame_proven &&
                !rejected_proof_once.frame_presented &&
                !rejected_proof_once.proven_frame_presented &&
                rejected_proof_once.presented_source ==
                    GuestFramePresentedSource::GuestProof &&
                rejected_proof_once.presented_frame_evidence_collected,
            "Erster abgelehnter Proof bleibt nicht als bounded Retry erhalten.");
    const auto rejected_proof_twice =
        pump_guest_frame_proof(retry_renderer, &retry_video);
    require(rejected_proof_twice.guest_frame_proven &&
                !rejected_proof_twice.frame_presented &&
                rejected_proof_twice.presented_source ==
                    GuestFramePresentedSource::GuestProof,
            "Proof-Retry geht nach dem zweiten abgelehnten Present verloren.");
    const auto accepted_proof_retry =
        pump_guest_frame_proof(retry_renderer, &retry_video);
    require(accepted_proof_retry.guest_frame_proven &&
                accepted_proof_retry.frame_presented &&
                accepted_proof_retry.proven_frame_presented &&
                accepted_proof_retry.presented_source ==
                    GuestFramePresentedSource::GuestProof &&
                accepted_proof_retry.presented_proof_source ==
                    PvrGuestFrameProofSource::DirectFramebuffer &&
                accepted_proof_retry.presented_frame.has_value() &&
                retry_video.present_attempts() == 3u &&
                retry_video.presented_frames() == 1u,
            "Bounded Proof-Retry erreicht nach zwei transienten Ablehnungen kein "
            "exakt einmal bestaetigtes Present.");

    PvrSoftwareRenderer identical_newer_scanout_renderer;
    require(
        identical_newer_scanout_renderer.retain_unpresented_guest_frame_proof(
            proof),
        "Identischer-Scanout-Test kann Proofkandidaten nicht setzen.");
    FakeVideoOutput identical_newer_scanout_video;
    identical_newer_scanout_video.reject_next_presentations(1u);
    const auto rejected_before_identical_scanout =
        pump_guest_frame_proof(
            identical_newer_scanout_renderer,
            &identical_newer_scanout_video);
    require(
        !rejected_before_identical_scanout.frame_presented &&
            identical_newer_scanout_renderer.retain_unpresented_scanout_frame(
                proof.frame),
        "Identischer-Scanout-Test kann neueren Scanout nicht hinterlegen.");
    const auto identical_newer_scanout_present =
        pump_guest_frame_proof(
            identical_newer_scanout_renderer,
            &identical_newer_scanout_video);
    const auto after_identical_newer_scanout_present =
        pump_guest_frame_proof(
            identical_newer_scanout_renderer,
            &identical_newer_scanout_video);
    require(
        identical_newer_scanout_present.guest_frame_proven &&
            identical_newer_scanout_present.frame_presented &&
            !identical_newer_scanout_present.proven_frame_presented &&
            identical_newer_scanout_present.presented_source ==
                GuestFramePresentedSource::Scanout &&
            !identical_newer_scanout_present.presented_proof_source.has_value() &&
            !identical_newer_scanout_present.presented_frame_evidence_collected &&
            identical_newer_scanout_present.presented_changed_pixels == 0u &&
            identical_newer_scanout_present.presented_frame.has_value() &&
            identical_newer_scanout_present.presented_frame->rgba ==
                proof.frame.rgba &&
            !after_identical_newer_scanout_present.frame_presented,
        "Bytegleicher neuer VBlank-Scanout erbt den aelteren Retry-Proof.");

    PvrSoftwareRenderer divergent_renderer;
    require(divergent_renderer.retain_unpresented_guest_frame_proof(proof),
            "Divergenztest kann Proofkandidaten nicht setzen.");
    FakeVideoOutput divergent_video;
    divergent_video.reject_next_presentations(1u);
    const auto rejected_before_divergence =
        pump_guest_frame_proof(divergent_renderer, &divergent_video);
    auto divergent_scanout = proof.frame;
    divergent_scanout.rgba.front() ^= 0xFFu;
    require(!rejected_before_divergence.frame_presented &&
                divergent_renderer.retain_unpresented_scanout_frame(
                    divergent_scanout),
            "Divergenztest kann neueren Scanout nicht hinterlegen.");
    const auto divergent_present =
        pump_guest_frame_proof(divergent_renderer, &divergent_video);
    const auto after_divergent_present =
        pump_guest_frame_proof(divergent_renderer, &divergent_video);
    require(divergent_present.guest_frame_proven &&
                divergent_present.frame_presented &&
                !divergent_present.proven_frame_presented &&
                divergent_present.presented_source ==
                    GuestFramePresentedSource::Scanout &&
                !divergent_present.presented_proof_source.has_value() &&
                !divergent_present.presented_frame_evidence_collected &&
                divergent_present.presented_frame.has_value() &&
                divergent_present.presented_frame->rgba ==
                    divergent_scanout.rgba &&
                !after_divergent_present.frame_presented,
            "Abweichender neuer Scanout erbt einen alten Proof oder der alte Proof "
            "bleibt nach newest-wins aktiv.");

    PvrSoftwareRenderer scanout_retry_renderer;
    require(scanout_retry_renderer.retain_unpresented_scanout_frame(
                divergent_scanout),
            "Scanout-Retrytest kann Kandidaten nicht setzen.");
    FakeVideoOutput scanout_retry_video;
    scanout_retry_video.reject_next_presentations(1u);
    const auto rejected_scanout =
        pump_guest_frame_proof(scanout_retry_renderer, &scanout_retry_video);
    const auto accepted_scanout =
        pump_guest_frame_proof(scanout_retry_renderer, &scanout_retry_video);
    require(!rejected_scanout.frame_presented &&
                rejected_scanout.presented_source ==
                    GuestFramePresentedSource::Scanout &&
                accepted_scanout.frame_presented &&
                accepted_scanout.presented_source ==
                    GuestFramePresentedSource::Scanout &&
                !accepted_scanout.proven_frame_presented &&
                scanout_retry_video.present_attempts() == 2u,
            "Unbestaetigter normaler Scanout wird nicht bounded erneut eingereicht.");

    EventScheduler scheduler;
    PvrRegisterFile registers(scheduler, PvrTiming{20u, 100u, 100u});
    LinearMemoryDevice vram(8u << 20u);
    PvrSoftwareRenderer renderer;
    constexpr std::uint32_t parameter_base = 0x00100000u;
    registers.write(pvr_register::FramebufferXClip, 0u);
    registers.write(pvr_register::FramebufferYClip, 0u);
    registers.write(pvr_register::FramebufferWriteControl, 6u);
    registers.write(pvr_register::FramebufferWriteSof1, 0x4000u);
    registers.write(pvr_register::ParameterBase, parameter_base);
    registers.write(pvr_register::BackgroundPlaneConfig, 1u << 24u);
    registers.write(pvr_register::BackgroundPlaneDepth, std::bit_cast<std::uint32_t>(0.5f));
    registers.write(pvr_register::FramebufferReadControl, 0xDu | (1u << 23u));
    registers.write(pvr_register::FramebufferReadSize, 1u << 20u);
    registers.write(pvr_register::FramebufferReadSof1, 0x4000u);
    registers.write(pvr_register::VideoControl,
                    registers.read(pvr_register::VideoControl) & ~0x8u);
    registers.write(pvr_register::SpgLoad, (9u << 16u) | 9u);
    registers.write(pvr_register::SpgVblankInterrupt, (2u << 16u) | 1u);
    const auto write_parameter_word =
        [&](const std::uint32_t logical_address,
            const std::uint32_t value) {
            vram.write_u32(
                dreamcast_vram_32bit_to_linear_offset(logical_address),
                value);
        };
    write_parameter_word(parameter_base, 0u);
    write_parameter_word(
        parameter_base + 4u,
        (1u << 29u) | (2u << 22u) | (1u << 20u));
    write_parameter_word(parameter_base + 8u, 0u);
    const auto write_vertex = [&](const std::uint32_t address) {
        write_parameter_word(
            address, std::bit_cast<std::uint32_t>(0.0f));
        write_parameter_word(
            address + 4u, std::bit_cast<std::uint32_t>(0.0f));
        write_parameter_word(address + 8u, 0u);
        write_parameter_word(address + 12u, 0xFF204060u);
    };
    write_vertex(parameter_base + 12u);
    write_vertex(parameter_base + 28u);
    write_vertex(parameter_base + 44u);
    registers.set_render_observer([&] { renderer.render({}, registers, vram); });
    registers.set_vblank_observer([&](const bool entering) {
        if (entering) renderer.observe_vblank_scanout(registers, vram.bytes());
    });
    registers.write(pvr_register::StartRender, 1u);
    static_cast<void>(scheduler.advance_to(20u, 8u));
    FakeVideoOutput scheduler_video;
    const auto early_pump = pump_guest_frame_proof(renderer, &scheduler_video);
    require(!early_pump.guest_frame_proven && early_pump.frame_presented &&
                !early_pump.proven_frame_presented &&
                !early_pump.proof_source.has_value() && early_pump.render_generation == 0u &&
                early_pump.write_generation_first == 0u &&
                early_pump.write_generation_last == 0u &&
                early_pump.presented_frame.has_value() &&
                !early_pump.presented_frame_evidence.valid &&
                !early_pump.presented_frame_evidence_collected &&
                early_pump.presented_frame_evidence.digest == 0u &&
                early_pump.presented_changed_pixels == 0u &&
                scheduler_video.presented_frames() == 1u,
            "Gueltiger VBlank-Scanout ohne Renderbeweis erreicht das Hostvideo "
            "nicht oder erzeugt unnoetig detaillierte Frame-Evidenz.");
    static_cast<void>(scheduler.advance_to(110u, 16u));
    const auto vblank_pump = pump_guest_frame_proof(renderer, &scheduler_video);
    require(vblank_pump.guest_frame_proven && vblank_pump.frame_presented &&
                vblank_pump.proven_frame_presented &&
                vblank_pump.proof_source == PvrGuestFrameProofSource::TaRender &&
                vblank_pump.render_generation != 0u &&
                vblank_pump.write_generation_first == 0u &&
                vblank_pump.write_generation_last == 0u &&
                vblank_pump.presented_frame.has_value() &&
                vblank_pump.presented_frame_evidence.valid &&
                vblank_pump.presented_frame_evidence_collected &&
                vblank_pump.presented_frame_evidence.digest != 0u &&
                vblank_pump.presented_changed_pixels != 0u &&
                scheduler_video.presented_frames() == 2u &&
                scheduler_video.last_frame().width == 1u &&
                scheduler_video.last_frame().height == 1u &&
                scheduler_video.last_frame().rgba ==
                    std::vector<std::uint8_t>({0x20u, 0x40u, 0x60u, 0xFFu}),
            "Scheduler-VBlank, Proof-Pump und Fake-Video bilden keinen ausführbaren Markerpfad.");
    renderer.observe_vblank_scanout(registers, vram.bytes());
    const auto stable_scanout_pump =
        pump_guest_frame_proof(renderer, &scheduler_video);
    require(!stable_scanout_pump.guest_frame_proven &&
                stable_scanout_pump.frame_presented &&
                !stable_scanout_pump.proven_frame_presented &&
                !stable_scanout_pump.proof_source.has_value() &&
                stable_scanout_pump.presented_frame.has_value() &&
                stable_scanout_pump.presented_changed_pixels == 0u &&
                scheduler_video.presented_frames() == 3u &&
                scheduler_video.last_frame().rgba ==
                    std::vector<std::uint8_t>({0x20u, 0x40u, 0x60u, 0xFFu}),
            "Unveraenderter aktiver PVR-Scanout wird ohne neuen Diagnosebeweis nicht "
            "an das Hostvideo ausgegeben.");
    write_parameter_word(parameter_base + 24u, 0xFF806040u);
    write_parameter_word(parameter_base + 40u, 0xFF806040u);
    write_parameter_word(parameter_base + 56u, 0xFF806040u);
    static_cast<void>(renderer.render({}, registers, vram));
    renderer.observe_vblank_scanout(registers, vram.bytes());
    renderer.observe_vblank_scanout(registers, vram.bytes());
    const auto mismatched_scanout_pump =
        pump_guest_frame_proof(renderer, &scheduler_video);
    require(mismatched_scanout_pump.guest_frame_proven &&
                mismatched_scanout_pump.frame_presented &&
                !mismatched_scanout_pump.proven_frame_presented &&
                mismatched_scanout_pump.proof_source ==
                    PvrGuestFrameProofSource::TaRender &&
                mismatched_scanout_pump.presented_source ==
                    GuestFramePresentedSource::Scanout &&
                !mismatched_scanout_pump.presented_proof_source.has_value() &&
                mismatched_scanout_pump.presented_frame.has_value() &&
                !mismatched_scanout_pump.presented_frame_evidence_collected &&
                mismatched_scanout_pump.presented_changed_pixels == 0u &&
                scheduler_video.presented_frames() == 4u,
            "Ein neuerer Scanout wird faelschlich als zum aelteren Renderbeweis "
            "gehoerender sichtbarer Frame gemeldet.");
    renderer.observe_vblank_scanout(registers, vram.bytes());
    renderer.reset_guest_frame_evidence(vram.bytes());
    const auto reset_pump = pump_guest_frame_proof(renderer, &scheduler_video);
    require(!reset_pump.guest_frame_proven && !reset_pump.frame_presented &&
                !reset_pump.proven_frame_presented &&
                scheduler_video.presented_frames() == 4u,
            "PVR-Evidenzreset laesst einen veralteten Scanout im Hostqueue zurueck.");
    registers.write(pvr_register::BorderColor, 0x00123456u);
    registers.write(pvr_register::VideoControl,
                    registers.read(pvr_register::VideoControl) | 0x8u);
    renderer.observe_vblank_scanout(registers, vram.bytes());
    const auto blanked_scanout_pump =
        pump_guest_frame_proof(renderer, &scheduler_video);
    require(!blanked_scanout_pump.guest_frame_proven &&
                blanked_scanout_pump.frame_presented &&
                !blanked_scanout_pump.proven_frame_presented &&
                scheduler_video.presented_frames() == 5u &&
                scheduler_video.last_frame().rgba ==
                    std::vector<std::uint8_t>({0x12u, 0x34u, 0x56u, 0xFFu}),
            "PVR-Blanking gibt den Borderframe nicht ohne falschen Gastframenachweis aus.");
    GuestFrameEvidenceTracker evidence;
    GuestFramePumpResult bootstrap_frame;
    bootstrap_frame.guest_frame_proven = true;
    bootstrap_frame.frame_presented = true;
    bootstrap_frame.proof_source = PvrGuestFrameProofSource::DirectFramebuffer;
    bootstrap_frame.render_generation = 3u;
    bootstrap_frame.write_generation_first = 1u;
    bootstrap_frame.write_generation_last = 3u;
    const auto bootstrap_observation = evidence.observe(bootstrap_frame, false);
    require(has_guest_frame_evidence_marker(
                bootstrap_observation.markers, GuestFrameEvidenceMarker::FirstGuestScanout) &&
                !has_guest_frame_evidence_marker(
                    bootstrap_observation.markers, GuestFrameEvidenceMarker::FirstTaFrame) &&
                !has_guest_frame_evidence_marker(
                    bootstrap_observation.markers,
                    GuestFrameEvidenceMarker::FirstPostBootstrapTaFrame) &&
                bootstrap_observation.write_generation_first == 1u &&
                bootstrap_observation.write_generation_last == 3u &&
                evidence.bootstrap_scanout_seen(),
            "Direct-FB-Bootstrapscanout wird als TA- oder Gameplayframe fehlklassifiziert.");
    GuestFramePumpResult first_ta_frame;
    first_ta_frame.guest_frame_proven = true;
    first_ta_frame.frame_presented = true;
    first_ta_frame.proof_source = PvrGuestFrameProofSource::TaRender;
    first_ta_frame.render_generation = 4u;
    const auto first_ta_observation = evidence.observe(first_ta_frame, true);
    require(has_guest_frame_evidence_marker(
                first_ta_observation.markers, GuestFrameEvidenceMarker::FirstTaFrame) &&
                has_guest_frame_evidence_marker(
                    first_ta_observation.markers,
                    GuestFrameEvidenceMarker::FirstPostBootstrapTaFrame) &&
                evidence.post_bootstrap_ta_frame_seen(),
            "TA-Frame nach Bootstrapscanout und Gastprogrammfortschritt wird nicht als "
            "Post-Bootstrap-TA-Frame klassifiziert.");

    GuestFrameEvidenceTracker conservative_evidence;
    const auto first_progressed_ta = conservative_evidence.observe(first_ta_frame, true);
    require(has_guest_frame_evidence_marker(
                first_progressed_ta.markers, GuestFrameEvidenceMarker::FirstGuestScanout) &&
                has_guest_frame_evidence_marker(
                    first_progressed_ta.markers, GuestFrameEvidenceMarker::FirstTaFrame) &&
                !has_guest_frame_evidence_marker(
                    first_progressed_ta.markers,
                    GuestFrameEvidenceMarker::FirstPostBootstrapTaFrame),
            "Erster TA-Scanout nach bereits beobachtetem Fortschritt reicht allein als "
            "Post-Bootstrap-Nachweis.");
    auto second_ta_frame = first_ta_frame;
    second_ta_frame.render_generation = 5u;
    const auto second_progressed_ta = conservative_evidence.observe(second_ta_frame, true);
    require(has_guest_frame_evidence_marker(
                second_progressed_ta.markers,
                GuestFrameEvidenceMarker::FirstPostBootstrapTaFrame),
            "Zweiter TA-Scanout nach Gastprogrammfortschritt liefert keinen "
            "Post-Bootstrap-TA-Nachweis.");

    constexpr std::uint32_t evidence_width = 80u;
    constexpr std::uint32_t evidence_height = 60u;
    const auto black_baseline =
        solid_frame(evidence_width, evidence_height, {0u, 0u, 0u});
    const auto white_frame =
        solid_frame(evidence_width, evidence_height, {0xFFu, 0xFFu, 0xFFu});
    const auto black_evidence =
        describe_guest_frame_presentation(black_baseline);
    const auto white_evidence =
        describe_guest_frame_presentation(white_frame);
    require(black_evidence.valid && white_evidence.valid &&
                black_evidence.digest != white_evidence.digest &&
                !has_relevant_guest_frame_content(black_evidence) &&
                !has_relevant_guest_frame_content(white_evidence) &&
                minimum_visible_guest_frame_changed_pixels(640u * 480u) == 1536u &&
                minimum_visible_guest_frame_changed_pixels(320u * 240u) == 1024u &&
                minimum_visible_guest_frame_changed_pixels(100u) == 100u,
            "Framebeschreibung oder skalierte Mindestveraenderung ist inkonsistent.");

    GuestFrameVisibilityClassifier uniform_classifier;
    uniform_classifier.begin_product_interval(black_baseline);
    const auto uniform_observation =
        uniform_classifier.observe(white_frame, white_evidence, white_evidence.pixel_count);
    require(uniform_observation.baseline_available &&
                uniform_observation.geometry_matches &&
                uniform_observation.digest_changed &&
                uniform_observation.changed_pixels == white_evidence.pixel_count &&
                uniform_observation.changed_tiles ==
                    guest_frame_visibility_tile_count &&
                !uniform_observation.visible_progress,
            "Uniformer Weissframe wird als sichtbarer Spielfortschritt akzeptiert.");

    auto two_color_frame = black_baseline;
    for (std::uint32_t y = 0u; y < evidence_height; ++y) {
        for (std::uint32_t x = evidence_width / 2u; x < evidence_width; ++x) {
            const auto offset =
                (static_cast<std::size_t>(y) * evidence_width + x) * 4u;
            two_color_frame.rgba[offset] = 0xFFu;
            two_color_frame.rgba[offset + 1u] = 0xFFu;
            two_color_frame.rgba[offset + 2u] = 0xFFu;
        }
    }
    const auto two_color_evidence =
        describe_guest_frame_presentation(two_color_frame);
    GuestFrameVisibilityClassifier two_color_classifier;
    two_color_classifier.begin_product_interval(black_baseline);
    const auto two_color_observation = two_color_classifier.observe(
        two_color_frame, two_color_evidence, two_color_evidence.pixel_count);
    require(two_color_observation.changed_pixels >=
                    two_color_observation.required_changed_pixels &&
                two_color_observation.changed_tiles >=
                    guest_frame_minimum_changed_tiles &&
                two_color_observation.relevant_color_classes == 2u &&
                two_color_observation.relevant_luminance_classes == 2u &&
                two_color_observation.changed_color_classes == 1u &&
                two_color_observation.changed_luminance_classes == 1u &&
                !two_color_observation.visible_progress,
            "Zweifarbiger Schwarz-Weiss-Frame umgeht die Inhaltsklassifikation.");

    auto sparse_three_color_clear = white_frame;
    for (std::uint32_t index = 0u; index < 80u; ++index) {
        const auto x = 10u + index % 40u;
        const auto y = 10u + index / 40u;
        const auto offset =
            (static_cast<std::size_t>(y) * evidence_width + x) * 4u;
        sparse_three_color_clear.rgba[offset] =
            index < 40u ? 0xFFu : 0u;
        sparse_three_color_clear.rgba[offset + 1u] =
            index < 40u ? 0u : 0xFFu;
        sparse_three_color_clear.rgba[offset + 2u] = 0u;
    }
    const auto sparse_three_color_evidence =
        describe_guest_frame_presentation(
            sparse_three_color_clear);
    GuestFrameVisibilityClassifier sparse_three_color_classifier;
    sparse_three_color_classifier.begin_product_interval(
        black_baseline);
    const auto sparse_three_color_observation =
        sparse_three_color_classifier.observe(
            sparse_three_color_clear,
            sparse_three_color_evidence,
            sparse_three_color_evidence.pixel_count);
    require(
        has_relevant_guest_frame_content(
            sparse_three_color_evidence) &&
            sparse_three_color_observation.changed_pixels >=
                sparse_three_color_observation.required_changed_pixels &&
            sparse_three_color_observation.changed_color_classes == 1u &&
            sparse_three_color_observation.changed_luminance_classes == 1u &&
            !sparse_three_color_observation.visible_progress,
        "Fast uniformer Clear-Frame mit kleinen Drei-Farb-Flecken "
        "umgeht die Klassifikation der tatsaechlich geaenderten Pixel.");

    auto border_frame = white_frame;
    constexpr std::uint32_t border_width = 8u;
    for (std::uint32_t y = 0u; y < evidence_height; ++y) {
        for (std::uint32_t x = 0u; x < evidence_width; ++x) {
            if (x >= border_width && x + border_width < evidence_width &&
                y >= border_width && y + border_width < evidence_height)
                continue;
            const auto offset =
                (static_cast<std::size_t>(y) * evidence_width + x) * 4u;
            border_frame.rgba[offset] = 0u;
            border_frame.rgba[offset + 1u] = 0u;
            border_frame.rgba[offset + 2u] = 0u;
        }
    }
    const auto border_evidence =
        describe_guest_frame_presentation(border_frame);
    GuestFrameVisibilityClassifier border_classifier;
    border_classifier.begin_product_interval(black_baseline);
    const auto border_observation = border_classifier.observe(
        border_frame, border_evidence, border_evidence.pixel_count);
    require(border_observation.changed_pixels >=
                    border_observation.required_changed_pixels &&
                border_observation.changed_interior_tiles != 0u &&
                !has_relevant_guest_frame_content(border_evidence) &&
                !border_observation.visible_progress,
            "Schwarzer Rand mit uniformer Innenflaeche wird als Spielbild akzeptiert.");

    const auto first_structured =
        structured_frame(evidence_width, evidence_height);
    const auto second_structured =
        structured_frame(evidence_width, evidence_height, 1u);
    const auto first_structured_evidence =
        describe_guest_frame_presentation(first_structured);
    const auto second_structured_evidence =
        describe_guest_frame_presentation(second_structured);
    require(has_relevant_guest_frame_content(first_structured_evidence) &&
                first_structured_evidence.nonblack_pixels ==
                    first_structured_evidence.pixel_count,
            "Strukturierter heller Frame besitzt keine drei relevanten Inhaltsklassen.");

    GuestFrameVisibilityClassifier missing_baseline_classifier;
    const auto rejected_missing_baseline_without_proof =
        missing_baseline_classifier.observe(
            first_structured, first_structured_evidence, 0u);
    const auto adopted_missing_baseline =
        missing_baseline_classifier.observe(
            first_structured,
            first_structured_evidence,
            first_structured_evidence.pixel_count);
    const auto post_adoption_progress =
        missing_baseline_classifier.observe(
            second_structured,
            second_structured_evidence,
            second_structured_evidence.pixel_count);
    require(!rejected_missing_baseline_without_proof.baseline_available &&
                !rejected_missing_baseline_without_proof.visible_progress &&
                !adopted_missing_baseline.baseline_available &&
                !adopted_missing_baseline.visible_progress &&
                missing_baseline_classifier.baseline_available() &&
                post_adoption_progress.baseline_available &&
                post_adoption_progress.geometry_matches &&
                post_adoption_progress.proof_changed_pixels >=
                    post_adoption_progress.required_changed_pixels &&
                post_adoption_progress.changed_pixels >=
                    post_adoption_progress.required_changed_pixels &&
                post_adoption_progress.changed_tiles >=
                    guest_frame_minimum_changed_tiles &&
                post_adoption_progress.changed_interior_tiles != 0u &&
                post_adoption_progress.visible_progress,
            "Fehlende Entry-Baseline wird nicht fail-closed adoptiert oder ein "
            "spaeterer strukturierter Unterschied bleibt gesperrt.");

    GuestFrameVisibilityClassifier one_pixel_classifier;
    one_pixel_classifier.begin_product_interval(first_structured);
    const auto identical_structured_observation =
        one_pixel_classifier.observe(
            first_structured,
            first_structured_evidence,
            first_structured_evidence.pixel_count);
    auto one_pixel_frame = first_structured;
    one_pixel_frame.rgba[0u] ^= 0x01u;
    const auto one_pixel_evidence =
        describe_guest_frame_presentation(one_pixel_frame);
    const auto one_pixel_observation =
        one_pixel_classifier.observe(one_pixel_frame, one_pixel_evidence, 1u);
    require(one_pixel_observation.relevant_color_classes >= 3u &&
                one_pixel_observation.relevant_luminance_classes >= 3u &&
                !identical_structured_observation.digest_changed &&
                identical_structured_observation.changed_pixels == 0u &&
                one_pixel_observation.digest_changed &&
                one_pixel_observation.changed_pixels == 0u &&
                one_pixel_observation.proof_changed_pixels == 1u &&
                one_pixel_observation.changed_tiles == 0u &&
                !one_pixel_observation.visible_progress,
            "Identischer oder unterhalb der Proof-Mindestmenge liegender "
            "Frame durchlaeuft den teuren Pixelvergleich oder umgeht das Gate.");

    constexpr std::uint32_t skew_width = 640u;
    constexpr std::uint32_t skew_height = 480u;
    const auto skew_baseline =
        solid_frame(skew_width, skew_height, {0u, 0u, 0u});
    auto skew_frame = skew_baseline;
    constexpr std::array<std::array<std::uint8_t, 3u>, 3u>
        skew_colors{{{{0xFFu, 0u, 0u}},
                     {{0u, 0xFFu, 0u}},
                     {{0u, 0u, 0xFFu}}}};
    for (std::uint32_t index = 0u; index < 1533u; ++index) {
        const auto x = 80u + index % 80u;
        const auto y = 80u + index / 80u;
        const auto offset =
            (static_cast<std::size_t>(y) * skew_width + x) * 4u;
        const auto& color = skew_colors[index % skew_colors.size()];
        skew_frame.rgba[offset] = color[0u];
        skew_frame.rgba[offset + 1u] = color[1u];
        skew_frame.rgba[offset + 2u] = color[2u];
    }
    for (const auto x : {240u, 320u, 400u}) {
        const auto offset =
            (static_cast<std::size_t>(80u) * skew_width + x) * 4u;
        skew_frame.rgba[offset] = 0xFFu;
    }
    const auto skew_evidence =
        describe_guest_frame_presentation(skew_frame);
    GuestFrameVisibilityClassifier skew_classifier;
    skew_classifier.begin_product_interval(skew_baseline);
    const auto skew_observation = skew_classifier.observe(
        skew_frame, skew_evidence, 1536u);
    require(
        skew_observation.changed_pixels == 1536u &&
            skew_observation.changed_color_classes >= 3u &&
            skew_observation.changed_luminance_classes >= 3u &&
            skew_observation.changed_tiles == 1u &&
            !skew_observation.visible_progress,
        "Stark lokalisierte Aenderung zaehlt Ein-Pixel-Auslaeufer als "
        "vier ausreichend belegte Bildkacheln.");

    GuestFrameVisibilityClassifier geometry_classifier;
    geometry_classifier.begin_product_interval(first_structured);
    const auto changed_geometry_first = structured_frame(64u, 48u, 1u);
    const auto changed_geometry_second = structured_frame(64u, 48u, 2u);
    const auto changed_geometry_first_evidence =
        describe_guest_frame_presentation(changed_geometry_first);
    const auto changed_geometry_second_evidence =
        describe_guest_frame_presentation(changed_geometry_second);
    const auto geometry_adoption = geometry_classifier.observe(
        changed_geometry_first,
        changed_geometry_first_evidence,
        changed_geometry_first_evidence.pixel_count);
    const auto geometry_post_adoption = geometry_classifier.observe(
        changed_geometry_second,
        changed_geometry_second_evidence,
        changed_geometry_second_evidence.pixel_count);
    require(geometry_adoption.baseline_available &&
                !geometry_adoption.geometry_matches &&
                !geometry_adoption.visible_progress &&
                geometry_post_adoption.geometry_matches &&
                geometry_post_adoption.visible_progress,
            "Geometriewechsel wird nicht einmalig fail-closed rebaselined.");
#ifdef _WIN32
    require(native_video_available(), "Win32-Hostvideo wird nicht als verfuegbar gemeldet.");
    auto video = create_native_video_output(
        {native_video_contract_version, "Katana synthetic frame", 320u, 240u, false});
    require(video->client_width() == 320u && video->client_height() == 240u &&
                (video->backend() == NativeVideoBackend::Win32D3d11Hardware ||
                 video->backend() == NativeVideoBackend::Win32Gdi) &&
                video->hardware_accelerated() ==
                    (video->backend() == NativeVideoBackend::Win32D3d11Hardware) &&
                (video->backend() != NativeVideoBackend::Win32Gdi ||
                 video->backend_fallbacks() != 0u),
            "Win32-Hostvideo verliert Geometrie oder meldet seinen Backendstatus falsch.");
    video->resize(400u, 300u);
    require(video->client_width() == 400u && video->client_height() == 300u,
            "Win32-Hostvideo verarbeitet Resize nicht.");
    // A hidden/occluded D3D11 swap chain may legitimately return
    // DXGI_STATUS_OCCLUDED. Make the counted presentation genuinely visible.
    video->show();
    video->poll_events();
    const PvrFrame generated_public_domain_frame{2u,
                                                 2u,
                                                 {0xFFu,
                                                  0x00u,
                                                  0x00u,
                                                  0xFFu,
                                                  0x00u,
                                                  0xFFu,
                                                  0x00u,
                                                  0xFFu,
                                                  0x00u,
                                                  0x00u,
                                                  0xFFu,
                                                  0xFFu,
                                                  0xFFu,
                                                  0xFFu,
                                                  0xFFu,
                                                  0xFFu}};
    std::uint32_t visible_present_attempts = 0u;
    while (video->presented_frames() == 0u && visible_present_attempts < 100u) {
        ++visible_present_attempts;
        video->poll_events();
        video->present(generated_public_domain_frame);
        video->poll_events();
        if (video->presented_frames() == 0u)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto presentation = video->presentation_telemetry();
    const bool genuinely_presented =
        presentation.submitted_frames == visible_present_attempts &&
        presentation.presented_frames == 1u &&
        video->presented_frames() == 1u &&
        presentation.last_outcome ==
            NativeVideoPresentationOutcome::Presented &&
        !presentation.currently_occluded &&
        (video->backend() != NativeVideoBackend::Win32D3d11Hardware ||
         (presentation.backend_failures == 0u &&
          presentation.not_presentable_frames == 0u &&
          presentation.occluded_frames + presentation.presented_frames ==
              presentation.submitted_frames));
    const bool genuinely_occluded =
        presentation.submitted_frames == visible_present_attempts &&
        presentation.presented_frames == 0u &&
        video->presented_frames() == 0u &&
        presentation.occluded_frames == presentation.submitted_frames &&
        presentation.not_presentable_frames == 0u &&
        presentation.backend_failures == 0u &&
        presentation.last_outcome ==
            NativeVideoPresentationOutcome::Occluded &&
        presentation.currently_occluded &&
        video->backend() == NativeVideoBackend::Win32D3d11Hardware &&
        video->hardware_accelerated() &&
        video->backend_fallbacks() == 0u;
    require((genuinely_presented || genuinely_occluded) &&
                video->hardware_accelerated() ==
                    (video->backend() == NativeVideoBackend::Win32D3d11Hardware) &&
                std::string_view(native_video_backend_name(video->backend())) != "unknown" &&
                (video->backend() != NativeVideoBackend::Win32Gdi ||
                 video->backend_fallbacks() != 0u),
            "Win32-Hostvideo zaehlt Present oder einen D3D11-GDI-Fallback nicht korrekt: "
            "frames=" +
                std::to_string(video->presented_frames()) +
                " backend=" + native_video_backend_name(video->backend()) +
                " hardware=" + std::to_string(video->hardware_accelerated()) +
                " fallbacks=" + std::to_string(video->backend_fallbacks()) +
                " attempts=" + std::to_string(visible_present_attempts) +
                " submitted=" +
                std::to_string(presentation.submitted_frames) +
                " presented=" +
                std::to_string(presentation.presented_frames) +
                " occluded=" +
                std::to_string(presentation.occluded_frames) +
                " not_presentable=" +
                std::to_string(presentation.not_presentable_frames) +
                " backend_failures=" +
                std::to_string(presentation.backend_failures) +
                " outcome=" +
                native_video_presentation_outcome_name(
                    presentation.last_outcome) +
                " currently_occluded=" +
                std::to_string(presentation.currently_occluded) +
                " client=" + std::to_string(video->client_width()) + "x" +
                std::to_string(video->client_height()) + ".");
    require(throws<std::invalid_argument>([&] { video->present({2u, 2u, {0u}}); }),
            "Win32-Hostvideo akzeptiert einen abgeschnittenen RGBA-Frame.");
    video->request_close();
    require(video->close_requested(), "Kontrollierte Close-Anforderung geht verloren.");
    const auto events = video->drain_events();
    require(!events.empty() && events.back().kind == NativeHostEventKind::Close,
            "Kontrolliertes Schliessen erzeugt kein explizites Hostereignis.");
    require(throws<std::invalid_argument>([] {
                static_cast<void>(create_native_video_output({999u, "invalid", 1u, 1u, false}));
            }),
            "Unbekannte native Videovertragsversion wird akzeptiert.");
#else
    require(!native_video_available(),
            "Nicht implementiertes Hostvideo wird als verfuegbar gemeldet.");
    require(throws<std::runtime_error>([] { static_cast<void>(create_native_video_output()); }),
            "Nicht implementiertes Hostvideo scheitert nicht explizit.");
#endif
    std::cout << "KR_NATIVE_VIDEO_CONTRACT_READY\n";
}
