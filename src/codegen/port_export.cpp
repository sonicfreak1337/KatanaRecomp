#include "katana/codegen/port_export.hpp"

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/graph_export.hpp"
#include "katana/analysis/hardware_audit.hpp"
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
#include "katana/runtime/disc_install.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/packed_disc.hpp"
#include "katana/runtime/wait_loop_trace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <compare>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_set>

namespace katana::codegen {
namespace {

void report_progress(const PortExportOptions& options, const std::string_view phase) {
    if (options.progress_callback != nullptr) options.progress_callback(phase);
}

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

std::string_view console_profile_enumerator(const std::string_view profile) {
    if (profile == "japan-ntsc") return "JapanNtsc";
    if (profile == "north-america-ntsc") return "NorthAmericaNtsc";
    if (profile == "europe-pal") return "EuropePal";
    if (profile == "vga") return "Vga";
    throw std::invalid_argument("Unbekanntes Dreamcast-Konsolenprofil.");
}

std::size_t port_codegen_jobs(const std::size_t partition_count) {
    auto requested = static_cast<std::size_t>(std::max(1u, std::thread::hardware_concurrency()));
    std::optional<std::string> configured;
#ifdef _WIN32
    char* value = nullptr;
    std::size_t value_size = 0u;
    if (_dupenv_s(&value, &value_size, "KATANA_PORT_CODEGEN_JOBS") == 0 && value != nullptr)
        configured = value;
    std::free(value);
#else
    if (const auto* value = std::getenv("KATANA_PORT_CODEGEN_JOBS");
        value != nullptr && *value != '\0')
        configured = value;
#endif
    if (configured && !configured->empty()) {
        std::size_t parsed = 0u;
        const auto jobs = std::stoull(*configured, &parsed, 10);
        if (parsed != configured->size() || jobs == 0u)
            throw std::invalid_argument("KATANA_PORT_CODEGEN_JOBS ist ungueltig.");
        requested = static_cast<std::size_t>(jobs);
    }
    return std::min(partition_count, requested);
}

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
           "#include \"katana/runtime/block_table.hpp\"\n"
           "#include \"katana/runtime/platform_services.hpp\"\n"
           "#include \"katana/runtime/runtime.hpp\"\n"
           "#include <string>\n\n"
           "namespace " +
           entry_namespace +
           " {\n"
           "void run(katana::runtime::CpuState& cpu);\n"
           "struct RuntimeRunResult {\n"
           "    std::uint64_t indirect_dispatches = 0u;\n"
           "    std::uint64_t runtime_dispatch_hits = 0u;\n"
           "    std::uint64_t runtime_dispatch_misses = 0u;\n"
           "    std::uint64_t runtime_dispatch_fallbacks = 0u;\n"
           "    std::uint64_t runtime_only_dispatch_hits = 0u;\n"
           "    std::uint64_t runtime_only_dispatch_misses = 0u;\n"
           "    std::uint64_t runtime_only_dispatch_fallbacks = 0u;\n"
           "    std::uint64_t runtime_only_sites = 0u;\n"
           "    std::uint64_t runtime_only_dispatch_share_ppm = 0u;\n"
           "    std::string runtime_only_profile_json;\n"
           "    std::uint32_t runtime_dispatch_first_error = 0u;\n"
           "    std::uint32_t final_pc = 0u;\n"
           "    std::uint64_t scheduler_cycle = 0u;\n"
           "    std::uint32_t guest_cycle_contract = "
           "katana::runtime::guest_cycle_contract_version;\n"
           "};\n"
           "struct RuntimeMaterializationStatus {\n"
           "    std::uint64_t requests = 0u;\n"
           "    std::uint64_t cache_hits = 0u;\n"
           "    std::uint64_t materializations = 0u;\n"
           "    std::uint64_t interpreter_materializations = 0u;\n"
           "    std::uint64_t misses = 0u;\n"
           "    std::uint64_t budget_failures = 0u;\n"
           "    std::uint32_t first_failure = 0u;\n"
           "    std::uint32_t first_failure_target = 0u;\n"
           "};\n"
           "const RuntimeMaterializationStatus& runtime_materialization_status() noexcept;\n"
           "RuntimeRunResult run_runtime(katana::runtime::CpuState& cpu,\n"
           "                             katana::runtime::PlatformServices& services,\n"
           "                             katana::runtime::RuntimeBlockTable& table);\n"
           "}\n";
}

std::vector<katana::runtime::RuntimeWaitLoopDescriptor>
runtime_wait_loop_descriptors(const katana::analysis::DreamcastHardwareAudit& audit) {
    using Classification = katana::analysis::HardwareLoopClassification;
    using Evidence = katana::runtime::RuntimeWaitLoopEvidence;
    using Kind = katana::analysis::HardwareAccessKind;
    using Descriptor = katana::runtime::RuntimeWaitLoopDescriptor;

    std::vector<Descriptor> descriptors;
    for (const auto& loop : audit.loops) {
        if (loop.classification == Classification::Counter) continue;

        const auto add = [&](const std::uint32_t read_site, const Evidence evidence) {
            descriptors.push_back(
                {loop.header_address, loop.latch_address, read_site, evidence});
        };
        for (const auto& access : loop.accesses) {
            if (access.kind == Kind::Read && access.guards_loop)
                add(access.instruction_address, Evidence::ProvenGuard);
        }
        for (const auto read_site : loop.unresolved_guard_read_instruction_addresses)
            add(read_site, Evidence::UnresolvedGuard);

        const bool conservative_candidates =
            loop.classification == Classification::RamPoll ||
            loop.classification == Classification::MmioPoll ||
            loop.classification == Classification::Mixed ||
            loop.classification == Classification::Unknown;
        if (!conservative_candidates) continue;
        for (const auto& access : loop.accesses) {
            if (access.kind == Kind::Read && !access.guards_loop)
                add(access.instruction_address, Evidence::ConservativeCandidate);
        }
    }

    const auto key = [](const Descriptor& descriptor) {
        return std::tuple{descriptor.loop_header,
                          descriptor.loop_latch,
                          descriptor.read_site,
                          static_cast<std::uint8_t>(descriptor.evidence)};
    };
    std::sort(descriptors.begin(), descriptors.end(), [&](const auto& left, const auto& right) {
        return key(left) < key(right);
    });
    descriptors.erase(
        std::unique(descriptors.begin(), descriptors.end(), [](const auto& left, const auto& right) {
            return left.loop_header == right.loop_header &&
                   left.loop_latch == right.loop_latch && left.read_site == right.read_site;
        }),
        descriptors.end());
    return descriptors;
}

std::string runtime_wait_loop_descriptor_contract(
    const std::span<const katana::runtime::RuntimeWaitLoopDescriptor> descriptors) {
    const auto evidence_enumerator = [](const katana::runtime::RuntimeWaitLoopEvidence evidence) {
        using Evidence = katana::runtime::RuntimeWaitLoopEvidence;
        switch (evidence) {
        case Evidence::ProvenGuard: return "ProvenGuard";
        case Evidence::UnresolvedGuard: return "UnresolvedGuard";
        case Evidence::ConservativeCandidate: return "ConservativeCandidate";
        }
        throw std::logic_error("Unbekannte Wait-Loop-Evidenz.");
    };
    const auto address = [](const std::uint32_t value) {
        std::ostringstream output;
        output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
               << value << 'u';
        return output.str();
    };

    std::ostringstream output;
    output << "constexpr std::array<katana::runtime::RuntimeWaitLoopDescriptor, "
           << descriptors.size() << "u> runtime_wait_loop_descriptors{{\n";
    for (const auto& descriptor : descriptors) {
        output << "    {" << address(descriptor.loop_header) << ", "
               << address(descriptor.loop_latch) << ", " << address(descriptor.read_site)
               << ", katana::runtime::RuntimeWaitLoopEvidence::"
               << evidence_enumerator(descriptor.evidence) << "},\n";
    }
    output << "}};\n";
    return output.str();
}

std::string handwritten_main(const std::string& entry_namespace,
                             const bool hle_bios_abi,
                             const bool diagnostic_partial,
                             const std::span<const katana::io::InputProvenance> inputs,
                             const std::span<const katana::runtime::RuntimeWaitLoopDescriptor>
                                 wait_loop_descriptors,
                             const std::string_view project_identity,
                             const std::string_view expected_content_identity,
                             const std::string_view expected_boot_sha256,
                             const std::string_view console_profile,
                             const std::uint32_t entry_address) {
    std::ostringstream identity_contract;
    identity_contract
        << "struct ExpectedInput { std::string_view role; std::string_view sha256; };\n"
        << "constexpr ExpectedInput expected_inputs[]{\n";
    for (const auto& input : inputs) {
        if (input.role != "gdi-descriptor" && !input.role.starts_with("gdi-track-")) continue;
        identity_contract << "    {" << katana::io::quote_json(input.role) << ", "
                          << katana::io::quote_json(input.sha256) << "},\n";
    }
    identity_contract << "};\n"
                      << "constexpr bool diagnostic_partial_port = "
                      << (diagnostic_partial ? "true" : "false") << ";\n"
                      << "constexpr std::string_view expected_project_identity = "
                      << katana::io::quote_json(project_identity) << ";\n"
                      << "constexpr std::string_view expected_content_identity = "
                      << katana::io::quote_json(expected_content_identity) << ";\n"
                      << "constexpr std::string_view expected_boot_sha256 = "
                      << katana::io::quote_json(expected_boot_sha256) << ";\n"
                      << runtime_wait_loop_descriptor_contract(wait_loop_descriptors);
    return "#include \"katana_port.hpp\"\n"
           "#include \"katana/runtime/block_guards.hpp\"\n"
           "#include \"katana/runtime/dreamcast_boot.hpp\"\n"
           "#include \"katana/runtime/disc_install.hpp\"\n"
           "#include \"katana/runtime/host_runtime.hpp\"\n"
           "#include \"katana/runtime/host_video.hpp\"\n"
           "#include \"katana/runtime/indirect_dispatch.hpp\"\n"
           "#include \"katana/runtime/packed_disc.hpp\"\n"
           "#include \"katana/runtime/runtime_probe.hpp\"\n"
           "#include \"katana/runtime/scheduler.hpp\"\n"
           "#include \"katana/runtime/scheduler_safepoint.hpp\"\n"
           "#include \"katana/runtime/system_replay.hpp\"\n"
           "#include \"katana/runtime/wait_loop_trace.hpp\"\n"
           "#include \"katana/io/input_provenance.hpp\"\n"
           "#include <algorithm>\n#include <array>\n#include <chrono>\n#include <cstdlib>\n#include "
           "<exception>\n#include <filesystem>\n#include <functional>\n#include "
           "<iostream>\n#include <limits>\n"
           "#include <optional>\n#include <span>\n#include <string>\n#include <string_view>\n"
           "#include <system_error>\n#include <thread>\n#include <unordered_map>\n#include "
           "<unordered_set>\n#include <vector>\n\n"
           "namespace {\n" +
           identity_contract.str() +
           "bool deterministic_runtime_probe_requested() {\n"
           "    const auto* value = std::getenv(\"KATANA_RUNTIME_PROBE\");\n"
           "    if (value == nullptr) return false;\n"
           "    if (std::string_view(value) == \"deterministic-v1\") return true;\n"
           "    throw std::invalid_argument(\"runtime-probe-profile-invalid\");\n"
           "}\n"
           "void validate_runtime_probe_environment(bool enabled) {\n"
           "    if (!enabled) return;\n"
           "    const auto* diagnostics = std::getenv(\"KATANA_PORT_DIAGNOSTICS\");\n"
           "    if (diagnostics == nullptr ||\n"
           "        (std::string_view(diagnostics) != \"0\" &&\n"
           "         std::string_view(diagnostics) != \"1\"))\n"
           "        throw std::invalid_argument(\"runtime-probe-diagnostics-invalid\");\n"
           "    if (!katana::runtime::guest_cycle_budget_from_environment().has_value())\n"
           "        throw std::invalid_argument(\"runtime-probe-budget-required\");\n"
           "    constexpr std::array forbidden{\n"
           "        \"KATANA_PORT_WAIT_LOOP_TRACE\", \"KATANA_PORT_DIAGNOSTICS_FULL\",\n"
           "        \"KATANA_PORT_PROGRESS_INTERVAL\", \"KATANA_PORT_LIFECYCLE_TEST\",\n"
           "        \"KATANA_PORT_BLOCK_LIMIT\", \"KATANA_PORT_IGNORE_FOCUS\",\n"
           "        \"KATANA_PORT_MEMORY_PROBES\"};\n"
           "    for (const auto* name : forbidden)\n"
           "        if (std::getenv(name) != nullptr)\n"
           "            throw std::invalid_argument(\"runtime-probe-environment-conflict\");\n"
           "}\n"
           "void record_runtime_probe_event(\n"
           "        katana::runtime::SystemReplayLog& replay,\n"
           "        const katana::runtime::EventScheduler& scheduler,\n"
           "        katana::runtime::SystemReplayEventKind kind,\n"
           "        std::string code, std::uint64_t detail = 0u,\n"
           "        std::uint64_t auxiliary = 0u) noexcept {\n"
           "    katana::runtime::SystemReplayEvent event{\n"
           "        0u, scheduler.current_cycle(), kind, std::move(code),\n"
           "        std::nullopt, std::nullopt, detail, auxiliary, false,\n"
           "        scheduler.reset_generation()};\n"
            "    static_cast<void>(replay.try_record(std::move(event)));\n"
            "}\n"
            "class RuntimeProbeMmioTraceSession final {\n"
            "  public:\n"
            "    RuntimeProbeMmioTraceSession(\n"
            "            katana::runtime::Memory& memory,\n"
            "            katana::runtime::SystemReplayLog& replay,\n"
            "            const katana::runtime::EventScheduler& scheduler)\n"
            "        : memory_(&memory) {\n"
            "        if (memory_->has_mmio_trace_handler())\n"
            "            throw std::logic_error(\"runtime-probe-mmio-observer-conflict\");\n"
            "        memory_->set_mmio_trace_handler(\n"
            "            katana::runtime::system_replay_mmio_observer(\n"
            "            replay, [&scheduler] { return scheduler.current_cycle(); },\n"
            "            \"guest-mmio\", [&scheduler] { return scheduler.reset_generation(); }));\n"
            "        try {\n"
            "            replay.enable_coverage(static_cast<\n"
            "                katana::runtime::SystemReplayCoverageMask>(\n"
            "                    katana::runtime::SystemReplayCoverage::Mmio));\n"
            "        } catch (...) {\n"
            "            memory_->clear_mmio_trace_handler();\n"
            "            throw;\n"
            "        }\n"
            "    }\n"
            "    RuntimeProbeMmioTraceSession(const RuntimeProbeMmioTraceSession&) = delete;\n"
            "    RuntimeProbeMmioTraceSession& operator=(const RuntimeProbeMmioTraceSession&) = "
            "delete;\n"
            "    ~RuntimeProbeMmioTraceSession() noexcept { finish(); }\n"
            "    void finish() noexcept {\n"
            "        if (memory_ == nullptr) return;\n"
            "        memory_->clear_mmio_trace_handler();\n"
            "        memory_ = nullptr;\n"
            "    }\n"
            "  private:\n"
            "    katana::runtime::Memory* memory_ = nullptr;\n"
            "};\n"
            "class RuntimeWaitLoopTraceSession final {\n"
           "  public:\n"
           "    RuntimeWaitLoopTraceSession(katana::runtime::Memory& memory, bool enabled)\n"
           "        : memory_(&memory) {\n"
           "        if (!enabled || runtime_wait_loop_descriptors.empty()) return;\n"
           "        std::cerr << \"KATANA_WAIT_LOOP_TRACE_NOTICE local-only; contains raw "
           "guest-memory values; do not share without review\\n\";\n"
           "        recorder_.emplace(runtime_wait_loop_descriptors);\n"
           "        memory_->set_guest_memory_access_sink(recorder_->sink());\n"
           "    }\n"
           "    RuntimeWaitLoopTraceSession(const RuntimeWaitLoopTraceSession&) = delete;\n"
           "    RuntimeWaitLoopTraceSession& operator=(const RuntimeWaitLoopTraceSession&) = "
           "delete;\n"
           "    ~RuntimeWaitLoopTraceSession() noexcept { finish(); }\n"
           "    void finish() noexcept {\n"
           "        if (!recorder_) return;\n"
           "        memory_->clear_guest_memory_access_sink();\n"
           "        if (reported_) return;\n"
           "        reported_ = true;\n"
           "        try {\n"
           "            std::cerr << \"KATANA_WAIT_LOOP_TRACE \"\n"
           "                      << recorder_->serialize_json() << '\\n';\n"
           "        } catch (...) {\n"
           "            std::cerr << \"KATANA_WAIT_LOOP_TRACE "
           "{\\\"schema\\\":\\\"katana.runtime-wait-loop-trace\\\","
           "\\\"trace_version\\\":1,\\\"complete\\\":false,"
           "\\\"contains_raw_guest_values\\\":true,"
           "\\\"serialization_error\\\":true}\\n\";\n"
           "        }\n"
           "    }\n"
           "  private:\n"
           "    katana::runtime::Memory* memory_ = nullptr;\n"
           "    std::optional<katana::runtime::RuntimeWaitLoopTraceRecorder> recorder_;\n"
           "    bool reported_ = false;\n"
           "};\n"
           "void verify_boot_identity(\n"
           "        const katana::runtime::DreamcastRuntimeBootImage& boot) {\n"
           "    const std::string_view boot_bytes(\n"
           "        reinterpret_cast<const char*>(boot.boot_file.data()), boot.boot_file.size());\n"
           "    if (katana::io::sha256_bytes(boot_bytes) != expected_boot_sha256)\n"
           "        throw std::runtime_error(\"source-identity-mismatch\");\n"
           "}\n"
           "void verify_source_identity(const std::filesystem::path& source,\n"
           "                            const katana::runtime::DreamcastRuntimeBootImage& boot) {\n"
           "    std::vector<katana::io::InputProvenance> actual;\n"
           "    actual.push_back(katana::io::capture_input_provenance(\"gdi-descriptor\", "
           "source));\n"
           "    const auto gdi = std::dynamic_pointer_cast<katana::runtime::GdiDiscSource>(\n"
           "        boot.source);\n"
           "    if (!gdi) throw std::runtime_error(\"source-identity-mismatch\");\n"
           "    for (const auto& track : gdi->descriptor().tracks)\n"
           "        actual.push_back(katana::io::capture_input_provenance(\n"
           "            \"gdi-track-\" + std::to_string(track.number), track.resolved_path));\n"
           "    if (actual.size() != std::size(expected_inputs))\n"
           "        throw std::runtime_error(\"source-identity-mismatch\");\n"
           "    for (std::size_t index = 0u; index < actual.size(); ++index)\n"
           "        if (actual[index].role != expected_inputs[index].role ||\n"
           "            actual[index].sha256 != expected_inputs[index].sha256)\n"
           "            throw std::runtime_error(\"source-identity-mismatch\");\n"
           "}\n"
           "void verify_pack_identity(const katana::runtime::PackedDiscSource& source) {\n"
           "    if (source.info().job_generation != expected_project_identity ||\n"
           "        source.info().content_identity != expected_content_identity)\n"
           "        throw std::runtime_error(\"source-identity-mismatch\");\n"
           "}\n"
           "void verify_recipe_identity(const katana::runtime::DiscInstallRecipe& recipe) {\n"
           "    if (recipe.job_generation != expected_project_identity ||\n"
           "        recipe.content_identity != expected_content_identity ||\n"
           "        recipe.boot_sha256 != expected_boot_sha256)\n"
           "        throw std::runtime_error(\"source-identity-mismatch\");\n"
           "}\n"
           "class PortPlatformServices final : public katana::runtime::PlatformServices {\n"
           "  public:\n"
            "    PortPlatformServices(katana::runtime::CpuState& cpu,\n"
            "                         const katana::runtime::DreamcastRuntimeState& state,\n"
            "                         std::function<katana::runtime::PlatformLifecycleState()> "
            "lifecycle_poll, std::function<void()> guest_frame_poll,\n"
            "                         bool eager_host_poll = false,\n"
            "                         bool runtime_probe_mode = false,\n"
            "                         katana::runtime::SystemReplayLog* replay_log = nullptr)\n"
            "        : cpu_(cpu), state_(state), lifecycle_poll_(std::move(lifecycle_poll)),\n"
            "          guest_frame_poll_(std::move(guest_frame_poll)),\n"
             "          replay_log_(replay_log), eager_host_poll_(eager_host_poll),\n"
             "          runtime_probe_mode_(runtime_probe_mode),\n"
             "          local_block_chaining_enabled_(\n"
             "              std::getenv(\"KATANA_PORT_BLOCK_LIMIT\") == nullptr) {\n"
            "        if (runtime_probe_mode_ && replay_log_ != nullptr) {\n"
            "            replay_log_->enable_coverage(\n"
            "                static_cast<katana::runtime::SystemReplayCoverageMask>(\n"
            "                    katana::runtime::SystemReplayCoverage::CpuSafepoint) |\n"
            "                static_cast<katana::runtime::SystemReplayCoverageMask>(\n"
            "                    katana::runtime::SystemReplayCoverage::AcceptedInterrupt) |\n"
            "                static_cast<katana::runtime::SystemReplayCoverageMask>(\n"
            "                    katana::runtime::SystemReplayCoverage::Dma));\n"
            "        }\n"
            "        if (const auto budget = "
           "katana::runtime::guest_cycle_budget_from_environment())\n"
           "            state_.scheduler->set_guest_cycle_budget(*budget);\n"
           "    }\n"
           "    std::string_view name() const noexcept override { return \"dreamcast-port\"; }\n"
           "    std::uint32_t abi_version() const noexcept override {\n"
           "        return katana::runtime::platform_services_abi_version;\n"
           "    }\n"
           "    std::uint32_t guest_cycle_contract() const noexcept override {\n"
           "        return katana::runtime::guest_cycle_contract_version;\n"
           "    }\n"
           "    katana::runtime::PlatformCapabilities capabilities() const noexcept override {\n"
           "        return katana::runtime::core_platform_capabilities;\n"
           "    }\n"
           "    void read_memory(std::uint32_t address, std::span<std::uint8_t> output) override "
           "{\n"
           "        for (auto& byte : output) byte = "
           "katana::runtime::guest_read_u8(cpu_, address++);\n"
           "    }\n"
           "    void write_memory(std::uint32_t address, std::span<const std::uint8_t> input) "
           "override {\n"
           "        for (const auto byte : input) katana::runtime::guest_write_u8(\n"
           "            cpu_, address++, byte, katana::runtime::CodeWriteSource::Fallback);\n"
           "    }\n"
           "    std::uint64_t scheduler_cycle() const noexcept override {\n"
           "        return state_.scheduler->current_cycle();\n"
           "    }\n"
           "    std::optional<std::uint64_t> next_scheduler_event_cycle() const noexcept override "
           "{\n"
           "        return state_.scheduler->next_event_cycle();\n"
           "    }\n"
           "    katana::runtime::PlatformSchedulerResult\n"
           "    consume_guest_cycles(std::uint64_t cycles, std::size_t budget) override {\n"
           "        if (!runtime_probe_mode_) {\n"
           "            for (;;) {\n"
           "                const auto lifecycle = poll_host_lifecycle();\n"
           "                if (lifecycle == katana::runtime::PlatformLifecycleState::Shutdown)\n"
           "                    throw katana::runtime::PlatformShutdownRequested();\n"
           "                if (lifecycle == katana::runtime::PlatformLifecycleState::Running)\n"
           "                    break;\n"
           "                std::this_thread::sleep_for(std::chrono::milliseconds(1));\n"
           "            }\n"
           "        }\n"
           "        const auto before = state_.scheduler->current_cycle();\n"
           "        const auto result = state_.scheduler->advance_by(cycles, budget);\n"
           "        if (replay_log_ != nullptr) {\n"
           "            const auto requested = cycles >\n"
           "                    std::numeric_limits<std::uint64_t>::max() - before\n"
           "                ? std::numeric_limits<std::uint64_t>::max() : before + cycles;\n"
           "            const auto jitter = requested > result.guest_cycle\n"
           "                ? requested - result.guest_cycle : result.guest_cycle - requested;\n"
           "            auto event = katana::runtime::make_safepoint_replay_event({\n"
           "                katana::runtime::SafepointKind::BlockEnd,\n"
           "                katana::runtime::ExecutionOrigin::Backend,\n"
           "                requested, result.guest_cycle, jitter, result.processed_events,\n"
           "                false,\n"
           "                result.status == "
           "katana::runtime::SchedulerAdvanceStatus::EventBudgetExhausted,\n"
           "                result.status == "
           "katana::runtime::SchedulerAdvanceStatus::GuestCycleBudgetExhausted});\n"
           "            event.time_epoch = state_.scheduler->reset_generation();\n"
           "            static_cast<void>(replay_log_->try_record(std::move(event)));\n"
           "        }\n"
           "        if (runtime_probe_mode_) {\n"
           "            const auto remaining = state_.scheduler->remaining_guest_cycles();\n"
           "            if (remaining && *remaining == 0u)\n"
           "                throw katana::runtime::RuntimeProbeBudgetReached(result.guest_cycle);\n"
           "        }\n"
           "        if (!runtime_probe_mode_) {\n"
           "            const auto host_now = std::chrono::steady_clock::now();\n"
           "            if (guest_frame_poll_ &&\n"
           "                (result.processed_events != 0u || host_now >= next_frame_poll_)) {\n"
           "                guest_frame_poll_();\n"
           "                next_frame_poll_ = host_now + std::chrono::milliseconds(1);\n"
           "            }\n"
           "        }\n"
           "        return {result.guest_cycle, result.processed_events,\n"
           "                result.status == "
           "katana::runtime::SchedulerAdvanceStatus::EventBudgetExhausted,\n"
           "                result.status == "
           "katana::runtime::SchedulerAdvanceStatus::GuestCycleBudgetExhausted};\n"
           "    }\n"
           "    std::optional<katana::runtime::PlatformInterruptRequest> poll_interrupt() override "
           "{\n"
           "        if (!state_.interrupt_router->accept(cpu_)) return std::nullopt;\n"
           "        if (replay_log_ != nullptr)\n"
           "            record_runtime_probe_event(\n"
           "                *replay_log_, *state_.scheduler,\n"
           "                katana::runtime::SystemReplayEventKind::Interrupt,\n"
           "                \"interrupt-accepted\", cpu_.intevt);\n"
           "        return katana::runtime::PlatformInterruptRequest{0u, 0u, cpu_.intevt};\n"
           "    }\n"
            "    katana::runtime::PlatformDmaResult\n"
            "    start_dma(const katana::runtime::PlatformDmaRequest& request) override {\n"
            "        if (request.length == 0u) return {};\n"
            "        if (replay_log_ != nullptr)\n"
            "            record_runtime_probe_event(\n"
            "                *replay_log_, *state_.scheduler,\n"
            "                katana::runtime::SystemReplayEventKind::Dma,\n"
            "                \"platform-dma-start\", request.length,\n"
            "                (static_cast<std::uint64_t>(request.source) << 32u) |\n"
            "                    request.destination);\n"
            "        state_.dmac->write_source(0u, request.source);\n"
           "        state_.dmac->write_destination(0u, request.destination);\n"
           "        state_.dmac->write_count(0u, request.length);\n"
           "        state_.dmac->write_control(0u, 0x00005400u |\n"
           "            katana::runtime::Sh4Dmac::interrupt_enable |\n"
           "            katana::runtime::Sh4Dmac::channel_enable);\n"
           "        state_.dmac->write_operation(katana::runtime::Sh4Dmac::master_enable);\n"
           "        return {0u, false};\n"
           "    }\n"
           "    katana::runtime::PlatformFallbackResult controlled_fallback(\n"
           "        katana::runtime::CpuState&, const katana::runtime::PlatformFallbackRequest&) "
           "override {\n"
           "        ++fallback_count_;\n"
           "        return {};\n"
           "    }\n"
           "    bool prefetch(katana::runtime::CpuState& cpu,\n"
           "                  katana::runtime::GuestInstructionOrigin instruction,\n"
           "                  std::uint32_t address) override {\n"
           "        katana::runtime::prefetch(cpu, address);\n"
           "        return state_.store_queues->prefetch(\n"
           "            address, instruction, cpu.retired_guest_instructions);\n"
           "    }\n"
           "    katana::runtime::PlatformLifecycleState poll_host_lifecycle() override {\n"
           "        if (runtime_probe_mode_) {\n"
           "            const auto remaining = state_.scheduler->remaining_guest_cycles();\n"
           "            if (remaining && *remaining == 0u)\n"
           "                throw katana::runtime::RuntimeProbeBudgetReached(\n"
           "                    state_.scheduler->current_cycle());\n"
           "            return katana::runtime::PlatformLifecycleState::Running;\n"
           "        }\n"
           "        if (!lifecycle_poll_)\n"
           "            return katana::runtime::PlatformLifecycleState::Running;\n"
           "        const auto host_now = std::chrono::steady_clock::now();\n"
           "        if (eager_host_poll_ || cached_lifecycle_ !=\n"
           "                katana::runtime::PlatformLifecycleState::Running ||\n"
           "            host_now >= next_lifecycle_poll_) {\n"
           "            cached_lifecycle_ = lifecycle_poll_();\n"
           "            next_lifecycle_poll_ = host_now + std::chrono::milliseconds(1);\n"
           "        }\n"
           "        return cached_lifecycle_;\n"
           "    }\n"
           "    void observe_guest_checkpoint(std::uint32_t address) noexcept override {\n"
           "        ++executed_blocks_;\n"
           "        if (address != " +
           std::to_string(entry_address) +
           "u) guest_checkpoint_ = true;\n"
           "    }\n"
           "    bool guest_checkpoint_reached() const noexcept { return guest_checkpoint_; }\n"
           "    std::uint64_t executed_blocks() const noexcept { return executed_blocks_; }\n"
           "    std::uint64_t fallback_count() const noexcept { return fallback_count_; }\n"
           "    void register_executable_block(std::uint32_t address, std::uint32_t size,\n"
           "                                   std::string_view identity) override {\n"
           "        static_cast<void>(state_.code_tracker->register_block(\n"
           "            {std::string(identity), "
           "katana::runtime::canonical_physical_address(address),\n"
           "             size, \"generated-port\", {},\n"
           "             katana::runtime::ExecutableBlockOrigin::ImageSegment}));\n"
           "        const auto maximum_guest_instructions =\n"
           "            std::max<std::uint64_t>(1u, (static_cast<std::uint64_t>(size) + 1u) / 2u);\n"
           "        executable_blocks_.insert_or_assign(address, ExecutableBlockRegistration{\n"
           "            std::string(identity), katana::runtime::base_guest_cycles_per_instruction *\n"
           "                maximum_guest_instructions});\n"
           "    }\n"
           "    void allow_executable_block_chaining(std::uint32_t address) override {\n"
           "        chainable_blocks_.insert(address);\n"
           "    }\n"
           "    void begin_executable_block(\n"
           "            const katana::runtime::BlockVariantKey& variant) noexcept override {\n"
           "        active_block_variant_ = variant;\n"
           "        chain_retired_baseline_ = cpu_.retired_guest_instructions;\n"
           "    }\n"
           "    bool can_chain_executable_block(std::uint32_t address) const noexcept override {\n"
           "        if (!local_block_chaining_enabled_ || !active_block_variant_ ||\n"
           "            !state_.address_space ||\n"
           "            (!cpu_.interrupts_blocked() && cpu_.interrupt_mask() != 15u) ||\n"
           "            cpu_.retired_guest_instructions < chain_retired_baseline_)\n"
           "            return false;\n"
           "        const auto pending_instructions =\n"
           "            cpu_.retired_guest_instructions - chain_retired_baseline_;\n"
           "        if (pending_instructions == 0u ||\n"
           "            pending_instructions > std::numeric_limits<std::uint64_t>::max() /\n"
           "                katana::runtime::base_guest_cycles_per_instruction) return false;\n"
           "        const auto pending_guest_cycles =\n"
           "            katana::runtime::base_guest_cycles_per_instruction * pending_instructions;\n"
           "        const auto found = executable_blocks_.find(address);\n"
           "        if (!chainable_blocks_.contains(address) ||\n"
           "            found == executable_blocks_.end() ||\n"
           "            found->second.maximum_guest_cycles > local_block_chain_guest_cycle_budget ||\n"
           "            pending_guest_cycles > local_block_chain_guest_cycle_budget -\n"
           "                found->second.maximum_guest_cycles) return false;\n"
           "        const auto current_cycle = state_.scheduler->current_cycle();\n"
           "        const auto prospective_guest_cycles =\n"
           "            pending_guest_cycles + found->second.maximum_guest_cycles;\n"
           "        if (const auto remaining = state_.scheduler->remaining_guest_cycles();\n"
           "            remaining && prospective_guest_cycles > *remaining) return false;\n"
           "        if (prospective_guest_cycles >\n"
           "            std::numeric_limits<std::uint64_t>::max() - current_cycle) return false;\n"
           "        if (const auto event = state_.scheduler->next_event_cycle(); event &&\n"
           "            *event <= current_cycle + prospective_guest_cycles) return false;\n"
           "        try {\n"
           "            static_cast<void>(state_.address_space->translate(\n"
           "                address, katana::runtime::TranslationAccess::Instruction,\n"
           "                cpu_.privileged_mode()));\n"
           "            const auto current = katana::runtime::block_variant_key(\n"
           "                state_.address_space->guard_for(address, cpu_.read_fpscr()),\n"
           "                active_block_variant_->runtime_generation);\n"
           "            if (current != *active_block_variant_) return false;\n"
           "        } catch (...) { return false; }\n"
           "        return state_.code_tracker->dispatchable(found->second.identity);\n"
           "    }\n"
            "    katana::runtime::ExecutableCodeTracker* executable_code_tracker() noexcept "
           "override {\n"
            "        return state_.code_tracker.get();\n"
            "    }\n"
            "    katana::runtime::ExecutableModuleCatalog* executable_module_catalog() noexcept "
            "override {\n"
            "        return state_.module_catalog.get();\n"
            "    }\n"
           "  private:\n"
           "    katana::runtime::CpuState& cpu_;\n"
           "    const katana::runtime::DreamcastRuntimeState& state_;\n"
             "    std::function<katana::runtime::PlatformLifecycleState()> lifecycle_poll_;\n"
             "    std::function<void()> guest_frame_poll_;\n"
             "    katana::runtime::SystemReplayLog* replay_log_ = nullptr;\n"
            "    std::chrono::steady_clock::time_point next_lifecycle_poll_{};\n"
            "    std::chrono::steady_clock::time_point next_frame_poll_{};\n"
            "    katana::runtime::PlatformLifecycleState cached_lifecycle_ =\n"
            "        katana::runtime::PlatformLifecycleState::Running;\n"
           "    std::uint64_t executed_blocks_ = 0u;\n"
           "    std::uint64_t fallback_count_ = 0u;\n"
           "    struct ExecutableBlockRegistration {\n"
           "        std::string identity;\n"
           "        std::uint64_t maximum_guest_cycles = 0u;\n"
           "    };\n"
           "    static constexpr std::uint64_t local_block_chain_guest_cycle_budget = 4'096u;\n"
           "    std::unordered_map<std::uint32_t, ExecutableBlockRegistration> executable_blocks_;\n"
           "    std::unordered_set<std::uint32_t> chainable_blocks_;\n"
           "    std::optional<katana::runtime::BlockVariantKey> active_block_variant_;\n"
           "    std::uint64_t chain_retired_baseline_ = 0u;\n"
           "    bool guest_checkpoint_ = false;\n"
            "    bool eager_host_poll_ = false;\n"
             "    bool runtime_probe_mode_ = false;\n"
             "    bool local_block_chaining_enabled_ = false;\n"
            "};\n\n"
            "int run_deterministic_runtime_probe(\n"
            "        katana::runtime::CpuState& cpu,\n"
            "        const katana::runtime::DreamcastRuntimeState& state,\n"
             "        bool diagnostics_enabled,\n"
             "        RuntimeWaitLoopTraceSession& wait_loop_trace) {\n"
             "    katana::runtime::SystemReplayLog replay({\n"
             "        katana::runtime::SystemReplayConfig::maximum_capacity, false,\n"
             "        katana::runtime::SystemReplayProfile::DeterministicV1});\n"
             "    state.scheduler->attach_replay_log(replay);\n"
             "    replay.enable_coverage(static_cast<\n"
             "        katana::runtime::SystemReplayCoverageMask>(\n"
             "            katana::runtime::SystemReplayCoverage::SchedulerCallback));\n"
             "    auto input = std::make_shared<katana::runtime::InjectedHostInput>();\n"
            "    state.maple->attach(0u, 0u,\n"
            "        std::make_shared<katana::runtime::MapleControllerDevice>(input));\n"
            "    katana::runtime::RecordingHostAudioOutput audio;\n"
            "    PortPlatformServices services(\n"
            "        cpu, state, {}, {}, false, true, &replay);\n"
            "    katana::runtime::DreamcastMediaClock media_clock(\n"
            "        *state.scheduler, {},\n"
            "        [&](const katana::runtime::VideoTick& tick) {\n"
            "            record_runtime_probe_event(\n"
            "                replay, *state.scheduler,\n"
            "                katana::runtime::SystemReplayEventKind::Video,\n"
            "                \"video-tick\", tick.guest_cycle, tick.frame_index);\n"
            "        },\n"
            "        [&](const katana::runtime::AudioTick& tick) {\n"
            "            audio.submit(state.aica_registers->render_audio(\n"
            "                             tick.frame_count, 44'100u),\n"
            "                         44'100u);\n"
            "            record_runtime_probe_event(\n"
            "                replay, *state.scheduler,\n"
            "                katana::runtime::SystemReplayEventKind::Audio,\n"
             "                \"audio-tick\", tick.frame_count, tick.buffer_index);\n"
             "        });\n"
             "    replay.enable_coverage(\n"
             "        static_cast<katana::runtime::SystemReplayCoverageMask>(\n"
             "            katana::runtime::SystemReplayCoverage::Video) |\n"
             "        static_cast<katana::runtime::SystemReplayCoverageMask>(\n"
             "            katana::runtime::SystemReplayCoverage::Audio));\n"
             "    RuntimeProbeMmioTraceSession mmio_trace(cpu.memory, replay, *state.scheduler);\n"
             "    replay.enable_coverage(static_cast<\n"
             "        katana::runtime::SystemReplayCoverageMask>(\n"
             "            katana::runtime::SystemReplayCoverage::Input));\n"
             "    input->inject(1u, state.scheduler->current_cycle(), {});\n"
             "    replay.inject({\n"
             "        0u, state.scheduler->current_cycle(),\n"
             "        katana::runtime::SystemReplayEventKind::ExternalInput,\n"
             "        \"neutral-controller-input\", std::nullopt, std::nullopt,\n"
             "        0u, 0u, false, state.scheduler->reset_generation()});\n"
             "    media_clock.start();\n"
            "    auto termination = katana::runtime::RuntimeProbeTermination::Completed;\n"
            "    try {\n"
            "        static_cast<void>(" +
            entry_namespace +
            "::run_runtime(cpu, services, *state.runtime_blocks));\n"
            "    } catch (const katana::runtime::RuntimeProbeBudgetReached& reached) {\n"
            "        if (!reached.final_guest_cycle().has_value() ||\n"
            "            *reached.final_guest_cycle() != state.scheduler->current_cycle()) {\n"
            "            media_clock.stop();\n"
            "            throw std::runtime_error(\"runtime-probe-budget-cycle-mismatch\");\n"
            "        }\n"
            "        termination = katana::runtime::RuntimeProbeTermination::BudgetReached;\n"
            "    } catch (...) {\n"
             "        media_clock.stop();\n"
             "        throw;\n"
             "    }\n"
             "    media_clock.stop();\n"
             "    mmio_trace.finish();\n"
             "    wait_loop_trace.finish();\n"
            "    const auto dreamcast = katana::runtime::capture_runtime_probe_dreamcast(\n"
            "        state, audio.submitted_buffers(), audio.submitted_frames(),\n"
            "        audio.deterministic_hash());\n"
            "    const std::array memory{\n"
            "        katana::runtime::RuntimeProbeMemoryRange{\n"
            "            katana::runtime::RuntimeProbeMemoryRegion::MainRam, 0u,\n"
            "            state.main_ram->bytes()},\n"
            "        katana::runtime::RuntimeProbeMemoryRange{\n"
            "            katana::runtime::RuntimeProbeMemoryRegion::VideoRam, 0u,\n"
            "            state.vram->bytes()},\n"
            "        katana::runtime::RuntimeProbeMemoryRange{\n"
            "            katana::runtime::RuntimeProbeMemoryRegion::AicaRam, 0u,\n"
            "            state.aica_ram->bytes()}};\n"
            "    const std::array persistent{\n"
            "        katana::runtime::RuntimeProbeMemoryRange{\n"
            "            katana::runtime::RuntimeProbeMemoryRegion::Flash, 0u,\n"
            "            std::span<const std::uint8_t>(dreamcast.flash)},\n"
            "        katana::runtime::RuntimeProbeMemoryRange{\n"
            "            katana::runtime::RuntimeProbeMemoryRegion::Vmu, 0u,\n"
            "            std::span<const std::uint8_t>(dreamcast.vmu)}};\n"
            "    const auto cpu_snapshot = katana::runtime::capture_runtime_probe_cpu(cpu);\n"
            "    const auto scheduler_snapshot =\n"
            "        katana::runtime::capture_runtime_probe_scheduler(*state.scheduler);\n"
            "    auto replay_snapshot = katana::runtime::capture_runtime_probe_replay(replay);\n"
            "    const auto provisional = katana::runtime::make_runtime_probe_report(\n"
            "        cpu_snapshot, scheduler_snapshot, memory, persistent,\n"
            "        dreamcast.devices, replay_snapshot);\n"
            "    replay.seal(provisional.hashes.guest_state);\n"
            "    replay_snapshot = katana::runtime::capture_runtime_probe_replay(replay);\n"
            "    auto report = katana::runtime::make_runtime_probe_report(\n"
            "        cpu_snapshot, scheduler_snapshot, memory, persistent,\n"
            "        dreamcast.devices, replay_snapshot);\n"
            "    report.termination = termination;\n"
            "    report.diagnostics_enabled = diagnostics_enabled;\n"
            "    if (report.status != katana::runtime::RuntimeProbeStatus::Complete)\n"
            "        throw std::runtime_error(\"runtime-probe-incomplete\");\n"
            "    if (termination != katana::runtime::RuntimeProbeTermination::BudgetReached)\n"
            "        throw std::runtime_error(\"runtime-probe-budget-not-reached\");\n"
            "    if (!report.guest_cycle_budget.has_value() ||\n"
            "        report.guest_cycle != *report.guest_cycle_budget)\n"
            "        throw std::runtime_error(\"runtime-probe-budget-not-exact\");\n"
             "    std::cout << \"KATANA_RUNTIME_PROBE \"\n"
            "              << katana::runtime::serialize_runtime_probe_report_json(report)\n"
            "              << '\\n';\n"
            "    return 0;\n"
            "}\n\n"
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
           "        std::error_code executable_error;\n"
           "        auto executable = std::filesystem::weakly_canonical(argv[0], "
           "executable_error);\n"
           "        if (executable_error || executable.empty())\n"
           "            executable = std::filesystem::absolute(argv[0]);\n"
           "        const auto port_root = executable.parent_path();\n"
           "        const bool deterministic_runtime_probe =\n"
           "            deterministic_runtime_probe_requested();\n"
           "        validate_runtime_probe_environment(deterministic_runtime_probe);\n"
           "        bool gdi_debug = false;\n"
           "        bool install_disc = false;\n"
           "        const auto recipe_path = port_root / \"content\" / \"game.katana-install\";\n"
           "        if (argc == 1) {\n"
           "            source = port_root / \"user-data\" / \"content\" / \"game.katana-disc\";\n"
           "        } else if (argc == 3 && std::string_view(argv[1]) == \"--content\") {\n"
           "            source = argv[2];\n"
           "        } else if (argc == 3 && std::string_view(argv[1]) == \"--install-disc\") {\n"
           "            source = argv[2]; install_disc = true;\n"
           "        } else if (argc == 3 && std::string_view(argv[1]) == \"--gdi-debug\") {\n"
           "            source = argv[2]; gdi_debug = true;\n"
           "        } else {\n"
           "            std::cerr << \"Aufruf: game [--install-disc <eigene.gdi>] "
           "[--content <lokaler-cache>] [--gdi-debug <disc.gdi>]\\n\";\n"
           "            return 2;\n"
           "        }\n"
           "        if (install_disc) {\n"
           "            try {\n"
           "                const auto recipe = "
           "katana::runtime::parse_disc_install_recipe(recipe_path);\n"
           "                verify_recipe_identity(recipe);\n"
           "                const auto destination = port_root / \"user-data\" / \"content\" / "
           "\"game.katana-disc\";\n"
           "                const auto info = katana::runtime::install_disc_content(recipe, "
           "source, destination);\n"
           "                auto packed = katana::runtime::PackedDiscSource::open(destination);\n"
           "                verify_pack_identity(*packed);\n"
           "                const auto boot = katana::runtime::load_dreamcast_runtime_boot(\n"
           "                    packed, packed->primary_data_lba(), "
           "packed->info().tracks.size());\n"
           "                verify_boot_identity(boot);\n"
           "                std::cout << \"KATANA_DISC_INSTALL_OK tracks=\" << info.tracks.size()\n"
           "                          << \" sectors=\" << info.packed_sectors << '\\n';\n"
           "                return 0;\n"
           "            } catch (const std::exception&) {\n"
           "                throw std::runtime_error(\"source-identity-mismatch\");\n"
           "            }\n"
           "        }\n"
           "        katana::runtime::DreamcastRuntimeBootImage boot;\n"
           "        try {\n"
           "            if (gdi_debug) {\n"
           "                boot = katana::runtime::load_dreamcast_runtime_boot(source);\n"
           "                verify_source_identity(source, boot);\n"
           "            } else {\n"
           "                auto packed = katana::runtime::PackedDiscSource::open(source);\n"
           "                verify_pack_identity(*packed);\n"
           "                boot = katana::runtime::load_dreamcast_runtime_boot(\n"
           "                    packed, packed->primary_data_lba(), "
           "packed->info().tracks.size());\n"
           "            }\n"
           "            verify_boot_identity(boot);\n"
           "        } catch (const std::exception&) { throw "
           "std::runtime_error(\"source-identity-mismatch\"); }\n"
           "        katana::runtime::DreamcastMutableStorageConfig storage_config;\n"
           "        storage_config.project_identity = expected_project_identity;\n"
           "        storage_config.storage_root = port_root / \"user-data\";\n"
           "        constexpr auto console_profile =\n"
           "            katana::runtime::DreamcastConsoleProfile::" +
           std::string(console_profile_enumerator(console_profile)) +
           ";\n"
           "        storage_config.region =\n"
           "            katana::runtime::dreamcast_region_for_console_profile(console_profile);\n"
           "        auto mutable_storage = katana::runtime::DreamcastMutableStorage::open(\n"
           "            std::move(storage_config));\n"
           "        katana::runtime::CpuState cpu;\n"
           "        const auto state = katana::runtime::initialize_dreamcast_runtime(\n"
           "            cpu, boot, katana::runtime::DreamcastRuntimeFirmwareMode::" +
           std::string(hle_bios_abi ? "HleBiosAbi" : "Direct") +
           ", mutable_storage, console_profile);\n"
           "        const auto* diagnostics_value = std::getenv(\"KATANA_PORT_DIAGNOSTICS\");\n"
           "        const bool detailed_diagnostics = diagnostics_value != nullptr &&\n"
           "            std::string_view(diagnostics_value) == \"1\";\n"
           "        cpu.memory.set_mmio_access_tracking(detailed_diagnostics);\n"
           "        const auto* wait_loop_trace_value =\n"
           "            std::getenv(\"KATANA_PORT_WAIT_LOOP_TRACE\");\n"
           "        const bool wait_loop_trace_enabled = wait_loop_trace_value != nullptr &&\n"
           "            std::string_view(wait_loop_trace_value) == \"1\";\n"
           "        RuntimeWaitLoopTraceSession wait_loop_trace(\n"
           "            cpu.memory, wait_loop_trace_enabled);\n"
           "        if (deterministic_runtime_probe)\n"
           "            return run_deterministic_runtime_probe(\n"
           "                cpu, state, detailed_diagnostics, wait_loop_trace);\n"
           "        auto input = std::make_shared<katana::runtime::InjectedHostInput>();\n"
           "        state.maple->attach(0u, 0u,\n"
           "            std::make_shared<katana::runtime::MapleControllerDevice>(input));\n"
           "        std::unique_ptr<katana::runtime::NativeVideoOutput> video;\n"
           "        if (katana::runtime::native_video_available()) {\n"
           "            video = katana::runtime::create_native_video_output(\n"
           "                {katana::runtime::native_video_contract_version,\n"
           "                 \"KatanaRecomp Game\", 640u, 480u, true});\n"
           "        }\n"
           "        std::uint64_t presented_frames = 0u;\n"
           "        bool first_guest_frame_reported = false;\n"
           "        bool first_presented_frame_reported = false;\n"
           "        std::uint64_t host_sequence = 1u;\n"
           "        katana::runtime::ControllerState controller;\n"
           "        const auto* lifecycle_test_value = "
           "std::getenv(\"KATANA_PORT_LIFECYCLE_TEST\");\n"
           "        const auto* ignore_focus_value = std::getenv(\"KATANA_PORT_IGNORE_FOCUS\");\n"
           "        const bool ignore_focus = ignore_focus_value != nullptr &&\n"
           "            std::string_view(ignore_focus_value) == \"1\";\n"
           "        const std::string_view lifecycle_test = lifecycle_test_value == nullptr\n"
           "            ? std::string_view{} : std::string_view(lifecycle_test_value);\n"
           "        std::size_t lifecycle_test_step = 0u;\n"
            "        katana::runtime::HostPacer pacer;\n"
           "        std::unique_ptr<katana::runtime::HostAudioOutput> audio =\n"
           "            katana::runtime::native_audio_available()\n"
           "                ? katana::runtime::create_native_audio_output()\n"
           "                : std::make_unique<katana::runtime::RecordingHostAudioOutput>();\n"
           "        katana::runtime::DreamcastMediaClock media_clock(\n"
            "            *state.scheduler, {},\n"
            "            [&](const katana::runtime::VideoTick& tick) {\n"
            "                pacer.pace(tick.guest_cycle);\n"
            "            },\n"
           "            [&](const katana::runtime::AudioTick& tick) {\n"
           "                audio->submit(state.aica_registers->render_audio(\n"
           "                                  tick.frame_count, 44'100u),\n"
           "                              44'100u);\n"
           "            });\n"
           "        katana::runtime::HostRuntimeSession host(\n"
           "            *state.scheduler, media_clock, input, *audio, &pacer,\n"
           "            [mutable_storage] { mutable_storage->save(); });\n"
           "        host.inject({host_sequence++, state.scheduler->current_cycle(),\n"
           "                     katana::runtime::HostRuntimeEventKind::Resume, {}});\n"
           "        const auto pump_guest_frame = [&] {\n"
           "            const auto result = katana::runtime::pump_guest_frame_proof(\n"
           "                *state.pvr_renderer, video.get());\n"
           "            if (result.guest_frame_proven && !first_guest_frame_reported) {\n"
            "                std::cout << \"KR_FIRST_GUEST_FRAME\\n\";\n"
            "                first_guest_frame_reported = true;\n"
           "            }\n"
           "            if (video) presented_frames = video->presented_frames();\n"
           "            if (result.frame_presented &&\n"
           "                !first_presented_frame_reported) {\n"
           "                std::cout << \"KR_FIRST_PRESENTED_FRAME\\n\";\n"
           "                first_presented_frame_reported = true;\n"
           "            }\n"
           "        };\n"
           "        const auto pump_host_events = [&] {\n"
           "            if (host.state() == katana::runtime::HostRuntimeState::Shutdown)\n"
           "                return;\n"
           "            if (!lifecycle_test.empty()) {\n"
           "                const auto inject = [&](katana::runtime::HostRuntimeEventKind kind) {\n"
           "                    host.inject({host_sequence++, state.scheduler->current_cycle(),\n"
           "                                 kind, controller});\n"
           "                };\n"
           "                if (lifecycle_test == \"running-close\") {\n"
           "                    if (lifecycle_test_step++ == 0u) {\n"
           "                        controller.pressed_buttons = 0x0004u;\n"
           "                        inject(katana::runtime::HostRuntimeEventKind::Controller);\n"
           "                        controller = {};\n"
           "                        inject(katana::runtime::HostRuntimeEventKind::Controller);\n"
           "                    } else inject(katana::runtime::HostRuntimeEventKind::Shutdown);\n"
           "                } else if (lifecycle_test == \"focus-resume-close\") {\n"
           "                    const auto step = lifecycle_test_step++;\n"
           "                    if (step == 0u) "
           "inject(katana::runtime::HostRuntimeEventKind::FocusLost);\n"
           "                    else if (step == 1u) "
           "inject(katana::runtime::HostRuntimeEventKind::FocusGained);\n"
           "                    else inject(katana::runtime::HostRuntimeEventKind::Shutdown);\n"
           "                } else if (lifecycle_test == \"paused-close\") {\n"
           "                    inject(lifecycle_test_step++ == 0u\n"
           "                        ? katana::runtime::HostRuntimeEventKind::FocusLost\n"
           "                        : katana::runtime::HostRuntimeEventKind::Shutdown);\n"
           "                } else {\n"
           "                    throw std::runtime_error(\"Unbekannter Lifecycle-Testmodus.\");\n"
           "                }\n"
           "                return;\n"
           "            }\n"
           "            if (!video) return;\n"
           "            video->poll_events();\n"
           "            for (const auto& event : video->drain_events()) {\n"
           "                if (host.state() == katana::runtime::HostRuntimeState::Shutdown) "
           "break;\n"
           "                auto kind = katana::runtime::HostRuntimeEventKind::Controller;\n"
           "                if (event.kind == katana::runtime::NativeHostEventKind::FocusGained)\n"
           "                    kind = katana::runtime::HostRuntimeEventKind::FocusGained;\n"
           "                else if (event.kind == "
           "katana::runtime::NativeHostEventKind::FocusLost)\n"
           "                    { if (ignore_focus) continue; "
           "kind = katana::runtime::HostRuntimeEventKind::FocusLost; controller = {}; }\n"
           "                else if (event.kind == katana::runtime::NativeHostEventKind::Close)\n"
           "                    kind = katana::runtime::HostRuntimeEventKind::Shutdown;\n"
           "                else {\n"
           "                    const auto down =\n"
           "                        event.kind == katana::runtime::NativeHostEventKind::KeyDown;\n"
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
           "                host.inject({host_sequence++, state.scheduler->current_cycle(), kind, "
           "controller});\n"
           "            }\n"
           "        };\n"
            "        PortPlatformServices services(cpu, state, [&] {\n"
           "            pump_host_events();\n"
           "            if (host.state() == katana::runtime::HostRuntimeState::Shutdown)\n"
           "                return katana::runtime::PlatformLifecycleState::Shutdown;\n"
           "            if (host.state() == katana::runtime::HostRuntimeState::Paused)\n"
           "                return katana::runtime::PlatformLifecycleState::Paused;\n"
            "            return katana::runtime::PlatformLifecycleState::Running;\n"
            "        }, pump_guest_frame, !lifecycle_test.empty());\n"
           "        auto report_progress = [&] {\n"
           "            const auto gdrom_status = state.gdrom->status();\n"
           "            const auto g1_dma = state.holly_dma.g1->state();\n"
           "            const auto pvr_dma = state.holly_dma.pvr->state();\n"
           "            const auto pvr = state.pvr_registers->snapshot();\n"
           "            const auto system_bus = state.system_bus_control->snapshot();\n"
           "            const auto active_irq = state.interrupt_controller->highest_pending();\n"
           "            const auto& materialization = " +
           entry_namespace +
           "::runtime_materialization_status();\n"
           "            std::cerr << \"KATANA_PORT_PROGRESS pc=\" << cpu.pc\n"
           "                      << \" hardware_cycles=\" << state.scheduler->current_cycle()\n"
           "                      << \" executed_blocks=\" << services.executed_blocks()\n"
           "                      << \" retired_guest_instructions=\"\n"
           "                      << cpu.retired_guest_instructions\n"
           "                      << \" exception_cause=\"\n"
           "                      << static_cast<unsigned>(cpu.last_exception_cause)\n"
           "                      << \" expevt=\" << cpu.expevt\n"
           "                      << \" intevt=\" << cpu.intevt\n"
           "                      << \" tea=\" << cpu.tea\n"
           "                      << \" spc=\" << cpu.spc\n"
           "                      << \" ssr=\" << cpu.ssr\n"
           "                      << \" vbr=\" << cpu.vbr;\n"
           "            for (std::size_t register_index = 0u; register_index < cpu.r.size();\n"
           "                 ++register_index)\n"
           "                std::cerr << \" r\" << register_index << '='\n"
           "                          << cpu.r[register_index];\n"
           "            std::cerr << \" pr=\" << cpu.pr\n"
           "                      << \" gbr=\" << cpu.gbr\n"
           "                      << \" sr=\" << cpu.read_sr()\n"
           "                      << \" fpscr=\" << cpu.read_fpscr()\n"
           "                      << \" mach=\" << cpu.mach\n"
           "                      << \" macl=\" << cpu.macl\n"
           "                      << \" fpul=\" << cpu.fpul\n"
           "                      << \" frames=\" << presented_frames\n"
           "                      << \" ta_transfers=\" << state.store_queue_transfers->size()\n"
           "                      << \" ta_transfers_dropped=\"\n"
           "                      << *state.dropped_store_queue_transfers\n"
           "                      << \" pvr_fb_r_ctrl=\"\n"
           "                      << pvr.framebuffer_read_control\n"
           "                      << \" pvr_fb_r_size=\"\n"
           "                      << pvr.framebuffer_read_size\n"
           "                      << \" pvr_fb_r_sof1=\"\n"
           "                      << pvr.framebuffer_read_sof1\n"
           "                      << \" pvr_render_requests=\"\n"
           "                      << pvr.render_requests\n"
           "                      << \" pvr_render_completions=\"\n"
           "                      << pvr.render_completions\n"
           "                      << \" pvr_software_frames=\"\n"
           "                      << state.pvr_renderer->metrics().frames\n"
           "                      << \" pvr_guest_frames=\"\n"
           "                      << state.pvr_renderer->metrics().proven_guest_frames\n"
           "                      << \" pvr_direct_frames=\"\n"
           "                      << state.pvr_renderer->metrics().direct_scanout_frames\n"
           "                      << \" pvr_direct_changed_pixels=\"\n"
           "                      << state.pvr_renderer->metrics().direct_scanout_changed_pixels\n"
           "                      << \" pvr_changed_pixels=\"\n"
           "                      << state.pvr_renderer->metrics().changed_pixels\n"
           "                      << \" pvr_ta_packets=\" << state.pvr_ta_fifo->metrics().packets\n"
           "                      << \" pvr_ta_vertices=\" << state.pvr_ta_fifo->metrics().vertices\n"
           "                      << \" pvr_yuv_macroblocks=\"\n"
           "                      << state.pvr_yuv_converter->converted_macroblocks();\n"
           "            std::cerr << \" irq_pending_count=\"\n"
           "                      << state.interrupt_controller->pending_count()\n"
           "                      << \" irq_source=\"\n"
           "                      << (active_irq ? active_irq->source : 0u)\n"
           "                      << \" irq_level=\"\n"
           "                      << (active_irq ? static_cast<unsigned>(active_irq->level) : 0u)\n"
           "                      << \" irq_event=\"\n"
           "                      << (active_irq ? active_irq->event_code : 0u);\n"
           "            std::cerr << \" dmac2_source=\" << state.dmac->source(2u)\n"
           "                      << \" dmac2_destination=\" << state.dmac->destination(2u)\n"
           "                      << \" dmac2_count=\" << state.dmac->count(2u)\n"
           "                      << \" dmac2_control=\" << state.dmac->control(2u)\n"
           "                      << \" dmac_operation=\" << state.dmac->operation()\n"
           "                      << \" channel2_start=\"\n"
           "                      << system_bus.channel2_start\n"
           "                      << \" channel2_length=\"\n"
           "                      << system_bus.channel2_length;\n"
           "            std::cerr << \" g1_dma_active=\" << g1_dma.active\n"
           "                      << \" g1_dma_address=\" << g1_dma.system_address\n"
           "                      << \" g1_dma_remaining=\" << g1_dma.remaining\n"
           "                      << \" pvr_dma_active=\" << pvr_dma.active\n"
           "                      << \" pvr_dma_system_address=\" << pvr_dma.system_address\n"
           "                      << \" pvr_dma_address=\" << pvr_dma.peripheral_address\n"
           "                      << \" pvr_dma_remaining=\" << pvr_dma.remaining;\n"
           "            for (std::size_t channel = 0u; channel < 4u; ++channel) {\n"
           "                const auto& g2 = state.holly_dma.g2->channel_state(channel);\n"
           "                std::cerr << \" g2_\" << channel << \"_active=\" << g2.active\n"
           "                          << \" g2_\" << channel << \"_remaining=\"\n"
           "                          << g2.remaining;\n"
           "            }\n"
           "            std::cerr << \" gdrom_ata_status=\"\n"
           "                      << static_cast<unsigned>(gdrom_status.ata_status)\n"
           "                      << \" gdrom_interrupt_reason=\"\n"
           "                      << static_cast<unsigned>(gdrom_status.interrupt_reason)\n"
           "                      << \" gdrom_pio_bytes=\" << gdrom_status.pio_bytes_available\n"
           "                      << \" gdrom_bios_requests=\" << gdrom_status.bios_requests\n"
           "                      << \" gdrom_commands=\" << gdrom_status.completed_commands\n"
           "                      << \" gdrom_dma=\" << gdrom_status.completed_dma;\n"
           "            std::cerr << \" materializer_requests=\" << materialization.requests\n"
           "                      << \" materializer_cache_hits=\" << materialization.cache_hits\n"
           "                      << \" materializations=\" << materialization.materializations\n"
           "                      << \" interpreter_materializations=\"\n"
           "                      << materialization.interpreter_materializations\n"
           "                      << \" materializer_misses=\" << materialization.misses\n"
           "                      << \" materializer_budget_failures=\"\n"
           "                      << materialization.budget_failures\n"
           "                      << \" materializer_first_failure=\"\n"
           "                      << materialization.first_failure\n"
           "                      << \" materializer_first_target=\"\n"
           "                      << materialization.first_failure_target;\n"
           "            if (const auto mmio = cpu.memory.last_mmio_access(); mmio)\n"
           "                std::cerr << \" last_mmio_operation=\"\n"
           "                          << (mmio->operation == katana::runtime::MemoryAccessOperation::Read\n"
           "                                  ? \"read\" : \"write\")\n"
           "                          << \" last_mmio_address=\" << mmio->address\n"
           "                          << \" last_mmio_width=\"\n"
           "                          << static_cast<unsigned>(mmio->width)\n"
           "                          << \" last_mmio_value=\" << mmio->value\n"
           "                          << \" last_mmio_region=\" << mmio->region_name;\n"
           "            std::cerr << \" aica_pending=\" << state.aica->interrupts().pending()\n"
           "                      << \" aica_enabled=\" << state.aica->interrupts().enabled()\n"
           "                      << \" aica_active_channels=\"\n"
           "                      << state.aica_registers->active_channel_count()\n"
           "                      << \" aica_audio_buffers=\"\n"
           "                      << state.aica_registers->rendered_buffer_count()\n"
           "                      << \" aica_rtc=\" << state.aica_rtc->counter()\n"
           "                      << \" aica_rtc_write_enabled=\"\n"
           "                      << state.aica_rtc->write_enabled();\n"
           "            if (const auto& error = state.pvr_renderer->first_error(); error)\n"
           "                std::cerr << \" pvr_first_error=\"\n"
           "                          << katana::runtime::pvr_render_error_name(error->error)\n"
           "                          << \" pvr_error_request=\" << error->render_request\n"
           "                          << \" pvr_error_detail=\" << error->detail;\n"
           "            if (const auto* probes = std::getenv(\"KATANA_PORT_MEMORY_PROBES\");\n"
           "                probes != nullptr && *probes != '\\0') {\n"
           "                const char* cursor = probes;\n"
           "                for (std::size_t index = 0u; index < 16u && *cursor != '\\0'; ++index) {\n"
           "                    errno = 0; char* end = nullptr;\n"
           "                    const auto parsed = std::strtoull(cursor, &end, 0);\n"
           "                    if (errno != 0 || end == cursor || parsed > 0xFFFFFFFFull ||\n"
           "                        (*end != '\\0' && *end != ',')) {\n"
           "                        std::cerr << \" memory_probe_parse_error=1\"; break;\n"
           "                    }\n"
           "                    const auto address = static_cast<std::uint32_t>(parsed);\n"
           "                    std::cerr << \" memory_probe_address=\" << address;\n"
           "                    try {\n"
           "                        const std::array<const katana::runtime::MemoryDevice*, 3u>\n"
           "                            permitted{state.main_ram.get(), state.vram.get(),\n"
           "                                      state.aica_ram.get()};\n"
           "                        std::cerr << \" memory_probe_value=\"\n"
           "                                  << katana::runtime::peek_guest_u32(\n"
           "                                         cpu, address, permitted);\n"
           "                    }\n"
           "                    catch (...) { std::cerr << \" memory_probe_error=1\"; }\n"
           "                    if (*end == '\\0') break; cursor = end + 1;\n"
           "                }\n"
           "            }\n"
           "            std::cerr << '\\n';\n"
           "        };\n"
           "        " +
           entry_namespace +
           "::RuntimeRunResult result;\n"
           "        try {\n"
           "            result = " +
           entry_namespace +
           "::run_runtime(cpu, services, *state.runtime_blocks);\n"
           "        } catch (const katana::runtime::PlatformLifecycleExit&) {\n"
           "            report_progress();\n"
           "            if (state.gdrom)\n"
           "                std::cerr << \"KATANA_GDROM_BIOS_EVENTS \"\n"
           "                          << state.gdrom->format_bios_call_events_json() << '\\n';\n"
           "            host.shutdown();\n"
           "            host.require_clean_shutdown();\n"
           "            throw;\n"
           "        } catch (...) {\n"
           "            report_progress();\n"
           "            throw;\n"
           "        }\n"
           "        if (host.state() == katana::runtime::HostRuntimeState::Shutdown) {\n"
           "            host.require_clean_shutdown();\n"
           "            if (state.scheduler->pending_event_count() != 0u)\n"
           "                throw std::runtime_error(\"Host-Shutdown hinterliess "
           "Schedulerereignisse.\");\n"
           "            std::cout << \"KR_HOST_SHUTDOWN guest_dispatch_stopped=1 host_events=\"\n"
           "                      << host.processed_events() << \" input_events=\"\n"
           "                      << input->injected_events() << '\\n';\n"
           "            return 0;\n"
           "        }\n"
           "        const std::uint64_t silent_failures =\n"
           "            (state.loaded_boot_bytes == 0u ? 1u : 0u) +\n"
           "            (result.scheduler_cycle == 0u ? 1u : 0u) +\n"
           "            (!services.guest_checkpoint_reached() ? 1u : 0u) +\n"
           "            (cpu.trap_pending ? 1u : 0u) +\n"
           "            (cpu.last_exception_cause != katana::runtime::ExceptionCause::None ? 1u : "
           "0u) +\n"
           "            services.fallback_count();\n"
           "        if (silent_failures != 0u) {\n"
           "            throw std::runtime_error(\"Runtime-Einstieg besitzt keinen "
           "Dispatchnachweis.\");\n"
           "        }\n"
           "        const auto audio_buffers = audio->submitted_buffers();\n"
           "        const auto audio_hash = audio->deterministic_hash();\n"
           "        const auto input_events = input->injected_events();\n"
           "        host.shutdown();\n"
           "        host.require_clean_shutdown();\n"
           "        if (state.scheduler->pending_event_count() != 0u)\n"
           "            throw std::runtime_error(\"Host-Shutdown hinterliess "
           "Schedulerereignisse.\");\n"
           "        if (diagnostic_partial_port) {\n"
           "            std::cout << \"KR_DIAGNOSTIC_PARTIAL_RUNTIME_REACHED guest_cycles=\"\n"
           "                      << result.scheduler_cycle << \" executed_blocks=\"\n"
           "                      << services.executed_blocks() << '\\n';\n"
           "            return 3;\n"
           "        }\n"
           "        std::cout << \"KR_GUEST_PROGRAM_ENTERED\\n\";\n"
           "        std::cout << \"KATANA_RUNTIME_METRICS silent_failures=\"\n"
           "                  << silent_failures << \" guest_cycles=\"\n"
           "                  << result.scheduler_cycle << \" indirect_dispatches=\"\n"
           "                  << result.indirect_dispatches\n"
           "                  << \" runtime_dispatch_hits=\"\n"
           "                  << result.runtime_dispatch_hits\n"
           "                  << \" runtime_dispatch_misses=\"\n"
           "                  << result.runtime_dispatch_misses\n"
           "                  << \" runtime_dispatch_fallbacks=\"\n"
           "                  << result.runtime_dispatch_fallbacks\n"
           "                  << \" runtime_only_dispatch_hits=\"\n"
           "                  << result.runtime_only_dispatch_hits\n"
           "                  << \" runtime_only_dispatch_misses=\"\n"
           "                  << result.runtime_only_dispatch_misses\n"
           "                  << \" runtime_only_dispatch_fallbacks=\"\n"
           "                  << result.runtime_only_dispatch_fallbacks\n"
           "                  << \" runtime_only_sites=\" << result.runtime_only_sites\n"
           "                  << \" runtime_only_dispatch_share_ppm=\"\n"
           "                  << result.runtime_only_dispatch_share_ppm\n"
           "                  << \" runtime_dispatch_first_error=\"\n"
           "                  << result.runtime_dispatch_first_error << \" frames=\"\n"
           "                  << presented_frames << \" audio_buffers=\" << audio_buffers\n"
           "                  << \" audio_hash=\" << audio_hash\n"
           "                  << \" input_events=\" << input_events\n"
           "                  << \" executed_blocks=\" << services.executed_blocks()\n"
           "                  << \" guest_cycle_contract=\" << result.guest_cycle_contract\n"
           "                  << \" guest_checkpoint=1 fallback_count=\"\n"
           "                  << services.fallback_count() << '\\n';\n"
           "        std::cout << \"KR_GENERATED_RUNTIME_STARTED boot_bytes=\"\n"
           "                  << state.loaded_boot_bytes << \" indirect_dispatches=\"\n"
           "                  << result.indirect_dispatches << \" final_pc=\" << result.final_pc "
           "<< '\\n';\n"
           "        return 0;\n"
           "    } catch (const katana::runtime::PlatformLifecycleExit& exit) {\n"
           "        const auto& evidence = exit.evidence();\n"
           "        const auto* reason = exit.reason() ==\n"
           "                katana::runtime::PlatformLifecycleExitReason::Reset ? \"reset\"\n"
           "            : exit.reason() ==\n"
           "                katana::runtime::PlatformLifecycleExitReason::BiosMenu\n"
           "                ? \"bios-menu\" : \"cd-menu\";\n"
           "        std::cerr << \"KATANA_PLATFORM_LIFECYCLE_EXIT {\\\"reason\\\":\\\"\"\n"
           "                  << reason << \"\\\",\\\"guest_cycle\\\":\"\n"
           "                  << evidence.guest_cycle << \" ,\\\"callsite\\\":\"\n"
           "                  << evidence.callsite << \" ,\\\"return_address\\\":\"\n"
           "                  << evidence.return_address << \" ,\\\"registers\\\":[\";\n"
           "        for (std::size_t index = 0u; index < evidence.registers.size(); ++index) {\n"
           "            if (index != 0u) std::cerr << ',';\n"
           "            std::cerr << evidence.registers[index];\n"
           "        }\n"
           "        std::cerr << \"],\\\"last_gdrom_request\\\":\"\n"
           "                  << evidence.last_gdrom_request\n"
           "                  << \" ,\\\"last_gdrom_command\\\":\"\n"
           "                  << evidence.last_gdrom_command\n"
           "                  << \" ,\\\"last_gdrom_state\\\":\"\n"
           "                  << evidence.last_gdrom_state\n"
           "                  << \" ,\\\"last_gdrom_status\\\":[\";\n"
           "        for (std::size_t index = 0u; index < evidence.last_gdrom_status.size();\n"
           "             ++index) {\n"
           "            if (index != 0u) std::cerr << ',';\n"
           "            std::cerr << evidence.last_gdrom_status[index];\n"
           "        }\n"
           "        std::cerr << \"]}\\n\";\n"
           "        return 4;\n"
           "    } catch (const katana::runtime::HostPacingException& error) {\n"
           "        std::cerr << \"KATANA_HOST_PACING_ERROR \" << error.serialize_json() << "
           "'\\n';\n"
           "        return 1;\n"
           "    } catch (const katana::runtime::IndirectDispatchError& error) {\n"
           "        std::cerr << \"KATANA_RUNTIME_DISPATCH_ERROR \"\n"
           "                  << error.metrics_json() << '\\n';\n"
           "        std::cerr << \"Portlauf fehlgeschlagen: \"\n"
           "                  << redact_source(error.what(), source) << '\\n';\n"
           "        return 1;\n"
           "    } catch (const std::exception& error) {\n"
           "        std::cerr << \"Portlauf fehlgeschlagen: \"\n"
           "                  << redact_source(error.what(), source) << '\\n';\n"
           "        return 1;\n"
           "    }\n"
           "}\n";
}

std::vector<ProjectArtifact>
runtime_dispatch_artifacts(const std::string& entry_namespace,
                           const std::span<const katana::ir::Function> program,
                           const std::uint32_t entry_address,
                           const bool diagnostic_interpreter,
                           const std::span<const katana::analysis::RuntimeCodeCopy>
                               runtime_code_copies,
                           const katana::io::ExecutableImage& image,
                           const std::uint32_t boot_address,
                           const std::size_t boot_size) {
    const auto symbol = [](const std::uint32_t address) {
        constexpr std::array digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
        std::string result(8u, '0');
        for (std::size_t index = 0u; index < result.size(); ++index)
            result[result.size() - index - 1u] = digits[(address >> (index * 4u)) & 0xFu];
        return result;
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
            return "Return";
        case O::ReturnFromException:
            return "ExceptionReturn";
        case O::Sleep:
            return "Sleep";
        case O::TrapAlways:
            return "Exception";
        default:
            return "Fallthrough";
        }
    };
    const auto dispatch_site_class = [](const katana::ir::DynamicTargetClass target_class) {
        using C = katana::ir::DynamicTargetClass;
        switch (target_class) {
        case C::GuardedComplete:
        case C::GuardedPartial:
            return "Guarded";
        case C::RuntimeOnly:
            return "RuntimeOnly";
        case C::Unresolved:
            return "Unresolved";
        case C::NotApplicable:
            return "NotDynamic";
        }
        return "NotDynamic";
    };
    struct NativeTemplateEmission {
        struct PatchTarget {
            std::uint32_t live_value = 0u;
            std::uint32_t block_address = 0u;

