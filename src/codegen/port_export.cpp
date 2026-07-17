#include "katana/codegen/port_export.hpp"

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/graph_export.hpp"
#include "katana/codegen/backend.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/codegen/naming.hpp"
#include "katana/codegen/project.hpp"
#include "katana/codegen/source_map.hpp"
#include "katana/io/input_output_error.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/io/json_report.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/ir/verifier.hpp"
#include "katana/platform/dreamcast_disc.hpp"
#include "katana/runtime/abi.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace katana::codegen {
namespace {

bool valid_target_name(const std::string_view value) noexcept {
    if (value.empty() || !std::isalpha(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    const bool valid = std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-';
    });
    return valid && value != "katana-recomp" && value != "katana_runtime" &&
           value != "katana_core" && value != "katana_generated" && value != "all" &&
           value != "clean" && value != "install" && value != "test" && value != "help" &&
           value != "rebuild_cache";
}

constexpr std::string_view port_namespace = "katana_port_generated";

std::vector<katana::ir::Function>
select_functions(const std::span<const katana::ir::Function> program,
                 const TranslationUnitPartition& partition) {
    std::vector<katana::ir::Function> selected;
    selected.reserve(partition.function_indices.size());
    for (const auto index : partition.function_indices) {
        if (index >= program.size()) {
            throw std::out_of_range("Portpartition verweist auf eine fehlende Funktion.");
        }
        selected.push_back(program[index]);
    }
    std::sort(selected.begin(), selected.end(), [](const auto& left, const auto& right) {
        return left.entry_address < right.entry_address;
    });
    return selected;
}

std::string generated_header(const std::string& entry_namespace) {
    return "#pragma once\n\n"
           "#include \"katana/runtime/platform_services.hpp\"\n"
           "#include \"katana/runtime/runtime.hpp\"\n\n"
           "namespace " +
           entry_namespace +
           " {\n"
           "void run(katana::runtime::CpuState& cpu);\n"
           "struct RuntimeRunResult {\n"
           "    std::uint64_t indirect_dispatches = 0u;\n"
           "    std::uint32_t final_pc = 0u;\n"
           "    std::uint64_t scheduler_cycle = 0u;\n"
           "};\n"
           "RuntimeRunResult run_runtime(katana::runtime::CpuState& cpu,\n"
           "                             katana::runtime::PlatformServices& services);\n"
           "}\n";
}

std::string handwritten_main(const std::string& entry_namespace,
                             const bool hle_bios_abi,
                             const std::span<const katana::io::InputProvenance> inputs,
                             const std::string_view expected_boot_sha256,
                             const std::uint32_t entry_address) {
    std::ostringstream identity_contract;
    identity_contract << "struct ExpectedInput { std::string_view role; std::string_view sha256; };\n"
                      << "constexpr ExpectedInput expected_inputs[]{\n";
    for (const auto& input : inputs) {
        if (input.role != "gdi-descriptor" && !input.role.starts_with("gdi-track-")) continue;
        identity_contract << "    {" << katana::io::quote_json(input.role) << ", "
                          << katana::io::quote_json(input.sha256) << "},\n";
    }
    identity_contract << "};\n"
                      << "constexpr std::string_view expected_boot_sha256 = "
                      << katana::io::quote_json(expected_boot_sha256) << ";\n";
    return "#include \"katana_port.hpp\"\n"
           "#include \"katana/runtime/dreamcast_boot.hpp\"\n"
           "#include \"katana/runtime/host_runtime.hpp\"\n"
           "#include \"katana/runtime/host_video.hpp\"\n"
           "#include \"katana/runtime/scheduler.hpp\"\n"
           "#include \"katana/io/input_provenance.hpp\"\n"
           "#include <algorithm>\n#include <exception>\n#include <filesystem>\n#include "
           "<iostream>\n"
           "#include <optional>\n#include <span>\n#include <string>\n#include <string_view>\n"
           "#include <system_error>\n#include <vector>\n\n"
           "namespace {\n" + identity_contract.str() +
           "void verify_source_identity(const std::filesystem::path& source,\n"
           "                            const katana::runtime::DreamcastRuntimeBootImage& boot) {\n"
           "    std::vector<katana::io::InputProvenance> actual;\n"
           "    actual.push_back(katana::io::capture_input_provenance(\"gdi-descriptor\", source));\n"
           "    for (const auto& track : boot.source->descriptor().tracks)\n"
           "        actual.push_back(katana::io::capture_input_provenance(\n"
           "            \"gdi-track-\" + std::to_string(track.number), track.resolved_path));\n"
           "    if (actual.size() != std::size(expected_inputs))\n"
           "        throw std::runtime_error(\"source-identity-mismatch\");\n"
           "    for (std::size_t index = 0u; index < actual.size(); ++index)\n"
           "        if (actual[index].role != expected_inputs[index].role ||\n"
           "            actual[index].sha256 != expected_inputs[index].sha256)\n"
           "            throw std::runtime_error(\"source-identity-mismatch\");\n"
           "    const std::string_view boot_bytes(\n"
           "        reinterpret_cast<const char*>(boot.boot_file.data()), boot.boot_file.size());\n"
           "    if (katana::io::sha256_bytes(boot_bytes) != expected_boot_sha256)\n"
           "        throw std::runtime_error(\"source-identity-mismatch\");\n"
           "}\n"
           "class PortPlatformServices final : public katana::runtime::PlatformServices {\n"
           "  public:\n"
           "    PortPlatformServices(katana::runtime::CpuState& cpu,\n"
           "                         const katana::runtime::DreamcastRuntimeState& state)\n"
           "        : cpu_(cpu), state_(state) {}\n"
           "    std::string_view name() const noexcept override { return \"dreamcast-port\"; }\n"
           "    std::uint32_t abi_version() const noexcept override {\n"
           "        return katana::runtime::platform_services_abi_version;\n"
           "    }\n"
           "    katana::runtime::PlatformCapabilities capabilities() const noexcept override {\n"
           "        return katana::runtime::core_platform_capabilities;\n"
           "    }\n"
           "    void read_memory(std::uint32_t address, std::span<std::uint8_t> output) override "
           "{\n"
           "        for (auto& byte : output) byte = cpu_.memory.read_u8(address++);\n"
           "    }\n"
           "    void write_memory(std::uint32_t address, std::span<const std::uint8_t> input) "
           "override {\n"
           "        const auto start = address;\n"
           "        for (const auto byte : input) cpu_.memory.write_u8(address++, byte);\n"
           "        static_cast<void>(state_.code_tracker->observe_write(\n"
           "            katana::runtime::canonical_physical_address(start), input.size(),\n"
           "            katana::runtime::CodeWriteSource::Cpu));\n"
           "    }\n"
           "    std::uint64_t scheduler_cycle() const noexcept override {\n"
           "        return state_.scheduler->current_cycle();\n"
           "    }\n"
           "    katana::runtime::PlatformSchedulerResult\n"
           "    advance_scheduler(std::uint64_t cycle, std::size_t budget) override {\n"
           "        const auto target = std::max(cycle, state_.scheduler->current_cycle());\n"
           "        const auto result = state_.scheduler->advance_to(target, budget);\n"
           "        static_cast<void>(state_.interrupt_router->synchronize());\n"
           "        return {result.guest_cycle, result.processed_events,\n"
           "                result.status == "
           "katana::runtime::SchedulerAdvanceStatus::EventBudgetExhausted};\n"
           "    }\n"
           "    std::optional<katana::runtime::PlatformInterruptRequest> poll_interrupt() override "
           "{\n"
           "        if (!state_.interrupt_router->accept(cpu_)) return std::nullopt;\n"
           "        return katana::runtime::PlatformInterruptRequest{0u, 0u, cpu_.intevt};\n"
           "    }\n"
           "    katana::runtime::PlatformDmaResult\n"
           "    start_dma(const katana::runtime::PlatformDmaRequest& request) override {\n"
           "        if (request.length == 0u) return {};\n"
           "        state_.dmac->write_source(0u, request.source);\n"
           "        state_.dmac->write_destination(0u, request.destination);\n"
           "        state_.dmac->write_count(0u, request.length);\n"
           "        state_.dmac->write_control(0u, 0x00000410u |\n"
           "            katana::runtime::Sh4Dmac::interrupt_enable |\n"
           "            katana::runtime::Sh4Dmac::channel_enable);\n"
           "        state_.dmac->write_operation(katana::runtime::Sh4Dmac::master_enable);\n"
           "        state_.dmac->request_transfer(0u);\n"
           "        return {0u, false};\n"
           "    }\n"
           "    katana::runtime::PlatformFallbackResult controlled_fallback(\n"
           "        katana::runtime::CpuState&, const katana::runtime::PlatformFallbackRequest&) "
           "override {\n"
           "        ++fallback_count_;\n"
           "        return {};\n"
           "    }\n"
           "    bool prefetch(katana::runtime::CpuState& cpu, std::uint32_t address) override {\n"
           "        katana::runtime::prefetch(cpu, address);\n"
           "        return state_.store_queues->prefetch(address);\n"
           "    }\n"
           "    void observe_guest_checkpoint(std::uint32_t address) noexcept override {\n"
           "        ++executed_blocks_;\n"
           "        if (address != " + std::to_string(entry_address) +
           "u) guest_checkpoint_ = true;\n"
           "    }\n"
           "    bool guest_checkpoint_reached() const noexcept { return guest_checkpoint_; }\n"
           "    std::uint64_t executed_blocks() const noexcept { return executed_blocks_; }\n"
           "    std::uint64_t fallback_count() const noexcept { return fallback_count_; }\n"
           "    void register_executable_block(std::uint32_t address, std::uint32_t size,\n"
           "                                   std::string_view identity) override {\n"
           "        static_cast<void>(state_.code_tracker->register_block(\n"
           "            {std::string(identity), katana::runtime::canonical_physical_address(address),\n"
           "             size, \"generated-port\", {},\n"
           "             katana::runtime::ExecutableBlockOrigin::ImageSegment}));\n"
           "    }\n"
           "  private:\n"
           "    katana::runtime::CpuState& cpu_;\n"
           "    const katana::runtime::DreamcastRuntimeState& state_;\n"
           "    std::uint64_t executed_blocks_ = 0u;\n"
           "    std::uint64_t fallback_count_ = 0u;\n"
           "    bool guest_checkpoint_ = false;\n"
           "};\n\n"
           "std::string redact_source(std::string message, const std::filesystem::path& source) {\n"
           "    std::error_code path_error;\n"
           "    const auto absolute = std::filesystem::weakly_canonical(source, path_error);\n"
           "    if (path_error || absolute.empty()) return message;\n"
           "    std::vector<std::string> values;\n"
           "    const auto add = [&values](const std::filesystem::path& path) {\n"
           "        const auto value = path.lexically_normal().string();\n"
           "        if (value.size() < 4u || path == path.root_path() || path == \".\" || "
           "path == \"..\") return;\n"
           "        if (std::find(values.begin(), values.end(), value) == values.end()) "
           "values.push_back(value);\n"
           "    };\n"
           "    add(absolute);\n"
           "    add(absolute.parent_path());\n"
           "    std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {\n"
           "        return left.size() > right.size();\n"
           "    });\n"
           "    for (const auto& value : values) {\n"
           "        for (auto offset = message.find(value); offset != std::string::npos;\n"
           "             offset = message.find(value, offset + 12u)) {\n"
           "            message.replace(offset, value.size(), \"<gdi-source>\");\n"
           "        }\n"
           "    }\n"
           "    return message;\n"
           "}\n"
           "} // namespace\n\n"
           "int main(const int argc, const char* const* argv) {\n"
           "    std::filesystem::path source;\n"
           "    try {\n"
           "        if (argc == 2 && !std::string_view(argv[1]).empty() &&\n"
           "            std::string_view(argv[1]).front() != '-') {\n"
           "            source = argv[1];\n"
           "        } else if (argc == 3 && (std::string_view(argv[1]) == \"--gdi\" ||\n"
           "                                      std::string_view(argv[1]) == "
           "\"--run-generated\")) {\n"
           "            source = argv[2];\n"
           "        } else {\n"
           "            std::cerr << \"Aufruf: game <disc.gdi> oder game --gdi <disc.gdi>\\n\";\n"
           "            return 2;\n"
           "        }\n"
           "        katana::runtime::DreamcastRuntimeBootImage boot;\n"
           "        try { boot = katana::runtime::load_dreamcast_runtime_boot(source); }\n"
           "        catch (const std::exception&) { throw std::runtime_error(\"source-identity-mismatch\"); }\n"
           "        verify_source_identity(source, boot);\n"
           "        katana::runtime::CpuState cpu;\n"
           "        const auto state = katana::runtime::initialize_dreamcast_runtime(\n"
           "            cpu, boot, katana::runtime::DreamcastRuntimeFirmwareMode::" +
           std::string(hle_bios_abi ? "HleBiosAbi" : "Direct") +
           ");\n"
           "        auto input = std::make_shared<katana::runtime::InjectedHostInput>();\n"
           "        state.maple->attach(0u, 0u,\n"
           "            std::make_shared<katana::runtime::MapleControllerDevice>(input));\n"
           "        katana::runtime::DreamcastMediaClock media_clock(*state.scheduler, {});\n"
           "        std::unique_ptr<katana::runtime::HostAudioOutput> audio =\n"
           "            katana::runtime::native_audio_available()\n"
           "                ? katana::runtime::create_native_audio_output()\n"
           "                : std::make_unique<katana::runtime::RecordingHostAudioOutput>();\n"
           "        katana::runtime::HostRuntimeSession host(\n"
           "            *state.scheduler, media_clock, input, *audio);\n"
           "        std::uint64_t host_sequence = 1u;\n"
           "        host.inject({host_sequence++, state.scheduler->current_cycle(),\n"
           "                     katana::runtime::HostRuntimeEventKind::Resume, {}});\n"
           "        PortPlatformServices services(cpu, state);\n"
           "        const auto result = " +
           entry_namespace +
           "::run_runtime(cpu, services);\n"
           "        const std::uint64_t silent_failures =\n"
           "            (state.loaded_boot_bytes == 0u ? 1u : 0u) +\n"
           "            (result.scheduler_cycle == 0u ? 1u : 0u) +\n"
           "            (!services.guest_checkpoint_reached() ? 1u : 0u) +\n"
           "            (cpu.trap_pending ? 1u : 0u) +\n"
           "            (cpu.last_exception_cause != katana::runtime::ExceptionCause::None ? 1u : 0u) +\n"
           "            services.fallback_count();\n"
           "        if (silent_failures != 0u) {\n"
           "            throw std::runtime_error(\"Runtime-Einstieg besitzt keinen "
           "Dispatchnachweis.\");\n"
           "        }\n"
           "        katana::runtime::AicaMixer mixer;\n"
           "        audio->submit(mixer.mix({}, 735u), 44'100u);\n"
           "        std::uint64_t presented_frames = 0u;\n"
           "        katana::runtime::ControllerState controller;\n"
           "        if (katana::runtime::native_video_available()) {\n"
           "            auto video = katana::runtime::create_native_video_output(\n"
           "                {katana::runtime::native_video_contract_version,\n"
           "                 \"KatanaRecomp Game\", 640u, 480u, true});\n"
           "            katana::runtime::PvrFramebuffer framebuffer;\n"
           "            framebuffer.configure(640u, 480u, 1280u,\n"
           "                                  katana::runtime::PvrFramebufferFormat::Rgb565);\n"
           "            video->present(framebuffer.capture(state.vram->bytes()));\n"
           "            video->poll_events();\n"
           "            for (const auto& event : video->drain_events()) {\n"
           "                auto kind = katana::runtime::HostRuntimeEventKind::Controller;\n"
           "                if (event.kind == katana::runtime::NativeHostEventKind::FocusGained)\n"
           "                    kind = katana::runtime::HostRuntimeEventKind::FocusGained;\n"
           "                else if (event.kind == "
           "katana::runtime::NativeHostEventKind::FocusLost)\n"
           "                    kind = katana::runtime::HostRuntimeEventKind::FocusLost;\n"
           "                else if (event.kind == katana::runtime::NativeHostEventKind::Close)\n"
           "                    kind = katana::runtime::HostRuntimeEventKind::Shutdown;\n"
           "                else {\n"
           "                    const auto down = event.kind == "
           "katana::runtime::NativeHostEventKind::KeyDown;\n"
           "                    const auto bit = event.key == "
           "katana::runtime::NativeHostKey::Start ? 0x0008u :\n"
           "                        event.key == katana::runtime::NativeHostKey::A ? 0x0004u :\n"
           "                        event.key == katana::runtime::NativeHostKey::B ? 0x0002u :\n"
           "                        event.key == katana::runtime::NativeHostKey::X ? 0x0400u :\n"
           "                        event.key == katana::runtime::NativeHostKey::Y ? 0x0200u :\n"
           "                        event.key == katana::runtime::NativeHostKey::Up ? 0x0010u :\n"
           "                        event.key == katana::runtime::NativeHostKey::Down ? 0x0020u :\n"
           "                        event.key == katana::runtime::NativeHostKey::Left ? 0x0040u :\n"
           "                        event.key == katana::runtime::NativeHostKey::Right ? 0x0080u : "
           "0u;\n"
           "                    if (down) controller.pressed_buttons |= "
           "static_cast<std::uint16_t>(bit);\n"
           "                    else controller.pressed_buttons &= "
           "static_cast<std::uint16_t>(~bit);\n"
           "                }\n"
           "                if (host.state() != katana::runtime::HostRuntimeState::Shutdown)\n"
           "                    host.inject({host_sequence++, state.scheduler->current_cycle(), "
           "kind, controller});\n"
           "            }\n"
           "            presented_frames = video->presented_frames();\n"
           "        }\n"
           "        const auto audio_buffers = audio->submitted_buffers();\n"
           "        const auto audio_hash = audio->deterministic_hash();\n"
           "        const auto input_events = input->injected_events();\n"
           "        host.shutdown();\n"
           "        if (state.scheduler->pending_event_count() != 0u)\n"
           "            throw std::runtime_error(\"Host-Shutdown hinterliess "
           "Schedulerereignisse.\");\n"
           "        std::cout << \"SA_MAIN_ENTERED\\n\";\n"
           "        std::cout << \"KATANA_RUNTIME_METRICS silent_failures=\"\n"
           "                  << silent_failures << \" guest_cycles=\"\n"
           "                  << result.scheduler_cycle << \" indirect_dispatches=\"\n"
           "                  << result.indirect_dispatches << \" frames=\"\n"
           "                  << presented_frames << \" audio_buffers=\" << audio_buffers\n"
           "                  << \" audio_hash=\" << audio_hash\n"
           "                  << \" input_events=\" << input_events\n"
           "                  << \" executed_blocks=\" << services.executed_blocks()\n"
           "                  << \" guest_checkpoint=1 fallback_count=\"\n"
           "                  << services.fallback_count() << '\\n';\n"
           "        std::cout << \"KR_GENERATED_RUNTIME_STARTED boot_bytes=\"\n"
           "                  << state.loaded_boot_bytes << \" indirect_dispatches=\"\n"
           "                  << result.indirect_dispatches << \" final_pc=\" << result.final_pc "
           "<< '\\n';\n"
           "        return 0;\n"
           "    } catch (const std::exception& error) {\n"
           "        std::cerr << \"Portlauf fehlgeschlagen: \"\n"
           "                  << redact_source(error.what(), source) << '\\n';\n"
           "        return 1;\n"
           "    }\n"
           "}\n";
}

std::string runtime_dispatch_adapter(const std::string& entry_namespace,
                                     const std::span<const katana::ir::Function> program,
                                     const std::uint32_t entry_address) {
    const auto symbol = [](const std::uint32_t address) {
        std::ostringstream output;
        output << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << address;
        return output.str();
    };
    const auto end_kind = [](const katana::ir::BasicBlock& block) {
        using O = katana::ir::Operation;
        const katana::ir::Instruction* terminal = nullptr;
        for (const auto& instruction : block.instructions) {
            if (instruction.delay_slot.role != katana::ir::DelaySlotRole::Slot)
                terminal = &instruction;
        }
        if (terminal == nullptr) return "Fallthrough";
        switch (terminal->operation) {
        case O::Branch:
            return "StaticBranch";
        case O::BranchIfTrue:
        case O::BranchIfFalse:
            return "ConditionalBranch";
        case O::JumpRegister:
            return "DynamicBranch";
        case O::Call:
        case O::CallRegister:
            return "Call";
        case O::Return:
        case O::ReturnFromException:
            return "Return";
        case O::TrapAlways:
            return "Exception";
        default:
            return "Fallthrough";
        }
    };
    std::set<std::uint32_t> block_addresses;
    std::ostringstream output;
    output << "#include \"../include/katana_port.hpp\"\n"
           << "#include \"katana/runtime/block_abi.hpp\"\n"
           << "#include \"katana/runtime/block_table.hpp\"\n"
           << "#include \"katana/runtime/dispatch_diagnostics.hpp\"\n"
           << "#include \"katana/runtime/indirect_dispatch.hpp\"\n"
           << "#include <stdexcept>\n#include <utility>\n\n"
           << "namespace " << entry_namespace << " {\n";
    for (const auto& function : program) {
        output << "void fn_" << symbol(function.entry_address)
               << "_with_services(katana::runtime::CpuState&, "
                  "katana::runtime::PlatformServices*);\n";
    }
    output << "namespace {\n"
           << "thread_local katana::runtime::PlatformServices* active_services = nullptr;\n"
           << "thread_local const katana::runtime::RuntimeBlockTable* active_table = nullptr;\n"
           << "thread_local katana::runtime::BlockExecutionContext* active_context = nullptr;\n"
           << "thread_local katana::runtime::DispatchDiagnosticRecorder* active_diagnostics = nullptr;\n"
           << "thread_local bool tail_dispatch_completed = false;\n"
           << "void dispatch_chain(katana::runtime::CpuState&, std::uint32_t, "
              "katana::runtime::IndirectDispatchKind, bool);\n"
           << "class ServiceScope {\n"
           << "  public:\n"
           << "    ServiceScope(katana::runtime::PlatformServices& services,\n"
              "                 const katana::runtime::RuntimeBlockTable& table,\n"
              "                 katana::runtime::BlockExecutionContext& context,\n"
              "                 katana::runtime::DispatchDiagnosticRecorder& diagnostics) {\n"
           << "        if (active_services != nullptr) throw std::runtime_error(\"Runtime-Dispatch "
              "ist nicht reentrant.\");\n"
           << "        active_services = &services; active_table = &table;\n"
              "        active_context = &context; active_diagnostics = &diagnostics;\n"
           << "    }\n"
           << "    ~ServiceScope() { active_services = nullptr; active_table = nullptr;\n"
              "        active_context = nullptr; active_diagnostics = nullptr; }\n"
           << "};\n";
    for (const auto& function : program) {
        const auto owner = symbol(function.entry_address);
        for (const auto& block : function.blocks) {
            if (!block_addresses.insert(block.start_address).second)
                throw std::runtime_error("IR-Basic-Block besitzt mehrere Funktionsbesitzer.");
            const auto address = symbol(block.start_address);
            output << "katana::runtime::BlockExit dispatch_" << address
               << "(katana::runtime::CpuState& cpu, katana::runtime::BlockExecutionContext& "
                  "context) {\n"
               << "    if (active_services == nullptr) throw "
                  "std::runtime_error(\"Runtime-Plattformdienste fehlen.\");\n"
               << "    fn_" << owner << "_with_services(cpu, active_services);\n"
               << "    auto kind = cpu.trap_pending ? katana::runtime::BlockEndKind::Exception :\n"
               << "        cpu.last_exception_cause == katana::runtime::ExceptionCause::Interrupt\n"
               << "            ? katana::runtime::BlockEndKind::InterruptSafepoint\n"
               << "            : katana::runtime::BlockEndKind::" << end_kind(block) << ";\n"
               << "    if (std::exchange(tail_dispatch_completed, false))\n"
               << "        kind = katana::runtime::BlockEndKind::Return;\n"
               << "    return katana::runtime::make_block_exit(cpu, context,\n"
               << "        kind, {0x" << address
               << "u, katana::runtime::canonical_physical_address(0x" << address
               << "u)}, katana::runtime::BlockAddress{cpu.pc, "
                  "katana::runtime::canonical_physical_address(cpu.pc)});\n"
               << "}\n";
        }
    }
    output << "void dispatch_chain(katana::runtime::CpuState& cpu, std::uint32_t target,\n"
              "                    katana::runtime::IndirectDispatchKind kind, bool diagnostic) {\n"
              "    for (std::size_t blocks = 0u; blocks < 1000000u; ++blocks) {\n"
              "        const auto selected = katana::runtime::dispatch_indirect(cpu, *active_table,\n"
              "            {kind, cpu.pc, target, cpu.pr, {cpu.pc, "
              "katana::runtime::canonical_physical_address(cpu.pc)}, {},\n"
              "             katana::runtime::DispatchResolutionOrigin::TableLookup,\n"
              "             diagnostic ? active_diagnostics : nullptr});\n"
              "        if (selected.block == nullptr || selected.block->function == nullptr)\n"
              "            throw std::runtime_error(\"Runtime-Dispatchziel besitzt keinen generierten Block.\");\n"
              "        const auto exit = selected.block->function(cpu, *active_context);\n"
              "        if (exit.kind == katana::runtime::BlockEndKind::Exception || cpu.trap_pending)\n"
              "            throw std::runtime_error(\"guest-exception-before-checkpoint\");\n"
              "        if (exit.kind == katana::runtime::BlockEndKind::Return) return;\n"
              "        target = cpu.pc; kind = katana::runtime::IndirectDispatchKind::TailJump;\n"
              "        diagnostic = false;\n"
              "    }\n"
              "    throw std::runtime_error(\"Runtime-Blockbudget erschoepft.\");\n"
              "}\n"
              "} // namespace\n\n"
           << "void unresolved_call(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
              "    dispatch_chain(cpu, target, katana::runtime::IndirectDispatchKind::Call, true);\n"
              "}\n"
              "void unresolved_jump(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
              "    dispatch_chain(cpu, target, katana::runtime::IndirectDispatchKind::TailJump, true);\n"
              "    tail_dispatch_completed = true;\n"
              "}\n\n"
            << "RuntimeRunResult run_runtime(katana::runtime::CpuState& cpu,\n"
           << "                             katana::runtime::PlatformServices& services) {\n"
           << "    katana::runtime::validate_platform_services(services);\n"
           << "    katana::runtime::RuntimeBlockTable table;\n";
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            const auto address = symbol(block.start_address);
            std::uint32_t end = block.start_address + 2u;
            for (const auto& instruction : block.instructions)
                end = std::max(end, instruction.source_address + 2u);
            output << "    table.register_static({0x" << address
                   << "u, katana::runtime::canonical_physical_address(0x" << address
                   << "u), " << (end - block.start_address)
                   << "u, katana::runtime::BlockEndKind::" << end_kind(block)
                   << ", {}, &dispatch_" << address << ", \"generated-block-" << address
                   << "\"});\n";
            output << "    services.register_executable_block(0x" << address << "u, "
                   << (end - block.start_address) << "u, \"generated-block-" << address
                   << "\");\n";
        }
    }
    const auto entry = symbol(entry_address);
    output << "    katana::runtime::DispatchDiagnosticRecorder diagnostics;\n"
           << "    katana::runtime::BlockExecutionContext context;\n"
           << "    context.scheduler_cycle = services.scheduler_cycle();\n"
           << "    context.scheduler_event_budget = 1024u;\n"
           << "    ServiceScope scope(services, table, context, diagnostics);\n"
           << "    dispatch_chain(cpu, 0x" << entry
           << "u, katana::runtime::IndirectDispatchKind::TailJump, false);\n"
           << "    return {diagnostics.total_occurrences(), cpu.pc, services.scheduler_cycle()};\n"
           << "}\n"
           << "} // namespace " << entry_namespace << "\n";
    return output.str();
}

