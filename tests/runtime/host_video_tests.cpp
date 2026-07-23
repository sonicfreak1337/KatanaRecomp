#include "katana/runtime/host_video.hpp"

#include <bit>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

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

  private:
    katana::runtime::PvrFrame last_frame_;
    std::uint64_t presented_frames_ = 0u;
    std::uint32_t width_ = 1u;
    std::uint32_t height_ = 1u;
    bool acknowledge_present_ = true;
    bool close_requested_ = false;
};
} // namespace

int main() {
    using namespace katana::runtime;
    static_assert(native_video_contract_version == 2u);
    PvrGuestFrameProof proof;
    proof.render_generation = 7u;
    proof.changed_pixels = 2u;
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
    vram.write_u32(parameter_base, 0u);
    vram.write_u32(parameter_base + 4u, (1u << 29u) | (2u << 22u) | (1u << 20u));
    vram.write_u32(parameter_base + 8u, 0u);
    const auto write_vertex = [&](const std::uint32_t address) {
        vram.write_u32(address, std::bit_cast<std::uint32_t>(0.0f));
        vram.write_u32(address + 4u, std::bit_cast<std::uint32_t>(0.0f));
        vram.write_u32(address + 8u, 0u);
        vram.write_u32(address + 12u, 0xFF204060u);
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
    require(!early_pump.guest_frame_proven && !early_pump.frame_presented,
            "Render nach einem frueheren VBlank wird rueckwirkend als Hostframe gepumpt.");
    static_cast<void>(scheduler.advance_to(110u, 16u));
    const auto vblank_pump = pump_guest_frame_proof(renderer, &scheduler_video);
    require(vblank_pump.guest_frame_proven && vblank_pump.frame_presented &&
                scheduler_video.presented_frames() == 1u &&
                scheduler_video.last_frame().width == 1u &&
                scheduler_video.last_frame().height == 1u &&
                scheduler_video.last_frame().rgba ==
                    std::vector<std::uint8_t>({0x20u, 0x40u, 0x60u, 0xFFu}),
            "Scheduler-VBlank, Proof-Pump und Fake-Video bilden keinen ausführbaren Markerpfad.");
#ifdef _WIN32
    require(native_video_available(), "Win32-Hostvideo wird nicht als verfuegbar gemeldet.");
    auto video = create_native_video_output(
        {native_video_contract_version, "Katana synthetic frame", 320u, 240u, false});
    require(video->client_width() == 320u && video->client_height() == 240u,
            "Win32-Hostvideo verliert die initiale Clientgeometrie.");
    video->resize(400u, 300u);
    require(video->client_width() == 400u && video->client_height() == 300u,
            "Win32-Hostvideo verarbeitet Resize nicht.");
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
    video->present(generated_public_domain_frame);
    video->poll_events();
    require(video->presented_frames() == 1u, "Win32-Hostvideo zaehlt Present nicht.");
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