            auto operator<=>(const PatchTarget&) const = default;
        };

        std::string_view module_id;
        std::string expected_source_identity;
        std::uint32_t source_start = 0u;
        std::uint32_t extent = 0u;
        std::int32_t destination_vbr_delta = 0;
        std::map<std::uint32_t, std::vector<PatchTarget>> patch_targets;
    };
    const auto range_contains = [](const std::uint32_t outer_start,
                                   const std::uint64_t outer_size,
                                   const std::uint32_t inner_start,
                                   const std::uint32_t inner_size) {
        const auto outer_end = static_cast<std::uint64_t>(outer_start) + outer_size;
        const auto inner_end = static_cast<std::uint64_t>(inner_start) + inner_size;
        return inner_size != 0u && outer_end <= 0x1'0000'0000ull &&
               inner_end <= 0x1'0000'0000ull && inner_start >= outer_start &&
               inner_end <= outer_end;
    };
    std::vector<NativeTemplateEmission> native_templates;
    for (const auto& copy : runtime_code_copies) {
        if (copy.source_byte_count == 0u ||
            static_cast<std::uint64_t>(copy.source_begin) + copy.source_byte_count >
                0x1'0000'0000ull ||
            copy.source_end_inclusive !=
                copy.source_begin + copy.source_byte_count - sizeof(std::uint32_t))
            throw std::runtime_error("Runtime-Codecopy besitzt ungueltige Quellgrenzen.");
        std::string_view module_id;
        std::uint32_t module_start = 0u;
        std::size_t module_size = 0u;
        if (range_contains(katana::runtime::dreamcast_system_bootstrap_address,
                           katana::runtime::dreamcast_system_bootstrap_size,
                           copy.source_begin,
                           copy.source_byte_count)) {
            module_id = katana::runtime::dreamcast_initial_disc_bootstrap_module_id;
            module_start = katana::runtime::dreamcast_system_bootstrap_address;
            module_size = katana::runtime::dreamcast_system_bootstrap_size;
        } else if (range_contains(
                       boot_address, boot_size, copy.source_begin, copy.source_byte_count)) {
            module_id = katana::runtime::dreamcast_initial_boot_executable_module_id;
            module_start = boot_address;
            module_size = boot_size;
        } else {
            throw std::runtime_error(
                "Runtime-Codecopy liegt ausserhalb lokal gebundener Disc-Bootmodule.");
        }
        const auto* module_segment = image.find_segment(module_start, module_size);
        if (module_segment == nullptr) {
            throw std::runtime_error(
                "Runtime-Codecopy besitzt kein vollstaendiges lokales Quellsegment.");
        }
        const auto module_offset = module_segment->byte_offset(module_start);
        if (!module_offset.has_value() || *module_offset > module_segment->bytes.size() ||
            module_size > module_segment->bytes.size() - *module_offset) {
            throw std::runtime_error(
                "Runtime-Codecopy-Quellsegment besitzt keine vollstaendigen Modulbytes.");
        }
        const auto expected_source_identity =
            "sha256:" + katana::io::sha256_bytes(std::string_view(
                            reinterpret_cast<const char*>(module_segment->bytes.data() +
                                                          *module_offset),
                            module_size));
        auto existing = std::find_if(native_templates.begin(),
                                     native_templates.end(),
                                     [&](const auto& candidate) {
                                         return candidate.module_id == module_id &&
                                                candidate.expected_source_identity ==
                                                    expected_source_identity &&
                                                candidate.source_start == copy.source_begin &&
                                                candidate.extent == copy.source_byte_count &&
                                                candidate.destination_vbr_delta ==
                                                    copy.destination_vbr_delta;
                                     });
        if (existing == native_templates.end()) {
            native_templates.push_back({module_id,
                                        expected_source_identity,
                                        copy.source_begin,
                                        copy.source_byte_count,
                                        copy.destination_vbr_delta,
                                        {}});
            existing = std::prev(native_templates.end());
        }
        for (const auto& patch : copy.patch_candidates) {
            if (patch.slot_address < copy.source_begin ||
                static_cast<std::uint64_t>(patch.slot_address) + sizeof(std::uint32_t) >
                    static_cast<std::uint64_t>(copy.source_begin) + copy.source_byte_count)
                throw std::runtime_error("Runtime-Codecopy-Patchslot liegt ausserhalb der Vorlage.");
            existing->patch_targets[patch.slot_address - copy.source_begin].push_back(
                {patch.live_value, patch.target_address});
        }
    }
    for (auto& native_template : native_templates) {
        for (auto& [offset, targets] : native_template.patch_targets) {
            static_cast<void>(offset);
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        }
    }
    struct DispatchBlock {
        std::uint32_t owner;
        std::uint32_t address;
        std::uint32_t exit_source;
        std::uint32_t size;
        const char* end_kind;
        katana::ir::DynamicTargetClass target_class;
    };
    std::size_t block_count = 0u;
    for (const auto& function : program) block_count += function.blocks.size();
    std::unordered_set<std::uint32_t> block_addresses;
    block_addresses.reserve(block_count);
    std::vector<DispatchBlock> dispatch_blocks;
    dispatch_blocks.reserve(block_count);
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            if (!block_addresses.insert(block.start_address).second)
                throw std::runtime_error("IR-Basic-Block besitzt mehrere Funktionsbesitzer.");
            std::uint32_t end = block.start_address + 2u;
            const katana::ir::Instruction* terminal = nullptr;
            for (const auto& instruction : block.instructions) {
                end = std::max(end, instruction.source_address + 2u);
                if (instruction.delay_slot.role != katana::ir::DelaySlotRole::Slot)
                    terminal = &instruction;
            }
            dispatch_blocks.push_back({function.entry_address,
                                       block.start_address,
                                       terminal != nullptr ? terminal->source_address
                                                           : block.start_address,
                                       end - block.start_address,
                                       end_kind(block),
                                       terminal != nullptr
                                            ? terminal->dynamic_target_class
                                            : katana::ir::DynamicTargetClass::NotApplicable});
        }
    }
    std::sort(dispatch_blocks.begin(), dispatch_blocks.end(), [](const auto& left, const auto& right) {
        return left.address < right.address;
    });
    if (dispatch_blocks.empty())
        throw std::runtime_error("Runtime-Dispatch besitzt keine generierten Bloecke.");
    for (const auto& native_template : native_templates) {
        if (!block_addresses.contains(native_template.source_start))
            throw std::runtime_error(
                "Runtime-Codecopy besitzt keinen generierten AOT-Quellblock.");
        for (const auto& [offset, targets] : native_template.patch_targets) {
            static_cast<void>(offset);
            if (std::any_of(targets.begin(), targets.end(), [&](const auto& target) {
                    return !block_addresses.contains(target.block_address);
                }))
                throw std::runtime_error(
                    "Runtime-Codecopy-Patchziel besitzt keinen generierten AOT-Block.");
        }
    }

    constexpr std::size_t dispatch_blocks_per_shard = 512u;
    const auto shard_count =
        (dispatch_blocks.size() + dispatch_blocks_per_shard - 1u) / dispatch_blocks_per_shard;
    const auto shard_symbol = [](const std::size_t index) {
        std::ostringstream name;
        name << std::setfill('0') << std::setw(5) << index;
        return name.str();
    };

    std::vector<ProjectArtifact> result;
    result.reserve(shard_count + 2u);
    std::ostringstream internal_header;
    internal_header
        << "#pragma once\n\n"
        << "#include \"katana/runtime/block_abi.hpp\"\n"
        << "#include \"katana/runtime/block_table.hpp\"\n"
        << "#include \"katana/runtime/indirect_dispatch.hpp\"\n"
        << "#include \"katana/runtime/platform_services.hpp\"\n"
        << "#include <cstdint>\n#include <vector>\n\n"
        << "namespace " << entry_namespace << "::runtime_dispatch_detail {\n"
        << "extern thread_local katana::runtime::PlatformServices* active_services;\n"
        << "extern thread_local katana::runtime::BlockAddress active_exit_source;\n"
        << "extern thread_local katana::runtime::BlockEndKind active_exit_kind;\n"
        << "extern thread_local katana::runtime::DynamicDispatchSiteClass "
           "active_exit_site_class;\n"
        << "extern thread_local bool tail_dispatch_completed;\n"
        << "void append_static_block(\n"
        << "    std::vector<katana::runtime::RuntimeBlock>& blocks,\n"
        << "    std::uint32_t address, std::uint32_t size,\n"
        << "    katana::runtime::BlockEndKind end_kind,\n"
        << "    katana::runtime::BackendBlockFunction function, const char* provenance);\n"
        << "void register_executable_block(\n"
        << "    const katana::runtime::RuntimeBlockTable& table,\n"
        << "    katana::runtime::PlatformServices& services,\n"
        << "    std::uint32_t address, std::uint32_t size);\n";
    for (std::size_t shard = 0u; shard < shard_count; ++shard) {
        const auto suffix = shard_symbol(shard);
        internal_header << "bool note_block_entry_shard_" << suffix
                        << "(std::uint32_t address) noexcept;\n"
                        << "void append_static_blocks_shard_" << suffix
                        << "(std::vector<katana::runtime::RuntimeBlock>& blocks);\n"
                        << "void register_executable_blocks_shard_" << suffix
                        << "(const katana::runtime::RuntimeBlockTable& table, "
                           "katana::runtime::PlatformServices& services);\n";
    }
    internal_header << "} // namespace " << entry_namespace << "::runtime_dispatch_detail\n";
    result.push_back({"include/runtime-dispatch-internal.hpp", internal_header.str()});

    for (std::size_t shard = 0u; shard < shard_count; ++shard) {
        const auto begin = shard * dispatch_blocks_per_shard;
        const auto end = std::min(begin + dispatch_blocks_per_shard, dispatch_blocks.size());
        const auto suffix = shard_symbol(shard);
        std::vector<std::uint32_t> owners;
        owners.reserve(end - begin);
        for (auto index = begin; index < end; ++index) owners.push_back(dispatch_blocks[index].owner);
        std::sort(owners.begin(), owners.end());
        owners.erase(std::unique(owners.begin(), owners.end()), owners.end());

        std::ostringstream shard_output;
        shard_output << "#include \"../include/runtime-dispatch-internal.hpp\"\n"
                     << "#include <stdexcept>\n#include <utility>\n\n"
                     << "namespace " << entry_namespace << " {\n";
        for (const auto owner : owners) {
            shard_output << "void fn_" << symbol(owner)
                         << "_with_services(katana::runtime::CpuState&, "
                            "katana::runtime::PlatformServices*);\n";
        }
        shard_output << "namespace runtime_dispatch_detail {\nnamespace {\n";
        for (const auto owner : owners) {
            shard_output
                << "katana::runtime::BlockExit dispatch_owner_" << symbol(owner)
                << "(katana::runtime::CpuState& cpu, "
                   "katana::runtime::BlockExecutionContext& context) {\n"
                << "    if (active_services == nullptr) throw "
                   "std::runtime_error(\"Runtime-Plattformdienste fehlen.\");\n"
                << "    active_exit_source = {cpu.pc, "
                   "katana::runtime::canonical_physical_address(cpu.pc)};\n"
                << "    active_exit_kind = katana::runtime::BlockEndKind::Fallthrough;\n"
                << "    active_exit_site_class = "
                   "katana::runtime::DynamicDispatchSiteClass::NotDynamic;\n"
                << "    const bool exception_active_on_entry = cpu.trap_pending;\n"
                << "    fn_" << symbol(owner)
                << "_with_services(cpu, active_services);\n"
                << "    context.scheduler_cycle = active_services->scheduler_cycle();\n"
                << "    auto kind = active_exit_kind;\n"
                << "    if (!exception_active_on_entry && cpu.trap_pending)\n"
                << "        kind = katana::runtime::BlockEndKind::Exception;\n"
                << "    if (std::exchange(tail_dispatch_completed, false))\n"
                << "        kind = katana::runtime::BlockEndKind::Return;\n"
                << "    return katana::runtime::make_block_exit(cpu, context,\n"
                << "        kind, active_exit_source, katana::runtime::BlockAddress{cpu.pc, "
                   "katana::runtime::canonical_physical_address(cpu.pc)});\n"
                << "}\n";
        }
        shard_output << "} // namespace\n\n"
                     << "bool note_block_entry_shard_" << suffix
                     << "(const std::uint32_t address) noexcept {\n"
                     << "    switch (address) {\n";
        for (auto index = begin; index < end; ++index) {
            const auto& block = dispatch_blocks[index];
            const auto address = symbol(block.address);
            const auto exit_source = symbol(block.exit_source);
            shard_output << "    case 0x" << address << "u:\n"
                         << "        active_exit_source = {"
                            "katana::runtime::relocate_code_address(0x"
                         << exit_source
                         << "u), katana::runtime::canonical_physical_address("
                            "katana::runtime::relocate_code_address(0x"
                         << exit_source << "u))};\n"
                         << "        active_exit_kind = katana::runtime::BlockEndKind::"
                         << block.end_kind << ";\n"
                         << "        active_exit_site_class = "
                            "katana::runtime::DynamicDispatchSiteClass::"
                         << dispatch_site_class(block.target_class) << ";\n"
                         << "        return true;\n";
        }
        shard_output << "    default: return false;\n"
                     << "    }\n}\n\n"
                     << "void append_static_blocks_shard_" << suffix
                     << "(std::vector<katana::runtime::RuntimeBlock>& blocks) {\n";
        for (auto index = begin; index < end; ++index) {
            const auto& block = dispatch_blocks[index];
            const auto address = symbol(block.address);
            shard_output << "    append_static_block(blocks, 0x" << address << "u, "
                         << block.size << "u, katana::runtime::BlockEndKind::" << block.end_kind
                         << ", &dispatch_owner_" << symbol(block.owner)
                         << ", \"generated-block-" << address
                         << "\");\n";
        }
        shard_output << "}\n\n"
                     << "void register_executable_blocks_shard_" << suffix
                     << "(const katana::runtime::RuntimeBlockTable& table, "
                        "katana::runtime::PlatformServices& services) {\n";
        for (auto index = begin; index < end; ++index) {
            const auto& block = dispatch_blocks[index];
            const auto address = symbol(block.address);
            shard_output << "    register_executable_block(table, services, 0x" << address
                         << "u, " << block.size << "u);\n";
            if (std::string_view(block.end_kind) != "Return" &&
                std::string_view(block.end_kind) != "ExceptionReturn" &&
                std::string_view(block.end_kind) != "Sleep" &&
                std::string_view(block.end_kind) != "Exception")
                shard_output << "    services.allow_executable_block_chaining(0x" << address
                             << "u);\n";
        }
        shard_output << "}\n"
                     << "} // namespace runtime_dispatch_detail\n"
                     << "} // namespace " << entry_namespace << "\n";
        result.push_back({std::filesystem::path("code") /
                              ("runtime-dispatch-shard-" + suffix + ".cpp"),
                          shard_output.str()});
    }

    std::ostringstream output;
    output << "#include \"../include/katana_port.hpp\"\n"
           << "#include \"../include/runtime-dispatch-internal.hpp\"\n"
           << "#include \"katana/runtime/block_abi.hpp\"\n"
           << "#include \"katana/runtime/block_table.hpp\"\n"
           << "#include \"katana/runtime/dispatch_diagnostics.hpp\"\n"
           << "#include \"katana/runtime/dreamcast_boot.hpp\"\n"
           << "#include \"katana/runtime/executable_modules.hpp\"\n"
           << "#include \"katana/runtime/indirect_dispatch.hpp\"\n"
           << "#include \"katana/runtime/runtime_probe.hpp\"\n";
    output << "#include \"katana/runtime/native_aot_template.hpp\"\n";
    if (diagnostic_interpreter)
        output << "#include \"katana/runtime/dynamic_interpreter.hpp\"\n"
               << "#include \"katana/sh4/decoder.hpp\"\n";
    output
           << "#include <cerrno>\n#include <chrono>\n#include <cstdlib>\n#include "
              "<iostream>\n#include <limits>\n"
              "#include <stdexcept>\n#include <string>\n#include <thread>\n#include <utility>\n#include <vector>\n\n"
           << "namespace " << entry_namespace << " {\n";
    output << "thread_local RuntimeMaterializationStatus last_materialization_status;\n"
           << "const RuntimeMaterializationStatus& runtime_materialization_status() noexcept {\n"
           << "    return last_materialization_status;\n"
           << "}\n"
           << "namespace runtime_dispatch_detail {\n"
           << "thread_local katana::runtime::PlatformServices* active_services = nullptr;\n"
           << "thread_local katana::runtime::BlockAddress active_exit_source;\n"
           << "thread_local katana::runtime::BlockEndKind active_exit_kind =\n"
              "    katana::runtime::BlockEndKind::Fallthrough;\n"
           << "thread_local katana::runtime::DynamicDispatchSiteClass active_exit_site_class =\n"
              "    katana::runtime::DynamicDispatchSiteClass::NotDynamic;\n"
           << "thread_local bool tail_dispatch_completed = false;\n"
           << "void register_executable_block(\n"
           << "    const katana::runtime::RuntimeBlockTable& table,\n"
           << "    katana::runtime::PlatformServices& services,\n"
           << "    std::uint32_t address, std::uint32_t size) {\n"
           << "    const auto registered_handle = table.lookup(address, {});\n"
           << "    if (!registered_handle) throw std::runtime_error(\"Registrierter Block "
              "fehlt.\");\n"
           << "    const auto registered = table.resolve(*registered_handle);\n"
           << "    if (!registered) throw std::runtime_error(\"Registrierter Block ist "
              "stale.\");\n"
           << "    services.register_executable_block(\n"
           << "        address, size, "
              "katana::runtime::stable_runtime_block_identity(registered->get()));\n"
           << "}\n"
           << "void append_static_block(\n"
           << "    std::vector<katana::runtime::RuntimeBlock>& blocks,\n"
           << "    std::uint32_t address, std::uint32_t size,\n"
           << "    katana::runtime::BlockEndKind end_kind,\n"
           << "    katana::runtime::BackendBlockFunction function, const char* provenance) {\n"
           << "    katana::runtime::RuntimeBlock block;\n"
           << "    block.virtual_start = address;\n"
           << "    block.physical_origin = "
              "katana::runtime::canonical_physical_address(address);\n"
           << "    block.size = size; block.end_kind = end_kind; block.function = function;\n"
           << "    block.provenance = provenance;\n"
           << "    blocks.emplace_back(std::move(block));\n"
           << "}\n"
           << "} // namespace runtime_dispatch_detail\n"
           << "using runtime_dispatch_detail::active_exit_kind;\n"
           << "using runtime_dispatch_detail::active_exit_site_class;\n"
           << "using runtime_dispatch_detail::active_exit_source;\n"
           << "using runtime_dispatch_detail::active_services;\n"
           << "using runtime_dispatch_detail::tail_dispatch_completed;\n"
           << "namespace {\n"
           << "thread_local katana::runtime::RuntimeBlockTable* active_table = nullptr;\n"
           << "thread_local katana::runtime::BlockExecutionContext* active_context = nullptr;\n"
           << "thread_local katana::runtime::DispatchDiagnosticRecorder* active_diagnostics = "
              "nullptr;\n"
           << "thread_local katana::runtime::IndirectDispatchMetrics* active_dispatch_metrics = "
               "nullptr;\n"
           << "thread_local katana::runtime::DemandBlockMaterializer* active_materializer = "
              "nullptr;\n"
           << "thread_local std::uint64_t executed_dispatch_blocks = 0u;\n"
           << "enum class DispatchChainBoundary { NestedCall, ProgramRoot };\n"
           << "void dispatch_chain(katana::runtime::CpuState&, std::uint32_t, "
              "katana::runtime::IndirectDispatchKind, "
              "katana::runtime::RuntimeDispatchClass, bool, DispatchChainBoundary);\n"
           << "class ServiceScope {\n"
           << "  public:\n"
           << "    ServiceScope(katana::runtime::PlatformServices& services,\n"
              "                 katana::runtime::RuntimeBlockTable& table,\n"
               "                 katana::runtime::BlockExecutionContext& context,\n"
               "                 katana::runtime::DispatchDiagnosticRecorder& diagnostics,\n"
               "                 katana::runtime::IndirectDispatchMetrics& dispatch_metrics,\n"
               "                 katana::runtime::DemandBlockMaterializer& materializer) {\n"
           << "        if (active_services != nullptr) throw std::runtime_error(\"Runtime-Dispatch "
              "ist nicht reentrant.\");\n"
           << "        active_services = &services; active_table = &table;\n"
              "        active_context = &context; active_diagnostics = &diagnostics;\n"
               "        active_dispatch_metrics = &dispatch_metrics;\n"
               "        active_materializer = &materializer;\n"
              "        executed_dispatch_blocks = 0u;\n"
           << "    }\n"
           << "    ~ServiceScope() { active_services = nullptr; active_table = nullptr;\n"
               "        active_context = nullptr; active_diagnostics = nullptr;\n"
               "        active_dispatch_metrics = nullptr; active_materializer = nullptr; }\n"
           << "};\n";
    if (diagnostic_interpreter)
        output << "katana::runtime::BlockExit dispatch_dynamic_interpreter(\n"
                  "        katana::runtime::CpuState& cpu,\n"
                  "        katana::runtime::BlockExecutionContext& context) {\n"
                  "    if (active_services == nullptr)\n"
                  "        throw std::runtime_error(\"Dynamischer SH-4-Pfad ohne Plattformdienste.\");\n"
                  "    const auto source = cpu.pc;\n"
                  "    const auto interpreted =\n"
                  "        katana::runtime::execute_dynamic_sh4_block(cpu, *active_services, 1u);\n"
                  "    const auto scheduler = active_services->consume_guest_cycles(\n"
                  "        interpreted.guest_cycles, context.scheduler_event_budget);\n"
                  "    if (scheduler.budget_exhausted || scheduler.guest_cycle_budget_exhausted)\n"
                  "        throw std::runtime_error(\"Dynamischer SH-4-Schedulerbudgetabbruch.\");\n"
                  "    context.scheduler_cycle = scheduler.guest_cycle;\n"
                  "    return katana::runtime::make_block_exit(\n"
                  "        cpu, context, interpreted.end_kind,\n"
                  "        {source, katana::runtime::canonical_physical_address(source)},\n"
                  "        katana::runtime::BlockAddress{\n"
                  "            cpu.pc, katana::runtime::canonical_physical_address(cpu.pc)});\n"
                  "}\n";
    output
        << "std::uint64_t configured_block_budget() {\n"
           "    static const auto budget = [] {\n"
           "        const auto* text = std::getenv(\"KATANA_PORT_BLOCK_LIMIT\");\n"
           "        if (text == nullptr || *text == '\\0')\n"
           "            return std::numeric_limits<std::uint64_t>::max();\n"
           "        errno = 0;\n"
           "        char* end = nullptr;\n"
           "        const auto value = std::strtoull(text, &end, 10);\n"
           "        if (errno != 0 || end == text || *end != '\\0' || value == 0u)\n"
           "            throw std::runtime_error(\"KATANA_PORT_BLOCK_LIMIT ist ungueltig.\");\n"
           "        return static_cast<std::uint64_t>(value);\n"
           "    }();\n"
           "    return budget;\n"
           "}\n\n"
           "std::uint64_t configured_progress_interval() {\n"
           "    static const auto interval = [] {\n"
           "        const auto* text = std::getenv(\"KATANA_PORT_PROGRESS_INTERVAL\");\n"
           "        if (text == nullptr || *text == '\\0') return std::uint64_t{0u};\n"
           "        errno = 0; char* end = nullptr;\n"
           "        const auto value = std::strtoull(text, &end, 10);\n"
           "        if (errno != 0 || end == text || *end != '\\0')\n"
           "            throw std::runtime_error(\"KATANA_PORT_PROGRESS_INTERVAL ist "
           "ungueltig.\");\n"
           "        return static_cast<std::uint64_t>(value);\n"
           "    }();\n"
           "    return interval;\n"
           "}\n\n"
           "void dispatch_chain(katana::runtime::CpuState& cpu, std::uint32_t target,\n"
           "                    katana::runtime::IndirectDispatchKind kind,\n"
           "                    katana::runtime::RuntimeDispatchClass dispatch_class,\n"
           "                    bool diagnostic, DispatchChainBoundary boundary) {\n"
           "    const auto block_budget = configured_block_budget();\n"
           "    const auto program_return_sentinel = cpu.pr;\n"
           "    std::uint32_t dispatch_callsite = cpu.pc;\n"
           "    katana::runtime::BlockAddress dispatch_source{cpu.pc,\n"
           "        katana::runtime::canonical_physical_address(cpu.pc)};\n"
           "    auto dispatch_origin =\n"
           "        dispatch_class == katana::runtime::RuntimeDispatchClass::RuntimeOnly\n"
           "            ? katana::runtime::DispatchResolutionOrigin::RuntimeOnly\n"
           "            : katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
           "    for (;;) {\n"
           "        const auto lifecycle = active_services->poll_host_lifecycle();\n"
           "        if (lifecycle == katana::runtime::PlatformLifecycleState::Shutdown)\n"
           "            throw katana::runtime::PlatformShutdownRequested();\n"
           "        if (lifecycle == katana::runtime::PlatformLifecycleState::Paused) {\n"
           "            std::this_thread::sleep_for(std::chrono::milliseconds(1));\n"
           "            continue;\n"
           "        }\n"
           "        if (executed_dispatch_blocks >= block_budget)\n"
           "            throw std::runtime_error(\"KATANA_PORT_BLOCK_LIMIT erreicht.\");\n"
           "        ++executed_dispatch_blocks;\n"
           "        const auto progress_interval = configured_progress_interval();\n"
           "        if (progress_interval != 0u &&\n"
           "            executed_dispatch_blocks % progress_interval == 0u)\n"
           "            std::cerr << \"KATANA_PORT_BLOCK_PROGRESS blocks=\"\n"
           "                      << executed_dispatch_blocks << \" pc=\" << cpu.pc\n"
           "                      << \" guest_cycles=\" << active_services->scheduler_cycle()\n"
           "                      << '\\n';\n"
           "        if (cpu.sleeping) {\n"
           "            if (active_services->poll_interrupt().has_value()) {\n"
           "                target = cpu.pc;\n"
           "                kind = katana::runtime::IndirectDispatchKind::TailJump;\n"
           "                dispatch_class = "
           "katana::runtime::RuntimeDispatchClass::GuardedFallback;\n"
           "                dispatch_origin = "
           "katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
           "                diagnostic = false;\n"
           "            } else {\n"
           "                const auto next_event = "
           "active_services->next_scheduler_event_cycle();\n"
           "                if (!next_event) throw std::runtime_error(\"SLEEP besitzt kein "
           "Wakeup-Ereignis.\");\n"
           "                const auto scheduler = active_services->consume_guest_cycles(\n"
           "                    *next_event - active_services->scheduler_cycle(), 1024u);\n"
           "            if (scheduler.budget_exhausted)\n"
           "                throw std::runtime_error(\"Schedulerbudget erschoepft\");\n"
           "                if (scheduler.guest_cycle_budget_exhausted)\n"
           "                    throw std::runtime_error(\"Gastzyklusbudget erschoepft\");\n"
           "                if (!active_services->poll_interrupt().has_value()) continue;\n"
           "                target = cpu.pc;\n"
           "                kind = katana::runtime::IndirectDispatchKind::TailJump;\n"
           "                dispatch_class = "
           "katana::runtime::RuntimeDispatchClass::GuardedFallback;\n"
           "                dispatch_origin = "
           "katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
           "                diagnostic = false;\n"
           "            }\n"
           "        }\n"
           "        const auto selected = katana::runtime::dispatch_indirect(cpu, *active_table,\n"
           "            {kind, dispatch_callsite, target, cpu.pr, dispatch_source, {},\n"
           "             dispatch_origin,\n"
           "             diagnostic ? active_diagnostics : nullptr, dispatch_class,\n"
            "             active_dispatch_metrics, active_materializer});\n"
           "        const auto selected_block = active_table->resolve(selected.block);\n"
           "        if (!selected_block || selected_block->get().function == nullptr)\n"
           "            throw std::runtime_error(\"Runtime-Dispatchziel besitzt keinen generierten "
           "Block.\");\n"
           "        active_services->begin_executable_block(selected_block->get().variant);\n"
           "        const auto retired_before = cpu.retired_guest_instructions;\n"
           "        active_exit_source = {selected.diagnostic_target, selected.physical_target};\n"
           "        active_exit_site_class = "
           "katana::runtime::DynamicDispatchSiteClass::NotDynamic;\n"
           "        auto exit = katana::runtime::execute_runtime_block(\n"
           "            selected_block->get(), cpu, *active_context);\n"
           "        if (cpu.retired_guest_instructions < retired_before)\n"
           "            throw std::runtime_error(\"Gastinstruktionszaehler lief rueckwaerts.\");\n"
           "        const auto retired = cpu.retired_guest_instructions - retired_before;\n"
           "        const auto expected_fallthrough = static_cast<std::uint32_t>(\n"
           "            exit.source.virtual_address + 2u);\n"
           "        if (retired != 0u &&\n"
           "            exit.kind == katana::runtime::BlockEndKind::Fallthrough &&\n"
           "            (!exit.target.has_value() ||\n"
           "             exit.target->virtual_address != expected_fallthrough))\n"
           "            throw std::runtime_error(\n"
           "                \"Generierter Fallthrough verfehlte den exakten Gast-PC: source=\" +\n"
           "                std::to_string(exit.source.virtual_address) + \" expected=\" +\n"
           "                std::to_string(expected_fallthrough) + \" actual=\" +\n"
           "                std::to_string(cpu.pc));\n"
           "        if (retired != 0u) {\n"
           "            const auto scheduler = active_services->consume_guest_cycles(\n"
           "                katana::runtime::base_guest_cycles_per_instruction * retired,\n"
           "                active_context->scheduler_event_budget);\n"
           "            if (scheduler.budget_exhausted)\n"
           "                throw std::runtime_error(\"Schedulerbudget erschoepft\");\n"
           "            if (scheduler.guest_cycle_budget_exhausted)\n"
           "                throw std::runtime_error(\"Gastzyklusbudget erschoepft\");\n"
           "            active_context->scheduler_cycle = scheduler.guest_cycle;\n"
           "        }\n"
           "        active_services->observe_guest_checkpoint(selected.diagnostic_target);\n"
           "        if (active_services->poll_interrupt().has_value())\n"
           "            exit.kind = katana::runtime::BlockEndKind::InterruptSafepoint;\n"
           "        if (exit.kind == katana::runtime::BlockEndKind::Return) {\n"
           "            if (boundary == DispatchChainBoundary::NestedCall ||\n"
           "                cpu.pc == program_return_sentinel)\n"
           "                return;\n"
           "            // RTS latches PR before its delay slot. Follow the resulting guest PC;\n"
           "            // only a nested host-call boundary or the root sentinel ends a chain.\n"
           "            target = cpu.pc;\n"
           "            dispatch_callsite = exit.source.virtual_address;\n"
           "            dispatch_source = exit.source;\n"
           "            kind = katana::runtime::IndirectDispatchKind::TailJump;\n"
           "            dispatch_class = "
           "katana::runtime::RuntimeDispatchClass::GuardedFallback;\n"
           "            dispatch_origin = "
           "katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
           "            diagnostic = false;\n"
           "            continue;\n"
           "        }\n"
           "        if (exit.kind == katana::runtime::BlockEndKind::Exception ||\n"
           "            exit.kind == katana::runtime::BlockEndKind::ExceptionReturn ||\n"
           "            exit.kind == katana::runtime::BlockEndKind::InterruptSafepoint ||\n"
           "            exit.kind == katana::runtime::BlockEndKind::Sleep) {\n"
           "            target = cpu.pc;\n"
           "            dispatch_callsite = exit.source.virtual_address;\n"
           "            dispatch_source = exit.source;\n"
           "            kind = katana::runtime::IndirectDispatchKind::TailJump;\n"
           "            dispatch_class = "
           "katana::runtime::RuntimeDispatchClass::GuardedFallback;\n"
           "            dispatch_origin = "
           "katana::runtime::DispatchResolutionOrigin::TableLookup;\n"
           "            diagnostic = false;\n"
           "            continue;\n"
           "        }\n"
           "        const auto continuation =\n"
           "            katana::runtime::make_indirect_dispatch_continuation(\n"
           "                exit, active_exit_site_class);\n"
           "        target = cpu.pc;\n"
           "        kind = continuation.kind;\n"
           "        dispatch_callsite = continuation.callsite;\n"
           "        dispatch_source = continuation.source;\n"
           "        dispatch_origin = continuation.resolution_origin;\n"
           "        dispatch_class = continuation.dispatch_class;\n"
           "        diagnostic = continuation.record_diagnostics;\n"
           "    }\n"
            "    throw std::runtime_error(\"Runtime-Blockbudget erschoepft.\");\n"
            "}\n"
            "} // namespace\n\n"
            "void note_block_entry(const std::uint32_t address) noexcept {\n"
            "    const auto source_address = "
            "katana::runtime::unrelocate_code_address(address);\n"
            "    const bool matched = [&]() noexcept {\n";
    const auto emit_note_router = [&](const auto& self,
                                      const std::size_t begin,
                                      const std::size_t end,
                                      const std::string_view indentation) -> void {
        if (end - begin == 1u) {
            output << indentation << "return runtime_dispatch_detail::note_block_entry_shard_"
                   << shard_symbol(begin) << "(source_address);\n";
            return;
        }
        const auto middle = begin + (end - begin) / 2u;
        const auto pivot = dispatch_blocks[middle * dispatch_blocks_per_shard - 1u].address;
        output << indentation << "if (source_address <= 0x" << symbol(pivot) << "u) {\n";
        const auto nested = std::string(indentation) + "    ";
        self(self, begin, middle, nested);
        output << indentation << "} else {\n";
        self(self, middle, end, nested);
        output << indentation << "}\n";
    };
    emit_note_router(emit_note_router, 0u, shard_count, "        ");
    output << "    }();\n"
              "    if (matched) return;\n"
              "    active_exit_source = {address, "
              "katana::runtime::canonical_physical_address(address)};\n"
              "    active_exit_kind = katana::runtime::BlockEndKind::Fallthrough;\n"
              "    active_exit_site_class = "
              "katana::runtime::DynamicDispatchSiteClass::NotDynamic;\n"
              "}\n\n"
        << "void static_call(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
           "    dispatch_chain(cpu, target, katana::runtime::IndirectDispatchKind::Call,\n"
           "        katana::runtime::RuntimeDispatchClass::GuardedFallback, false,\n"
           "        DispatchChainBoundary::NestedCall);\n"
           "}\n"
           "void resolved_call(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
           "    dispatch_chain(cpu, target, katana::runtime::IndirectDispatchKind::Call,\n"
           "        katana::runtime::RuntimeDispatchClass::GuardedFallback, true,\n"
           "        DispatchChainBoundary::NestedCall);\n"
           "}\n"
           "void guarded_call(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
           "    dispatch_chain(cpu, target, katana::runtime::IndirectDispatchKind::Call,\n"
           "        katana::runtime::RuntimeDispatchClass::GuardedFallback, true,\n"
           "        DispatchChainBoundary::NestedCall);\n"
           "}\n"
           "void guarded_jump(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
           "    dispatch_chain(cpu, target, katana::runtime::IndirectDispatchKind::TailJump,\n"
           "        katana::runtime::RuntimeDispatchClass::GuardedFallback, true,\n"
           "        DispatchChainBoundary::NestedCall);\n"
           "    tail_dispatch_completed = true;\n"
           "}\n"
           "void runtime_only_call(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
           "    dispatch_chain(cpu, target, katana::runtime::IndirectDispatchKind::Call,\n"
           "        katana::runtime::RuntimeDispatchClass::RuntimeOnly, true,\n"
           "        DispatchChainBoundary::NestedCall);\n"
           "}\n"
           "void runtime_only_jump(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
           "    dispatch_chain(cpu, target, katana::runtime::IndirectDispatchKind::TailJump,\n"
           "        katana::runtime::RuntimeDispatchClass::RuntimeOnly, true,\n"
           "        DispatchChainBoundary::NestedCall);\n"
           "    tail_dispatch_completed = true;\n"
           "}\n"
           "void unresolved_call(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
           "    katana::runtime::unresolved_call(cpu, target);\n"
           "}\n"
           "void unresolved_jump(katana::runtime::CpuState& cpu, std::uint32_t target) {\n"
           "    katana::runtime::unresolved_jump(cpu, target);\n"
           "}\n\n"
        << "RuntimeRunResult run_runtime(katana::runtime::CpuState& cpu,\n"
        << "                             katana::runtime::PlatformServices& services,\n"
        << "                             katana::runtime::RuntimeBlockTable& table) {\n"
        << "    katana::runtime::validate_platform_services(services);\n";
    output << "    table.bind_code_tracker(services.executable_code_tracker());\n";
    output << "    std::vector<katana::runtime::RuntimeBlock> static_blocks;\n";
    output << "    static_blocks.reserve(" << block_addresses.size() << "u);\n";
    for (std::size_t shard = 0u; shard < shard_count; ++shard)
        output << "    runtime_dispatch_detail::append_static_blocks_shard_"
               << shard_symbol(shard) << "(static_blocks);\n";
    output << "    static_cast<void>(table.register_static_bulk(std::move(static_blocks)));\n";
    for (std::size_t shard = 0u; shard < shard_count; ++shard)
        output << "    runtime_dispatch_detail::register_executable_blocks_shard_"
               << shard_symbol(shard) << "(table, services);\n";
    const auto entry = symbol(entry_address);
    output << "    katana::runtime::DispatchDiagnosticRecorder diagnostics;\n"
           << "    katana::runtime::IndirectDispatchMetrics dispatch_metrics;\n"
           << "    katana::runtime::BlockExecutionContext context;\n"
           << "    context.scheduler_cycle = services.scheduler_cycle();\n"
           << "    context.scheduler_event_budget = 1024u;\n"
           << "    auto* modules = services.executable_module_catalog();\n"
           << "    if (modules == nullptr)\n"
           << "        throw std::runtime_error(\"Produktpfad besitzt keinen Modul-Catalog.\");\n"
           << "    const std::vector<katana::runtime::NativeAotTemplate> native_aot_templates{\n";
    for (const auto& native_template : native_templates) {
        output << "        {std::string(katana::runtime::";
        if (native_template.module_id ==
            katana::runtime::dreamcast_initial_disc_bootstrap_module_id) {
            output << "dreamcast_initial_disc_bootstrap_module_id";
        } else if (native_template.module_id ==
                   katana::runtime::dreamcast_initial_boot_executable_module_id) {
            output << "dreamcast_initial_boot_executable_module_id";
        } else {
            throw std::logic_error("Unbekannte lokale AOT-Quellmodulkennung.");
        }
        output << "), \"" << native_template.expected_source_identity << "\", 0x"
               << symbol(native_template.source_start) << "u, "
               << native_template.extent << "u, " << native_template.destination_vbr_delta
               << ", {";
        for (const auto& [offset, targets] : native_template.patch_targets) {
            output << "{" << offset << "u, {";
            for (std::size_t index = 0u; index < targets.size(); ++index) {
                if (index != 0u) output << ',';
                output << "{0x" << symbol(targets[index].live_value) << "u,0x"
                       << symbol(targets[index].block_address) << "u}";
            }
            output << "}},";
        }
        output << "}},\n";
    }
    output << "    };\n"
           << "    katana::runtime::NativeAotTemplateBinder native_aot_binder(\n"
           << "        cpu, *modules, table, native_aot_templates);\n"
           << "    katana::runtime::BlockMaterializationPolicy materialization_policy;\n"
           << "    materialization_policy.max_blocks = 65536u;\n"
           << "    materialization_policy.max_bytes = 64u * 1024u * 1024u;\n"
           << "    materialization_policy.max_materializations_per_run = 65536u;\n"
           << "    if (const auto* probe = std::getenv(\"KATANA_RUNTIME_PROBE\");\n"
              "        probe != nullptr && std::string_view(probe) == \"deterministic-v1\")\n"
              "        materialization_policy.deterministic_no_host_time = true;\n";
    if (diagnostic_interpreter) {
        output << "    materialization_policy.enabled = true;\n"
               << "    katana::runtime::DemandBlockMaterializer materializer(\n"
               << "        *modules, table, services.executable_code_tracker(), materialization_policy,\n"
               << "        [&native_aot_binder](const std::uint32_t target,\n"
               << "           const std::uint32_t physical_origin,\n"
               << "           const std::span<const std::uint8_t> bytes,\n"
               << "           const katana::runtime::BlockVariantKey& variant) {\n"
               << "            auto native = native_aot_binder.bind(\n"
               << "                target, physical_origin, bytes, variant);\n"
               << "            if (native) return std::move(native.candidate);\n"
               << "            katana::runtime::MaterializedBlockCandidate candidate;\n"
               << "            if (bytes.size() < 2u) return candidate;\n"
               << "            const auto opcode = static_cast<std::uint16_t>(bytes[0]) |\n"
               << "                static_cast<std::uint16_t>(bytes[1] << 8u);\n"
               << "            const auto decoded = katana::sh4::decode(opcode);\n"
               << "            if (!decoded.is_known()) return candidate;\n"
               << "            std::size_t size = 2u;\n"
               << "            candidate.instructions = 1u;\n"
               << "            if (decoded.has_delay_slot) {\n"
               << "                if (bytes.size() < 4u) return candidate;\n"
               << "                const auto slot_opcode = static_cast<std::uint16_t>(bytes[2]) |\n"
               << "                    static_cast<std::uint16_t>(bytes[3] << 8u);\n"
               << "                const auto slot = katana::sh4::decode(slot_opcode);\n"
               << "                if (!slot.is_known() || slot.changes_control_flow())\n"
               << "                    return candidate;\n"
               << "                size = 4u; ++candidate.instructions;\n"
               << "            }\n"
               << "            candidate.block = {target,\n"
               << "                katana::runtime::canonical_physical_address(target),\n"
               << "                static_cast<std::uint32_t>(size),\n"
               << "                katana::runtime::BlockEndKind::DynamicBranch, variant,\n"
               << "                &dispatch_dynamic_interpreter, \"runtime-sh4-interpreter\"};\n"
               << "            candidate.decode_candidate_validated = true;\n"
               << "            candidate.interpreter_backed = true;\n"
               << "            candidate.bounded_analysis_complete = false;\n"
               << "            candidate.ir_verified = false;\n"
               << "            candidate.code_generated = false;\n"
               << "            candidate.guest_cycles = candidate.instructions;\n"
               << "            return candidate;\n"
               << "        });\n";
    } else {
        output << "    // Product ports execute only statically generated native/AOT blocks.\n"
               << "    // Runtime copies bind only to analysis-proven, pre-generated native code.\n"
               << "    materialization_policy.enabled = "
               << (native_templates.empty() ? "false" : "true") << ";\n"
               << "    katana::runtime::DemandBlockMaterializer materializer(\n"
               << "        *modules, table, services.executable_code_tracker(),\n"
               << "        materialization_policy,\n"
               << "        [&native_aot_binder](const std::uint32_t target,\n"
               << "           const std::uint32_t physical_origin,\n"
               << "           const std::span<const std::uint8_t> bytes,\n"
               << "           const katana::runtime::BlockVariantKey& variant) {\n"
               << "            return std::move(\n"
               << "                native_aot_binder.bind(\n"
               << "                    target, physical_origin, bytes, variant).candidate);\n"
               << "        });\n";
    }
    output << "    last_materialization_status = {};\n"
           << "    const auto capture_materialization_status = [&] {\n"
           << "        const auto& metrics = materializer.metrics();\n"
           << "        last_materialization_status = {metrics.requests, metrics.cache_hits,\n"
           << "            metrics.materializations, metrics.interpreter_materializations,\n"
           << "            metrics.misses, metrics.budget_failures,\n"
           << "            static_cast<std::uint32_t>(metrics.first_failure),\n"
           << "            metrics.first_failure_target};\n"
           << "    };\n"
           << "    ServiceScope scope(\n"
           << "        services, table, context, diagnostics, dispatch_metrics, materializer);\n"
           << "    try {\n"
           << "        dispatch_chain(cpu, 0x" << entry
           << "u, katana::runtime::IndirectDispatchKind::TailJump,\n"
           << "        katana::runtime::RuntimeDispatchClass::GuardedFallback, false,\n"
           << "        DispatchChainBoundary::ProgramRoot);\n"
           << "    } catch (const katana::runtime::PlatformShutdownRequested&) {\n"
           << "        // A host close request is a controlled end of native guest dispatch.\n"
           << "    } catch (const katana::runtime::RuntimeProbeBudgetReached&) {\n"
           << "        capture_materialization_status();\n"
           << "        throw;\n"
           << "    } catch (...) {\n"
           << "        capture_materialization_status();\n"
           << "        if (const auto* value = std::getenv(\"KATANA_PORT_DIAGNOSTICS\");\n"
           << "            value != nullptr && std::string_view(value) == \"1\")\n"
           << "            std::cerr << \"KATANA_RUNTIME_DISPATCH_DIAGNOSTICS \"\n"
           << "                      << diagnostics.serialize_hotspots_json() << '\\n';\n"
           << "        if (const auto* value = std::getenv(\"KATANA_PORT_DIAGNOSTICS_FULL\");\n"
           << "            value != nullptr && std::string_view(value) == \"1\")\n"
           << "            std::cerr << \"KATANA_RUNTIME_DISPATCH_EVENTS \"\n"
           << "                      << diagnostics.serialize_json() << '\\n';\n"
           << "        throw;\n"
           << "    }\n"
           << "    capture_materialization_status();\n"
           << "    const auto first_error = dispatch_metrics.first_error();\n"
           << "    return {diagnostics.total_occurrences(), dispatch_metrics.hits(),\n"
           << "        dispatch_metrics.misses(), dispatch_metrics.fallbacks(),\n"
           << "        dispatch_metrics.runtime_only_hits(),\n"
           << "        dispatch_metrics.runtime_only_misses(),\n"
           << "        dispatch_metrics.runtime_only_fallbacks(),\n"
           << "        dispatch_metrics.runtime_only_site_count(),\n"
           << "        dispatch_metrics.runtime_only_dispatch_share_ppm(),\n"
           << "        dispatch_metrics.serialize_json(true),\n"
           << "        static_cast<std::uint32_t>(first_error ? first_error->error\n"
           << "            : katana::runtime::DispatchDiagnosticError::None),\n"
           << "        cpu.pc, services.scheduler_cycle()};\n"
           << "}\n"
           << "} // namespace " << entry_namespace << "\n";
    result.push_back({"code/runtime-dispatch.cpp", output.str()});
    return result;
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