std::string root_cmake() {
    return "cmake_minimum_required(VERSION 3.25)\n"
           "project(KatanaPort LANGUAGES CXX)\n"
           "set(KATANA_RUNTIME_ROOT \"\" CACHE PATH \"KatanaRecomp source root\")\n"
           "if(NOT TARGET katana_runtime)\n"
           "  if(KATANA_RUNTIME_ROOT STREQUAL \"\")\n"
           "    message(FATAL_ERROR \"Set KATANA_RUNTIME_ROOT to the compatible KatanaRecomp "
           "source tree\")\n"
           "  endif()\n"
           "  add_subdirectory(\"${KATANA_RUNTIME_ROOT}\" \"${CMAKE_BINARY_DIR}/katana-runtime\")\n"
           "endif()\n"
           "add_subdirectory(generated)\n"
           "target_link_libraries(katana_generated PUBLIC katana_core)\n"
           "include(\"${CMAKE_CURRENT_SOURCE_DIR}/generated/katana-port.cmake\")\n";
}

std::string port_cmake(const std::string& target_name) {
    return "add_executable(" + target_name +
           " \"${CMAKE_CURRENT_LIST_DIR}/../src/main.cpp\")\n"
           "target_compile_features(" +
           target_name +
           " PRIVATE cxx_std_20)\n"
           "target_include_directories(" +
           target_name +
           " PRIVATE \"${CMAKE_CURRENT_LIST_DIR}/include\")\n"
           "target_link_libraries(" +
           target_name + " PRIVATE katana_generated katana_runtime)\n";
}

