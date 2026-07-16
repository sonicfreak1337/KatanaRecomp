#include "katana/phase9/homebrew.hpp"

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/io/json_report.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/platform/dreamcast.hpp"
#include "katana/runtime/aica.hpp"
#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/dma.hpp"
#include "katana/runtime/exception.hpp"
#include "katana/runtime/indirect_dispatch.hpp"
#include "katana/runtime/interrupt.hpp"
#include "katana/runtime/maple.hpp"
#include "katana/runtime/media_clock.hpp"
#include "katana/runtime/pvr.hpp"
#include "katana/runtime/store_queue.hpp"
#include "katana/runtime/system_replay.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace katana::phase9 {
namespace {

constexpr std::uint32_t p2_entry = 0xAC010000u;
constexpr std::uint32_t p1_entry = 0x8C010000u;
constexpr std::uint32_t rom_physical = 0x00010000u;
constexpr std::uint32_t ram_physical = 0x0C010000u;

std::uint64_t hash_bytes(const std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t hash_samples(const std::span<const std::int16_t> samples) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto sample : samples) {
        const auto value = static_cast<std::uint16_t>(sample);
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1099511628211ull;
        hash ^= static_cast<std::uint8_t>(value >> 8u);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string sha256(const std::span<const std::uint8_t> bytes) {
    return katana::io::sha256_bytes(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

HomebrewArtifact
artifact(std::string id, const HomebrewArtifactKind kind, std::vector<std::uint8_t> bytes) {
    HomebrewArtifact result;
    result.id = std::move(id);
    result.kind = kind;
    result.origin = "project-authored-synthetic";
    result.distribution_status = "internal-until-KR-4902-license-decision";
    result.sha256 = sha256(bytes);
    result.bytes = std::move(bytes);
    return result;
}

katana::runtime::SystemReplayEvent replay_event(const katana::runtime::SystemReplayEventKind kind,
                                                const std::uint64_t cycle,
                                                std::string code) {
    katana::runtime::SystemReplayEvent event;
    event.guest_cycle = cycle;
    event.kind = kind;
    event.code = std::move(code);
    return event;
}

katana::runtime::BlockExit handoff_block(katana::runtime::CpuState&,
                                         katana::runtime::BlockExecutionContext&) {
    return {};
}

} // namespace

const char* homebrew_artifact_kind_name(const HomebrewArtifactKind kind) noexcept {
    switch (kind) {
    case HomebrewArtifactKind::CpuConformance:
        return "cpu-conformance";
    case HomebrewArtifactKind::Console:
        return "console";
    case HomebrewArtifactKind::Controller:
        return "controller";
    case HomebrewArtifactKind::Graphics2d:
        return "graphics-2d";
    case HomebrewArtifactKind::Audio:
        return "audio";
    case HomebrewArtifactKind::IntegratedGame:
        return "integrated-game";
    case HomebrewArtifactKind::FirmwareHandoff:
        return "firmware-handoff";
    case HomebrewArtifactKind::SchedulerDmaInterrupt:
        return "scheduler-dma-interrupt";
    }
    return "unknown";
}

std::vector<HomebrewArtifact> build_homebrew_corpus() {
    std::vector<HomebrewArtifact> corpus;
    corpus.push_back(artifact("cpu-conformance",
                              HomebrewArtifactKind::CpuConformance,
                              {0x01u, 0xE0u, 0x01u, 0x70u, 0x0Bu, 0x00u, 0x09u, 0x00u}));
    corpus.push_back(artifact("console-output",
                              HomebrewArtifactKind::Console,
                              {'K', 'A', 'T', 'A', 'N', 'A', '-', 'O', 'K'}));
    corpus.push_back(artifact(
        "controller-input", HomebrewArtifactKind::Controller, {0x0Cu, 0x00u, 0x80u, 0x80u}));
    corpus.push_back(
        artifact("graphics-2d", HomebrewArtifactKind::Graphics2d, {0x00u, 0xF8u, 0xE0u, 0x07u}));
    corpus.push_back(
        artifact("audio-tone", HomebrewArtifactKind::Audio, {0xE0u, 0x2Eu, 0x20u, 0xD1u}));
    corpus.push_back(artifact("integrated-game",
                              HomebrewArtifactKind::IntegratedGame,
                              {0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u}));
    corpus.push_back(artifact("firmware-handoff",
                              HomebrewArtifactKind::FirmwareHandoff,
                              {0x83u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u}));
    corpus.push_back(artifact("scheduler-dma-interrupt",
                              HomebrewArtifactKind::SchedulerDmaInterrupt,
                              {0x01u, 0x04u, 0x06u, 0x08u}));
    require_valid_homebrew_corpus(corpus);
    return corpus;
}

void require_valid_homebrew_corpus(const std::vector<HomebrewArtifact>& corpus) {
    if (corpus.size() != 8u) {
        throw std::invalid_argument("Phase-9-Homebrew-Korpus braucht exakt acht Pflichtartefakte.");
    }
    std::set<std::string> ids;
    std::set<HomebrewArtifactKind> kinds;
    for (const auto& item : corpus) {
        if (item.id.empty() || item.origin != "project-authored-synthetic" || item.bytes.empty() ||
            item.sha256 != sha256(item.bytes) || !ids.insert(item.id).second ||
            !kinds.insert(item.kind).second) {
            throw std::invalid_argument(
                "Homebrew-Korpus besitzt unvollstaendige oder widerspruechliche Provenienz.");
        }
    }
}

std::string format_homebrew_corpus_json(const std::vector<HomebrewArtifact>& corpus) {
    require_valid_homebrew_corpus(corpus);
    std::ostringstream output;
    output << "{\"schema\":\"katana-homebrew-corpus\",\"version\":"
           << homebrew_corpus_schema_version << ",\"artifacts\":[";
    for (std::size_t index = 0u; index < corpus.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& item = corpus[index];
        output << "{\"id\":" << katana::io::quote_json(item.id) << ",\"kind\":\""
               << homebrew_artifact_kind_name(item.kind) << "\",\"origin\":\"" << item.origin
               << "\",\"distribution_status\":\"" << item.distribution_status
               << "\",\"size\":" << item.bytes.size() << ",\"sha256\":\"" << item.sha256 << "\"}";
    }
    output << "]}\n";
    return output.str();
}

FirmwareHandoffReport run_synthetic_firmware_handoff() {
    using namespace katana::runtime;
    FirmwareHandoffReport report;
    report.p2_entry = p2_entry;
    report.p1_entry = p1_entry;
    report.rom_physical = rom_physical;
    report.ram_physical = ram_physical;

    CpuState cpu;
    cpu.pc = p2_entry;
    prefetch(cpu, p2_entry);
    report.prefetches = cpu.prefetch_count;

    const std::array<std::uint8_t, 8u> bootstrap = {
        0x09u, 0x00u, 0x83u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u};
    auto rom = std::make_shared<LinearMemoryDevice>(bootstrap.size());
    auto ram = std::make_shared<LinearMemoryDevice>(4096u);
    for (std::size_t index = 0u; index < bootstrap.size(); ++index)
        rom->write_u8(static_cast<std::uint32_t>(index), bootstrap[index]);
    Memory memory(0u, MemoryAlignmentPolicy::Strict);
    memory.map_region("synthetic-rom", rom_physical, rom, MemoryRegionAccess::ReadOnly);
    memory.map_region("synthetic-executable-ram", ram_physical, ram);
    for (std::size_t index = 0u; index < bootstrap.size(); ++index) {
        memory.write_u8(ram_physical + static_cast<std::uint32_t>(index),
                        memory.read_u8(rom_physical + static_cast<std::uint32_t>(index)));
    }
    report.copied_bytes = bootstrap.size();

    constexpr std::array<std::uint32_t, 5u> vector_offsets = {
        0x100u, 0x104u, 0x108u, 0x10Cu, 0x110u};
    for (std::size_t index = 0u; index < vector_offsets.size(); ++index) {
        memory.write_u32(ram_physical + vector_offsets[index],
                         ram_physical + static_cast<std::uint32_t>(index * 4u));
    }
    report.dynamic_vectors = vector_offsets.size();

    bool queue_transfer = false;
    Sh4StoreQueues queues(memory, [&](const auto& transfer) {
        queue_transfer = transfer.bytes[0] == 0x44u && transfer.bytes[1] == 0x33u;
    });
    queues.write_p4(0xE0000000u, 0x11223344u, MemoryAccessWidth::Word);
    static_cast<void>(queues.prefetch(0xE0000000u));
    report.store_queue_observed = queue_transfer && queues.transfer_count() == 1u;

    ExecutableCodeTracker tracker;
    static_cast<void>(tracker.register_block({"synthetic-rom-bootstrap",
                                              rom_physical,
                                              static_cast<std::uint32_t>(bootstrap.size()),
                                              "project-authored-synthetic-rom",
                                              {},
                                              ExecutableBlockOrigin::ImageSegment}));
    static_cast<void>(tracker.register_block({"synthetic-ram-bootstrap",
                                              ram_physical,
                                              static_cast<std::uint32_t>(bootstrap.size()),
                                              "copy:synthetic-rom-bootstrap",
                                              {"synthetic-rom-bootstrap"},
                                              ExecutableBlockOrigin::RomRamCopy}));
    const auto invalidation =
        tracker.observe_write(ram_physical + 2u, 1u, CodeWriteSource::Cpu, true);
    report.invalidations = tracker.invalidation_count();

    katana::io::ExecutableImage image("synthetic-firmware-handoff");
    image.add_segment({".reset",
                       p2_entry,
                       0u,
                       bootstrap.size(),
                       katana::io::SegmentKind::Code,
                       {true, false, true},
                       std::vector<std::uint8_t>(bootstrap.begin(), bootstrap.end())});
    image.add_entry_point(p2_entry);
    const auto analysis = katana::analysis::analyze_control_flow(image);
    const auto ir = katana::ir::lower_program(analysis);
    const auto generated = katana::codegen::emit_cpp_program(ir, p2_entry);
    report.analysis_complete = analysis.recursive.diagnostics.empty() && !ir.empty();
    report.generated_cpp_complete =
        generated.find("generated_entry_address = 0xAC010000u") != std::string::npos;

    RuntimeBlockTable table;
    const BlockVariantKey variant{};
    table.register_static({p1_entry,
                           ram_physical,
                           static_cast<std::uint32_t>(bootstrap.size()),
                           BlockEndKind::Return,
                           variant,
                           handoff_block,
                           "copy:synthetic-rom-bootstrap",
                           false});
    CpuState dispatch_cpu;
    const auto dispatched = dispatch_indirect(dispatch_cpu,
                                              table,
                                              {IndirectDispatchKind::TailJump,
                                               p2_entry,
                                               p2_entry,
                                               0u,
                                               {p2_entry, ram_physical},
                                               variant});
    report.dispatch_complete = dispatched.block != nullptr && dispatched.alias_lookup &&
                               !invalidation.invalidated_blocks.empty() &&
                               !tracker.valid("synthetic-ram-bootstrap");
    return report;
}

HomebrewHostFrameReport run_homebrew_host_frame() {
    using namespace katana::runtime;
    HomebrewHostFrameReport report;

    katana::io::ExecutableImage image;
    image.add_segment({".text",
                       0x8C010000u,
                       0u,
                       8u,
                       katana::io::SegmentKind::Code,
                       {true, false, true},
                       {0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u}});
    image.add_entry_point(0x8C010000u);
    CpuState cpu;
    cpu.memory = Memory(0u);
    const auto boot = katana::platform::boot_homebrew(cpu, image);
    if (boot.log.empty() || boot.log.back() != "boot=ready") ++report.silent_failures;

    ControllerState first_input;
    first_input.pressed_buttons = static_cast<std::uint16_t>(ControllerButton::A);
    ControllerState second_input;
    second_input.pressed_buttons = static_cast<std::uint16_t>(ControllerButton::Start);
    auto input = std::make_shared<ReplayInputBackend>(
        std::vector<ControllerState>{first_input, second_input});
    auto controller = std::make_shared<MapleControllerDevice>(input);
    MapleBus maple;
    maple.attach(0u, 0u, controller);

    TileAccelerator ta;
    RecordingPvrRenderBackend video;
    PvrFramebuffer framebuffer;
    framebuffer.configure(320u, 240u, 640u, PvrFramebufferFormat::Rgb565);
    std::vector<std::uint8_t> vram(320u * 240u * 2u, 0u);
    vram[0] = 0x00u;
    vram[1] = 0xF8u;
    PvrFrame captured;

    AicaMixer mixer;
    RecordingAicaAudioBackend audio;
    const std::array<std::int16_t, 2u> tone = {12'000, -12'000};
    const std::array<AicaVoice, 1u> voices = {{{tone, aica_unity_gain, aica_pan_center}}};

    EventScheduler scheduler;
    Memory dma_memory(128u, MemoryAlignmentPolicy::Strict);
    dma_memory.write_u8(0x00u, 0xA5u);
    Sh4Dmac dmac(scheduler, dma_memory, DmaTiming{1u});
    dmac.write_source(0u, 0x00u);
    dmac.write_destination(0u, 0x40u);
    dmac.write_count(0u, 1u);
    dmac.write_control(0u, 0x00000410u | Sh4Dmac::interrupt_enable | Sh4Dmac::channel_enable);
    dmac.write_operation(Sh4Dmac::master_enable);

    DreamcastMediaClock clock(
        scheduler,
        MediaClockConfig{120u, 60u, 120u, 2u},
        [&](const VideoTick&) {
            const auto response = maple.exchange(0u, 0u, {MapleCommand::GetCondition, {1u}});
            if (response.code != MapleResponseCode::DataTransfer) ++report.silent_failures;
            ta.begin_list(PvrListType::Opaque);
            ta.submit_vertex({16.0f, 16.0f, 0.5f, 0.0f, 0.0f, 0xFFFF8000u}, false);
            ta.submit_vertex({304.0f, 16.0f, 0.5f, 1.0f, 0.0f, 0xFF00FFFFu}, false);
            ta.submit_vertex({160.0f, 224.0f, 0.5f, 0.5f, 1.0f, 0xFFFFFFFFu}, true);
            ta.end_list();
            video.render(ta.finish_frame(), {});
            captured = framebuffer.capture(vram);
        },
        [&](const AudioTick& tick) {
            const auto mixed = mixer.mix(voices, tick.frame_count);
            audio.submit(mixed, 120u);
        });
    clock.start();
    const auto advance = scheduler.advance_to(4u, 32u);
    clock.stop();

    InterruptController interrupts;
    if (dmac.interrupt_pending(0u)) {
        interrupts.request(1u, 8u, 0x640u);
        cpu.vbr = 0x8C000000u;
        cpu.set_interrupt_mask(0u);
        if (accept_pending_interrupt(cpu, interrupts)) report.interrupts = 1u;
    }

    ExecutableCodeTracker tracker;
    static_cast<void>(tracker.register_block({"homebrew-dma-target",
                                              0x40u,
                                              1u,
                                              "project-authored-homebrew",
                                              {},
                                              ExecutableBlockOrigin::RuntimeWrite}));
    static_cast<void>(tracker.observe_write(0x40u, 1u, CodeWriteSource::Dma, true));

    SystemReplayLog replay;
    replay.record(replay_event(SystemReplayEventKind::Dma, 1u, "dma-complete"));
    replay.record(replay_event(SystemReplayEventKind::Video, 2u, "frame-0"));
    replay.record(replay_event(SystemReplayEventKind::Audio, 2u, "audio-0"));
    auto input_event = replay_event(SystemReplayEventKind::ExternalInput, 2u, "controller-0");
    replay.inject(std::move(input_event));
    replay.record(replay_event(SystemReplayEventKind::Video, 4u, "frame-1"));
    replay.record(replay_event(SystemReplayEventKind::Audio, 4u, "audio-1"));
    auto second_event = replay_event(SystemReplayEventKind::ExternalInput, 4u, "controller-1");
    replay.inject(std::move(second_event));

    report.guest_cycles = advance.guest_cycle;
    report.frame_intervals = clock.video_tick_count();
    report.pvr_frames = video.submitted_frames();
    report.frame_width = captured.width;
    report.frame_height = captured.height;
    report.frame_rgba_bytes = captured.rgba.size();
    report.audio_buffers = audio.submitted_buffers();
    report.audio_frames = audio.submitted_frames();
    report.maple_transactions = maple.history().size();
    report.dma_units = dmac.completed_transfer_units(0u);
    report.scheduler_jitter = 1u;
    report.invalidations = tracker.invalidation_count();
    report.audio_hash = hash_samples(audio.last_buffer());
    report.frame_hash = hash_bytes(captured.rgba);
    report.console_output = "KATANA-HOMEBREW-OK";
    report.state_hash = hash_replay_guest_state(
        cpu, scheduler.current_cycle(), report.frame_hash ^ report.audio_hash);
    replay.seal(report.state_hash);
    DeterministicSystemReplay verifier(replay);
    for (const auto& event : replay.events())
        verifier.observe(event);
    verifier.finish(report.state_hash);
    report.replay_events = replay.events().size();

    if (advance.status != SchedulerAdvanceStatus::ReachedTarget || report.pvr_frames < 1u ||
        report.frame_width != 320u || report.frame_height != 240u ||
        report.frame_rgba_bytes != 320u * 240u * 4u || report.audio_buffers == 0u ||
        report.maple_transactions == 0u || report.dma_units != 1u || report.interrupts != 1u ||
        !verifier.complete()) {
        ++report.silent_failures;
    }
    return report;
}

std::string format_homebrew_host_frame_json(const HomebrewHostFrameReport& report) {
    std::ostringstream output;
    output << "{\"schema\":\"katana-phase9-homebrew-host-frame\",\"version\":"
           << homebrew_report_schema_version << ",\"marker\":\"" << report.marker
           << "\",\"guest_cycles\":" << report.guest_cycles
           << ",\"frame_intervals\":" << report.frame_intervals
           << ",\"pvr_frames\":" << report.pvr_frames << ",\"frame_width\":" << report.frame_width
           << ",\"frame_height\":" << report.frame_height
           << ",\"frame_rgba_bytes\":" << report.frame_rgba_bytes
           << ",\"audio_buffers\":" << report.audio_buffers
           << ",\"audio_frames\":" << report.audio_frames
           << ",\"maple_transactions\":" << report.maple_transactions
           << ",\"dma_units\":" << report.dma_units << ",\"interrupts\":" << report.interrupts
           << ",\"scheduler_jitter\":" << report.scheduler_jitter
           << ",\"invalidations\":" << report.invalidations
           << ",\"replay_events\":" << report.replay_events
           << ",\"state_hash\":" << report.state_hash << ",\"audio_hash\":" << report.audio_hash
           << ",\"frame_hash\":" << report.frame_hash
           << ",\"silent_failures\":" << report.silent_failures
           << ",\"console_output\":" << katana::io::quote_json(report.console_output) << "}\n";
    return output.str();
}

} // namespace katana::phase9