std::string
port_metadata(const PortExportOptions& options,
              const std::size_t function_count,
              const std::span<const TranslationUnitPartition> partitions,
              const std::uint32_t entry_address,
              const std::size_t boot_size,
              const std::string_view project_identity,
              const std::span<const katana::analysis::IndirectControlFlowResolution> indirect) {
    const auto count = [indirect](const auto status) {
        return std::count_if(indirect.begin(), indirect.end(), [status](const auto& resolution) {
            return katana::analysis::control_flow_report_status(resolution) == status;
        });
    };
    std::ostringstream output;
    katana::io::write_json_report_header(output, "katana-port-project", "port-project");
    output << ",\"contract_version\":" << port_project_contract_version
           << ",\"target_name\":" << katana::io::quote_json(options.target_name)
           << ",\"console_profile\":" << katana::io::quote_json(options.console_profile)
           << ",\"diagnostic_partial\":" << (options.diagnostic_partial ? "true" : "false")
           << ",\"execution_profile\":"
           << katana::io::quote_json(options.diagnostic_partial ? "diagnostic-interpreter"
                                                                : "native-aot-product")
           << ",\"runtime_interpreter_enabled\":"
           << (options.diagnostic_partial ? "true" : "false")
           << ",\"unbound_code_policy\":"
           << katana::io::quote_json(options.diagnostic_partial ? "diagnostic-interpreter"
                                                                : "typed-materialization-error")
           << ",\"runtime_abi\":" << katana::runtime::abi_version
           << ",\"backend_abi\":" << backend_interface_abi_version
           << ",\"execution_coverage_contract\":"
           << katana::io::quote_json(options.diagnostic_partial
                                         ? "diagnostic-validated-demand-v1"
                                         : "native-aot-or-typed-abort-v1")
           << ",\"dispatch_paths_without_validation\":0"
           << ",\"project_identity\":" << katana::io::quote_json(project_identity)
           << ",\"entry_address\":" << entry_address << ",\"boot_size\":" << boot_size
           << ",\"function_count\":" << function_count << ",\"resolved_control_flow\":"
           << count(katana::analysis::ControlFlowReportStatus::Resolved)
           << ",\"guarded_control_flow\":"
           << count(katana::analysis::ControlFlowReportStatus::GuardedComplete) +
                  count(katana::analysis::ControlFlowReportStatus::GuardedPartial)
           << ",\"guarded_complete_control_flow\":"
           << count(katana::analysis::ControlFlowReportStatus::GuardedComplete)
           << ",\"guarded_partial_control_flow\":"
           << count(katana::analysis::ControlFlowReportStatus::GuardedPartial)
           << ",\"runtime_only_control_flow\":"
           << count(katana::analysis::ControlFlowReportStatus::RuntimeOnly)
           << ",\"unresolved_control_flow\":"
           << count(katana::analysis::ControlFlowReportStatus::Unresolved) << ",\"partitions\":[";
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
    if (std::filesystem::is_regular_file(path) &&
        std::filesystem::file_size(path) == content.size()) {
        std::ifstream existing(path, std::ios::binary);
        std::string current(content.size(), '\0');
        existing.read(current.data(), static_cast<std::streamsize>(current.size()));
        if (existing && current == content) return;
    }
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

void preserve_local_port_user_data(const std::filesystem::path& previous_root,
                                   const std::filesystem::path& published_root) {
    const auto previous = previous_root / "user-data";
    std::error_code status_error;
    const auto previous_status = std::filesystem::symlink_status(previous, status_error);
    if (status_error == std::errc::no_such_file_or_directory ||
        previous_status.type() == std::filesystem::file_type::not_found)
        return;
    if (status_error) throw std::filesystem::filesystem_error(
        "Lokale Portdaten konnten nicht geprueft werden.", previous, status_error);
    if (!std::filesystem::is_directory(previous_status) ||
        std::filesystem::is_symlink(previous_status))
        throw std::runtime_error("Lokale Portdaten sind kein sicherer regulaerer Ordner.");

    const auto published = published_root / "user-data";
    if (std::filesystem::exists(published)) {
        const auto published_status = std::filesystem::symlink_status(published);
        if (!std::filesystem::is_directory(published_status) ||
            std::filesystem::is_symlink(published_status))
            throw std::runtime_error(
                "Frisch publizierte lokale Daten sind kein sicherer Ordner.");
        for (const auto& entry : std::filesystem::recursive_directory_iterator(published)) {
            const auto status = entry.symlink_status();
            if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
                throw std::runtime_error(
                    "Frisch publizierter Port besitzt unerwartet lokale Datendateien.");
        }
    }
    std::filesystem::remove_all(published);
    std::filesystem::rename(previous, published);
}

PortExportResult export_dreamcast_port_project(const PreparedPortProgram& prepared,
                                               const std::filesystem::path& output_root,
                                               const PortExportOptions& options) {
    report_progress(options, "program-validation");
    if (output_root.empty() || !valid_target_name(options.target_name) ||
        options.tool_version.empty() || prepared.entry_address == 0u || prepared.program.empty()) {
        throw std::invalid_argument(
            "Portexport braucht vorbereitetes IR, Einstieg, Ausgabe, Zielkennung und "
            "Werkzeugversion.");
    }
    static_cast<void>(console_profile_enumerator(options.console_profile));
    if (!options.diagnostic_partial && !prepared.analysis.recursive.diagnostics.empty()) {
        throw std::runtime_error("Portanalyse enthaelt unbekannte Instruktionen.");
    }
    const auto incomplete = std::count_if(
        prepared.analysis.indirect_control_flow.begin(),
        prepared.analysis.indirect_control_flow.end(),
        [](const auto& resolution) {
            const auto status = katana::analysis::control_flow_report_status(resolution);
            return status == katana::analysis::ControlFlowReportStatus::GuardedPartial ||
                   status == katana::analysis::ControlFlowReportStatus::Unresolved;
        });
    if (!options.diagnostic_partial && incomplete != 0u) {
        throw std::runtime_error("Portanalyse ist unvollstaendig: " + std::to_string(incomplete) +
                                 " partielle oder ungeloeste Kontrollflussstellen.");
    }
    katana::ir::require_valid_program(prepared.program);
    const auto wait_loop_descriptors = [&] {
        const auto hardware_audit =
            katana::analysis::audit_dreamcast_hardware(prepared.image, prepared.analysis);
        return runtime_wait_loop_descriptors(hardware_audit);
    }();
    report_progress(options, "partition-codegen");
    const auto partitions =
        partition_translation_units(prepared.program, options.partition_options);
    if (partitions.empty()) throw std::runtime_error("Portcodegen erzeugte keine Partition.");

    std::vector<ProjectArtifact> artifacts;
    artifacts.reserve(partitions.size() + 9u);
    const auto emit_partition = [&](const TranslationUnitPartition& partition) {
        auto functions = select_functions(prepared.program, partition);
        const auto contains_program_entry =
            std::any_of(functions.begin(), functions.end(), [&prepared](const auto& function) {
                return function.entry_address == prepared.entry_address;
            });
        // Port units return every block transition to the runtime dispatcher. Advertising every
        // other partition as a direct-link target would only emit the complete global declaration
        // table into every unit, turning source generation and output size quadratic.
        const BackendRequest request{functions,
                                     functions.front().entry_address,
                                     {},
                                     {},
                                     port_namespace,
                                     contains_program_entry,
                                     true,
                                     prepared.entry_address,
                                     true,
                                     true,
                                     true};
        const CppBackend backend;
        return ProjectArtifact{
            std::filesystem::path("code") /
                deterministic_translation_unit_name(partition, prepared.program),
            backend.emit(request).joined_text()};
    };
    const auto codegen_jobs = port_codegen_jobs(partitions.size());
    std::vector<std::optional<ProjectArtifact>> generated(partitions.size());
    std::atomic_size_t next_partition = 0u;
    std::vector<std::future<void>> workers;
    workers.reserve(codegen_jobs);
    for (std::size_t worker = 0u; worker < codegen_jobs; ++worker) {
        workers.push_back(std::async(std::launch::async, [&] {
            for (;;) {
                const auto index = next_partition.fetch_add(1u, std::memory_order_relaxed);
                if (index >= partitions.size()) return;
                generated[index] = emit_partition(partitions[index]);
            }
        }));
    }
    for (auto& worker : workers) worker.get();
    for (auto& artifact : generated) artifacts.push_back(std::move(*artifact));
    report_progress(options, "metadata");
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
    auto dispatch_artifacts = runtime_dispatch_artifacts(entry_namespace,
                                                         prepared.program,
                                                         prepared.entry_address,
                                                         options.diagnostic_partial,
                                                         prepared.analysis.runtime_code_copies.copies,
                                                         prepared.image,
                                                         prepared.boot_address,
                                                         prepared.boot_size);
    for (auto& artifact : dispatch_artifacts) artifacts.push_back(std::move(artifact));
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
    const auto descriptor_input =
        std::find_if(prepared.inputs.begin(), prepared.inputs.end(), [](const auto& input) {
            return input.role == "gdi-descriptor";
        });
    if (descriptor_input == prepared.inputs.end() || descriptor_input->local_path.empty())
        throw std::invalid_argument(
            "Portexport besitzt keine lokale GDI-Eingabe fuer die Installations-Recipe.");
    report_progress(options, "disc-recipe");
    const auto disc_source = katana::runtime::GdiDiscSource::open(descriptor_input->local_path);
    const auto& disc_descriptor = disc_source->descriptor();
    if (disc_descriptor.sha256 != descriptor_input->sha256)
        throw std::runtime_error("GDI wurde vor dem Recipe-Export veraendert.");
    for (const auto& track : disc_descriptor.tracks) {
        const auto role = "gdi-track-" + std::to_string(track.number);
        const auto expected = std::find_if(prepared.inputs.begin(),
                                           prepared.inputs.end(),
                                           [&](const auto& input) { return input.role == role; });
        if (expected == prepared.inputs.end() || expected->sha256 != track.sha256)
            throw std::runtime_error("GDI-Track wurde vor dem Recipe-Export veraendert.");
    }
    const auto* boot_segment =
        prepared.image.find_segment(prepared.boot_address, prepared.boot_size);
    if (boot_segment == nullptr)
        throw std::runtime_error("Portvertrag kann das analysierte Bootprogramm nicht binden.");
    const auto boot_offset = boot_segment->byte_offset(prepared.boot_address);
    if (!boot_offset || *boot_offset > boot_segment->bytes.size() ||
        prepared.boot_size > boot_segment->bytes.size() - *boot_offset)
        throw std::runtime_error("Portvertrag findet keine vollstaendigen Bootbytes.");
    const auto boot_bytes =
        std::string_view(reinterpret_cast<const char*>(boot_segment->bytes.data() + *boot_offset),
                         prepared.boot_size);
    const auto boot_sha256 = katana::io::sha256_bytes(boot_bytes);
    const auto recipe = katana::runtime::make_disc_install_recipe(
        *disc_source, std::string(prepared.project_identity), boot_sha256);
    const auto recipe_path = canonical_root / "content" / "game.katana-install";
    write_port_file(canonical_root,
                    "content/game.katana-install",
                    katana::runtime::format_disc_install_recipe(recipe),
                    true);
    std::filesystem::create_directories(canonical_root / "runtime");
    std::filesystem::create_directories(canonical_root / "user-data" / "content");
    report_progress(options, "artifact-write");
    const auto write = write_codegen_project(canonical_root / "generated",
                                             std::move(artifacts),
                                             ProjectWriteOptions{codegen_jobs});
    write_port_file(canonical_root, "CMakeLists.txt", root_cmake(), true);
    write_port_file(canonical_root,
                    ".gitignore",
                    "/build/\n/build-*/\n/user-data/\n/content/*.katana-disc\n*.katana-disc\n",
                    true);
    write_port_file(
        canonical_root,
        "INSTALL_ORIGINAL_DISC.txt",
        "ORIGINAL DISC REQUIRED - DISTRIBUTABLE PORT\n\n"
        "This package contains native AOT code and a hash/layout recipe, but no retail disc "
        "sectors.\nRun: game.exe --install-disc <path-to-your-own-disc.gdi>\n\n"
        "The installer validates descriptor, region-bearing boot data, complete track layout "
        "and SHA-256 identities before creating user-data/content/game.katana-disc locally.\n"
        "That local cache contains complete retail data and must never be redistributed.\n"
        "The original GDI and tracks are opened read-only and are never modified or deleted.\n",
        true);
    write_port_file(canonical_root,
                    "src/main.cpp",
                    handwritten_main(entry_namespace,
                                     prepared.hle_bios_abi,
                                     options.diagnostic_partial,
                                     prepared.inputs,
                                     wait_loop_descriptors,
                                     recipe.job_generation,
                                     recipe.content_identity,
                                     boot_sha256,
                                     options.console_profile,
                                     prepared.entry_address),
                    true);

    PortExportResult result;
    result.output_root = canonical_root;
    result.functions = prepared.program.size();
    result.partitions = partitions.size();
    result.generated_files = write.written_files.size();
    result.removed_files = write.removed_files.size();
    result.disc_install_recipe = recipe_path;
    result.job_generation = recipe.job_generation;
    result.content_identity = recipe.content_identity;
    result.disc_tracks = recipe.tracks.size();
    result.checkpoints = {"gdi-validated",
                          "disc-recipe-written",
                          "retail-content-excluded",
                          "boot-image-loaded",
                          "analysis-complete",
                          "ir-lowered",
                          "partitioned-codegen-complete",
                          "port-project-written"};
    return result;
}

PortExportResult export_dreamcast_port_project(const std::filesystem::path& gdi_path,
                                               const std::filesystem::path& output_root,
                                               const PortExportOptions& options) {
    if (gdi_path.empty()) {
        throw std::invalid_argument("Portexport braucht eine GDI-Quelle.");
    }
    report_progress(options, "disc-load");
    const auto disc = katana::platform::load_dreamcast_gdi_boot(gdi_path);
    report_progress(options, "boot-image");
    auto image = katana::platform::make_dreamcast_disc_executable(
        disc, katana::platform::DreamcastDiscExecutionPath::NativeSystemBootstrap);
    report_progress(options, "control-flow-analysis");
    const auto analysis_started = std::chrono::steady_clock::now();
    const auto analysis = katana::analysis::analyze_control_flow(
        image,
        nullptr,
        [&options, analysis_started](
            const katana::analysis::ControlFlowAnalysisProgress& progress) {
            if (options.progress_callback == nullptr) return;
            const bool sampled_iteration =
                progress.iteration <= 16u ||
                (progress.iteration != 0u &&
                 (progress.iteration & (progress.iteration - 1u)) == 0u);
            if (!sampled_iteration && progress.phase != "fixpoint-complete" &&
                progress.phase != "complete")
                return;
            std::ostringstream marker;
            marker << "control-flow-" << progress.phase << "-i" << progress.iteration << "-s"
                   << progress.seeds << "-n" << progress.instructions << "-c"
                   << progress.contexts << "-r" << progress.resolutions << "-ms"
                   << std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - analysis_started)
                          .count();
            const auto text = marker.str();
            report_progress(options, text);
        });
    report_progress(options, "ir-lowering");
    auto program = katana::ir::lower_program(analysis);
    report_progress(options, "ir-optimization");
    static_cast<void>(katana::ir::optimize_program(program));
    report_progress(options, "input-provenance");
    std::vector<katana::io::InputProvenance> inputs;
    const auto& descriptor = disc.source->descriptor();
    inputs.push_back(
        {"gdi-descriptor", descriptor.size, descriptor.sha256, descriptor.resolved_path});
    for (const auto& track : disc.source->descriptor().tracks) {
        inputs.push_back({"gdi-track-" + std::to_string(track.number),
                          track.file_offset + track.sector_count * track.sector_size,
                          track.sha256,
                          track.resolved_path});
    }
    std::ostringstream identity_material;
    for (const auto& input : inputs) {
        identity_material << input.role << ':' << input.size << ':' << input.sha256 << '\n';
    }
    const auto project_identity = katana::io::sha256_bytes(identity_material.str());
    return export_dreamcast_port_project({image,
                                          analysis,
                                          program,
                                          inputs,
                                          katana::platform::dreamcast_system_bootstrap_entry_address,
                                          katana::platform::dreamcast_disc_boot_address,
                                          disc.boot_file.size(),
                                          project_identity,
                                          true},
                                         output_root,
                                         options);
}

} // namespace katana::codegen