std::string port_metadata(const PortExportOptions& options,
                          const std::size_t function_count,
                          const std::span<const TranslationUnitPartition> partitions,
                          const std::uint32_t entry_address,
                          const std::size_t boot_size,
                          const std::string_view project_identity,
                          const std::span<const katana::analysis::IndirectControlFlowResolution>
                              indirect) {
    const auto count = [indirect](const auto status) {
        return std::count_if(indirect.begin(), indirect.end(), [status](const auto& resolution) {
            return resolution.status == status;
        });
    };
    std::ostringstream output;
    katana::io::write_json_report_header(output, "katana-port-project", "port-project");
    output << ",\"contract_version\":" << port_project_contract_version
           << ",\"target_name\":" << katana::io::quote_json(options.target_name)
           << ",\"runtime_abi\":" << katana::runtime::abi_version
           << ",\"backend_abi\":" << backend_interface_abi_version
           << ",\"project_identity\":" << katana::io::quote_json(project_identity)
           << ",\"entry_address\":" << entry_address << ",\"boot_size\":" << boot_size
           << ",\"function_count\":" << function_count
           << ",\"resolved_control_flow\":"
           << count(katana::analysis::ResolutionStatus::Resolved)
           << ",\"guarded_control_flow\":"
           << count(katana::analysis::ResolutionStatus::Guarded)
           << ",\"unresolved_control_flow\":"
           << count(katana::analysis::ResolutionStatus::Unresolved) << ",\"partitions\":[";
    for (std::size_t index = 0u; index < partitions.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& partition = partitions[index];
        output << "{\"index\":" << partition.index
               << ",\"functions\":" << partition.function_indices.size()
               << ",\"instructions\":" << partition.instruction_count
               << ",\"first_entry\":" << partition.first_entry_address
               << ",\"last_entry\":" << partition.last_entry_address << '}';
    }
    output << "]}";
    return output.str();
}

