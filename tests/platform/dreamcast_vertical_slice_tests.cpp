#include "katana/platform/dreamcast.hpp"
#include "katana/runtime/aica.hpp"
#include "katana/runtime/maple.hpp"
#include "katana/runtime/media_clock.hpp"
#include "katana/runtime/pvr.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    using namespace katana;
    using namespace katana::runtime;

    io::ExecutableImage image;
    io::ImageSegment text;
    text.name = ".text";
    text.virtual_address = 0x8C010000u;
    text.memory_size = 8u;
    text.bytes = {0x09u, 0x00u, 0x0Bu, 0x00u}; // NOP; RTS
    text.permissions = {true, false, true};
    image.add_segment(std::move(text));
    image.add_entry_point(0x8C010000u);

    CpuState cpu;
    cpu.memory = Memory(0u);
    const auto boot = platform::boot_homebrew(cpu, image);

    ControllerState input_frame;
    input_frame.pressed_buttons = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(ControllerButton::A) |
        static_cast<std::uint16_t>(ControllerButton::Start)
    );
    auto input = std::make_shared<ReplayInputBackend>(
        std::vector<ControllerState>{input_frame}
    );
    auto controller = std::make_shared<MapleControllerDevice>(input);
    MapleBus maple;
    maple.attach(0u, 0u, controller);

    TileAccelerator ta;
    RecordingPvrRenderBackend video;
    AicaMixer mixer;
    RecordingAicaAudioBackend audio;
    const std::array<std::int16_t, 1u> tone = {12'000};
    const std::array<AicaVoice, 1u> voices = {{
        {tone, aica_unity_gain, aica_pan_center}
    }};
    bool input_observed = false;
    bool boot_observed = false;

    EventScheduler scheduler;
    DreamcastMediaClock clock(
        scheduler,
        MediaClockConfig{60u, 60u, 60u, 1u},
        [&](const VideoTick&) {
            boot_observed = cpu.pc == boot.entry_point &&
                cpu.memory.read_u16(boot.entry_point) == 0x0009u;
            const auto response = maple.exchange(
                0u,
                0u,
                {MapleCommand::GetCondition, {0x01000000u}}
            );
            const auto pressed = static_cast<std::uint16_t>(response.payload.at(1u));
            input_observed = response.code == MapleResponseCode::DataTransfer &&
                (pressed & static_cast<std::uint16_t>(ControllerButton::A)) == 0u &&
                (pressed & static_cast<std::uint16_t>(ControllerButton::Start)) == 0u;

            ta.begin_list(PvrListType::Opaque);
            ta.submit_vertex({32.0f, 32.0f, 0.5f, 0.0f, 0.0f, 0xFFFF0000u}, false);
            ta.submit_vertex({288.0f, 32.0f, 0.5f, 1.0f, 0.0f, 0xFF00FF00u}, false);
            ta.submit_vertex({160.0f, 224.0f, 0.5f, 0.5f, 1.0f, 0xFF0000FFu}, true);
            ta.end_list();
            video.render(ta.finish_frame(), {});
        },
        [&](const AudioTick& tick) {
            const auto mixed = mixer.mix(voices, tick.frame_count);
            audio.submit(mixed, 60u);
        }
    );

    clock.start();
    const auto run = scheduler.advance_to(1u, 2u);
    clock.stop();

    require(
        boot_observed && boot.log.back() == "boot=ready",
        "Synthetisches Homebrew erreicht den gemeinsamen Plattformlauf nicht."
    );
    require(
        input_observed && controller->sampled_frames() == 1u && maple.history().size() == 1u,
        "Der gemeinsame Lauf nimmt den deterministischen Controllerzustand nicht an."
    );
    require(
        video.submitted_frames() == 1u && video.last_frame().primitives.size() == 1u &&
            video.last_frame().primitives.front().vertices.size() == 3u,
        "Der gemeinsame Lauf erzeugt kein sichtbares synthetisches Bildprimitiv."
    );
    require(
        audio.submitted_buffers() == 1u && audio.submitted_frames() == 1u &&
            audio.last_buffer() == std::vector<std::int16_t>({12'000, 12'000}),
        "Der gemeinsame Lauf erzeugt kein deterministisches, nicht-stummes Audioframe."
    );
    require(
        run.status == SchedulerAdvanceStatus::ReachedTarget && run.processed_events == 2u &&
            run.guest_cycle == 1u && scheduler.pending_event_count() == 0u,
        "Der Vertical Slice laeuft nicht als ein begrenzter deterministischer Scheduler-Schritt."
    );

    std::cout << "Frei verteilbarer Phase-6-Homebrew-Vertical-Slice erfolgreich.\n";
    return 0;
}