void write_port_file(const std::filesystem::path& root,
                     const std::filesystem::path& relative,
                     const std::string_view content,
                     const bool replace_existing = false) {
    auto candidate = root;
    for (const auto& component : relative) {
        candidate /= component;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (!error && std::filesystem::is_symlink(status)) {
            throw std::runtime_error("Port-Bootstrappfad enthaelt einen symbolischen Link.");
        }
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::runtime_error("Port-Bootstrappfad konnte nicht geprueft werden.");
        }
    }
    const auto path = root / relative;
    if (std::filesystem::exists(path) && !replace_existing) return;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw katana::io::InputOutputError("Port-Bootstrapdatei konnte nicht geoeffnet werden.");
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output)
        throw katana::io::InputOutputError("Port-Bootstrapdatei konnte nicht geschrieben werden.");
}

bool path_is_within(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

std::filesystem::path resolve_existing_parents(std::filesystem::path path) {
    std::vector<std::filesystem::path> missing;
    std::error_code error;
    while (!path.empty() && !std::filesystem::exists(path, error)) {
        if (error) throw std::runtime_error("Port-Ausgabepfad konnte nicht geprueft werden.");
        missing.push_back(path.filename());
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    auto resolved = std::filesystem::canonical(path);
    for (auto iterator = missing.rbegin(); iterator != missing.rend(); ++iterator)
        resolved /= *iterator;
    return resolved.lexically_normal();
}

} // namespace

PortExportResult export_dreamcast_port_project(const PreparedPortProgram& prepared,
                                               const std::filesystem::path& output_root,
                                               const PortExportOptions& options) {
    if (output_root.empty() || !valid_target_name(options.target_name) ||
        options.tool_version.empty() || prepared.entry_address == 0u || prepared.program.empty()) {
        throw std::invalid_argument(
            "Portexport braucht vorbereitetes IR, Einstieg, Ausgabe, Zielkennung und "
            "Werkzeugversion.");
    }
    if (!prepared.analysis.recursive.diagnostics.empty()) {
        throw std::runtime_error("Portanalyse enthaelt unbekannte Instruktionen.");
    }
    const auto unresolved =
        std::count_if(prepared.analysis.indirect_control_flow.begin(),
                      prepared.analysis.indirect_control_flow.end(),
                      [](const auto& resolution) {
                          return resolution.status ==
                                 katana::analysis::ResolutionStatus::Unresolved;
                      });
    if (unresolved != 0u) {
        throw std::runtime_error("Portanalyse ist unvollstaendig: " + std::to_string(unresolved) +
                                 " ungeloeste Kontrollflussstellen.");
    }
    katana::ir::require_valid_program(prepared.program);
    const auto partitions =
        partition_translation_units(prepared.program, options.partition_options);
    if (partitions.empty()) throw std::runtime_error("Portcodegen erzeugte keine Partition.");

    std::vector<std::uint32_t> global_entries;
    global_entries.reserve(prepared.program.size());
    for (const auto& function : prepared.program)
        global_entries.push_back(function.entry_address);
    const CppBackend backend;
    std::vector<ProjectArtifact> artifacts;
    artifacts.reserve(partitions.size() + 9u);
    for (const auto& partition : partitions) {
        auto functions = select_functions(prepared.program, partition);
        const auto contains_program_entry =
            std::any_of(functions.begin(), functions.end(), [&prepared](const auto& function) {
                return function.entry_address == prepared.entry_address;
            });
        const BackendRequest request{functions,
                                     functions.front().entry_address,
                                     {},
                                     global_entries,
                                     port_namespace,
                                     contains_program_entry,
                                     true,
                                     prepared.entry_address,
                                     true,
                                     true};
        auto source = backend.emit(request).joined_text();
        artifacts.push_back({std::filesystem::path("code") /
                                 deterministic_translation_unit_name(partition, prepared.program),
                             std::move(source)});
    }
    const auto entry_partition =
        std::find_if(partitions.begin(), partitions.end(), [&prepared](const auto& partition) {
            return std::any_of(partition.function_indices.begin(),
                               partition.function_indices.end(),
                               [&prepared](const auto index) {
                                   return prepared.program[index].entry_address ==
                                          prepared.entry_address;
                               });
        });
    if (entry_partition == partitions.end()) {
        throw std::runtime_error("Portcodegen besitzt keine Einstiegspartition.");
    }
    const auto entry_namespace = std::string(port_namespace);
    const auto source_map = build_address_source_map(prepared.image, artifacts);
    const auto control_flow_graph = katana::analysis::build_control_flow_graph(prepared.analysis);
    const auto call_graph = katana::analysis::build_call_graph(prepared.analysis);
    katana::io::BuildProvenance provenance;
    provenance.tool_version = options.tool_version;
    provenance.manifest_version = port_project_contract_version;
    provenance.manifest_sha256 = katana::io::sha256_bytes(
        options.target_name + ":" + std::to_string(port_project_contract_version));
    provenance.ir_version = 2u;
    provenance.runtime_abi = katana::runtime::abi_version;
    provenance.backend_name = "cpp";
    provenance.backend_abi = backend_interface_abi_version;
    provenance.inputs.assign(prepared.inputs.begin(), prepared.inputs.end());

    artifacts.push_back({"include/katana_port.hpp", generated_header(entry_namespace)});
    artifacts.push_back(
        {"code/runtime-dispatch.cpp",
         runtime_dispatch_adapter(entry_namespace, prepared.program, prepared.entry_address)});
    artifacts.push_back({"katana-port.cmake", port_cmake(options.target_name)});
    artifacts.push_back({"metadata/port-project.json",
                         port_metadata(options,
                                       prepared.program.size(),
                                       partitions,
                                       prepared.entry_address,
                                       prepared.boot_size,
                                       prepared.project_identity,
                                       prepared.analysis.indirect_control_flow)});
    artifacts.push_back(
        {"metadata/provenance.json", katana::io::format_build_provenance_json(provenance)});
    artifacts.push_back({"metadata/source-map.json", serialize_address_source_map(source_map)});
    artifacts.push_back(
        {"metadata/cfg.json", katana::analysis::serialize_analysis_graph_json(control_flow_graph)});
    artifacts.push_back(
        {"metadata/cfg.dot", katana::analysis::serialize_analysis_graph_dot(control_flow_graph)});
    artifacts.push_back(
        {"metadata/callgraph.json", katana::analysis::serialize_analysis_graph_json(call_graph)});
    artifacts.push_back(
        {"metadata/callgraph.dot", katana::analysis::serialize_analysis_graph_dot(call_graph)});

    const auto absolute_root = std::filesystem::absolute(output_root).lexically_normal();
    const auto resolved_root = resolve_existing_parents(absolute_root);
    if (!options.forbidden_source_root.empty()) {
        const auto source_root = std::filesystem::canonical(options.forbidden_source_root);
        if (path_is_within(resolved_root, source_root)) {
            throw std::invalid_argument(
                "Port-Ausgabe muss ausserhalb des KatanaRecomp-Quellbaums liegen.");
        }
    }
    std::error_code root_error;
    const auto root_status = std::filesystem::symlink_status(absolute_root, root_error);
    if (!root_error && std::filesystem::is_symlink(root_status)) {
        throw std::runtime_error("Port-Ausgabeziel darf kein symbolischer Link sein.");
    }
    if (root_error && root_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("Port-Ausgabeziel konnte nicht geprueft werden.");
    }
    std::filesystem::create_directories(absolute_root);
    const auto canonical_root = std::filesystem::canonical(absolute_root);
    if (!options.forbidden_source_root.empty() &&
        path_is_within(canonical_root, std::filesystem::canonical(options.forbidden_source_root))) {
        throw std::invalid_argument("Kanonische Port-Ausgabe liegt im KatanaRecomp-Quellbaum.");
    }
    const auto write = write_codegen_project(canonical_root / "generated", std::move(artifacts));
    write_port_file(canonical_root, "CMakeLists.txt", root_cmake());
    write_port_file(canonical_root, ".gitignore", "/build/\n");
    const auto* boot_segment = prepared.image.find_segment(prepared.boot_address, prepared.boot_size);
    if (boot_segment == nullptr)
        throw std::runtime_error("Portvertrag kann das analysierte Bootprogramm nicht binden.");
    const auto boot_offset = boot_segment->byte_offset(prepared.boot_address);
    if (!boot_offset || *boot_offset > boot_segment->bytes.size() ||
        prepared.boot_size > boot_segment->bytes.size() - *boot_offset)
        throw std::runtime_error("Portvertrag findet keine vollstaendigen Bootbytes.");
    const auto boot_bytes = std::string_view(
        reinterpret_cast<const char*>(boot_segment->bytes.data() + *boot_offset),
        prepared.boot_size);
    write_port_file(
        canonical_root,
        "src/main.cpp",
        handwritten_main(entry_namespace,
                         prepared.hle_bios_abi,
                         prepared.inputs,
                         katana::io::sha256_bytes(boot_bytes),
                         prepared.entry_address),
        true);

    return {canonical_root,
            prepared.program.size(),
            partitions.size(),
            write.written_files.size(),
            write.removed_files.size(),
            {"gdi-validated",
             "boot-image-loaded",
             "analysis-complete",
             "ir-lowered",
             "partitioned-codegen-complete",
             "port-project-written"}};
}

PortExportResult export_dreamcast_port_project(const std::filesystem::path& gdi_path,
                                               const std::filesystem::path& output_root,
                                               const PortExportOptions& options) {
    if (gdi_path.empty()) {
        throw std::invalid_argument("Portexport braucht eine GDI-Quelle.");
    }
    const auto disc = katana::platform::load_dreamcast_gdi_boot(gdi_path);
    auto image = katana::platform::make_dreamcast_disc_executable(disc);
    const auto analysis = katana::analysis::analyze_control_flow(image);
    auto program = katana::ir::lower_program(analysis);
    static_cast<void>(katana::ir::optimize_program(program));
    std::vector<katana::io::InputProvenance> inputs;
    inputs.push_back(katana::io::capture_input_provenance("gdi-descriptor", gdi_path));
    for (const auto& track : disc.source->descriptor().tracks) {
        inputs.push_back(katana::io::capture_input_provenance(
            "gdi-track-" + std::to_string(track.number), track.resolved_path));
    }
    return export_dreamcast_port_project({image,
                                          analysis,
                                          program,
                                          inputs,
                                          katana::platform::dreamcast_disc_boot_address,
                                          katana::platform::dreamcast_disc_boot_address,
                                          disc.boot_file.size(),
                                          {}},
                                         output_root,
                                         options);
}

} // namespace katana::codegen
