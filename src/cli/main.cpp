#include "katana/agent/materialization_world.hpp"
#include "katana/analysis/analysis_overrides.hpp"
#include "katana/analysis/basic_blocks.hpp"
#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/analysis/graph_export.hpp"
#include "katana/analysis/hardware_audit.hpp"
#include "katana/analysis/parallel_work.hpp"
#include "katana/analysis/recursive_analysis.hpp"
#include "katana/app/application.hpp"
#include "katana/cli/exit_code.hpp"
#include "katana/cli/hardware_audit_policy.hpp"
#include "katana/codegen/backend.hpp"
#include "katana/codegen/cache.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/codegen/latent_aot_registry.hpp"
#include "katana/codegen/native_aot_profile.hpp"
#include "katana/codegen/port_export.hpp"
#include "katana/codegen/probe.hpp"
#include "katana/component_identity.hpp"
#include "katana/io/elf32_sh_loader.hpp"
#include "katana/io/input_output_error.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/io/json_report.hpp"
#include "katana/io/project_manifest.hpp"
#include "katana/io/raw_binary_loader.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/ir/serialize.hpp"
#include "katana/platform/dreamcast_disc.hpp"
#include "katana/platform/firmware_diagnostics.hpp"
#include "katana/runtime/abi.hpp"
#include "katana/runtime/disc_install.hpp"
#include "katana/runtime/gdi.hpp"
#include "katana/runtime/game_entry_handoff_artifact.hpp"
#include "katana/runtime/game_project_artifact.hpp"
#include "katana/runtime/native_port_artifact.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"
#include "katana/sh4/isa_coverage.hpp"

#include "port_export_orchestration.hpp"
#include "port_build_telemetry.hpp"
#include "host_build_progress.hpp"
#include "../runtime/prs_decode.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <syncstream>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#undef CompareString
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif
#endif

namespace {

using katana::cli::port_export_cache_key;
using katana::cli::port_export_cache_version;
using katana::cli::port_export_implementation_identities;
using katana::cli::port_export_recipe_identity;
using katana::cli::port_export_workspace_key;
using katana::cli::port_ir_contract_version;

class PortPhaseTimingRecorder final {
  public:
    explicit PortPhaseTimingRecorder(
        katana::cli::PortBuildTelemetryRecorder* telemetry = nullptr)
        : started_(std::chrono::steady_clock::now()),
          phase_started_(started_),
          telemetry_(telemetry) {}

    PortPhaseTimingRecorder(const PortPhaseTimingRecorder&) = delete;
    PortPhaseTimingRecorder& operator=(const PortPhaseTimingRecorder&) = delete;

    ~PortPhaseTimingRecorder() {
        try {
            static_cast<void>(emit());
        } catch (...) {
        }
    }

    void transition(const std::string_view phase) {
        const auto now = std::chrono::steady_clock::now();
        {
            const std::scoped_lock lock(mutex_);
            finish_current(now);
            current_phase_ = std::string(phase);
            phase_started_ = now;
        }
        if (telemetry_ != nullptr)
            telemetry_->sample_resources(phase);
    }

    void record_parallel_sample(const std::string_view phase,
                                const std::int64_t duration_ms) {
        const std::scoped_lock lock(mutex_);
        pending_parallel_samples_.push_back(
            {std::string(phase), duration_ms, true});
    }

    [[nodiscard]] bool should_emit_dynamic_progress(
        const bool force = false) noexcept {
        const std::scoped_lock lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            now - last_dynamic_progress_ <
                std::chrono::seconds(1))
            return false;
        last_dynamic_progress_ = now;
        return true;
    }

    [[nodiscard]] bool emit() noexcept {
        try {
            const std::scoped_lock lock(mutex_);
            if (emitted_) return telemetry_recorded_;
            const auto now = std::chrono::steady_clock::now();
            finish_current(now);
            emitted_ = true;
            std::ostringstream output;
            output << "{\"schema\":\"katana-port-phase-timings-v1\","
                      "\"total_ms\":"
                   << std::chrono::duration_cast<
                          std::chrono::milliseconds>(now - started_)
                          .count()
                   << ",\"phases\":[";
            for (std::size_t index = 0u;
                 index < samples_.size();
                 ++index) {
                if (index != 0u) output << ',';
                output << "{\"phase\":"
                       << katana::io::quote_json(
                              samples_[index].phase)
                       << ",\"duration_ms\":"
                       << samples_[index].duration_ms
                       << ",\"parallel\":"
                       << (samples_[index].parallel
                               ? "true"
                               : "false")
                       << '}';
            }
            output << "]}";
            std::osyncstream(std::cout)
                << "KATANA_PORT_PHASE_TIMINGS "
                << output.str() << '\n' << std::flush;
            telemetry_recorded_ =
                telemetry_ == nullptr || !telemetry_->enabled();
            if (telemetry_ != nullptr && telemetry_->enabled()) {
                std::vector<
                    katana::cli::PortBuildPhaseTimingSample>
                    telemetry_samples;
                telemetry_samples.reserve(samples_.size());
                for (const auto& sample : samples_)
                    telemetry_samples.push_back({
                        sample.phase,
                        sample.duration_ms < 0
                            ? 0u
                            : static_cast<std::uint64_t>(
                                  sample.duration_ms),
                        sample.parallel});
                telemetry_recorded_ =
                    telemetry_->record_phase_timings(
                        static_cast<std::uint64_t>(
                            std::max<std::int64_t>(
                                0,
                                std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    now - started_)
                                    .count())),
                        telemetry_samples);
            }
            return telemetry_recorded_;
        } catch (...) {
            try {
                const std::scoped_lock lock(mutex_);
                emitted_ = true;
                telemetry_recorded_ = false;
            } catch (...) {
            }
            return false;
        }
    }

    [[nodiscard]] std::string current_phase() const {
        const std::scoped_lock lock(mutex_);
        return current_phase_.empty() ? last_phase_ : current_phase_;
    }

  private:
    struct Sample {
        std::string phase;
        std::int64_t duration_ms = 0;
        bool parallel = false;
    };

    void finish_current(
        const std::chrono::steady_clock::time_point now) {
        if (!current_phase_.empty()) {
            last_phase_ = current_phase_;
            samples_.push_back(
                {current_phase_,
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     now - phase_started_)
                     .count(),
                 false});
            current_phase_.clear();
        }
        for (auto& sample : pending_parallel_samples_)
            samples_.push_back(std::move(sample));
        pending_parallel_samples_.clear();
    }

    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point phase_started_;
    std::chrono::steady_clock::time_point last_dynamic_progress_{};
    mutable std::mutex mutex_;
    std::string current_phase_;
    std::string last_phase_;
    std::vector<Sample> samples_;
    std::vector<Sample> pending_parallel_samples_;
    katana::cli::PortBuildTelemetryRecorder* telemetry_ = nullptr;
    bool emitted_ = false;
    bool telemetry_recorded_ = false;
};

class PortBuildTelemetryRun final {
  public:
    PortBuildTelemetryRun(
        katana::cli::PortBuildTelemetryRecorder& telemetry,
        PortPhaseTimingRecorder& phase_timings,
        const katana::ProgressReporter& reporter,
        katana::ProgressScope& progress,
        const bool telemetry_required) noexcept
        : telemetry_(telemetry),
          phase_timings_(phase_timings),
          reporter_(reporter),
          progress_(progress),
          telemetry_required_(telemetry_required) {}

    PortBuildTelemetryRun(const PortBuildTelemetryRun&) = delete;
    PortBuildTelemetryRun& operator=(const PortBuildTelemetryRun&) = delete;

    ~PortBuildTelemetryRun() noexcept {
        if (finished_) return;
        static_cast<void>(
            fail(katana::cli::exit_status(
                katana::cli::ExitCode::InternalError)));
    }

    void complete() {
        if (finished_) return;
        progress_.complete();
        const auto phase_timings_recorded =
            phase_timings_.emit();
        const auto progress_sealed = reporter_.seal_and_flush();
        if (!progress_sealed)
            static_cast<void>(
                telemetry_.mark_upstream_incomplete(
                    "progress-seal-failed",
                    reporter_.dropped_observations()));
        const auto preterminal = telemetry_.status();
        if (telemetry_required_ &&
            (!phase_timings_recorded || !progress_sealed ||
             !preterminal.enabled ||
             preterminal.io_failed ||
             !preterminal.telemetry_complete)) {
            telemetry_.finish(
                katana::cli::PortBuildTerminalOutcome::Failed,
                katana::cli::exit_status(
                    katana::cli::ExitCode::InputOutput),
                phase_timings_.current_phase());
            finished_ = true;
            throw katana::cli::Error(
                katana::cli::ExitCode::InputOutput,
                "Explizit angeforderte Portbuild-Telemetrie ist "
                "unvollstaendig.");
        }
        telemetry_.finish(
            katana::cli::PortBuildTerminalOutcome::Completed,
            0,
            phase_timings_.current_phase());
        finished_ = true;
        const auto terminal = telemetry_.status();
        if (telemetry_required_ &&
            (!terminal.terminal_emitted ||
             terminal.io_failed ||
             !terminal.telemetry_complete))
            throw katana::cli::Error(
                katana::cli::ExitCode::InputOutput,
                "Explizit angeforderte Portbuild-Telemetrie konnte "
                "nicht vollstaendig abgeschlossen werden.");
    }

    [[nodiscard]] bool fail(const int exit_code) noexcept {
        if (finished_) {
            const auto terminal = telemetry_.status();
            return !telemetry_required_ ||
                   (terminal.terminal_emitted &&
                    !terminal.io_failed);
        }
        progress_.fail();
        const auto phase_timings_recorded =
            phase_timings_.emit();
        if (telemetry_required_ && !phase_timings_recorded)
            static_cast<void>(
                telemetry_.mark_upstream_incomplete(
                    "phase-timings-record-failed"));
        const auto progress_sealed = reporter_.seal_and_flush();
        if (!progress_sealed)
            static_cast<void>(
                telemetry_.mark_upstream_incomplete(
                    "progress-seal-failed",
                    reporter_.dropped_observations()));
        try {
            telemetry_.finish(
                katana::cli::PortBuildTerminalOutcome::Failed,
                exit_code,
                phase_timings_.current_phase());
        } catch (...) {
            telemetry_.finish(
                katana::cli::PortBuildTerminalOutcome::Failed,
                exit_code,
                "unknown");
        }
        finished_ = true;
        const auto terminal = telemetry_.status();
        // A failed product build may legitimately seal an incomplete upstream
        // stream (for example before host configuration exists).  Once that
        // state and the original exit code were atomically published, never
        // replace the causal processing error with a secondary telemetry
        // completeness error. Successful builds retain the strict complete()
        // contract above.
        return !telemetry_required_ ||
               (terminal.terminal_emitted &&
                !terminal.io_failed);
    }

  private:
    katana::cli::PortBuildTelemetryRecorder& telemetry_;
    PortPhaseTimingRecorder& phase_timings_;
    const katana::ProgressReporter& reporter_;
    katana::ProgressScope& progress_;
    bool telemetry_required_ = false;
    bool finished_ = false;
};

void require_requested_failure_telemetry(
    const bool complete) {
    if (complete) return;
    throw katana::cli::Error(
        katana::cli::ExitCode::InputOutput,
        "Der Portbuild ist fehlgeschlagen und die explizit "
        "angeforderte Fehlertelemetrie konnte nicht terminal "
        "und atomar veroeffentlicht werden.");
}

void observe_port_export_progress(
    PortPhaseTimingRecorder& phase_timings,
    const std::string_view phase) {
    constexpr std::string_view module_timing_prefix{
        "latent-aot-module-analysis-ms:"};
    bool recorded_module_timing = false;
    if (phase.starts_with(module_timing_prefix)) {
        const auto payload = phase.substr(module_timing_prefix.size());
        const auto delimiter = payload.find(':');
        if (delimiter != std::string_view::npos) {
            std::uint64_t candidate_index = 0u;
            std::uint64_t duration_ms = 0u;
            const auto index_text = payload.substr(0u, delimiter);
            const auto duration_text = payload.substr(delimiter + 1u);
            const auto index_parse = std::from_chars(
                index_text.data(),
                index_text.data() + index_text.size(),
                candidate_index);
            const auto duration_parse = std::from_chars(
                duration_text.data(),
                duration_text.data() + duration_text.size(),
                duration_ms);
            if (index_parse.ec == std::errc{} &&
                index_parse.ptr == index_text.data() + index_text.size() &&
                duration_parse.ec == std::errc{} &&
                duration_parse.ptr ==
                    duration_text.data() + duration_text.size() &&
                duration_ms <=
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
                phase_timings.record_parallel_sample(
                    "export:latent-aot-module-analysis-" +
                        std::to_string(candidate_index),
                    static_cast<std::int64_t>(duration_ms));
                recorded_module_timing = true;
            }
        }
    }
    constexpr std::array timing_boundaries{
        std::string_view{"program-validation"},
        std::string_view{"latent-aot-source-validation"},
        std::string_view{"latent-aot-discovery"},
        std::string_view{"game-project-validation"},
        std::string_view{"partition-codegen"},
        std::string_view{"metadata"},
        std::string_view{"disc-recipe"},
        std::string_view{"artifact-write"},
        std::string_view{"disc-load"},
        std::string_view{"boot-artifact-load"},
        std::string_view{"boot-image"},
        std::string_view{"control-flow-analysis"},
        std::string_view{"ir-lowering"},
        std::string_view{"ir-optimization"},
        std::string_view{"input-provenance"}};
    const bool timing_boundary =
        std::find(
            timing_boundaries.begin(),
            timing_boundaries.end(),
            phase) != timing_boundaries.end() ||
        phase.starts_with("latent-aot-discovery-fixpoint:") ||
        phase == "ir-lowering-final" ||
        phase.starts_with("game-project-validation:");
    if (!recorded_module_timing && timing_boundary)
        phase_timings.transition(
            std::string("export:") + std::string(phase));
    const bool terminal_dynamic =
        phase.find("budget-exhausted") !=
            std::string_view::npos ||
        phase.find("cycle-exhausted") !=
            std::string_view::npos ||
        phase.starts_with("latent-primary-root-seed-cache-") ||
        phase.ends_with("-complete");
    if (!recorded_module_timing && !timing_boundary &&
        !phase_timings.should_emit_dynamic_progress(
                 terminal_dynamic))
        return;
    std::osyncstream(std::cout)
        << "KATANA_PORT_SUBPHASE " << phase << '\n'
        << std::flush;
}

void observe_structured_progress(
    const katana::ProgressEvent& event) {
    std::osyncstream(std::cout)
        << katana::format_progress_event_human(event) << '\n'
                                << std::flush;
}

std::uint32_t
parse_hex_value(std::string text, const std::uint32_t maximum, const std::string& description) {
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.erase(0, 2);
    }

    if (text.empty()) {
        throw std::invalid_argument(description + " darf nicht leer sein.");
    }

    const auto is_valid_hex =
        std::all_of(text.begin(), text.end(), [](const unsigned char character) {
            return std::isxdigit(character) != 0;
        });

    if (!is_valid_hex) {
        throw std::invalid_argument(description + " enthaelt ungueltige Hex-Zeichen.");
    }

    std::size_t parsed_characters = 0;
    const auto value = std::stoull(text, &parsed_characters, 16);

    if (parsed_characters != text.length() || value > maximum) {
        throw std::invalid_argument(description + " liegt ausserhalb des erlaubten Bereichs.");
    }

    return static_cast<std::uint32_t>(value);
}

std::string format_disassembly_text(const katana::sh4::DisassemblyLine& line) {
    std::ostringstream output;
    output << line.instruction.text;

    if (line.target_address.has_value()) {
        output << " 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
               << *line.target_address;
    }

    if (line.is_delay_slot) {
        output << "  ; delay slot";
    }

    return output.str();
}

void print_address(const std::uint32_t address) {
    std::cout << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << address;
}

std::string attach_execution_profile_json(std::string document,
                                          const katana::io::ProjectManifest& profile) {
    const auto object = document.find('{');
    if (object == std::string::npos) {
        throw std::runtime_error("JSON-Bericht besitzt kein Wurzelobjekt.");
    }
    document.insert(object + 1u,
                    "\"execution_profile\":" +
                        katana::io::format_project_execution_profile_json(profile) + ",");
    return document;
}

katana::sh4::ExternalIsaEvidence load_external_isa_evidence(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw katana::io::InputOutputError(
            "Externer SST-Evidencebericht konnte nicht geoeffnet werden.");

    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    constexpr std::streamoff maximum_size = 16 * 1024 * 1024;
    if (end < 0)
        throw katana::io::InputOutputError(
            "Groesse des externen SST-Evidenceberichts konnte nicht gelesen werden.");
    if (end > maximum_size)
        throw std::invalid_argument("Externer SST-Evidencebericht ist groesser als 16 MiB.");

    std::string document(static_cast<std::size_t>(end), '\0');
    input.seekg(0, std::ios::beg);
    if (!document.empty()) {
        input.read(document.data(), static_cast<std::streamsize>(document.size()));
        if (input.gcount() != static_cast<std::streamsize>(document.size()))
            throw katana::io::InputOutputError(
                "Externer SST-Evidencebericht konnte nicht vollstaendig gelesen werden.");
    }
    char unexpected = '\0';
    if (input.get(unexpected))
        throw katana::io::InputOutputError(
            "Externer SST-Evidencebericht wurde waehrend des Lesens veraendert.");
    return katana::sh4::parse_external_isa_evidence_json(document);
}

void require_cpp_profile_capabilities(const katana::io::ProjectManifest& profile) {
    try {
        katana::app::require_cpp_profile_capabilities(profile);
    } catch (const std::exception& error) {
        throw katana::cli::Error(katana::cli::ExitCode::CodeGenerationFailure, error.what());
    }
}

std::string_view special_register_name(const katana::ir::SpecialRegister special_register) {
    using Register = katana::ir::SpecialRegister;
    switch (special_register) {
    case Register::None:
        return "none";
    case Register::Mach:
        return "mach";
    case Register::Macl:
        return "macl";
    case Register::Pr:
        return "pr";
    case Register::Fpul:
        return "fpul";
    case Register::Fpscr:
        return "fpscr";
    case Register::Sr:
        return "sr";
    case Register::Gbr:
        return "gbr";
    case Register::Vbr:
        return "vbr";
    case Register::Ssr:
        return "ssr";
    case Register::Spc:
        return "spc";
    case Register::Sgr:
        return "sgr";
    case Register::Dbr:
        return "dbr";
    case Register::Bank0:
        return "r0_bank";
    case Register::Bank1:
        return "r1_bank";
    case Register::Bank2:
        return "r2_bank";
    case Register::Bank3:
        return "r3_bank";
    case Register::Bank4:
        return "r4_bank";
    case Register::Bank5:
        return "r5_bank";
    case Register::Bank6:
        return "r6_bank";
    case Register::Bank7:
        return "r7_bank";
    }
    return "none";
}

void print_ir_instruction(const katana::ir::Instruction& instruction) {
    print_address(instruction.source_address);

    std::cout << "  " << katana::ir::operation_name(instruction.operation);

    switch (instruction.operation) {
    case katana::ir::Operation::MovImmediate:
    case katana::ir::Operation::AddImmediate:
    case katana::ir::Operation::AndImmediate:
    case katana::ir::Operation::OrImmediate:
    case katana::ir::Operation::XorImmediate:
    case katana::ir::Operation::CompareEqualImmediate:
    case katana::ir::Operation::TestImmediate:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", " << instruction.immediate;
        break;

    case katana::ir::Operation::MovRegister:
    case katana::ir::Operation::AddRegister:
    case katana::ir::Operation::SubRegister:
    case katana::ir::Operation::NegateRegister:
    case katana::ir::Operation::NotRegister:
    case katana::ir::Operation::AddWithCarry:
    case katana::ir::Operation::AddWithOverflow:
    case katana::ir::Operation::SubWithCarry:
    case katana::ir::Operation::SubWithOverflow:
    case katana::ir::Operation::NegateWithCarry:
    case katana::ir::Operation::ExtendUnsignedByte:
    case katana::ir::Operation::ExtendUnsignedWord:
    case katana::ir::Operation::ExtendSignedByte:
    case katana::ir::Operation::ExtendSignedWord:
    case katana::ir::Operation::SwapBytes:
    case katana::ir::Operation::SwapWords:
    case katana::ir::Operation::ExtractMiddle:
    case katana::ir::Operation::ShiftArithmeticDynamic:
    case katana::ir::Operation::ShiftLogicalDynamic:
    case katana::ir::Operation::MultiplyLong:
    case katana::ir::Operation::MultiplySignedWord:
    case katana::ir::Operation::MultiplyUnsignedWord:
    case katana::ir::Operation::DoubleMultiplySignedLong:
    case katana::ir::Operation::DoubleMultiplyUnsignedLong:
    case katana::ir::Operation::MultiplyAccumulateWord:
    case katana::ir::Operation::MultiplyAccumulateLong:
    case katana::ir::Operation::DivideInitializeSigned:
    case katana::ir::Operation::DivideStep:
    case katana::ir::Operation::AndRegister:
    case katana::ir::Operation::OrRegister:
    case katana::ir::Operation::XorRegister:
    case katana::ir::Operation::CompareEqualRegister:
    case katana::ir::Operation::CompareHigherOrSame:
    case katana::ir::Operation::CompareGreaterOrEqual:
    case katana::ir::Operation::CompareHigher:
    case katana::ir::Operation::CompareGreaterThan:
    case katana::ir::Operation::CompareString:
    case katana::ir::Operation::TestRegister:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", r" << static_cast<unsigned>(instruction.source_register);
        break;

    case katana::ir::Operation::ComparePositiveOrZero:
    case katana::ir::Operation::ComparePositive:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register);
        break;
    case katana::ir::Operation::DecrementAndTest:
    case katana::ir::Operation::MoveT:
    case katana::ir::Operation::ShiftLogicalLeftOne:
    case katana::ir::Operation::ShiftLogicalRightOne:
    case katana::ir::Operation::ShiftArithmeticLeftOne:
    case katana::ir::Operation::ShiftArithmeticRightOne:
    case katana::ir::Operation::ShiftLogicalLeftTwo:
    case katana::ir::Operation::ShiftLogicalLeftEight:
    case katana::ir::Operation::ShiftLogicalLeftSixteen:
    case katana::ir::Operation::ShiftLogicalRightTwo:
    case katana::ir::Operation::ShiftLogicalRightEight:
    case katana::ir::Operation::ShiftLogicalRightSixteen:
    case katana::ir::Operation::RotateLeft:
    case katana::ir::Operation::RotateRight:
    case katana::ir::Operation::RotateLeftThroughT:
    case katana::ir::Operation::RotateRightThroughT:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register);
        break;
    case katana::ir::Operation::LoadByteSignedPostIncrement:
    case katana::ir::Operation::LoadWordSignedPostIncrement:
    case katana::ir::Operation::LoadLongPostIncrement:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", [r" << static_cast<unsigned>(instruction.source_register) << "+]";
        break;

    case katana::ir::Operation::LoadByteSignedDisplacement:
    case katana::ir::Operation::LoadWordSignedDisplacement:
    case katana::ir::Operation::LoadLongDisplacement:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", [r" << static_cast<unsigned>(instruction.source_register) << " + "
                  << instruction.displacement << "]";
        break;

    case katana::ir::Operation::LoadByteSignedR0Indexed:
    case katana::ir::Operation::LoadWordSignedR0Indexed:
    case katana::ir::Operation::LoadLongR0Indexed:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", [r0 + r" << static_cast<unsigned>(instruction.source_register) << "]";
        break;

    case katana::ir::Operation::LoadByteSignedGbrDisplacement:
    case katana::ir::Operation::LoadWordSignedGbrDisplacement:
    case katana::ir::Operation::LoadLongGbrDisplacement:
        std::cout << " r0, [gbr + " << std::dec << instruction.displacement << "]";
        break;

    case katana::ir::Operation::LoadWordSignedPcRelative:
    case katana::ir::Operation::LoadLongPcRelative:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", [";
        if (instruction.effective_address.has_value()) {
            print_address(*instruction.effective_address);
        }
        std::cout << "]";
        break;

    case katana::ir::Operation::MoveAddressPcRelative:
        std::cout << " r0, ";
        if (instruction.effective_address.has_value()) {
            print_address(*instruction.effective_address);
        }
        break;

    case katana::ir::Operation::StoreSpecialRegister:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", " << special_register_name(instruction.special_register);
        break;

    case katana::ir::Operation::StoreSpecialRegisterPreDecrement:
        std::cout << " [--r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << "], " << special_register_name(instruction.special_register);
        break;

    case katana::ir::Operation::LoadSpecialRegister:
        std::cout << " " << special_register_name(instruction.special_register) << ", r" << std::dec
                  << static_cast<unsigned>(instruction.source_register);
        break;

    case katana::ir::Operation::LoadSpecialRegisterPostIncrement:
        std::cout << " " << special_register_name(instruction.special_register) << ", [r"
                  << std::dec << static_cast<unsigned>(instruction.source_register) << "+]";
        break;

    case katana::ir::Operation::LoadByteSigned:
    case katana::ir::Operation::LoadWordSigned:
    case katana::ir::Operation::LoadLong:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", [r" << static_cast<unsigned>(instruction.source_register) << "]";
        break;

    case katana::ir::Operation::StoreBytePreDecrement:
    case katana::ir::Operation::StoreWordPreDecrement:
    case katana::ir::Operation::StoreLongPreDecrement:
        std::cout << " [--r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << "], r" << static_cast<unsigned>(instruction.source_register);
        break;

    case katana::ir::Operation::StoreByteDisplacement:
    case katana::ir::Operation::StoreWordDisplacement:
    case katana::ir::Operation::StoreLongDisplacement:
        std::cout << " [r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << " + " << instruction.displacement << "], r"
                  << static_cast<unsigned>(instruction.source_register);
        break;

    case katana::ir::Operation::StoreByteR0Indexed:
    case katana::ir::Operation::StoreWordR0Indexed:
    case katana::ir::Operation::StoreLongR0Indexed:
        std::cout << " [r0 + r" << std::dec
                  << static_cast<unsigned>(instruction.destination_register) << "], r"
                  << static_cast<unsigned>(instruction.source_register);
        break;

    case katana::ir::Operation::StoreByteGbrDisplacement:
    case katana::ir::Operation::StoreWordGbrDisplacement:
    case katana::ir::Operation::StoreLongGbrDisplacement:
        std::cout << " [gbr + " << std::dec << instruction.displacement << "], r0";
        break;

    case katana::ir::Operation::StoreByte:
    case katana::ir::Operation::StoreWord:
    case katana::ir::Operation::StoreLong:
        std::cout << " [r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << "], r" << static_cast<unsigned>(instruction.source_register);
        break;
    case katana::ir::Operation::Branch:
    case katana::ir::Operation::Call:
    case katana::ir::Operation::BranchIfTrue:
    case katana::ir::Operation::BranchIfFalse:
        if (instruction.target_address.has_value()) {
            std::cout << " ";
            print_address(*instruction.target_address);
        }
        break;

    case katana::ir::Operation::JumpRegister:
    case katana::ir::Operation::CallRegister:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.branch_register);
        break;

    case katana::ir::Operation::TrapAlways:
        std::cout << " #" << std::dec << instruction.immediate;
        break;

    case katana::ir::Operation::Unknown:
    case katana::ir::Operation::Nop:
    case katana::ir::Operation::DivideInitializeUnsigned:
    case katana::ir::Operation::ClearS:
    case katana::ir::Operation::SetS:
    case katana::ir::Operation::ClearT:
    case katana::ir::Operation::SetT:
    case katana::ir::Operation::Return:
    case katana::ir::Operation::ReturnFromException:
    case katana::ir::Operation::Sleep:
        break;
    }

    if (instruction.delay_slot.role == katana::ir::DelaySlotRole::Owner) {
        std::cout << " [delayed]";
    }

    if (instruction.delay_slot.role == katana::ir::DelaySlotRole::Slot) {
        std::cout << " [delay-slot]";
    }

    if (instruction.is_privileged) {
        std::cout << " [privileged]";
    }

    std::cout << '\n';
}

std::vector<katana::ir::Function>
build_ir_program(const std::filesystem::path& path,
                 const std::uint32_t entry_address,
                 const std::uint32_t base_address,
                 const std::optional<std::filesystem::path>& override_path = std::nullopt,
                 std::optional<katana::io::ProjectManifest>* execution_profile = nullptr) {
    auto extension = path.extension().string();
    std::transform(
        extension.begin(), extension.end(), extension.begin(), [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });

    katana::io::ExecutableImage image;
    if (extension == ".katana" || extension == ".manifest") {
        auto project = katana::io::load_project(path);
        if (execution_profile != nullptr) {
            *execution_profile = project.execution_profile;
        }
        image = std::move(project.image);
    } else {
        std::ifstream input(path, std::ios::binary);
        std::array<unsigned char, 4> magic{};
        input.read(reinterpret_cast<char*>(magic.data()),
                   static_cast<std::streamsize>(magic.size()));
        const bool is_elf = input.gcount() == static_cast<std::streamsize>(magic.size()) &&
                            magic[0] == 0x7Fu && magic[1] == 'E' && magic[2] == 'L' &&
                            magic[3] == 'F';
        if (is_elf) {
            image = katana::io::load_elf32_sh(path);
        } else {
            katana::io::RawBinaryLoadOptions options;
            options.base_address = base_address;
            options.entry_point = entry_address;
            image = katana::io::load_raw_binary(path, options);
        }
    }
    image.add_entry_point(entry_address);

    std::optional<katana::analysis::AnalysisOverrides> overrides;
    if (override_path) {
        overrides = katana::analysis::parse_analysis_overrides(*override_path);
    }
    const auto analysis =
        katana::analysis::analyze_control_flow(image, overrides ? &*overrides : nullptr);
    return katana::ir::lower_program(analysis);
}

int decode_single_opcode(const std::string& text) {
    const auto opcode = static_cast<std::uint16_t>(parse_hex_value(text, 0xFFFFu, "Der Opcode"));

    const auto instruction = katana::sh4::decode(opcode);

    std::cout << "Opcode:        0x" << std::hex << std::uppercase << std::setw(4)
              << std::setfill('0') << opcode << '\n'
              << "Instruktion:   " << instruction.text << '\n'
              << "Status:        " << (instruction.is_known() ? "erkannt" : "unbekannt") << '\n'
              << "Kontrollfluss: " << (instruction.changes_control_flow() ? "ja" : "nein") << '\n'
              << "Delay Slot:    " << (instruction.has_delay_slot ? "ja" : "nein") << '\n';

    return instruction.is_known() ? 0 : 1;
}

int analyze_manifest(const std::filesystem::path& path,
                     const std::optional<std::filesystem::path>& override_path = std::nullopt,
                     const bool json = false) {
    const auto project = katana::io::load_project(path);
    std::optional<katana::analysis::AnalysisOverrides> overrides;
    if (override_path.has_value()) {
        overrides = katana::analysis::parse_analysis_overrides(*override_path);
    }
    const auto analysis = katana::analysis::analyze_control_flow(
        project.image, overrides.has_value() ? &*overrides : nullptr);
    if (json) {
        std::cout << attach_execution_profile_json(
            katana::analysis::format_control_flow_analysis_json(analysis),
            project.execution_profile);
    } else {
        std::cout << katana::io::format_project_execution_profile_text(project.execution_profile)
                  << '\n';
        std::cout << katana::analysis::format_recursive_analysis_report(
            analysis.recursive, analysis.symbolic_addresses);
        std::cout << katana::analysis::format_indirect_control_flow_report(
            analysis.indirect_control_flow, analysis.jump_tables, analysis.symbolic_addresses);
    }
    return 0;
}

int audit_callback_contracts_manifest(const std::filesystem::path& path,
                                      const bool json) {
    const auto project = katana::io::load_project(path);
    const auto analysis = katana::analysis::analyze_control_flow(project.image);
    if (!json) {
        std::cout << "Statische Callback-Vertraege: "
                  << (analysis.static_callback_contracts_materialized
                          ? "materialized"
                          : "not-materialized")
                  << '\n'
                  << "Callback-Sinks: "
                  << analysis.static_callback_sinks.size() << '\n';
        for (const auto& sink : analysis.static_callback_sinks) {
            std::cout << "  function=0x" << std::hex << std::uppercase
                      << sink.function_address << " argument-mask=0x"
                      << static_cast<unsigned>(sink.argument_mask) << std::dec
                      << '\n';
        }
        std::cout << "Persistent-Pointer-Sinks: "
                  << analysis.static_persistent_pointer_sinks.size() << '\n';
        for (const auto& sink : analysis.static_persistent_pointer_sinks) {
            std::cout << "  function=0x" << std::hex << std::uppercase
                      << sink.function_address << " argument-mask=0x"
                      << static_cast<unsigned>(sink.argument_mask) << std::dec
                      << '\n';
        }
        std::cout << "Callback-Field-Sinks: "
                  << analysis.static_callback_field_sinks.size() << '\n';
        for (const auto& sink : analysis.static_callback_field_sinks) {
            std::cout << "  function=0x" << std::hex << std::uppercase
                      << sink.function_address << " call=0x"
                      << sink.call_instruction_address << " load=0x"
                      << sink.load_instruction_address << std::dec
                      << " displacement=" << sink.displacement
                      << " width=" << static_cast<unsigned>(sink.width)
                      << " kind=" << (sink.call ? "call" : "jump") << '\n';
        }
        std::cout << "Callback-Record-Tables: "
                  << analysis.static_callback_record_tables.size() << '\n';
        for (const auto& table : analysis.static_callback_record_tables) {
            std::cout << "  function=0x" << std::hex << std::uppercase
                      << table.function_address << " call=0x"
                      << table.call_instruction_address << " load=0x"
                      << table.callback_load_instruction_address
                      << " sink=0x" << table.callback_sink_address
                      << std::dec << " header-table-field="
                      << table.header_table_pointer_displacement
                      << " stride=" << table.record_stride
                      << " callback-field="
                      << table.callback_displacement << " argument="
                      << static_cast<unsigned>(table.callback_argument)
                      << " width=" << static_cast<unsigned>(table.width)
                      << '\n';
        }
        return analysis.static_callback_contracts_materialized ? 0 : 2;
    }

    std::cout << "{\"schema\":\"katana.callback-contract-audit.v1\",";
    std::cout << "\"materialized\":"
              << (analysis.static_callback_contracts_materialized
                      ? "true"
                      : "false")
              << ",\"callback_sinks\":[";
    for (std::size_t index = 0u;
         index < analysis.static_callback_sinks.size(); ++index) {
        if (index != 0u) std::cout << ',';
        const auto& sink = analysis.static_callback_sinks[index];
        std::cout << "{\"function_address\":" << sink.function_address
                  << ",\"argument_mask\":"
                  << static_cast<unsigned>(sink.argument_mask) << '}';
    }
    std::cout << "],\"persistent_pointer_sinks\":[";
    for (std::size_t index = 0u;
         index < analysis.static_persistent_pointer_sinks.size(); ++index) {
        if (index != 0u) std::cout << ',';
        const auto& sink = analysis.static_persistent_pointer_sinks[index];
        std::cout << "{\"function_address\":" << sink.function_address
                  << ",\"argument_mask\":"
                  << static_cast<unsigned>(sink.argument_mask) << '}';
    }
    std::cout << "],\"callback_field_sinks\":[";
    for (std::size_t index = 0u;
         index < analysis.static_callback_field_sinks.size(); ++index) {
        if (index != 0u) std::cout << ',';
        const auto& sink = analysis.static_callback_field_sinks[index];
        std::cout << "{\"function_address\":" << sink.function_address
                  << ",\"call_instruction_address\":"
                  << sink.call_instruction_address
                  << ",\"load_instruction_address\":"
                  << sink.load_instruction_address
                  << ",\"displacement\":" << sink.displacement
                  << ",\"width\":" << static_cast<unsigned>(sink.width)
                  << ",\"call\":" << (sink.call ? "true" : "false")
                  << '}';
    }
    std::cout << "],\"callback_record_tables\":[";
    for (std::size_t index = 0u;
         index < analysis.static_callback_record_tables.size(); ++index) {
        if (index != 0u) std::cout << ',';
        const auto& table = analysis.static_callback_record_tables[index];
        std::cout << "{\"function_address\":" << table.function_address
                  << ",\"call_instruction_address\":"
                  << table.call_instruction_address
                  << ",\"callback_load_instruction_address\":"
                  << table.callback_load_instruction_address
                  << ",\"callback_sink_address\":"
                  << table.callback_sink_address
                  << ",\"header_table_pointer_displacement\":"
                  << table.header_table_pointer_displacement
                  << ",\"record_stride\":" << table.record_stride
                  << ",\"callback_displacement\":"
                  << table.callback_displacement
                  << ",\"callback_argument\":"
                  << static_cast<unsigned>(table.callback_argument)
                  << ",\"width\":"
                  << static_cast<unsigned>(table.width) << '}';
    }
    std::cout << "]}\n";
    return analysis.static_callback_contracts_materialized ? 0 : 2;
}

katana::analysis::DreamcastHardwareAudit analyze_disc_hardware(const std::filesystem::path& path) {
    const auto disc = katana::platform::load_dreamcast_gdi_boot(path);
    const auto image = katana::platform::make_dreamcast_disc_executable(
        disc, katana::platform::DreamcastDiscExecutionPath::NativeSystemBootstrap);
    const auto analysis = katana::analysis::analyze_control_flow(image);
    auto audit = katana::analysis::audit_dreamcast_hardware(image, analysis);
    audit.scope = "native_disc_aot_boot_graph";
    return audit;
}

int audit_disc_hardware(const std::filesystem::path& path,
                        const bool json,
                        const bool include_accesses,
                        const bool fail_on_gap,
                        const bool strict) {
    const auto audit = analyze_disc_hardware(path);
    if (json)
        std::cout << katana::analysis::format_hardware_audit_json(audit, include_accesses) << '\n';
    else
        std::cout << katana::analysis::format_hardware_audit_text(audit);
    return katana::cli::hardware_audit_failed(audit, fail_on_gap, strict) ? 2 : 0;
}

std::vector<std::uint8_t> load_latent_aot_module_audit_bytes(
    const std::filesystem::path& input_path) {
    const auto path = std::filesystem::absolute(input_path).lexically_normal();
    const auto status = std::filesystem::symlink_status(path);
    if (!std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status))
        throw std::invalid_argument(
            "latent-aot-module-audit erwartet eine regulaere Nicht-Symlink-Datei.");
    const auto size = std::filesystem::file_size(path);
    if (size == 0u ||
        size > katana::runtime::maximum_native_aot_template_extent ||
        size > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument(
            "latent-aot-module-audit besitzt eine ungueltige Modulgroesse.");
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || static_cast<std::uintmax_t>(input.tellg()) != size)
        throw std::runtime_error(
            "latent-aot-module-audit konnte das Modul nicht stabil oeffnen.");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error(
            "latent-aot-module-audit konnte das Modul nicht stabil lesen.");
    return bytes;
}

int audit_latent_aot_module_cli(
    const std::filesystem::path& path,
    const std::uint32_t source_address,
    std::vector<std::uint32_t> entry_offsets,
    std::vector<katana::codegen::LatentAotExternalCallbackSink>
        external_callback_sinks,
    const std::optional<std::uint32_t> runtime_base,
    const bool sega_prs,
    const bool json) {
    std::sort(entry_offsets.begin(), entry_offsets.end());
    entry_offsets.erase(
        std::unique(entry_offsets.begin(), entry_offsets.end()),
        entry_offsets.end());
    if (entry_offsets.empty())
        throw std::invalid_argument(
            "latent-aot-module-audit benoetigt mindestens einen --entry.");
    const auto bytes = load_latent_aot_module_audit_bytes(path);
    katana::codegen::LatentAotDiscoveryOptions options;
    options.mode = katana::codegen::LatentAotDiscoveryMode::ExactOnly;
    options.completeness_policy =
        katana::codegen::LatentAotCompletenessPolicy::
            ExactRuntimeOnlyStopOnMiss;
    std::sort(
        external_callback_sinks.begin(), external_callback_sinks.end(),
        [](const auto& left, const auto& right) {
            return left.function_address < right.function_address;
        });
    std::vector<katana::codegen::LatentAotExternalCallbackSink>
        merged_external_callback_sinks;
    for (const auto& sink : external_callback_sinks) {
        if (!merged_external_callback_sinks.empty() &&
            merged_external_callback_sinks.back().function_address ==
                sink.function_address) {
            merged_external_callback_sinks.back().argument_mask |=
                sink.argument_mask;
        } else {
            merged_external_callback_sinks.push_back(sink);
        }
    }
    std::vector<std::uint32_t> external_code_targets;
    external_code_targets.reserve(merged_external_callback_sinks.size());
    for (const auto& sink : merged_external_callback_sinks)
        external_code_targets.push_back(sink.function_address);
    options.external_code_targets = external_code_targets;
    options.external_callback_sinks = merged_external_callback_sinks;
    if (!json)
        options.progress =
            katana::ProgressReporter(observe_structured_progress);
    const auto audit = runtime_base.has_value()
                           ? (sega_prs
                                  ? katana::codegen::
                                        audit_latent_aot_sega_prs_module(
                                            bytes, source_address,
                                            entry_offsets, *runtime_base,
                                            options)
                                  : katana::codegen::audit_latent_aot_module(
                                        bytes, source_address, entry_offsets,
                                        *runtime_base, options))
                           : (sega_prs
                                  ? katana::codegen::
                                        audit_latent_aot_sega_prs_module(
                                            bytes, source_address,
                                            entry_offsets, options)
                                  : katana::codegen::audit_latent_aot_module(
                                        bytes, source_address, entry_offsets,
                                        options));
    if (!json) {
        std::cout << "Latent-AOT-Modulaudit: "
                  << (audit.admitted ? "admitted" : "rejected") << '\n'
                  << "Byte-Identitaet: " << audit.byte_identity << '\n'
                  << "Modulgroesse: " << audit.byte_size << '\n'
                  << "Initiale Roots: " << audit.initial_entry_offsets.size()
                  << '\n'
                  << "Finale Roots: " << audit.final_entry_offsets.size()
                  << '\n'
                  << "Emittierte Bloecke/Funktionen: "
                  << audit.emitted_block_offsets.size() << '/'
                  << audit.emitted_function_offsets.size()
                  << '\n'
                  << "Inferierte Runtime-Basis: 0x" << std::hex
                  << std::uppercase << audit.inferred_runtime_base << std::dec
                  << " (identity-consistent="
                  << (audit.inferred_runtime_base_identity_consistent
                          ? "true"
                          : "false")
                  << ")\n"
                  << "Analysierte Funktionen: "
                  << audit.analyzed_function_offsets.size() << '\n';
        for (const auto& diagnostic : audit.loader_tail_diagnostics) {
            std::cout << "pass=" << diagnostic.analysis_pass
                      << " root=0x" << std::hex << std::uppercase
                      << diagnostic.root_offset
                      << " block=0x" << diagnostic.block_offset
                      << " literal=0x" << diagnostic.literal_offset
                      << " raw=0x" << diagnostic.raw_target
                      << " target=0x" << diagnostic.target_offset
                      << std::dec << " status="
                      << katana::codegen::
                             latent_aot_loader_tail_audit_status_name(
                                 diagnostic.status)
                      << '\n';
        }
        if (!audit.admitted)
            std::cout << "Ablehnung: " << audit.rejection
                      << " (" << audit.rejection_detail << ")\n";
        return audit.admitted ? 0 : 2;
    }

    std::cout << "{\"schema\":\"katana.latent-aot-module-audit.v1\",";
    std::cout << "\"admitted\":"
              << (audit.admitted ? "true" : "false") << ',';
    std::cout << "\"byte_identity\":"
              << katana::io::quote_json(audit.byte_identity) << ',';
    std::cout << "\"byte_size\":" << audit.byte_size << ',';
    std::cout << "\"source_address\":" << audit.source_address << ',';
    const auto write_offsets = [](const std::span<const std::uint32_t> values) {
        std::cout << '[';
        for (std::size_t index = 0u; index < values.size(); ++index) {
            if (index != 0u) std::cout << ',';
            std::cout << values[index];
        }
        std::cout << ']';
    };
    std::cout << "\"initial_entry_offsets\":";
    write_offsets(audit.initial_entry_offsets);
    std::cout << ",\"final_entry_offsets\":";
    write_offsets(audit.final_entry_offsets);
    std::cout << ",\"emitted_block_offsets\":";
    write_offsets(audit.emitted_block_offsets);
    std::cout << ",\"emitted_function_offsets\":";
    write_offsets(audit.emitted_function_offsets);
    std::cout << ",\"inferred_runtime_base\":"
              << audit.inferred_runtime_base
              << ",\"inferred_runtime_base_identity_consistent\":"
              << (audit.inferred_runtime_base_identity_consistent
                      ? "true"
                      : "false")
              << ",\"analyzed_function_offsets\":";
    write_offsets(audit.analyzed_function_offsets);
    std::cout << ",\"loader_tail_diagnostics\":[";
    for (std::size_t index = 0u;
         index < audit.loader_tail_diagnostics.size(); ++index) {
        if (index != 0u) std::cout << ',';
        const auto& diagnostic = audit.loader_tail_diagnostics[index];
        std::cout << "{\"pass\":" << diagnostic.analysis_pass
                  << ",\"root_offset\":" << diagnostic.root_offset
                  << ",\"block_offset\":" << diagnostic.block_offset
                  << ",\"literal_offset\":" << diagnostic.literal_offset
                  << ",\"raw_target\":" << diagnostic.raw_target
                  << ",\"target_offset\":" << diagnostic.target_offset
                  << ",\"status\":"
                  << katana::io::quote_json(
                         katana::codegen::
                             latent_aot_loader_tail_audit_status_name(
                                 diagnostic.status))
                  << '}';
    }
    std::cout << "],\"rejection\":"
              << katana::io::quote_json(audit.rejection)
              << ",\"rejection_detail\":"
              << katana::io::quote_json(audit.rejection_detail)
              << "}\n";
    return audit.admitted ? 0 : 2;
}

struct DiscAuditSetEntry {
    std::filesystem::path path;
    std::optional<katana::analysis::DreamcastHardwareAudit> audit;
    std::string error;
};

int audit_disc_hardware_set(const std::filesystem::path& root,
                            const bool json,
                            const std::size_t requested_jobs,
                            const bool fail_on_gap,
                            const bool strict) {
    if (!std::filesystem::is_directory(root))
        throw std::invalid_argument("disc-audit-set erwartet ein Verzeichnis.");
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        auto extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](const char value) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
        });
        if (extension == ".gdi") paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    if (paths.empty()) throw std::invalid_argument("disc-audit-set fand keine GDI-Quelle.");
    const auto jobs = std::max<std::size_t>(1u, std::min(requested_jobs, paths.size()));
    std::vector<DiscAuditSetEntry> results(paths.size());
    for (std::size_t first = 0u; first < paths.size(); first += jobs) {
        const auto count = std::min(jobs, paths.size() - first);
        std::vector<std::future<DiscAuditSetEntry>> workers;
        workers.reserve(count);
        for (std::size_t offset = 0u; offset < count; ++offset) {
            const auto path = paths[first + offset];
            workers.push_back(std::async(std::launch::async, [path] {
                DiscAuditSetEntry result;
                result.path = path;
                try {
                    result.audit = analyze_disc_hardware(path);
                } catch (const std::exception& error) {
                    result.error = error.what();
                }
                return result;
            }));
        }
        for (std::size_t offset = 0u; offset < count; ++offset)
            results[first + offset] = workers[offset].get();
    }

    std::size_t failures = 0u;
    std::size_t definite_gaps = 0u;
    std::size_t strict_failures = 0u;
    for (const auto& result : results) {
        if (!result.audit) {
            ++failures;
            continue;
        }
        const auto& audit = *result.audit;
        const auto gap = katana::cli::hardware_audit_has_definite_gap(audit);
        definite_gaps += gap ? 1u : 0u;
        strict_failures += katana::cli::hardware_audit_failed(audit, false, true) ? 1u : 0u;
    }
    if (json) {
        std::cout << "{\"schema\":\"katana.hardware-audit-set.v2\",\"status\":"
                  << katana::io::quote_json(failures == 0u ? "success" : "partial")
                  << ",\"jobs\":" << jobs << ",\"disc_count\":" << results.size()
                  << ",\"load_failures\":" << failures << ",\"results\":[";
        for (std::size_t index = 0u; index < results.size(); ++index) {
            if (index != 0u) std::cout << ',';
            const auto& result = results[index];
            std::cout << "{\"source_name\":"
                      << katana::io::quote_json(result.path.filename().string());
            if (result.audit)
                std::cout << ",\"audit\":"
                          << katana::analysis::format_hardware_audit_json(*result.audit);
            else
                std::cout << ",\"error\":" << katana::io::quote_json(result.error);
            std::cout << '}';
        }
        std::cout << "]}\n";
    } else {
        std::cout << "Disc audit set: discs=" << results.size() << " jobs=" << jobs
                  << " load_failures=" << failures << '\n';
        for (const auto& result : results) {
            std::cout << result.path.filename().string() << ": ";
            if (!result.audit) {
                std::cout << "error=" << result.error << '\n';
                continue;
            }
            const auto& audit = *result.audit;
            std::cout << "instructions=" << audit.reachable_instructions
                      << " functions=" << audit.reachable_functions
                      << " unknown=" << audit.unknown_instructions
                      << " hardware=" << audit.addresses.size()
                      << " implemented=" << audit.implemented_addresses
                      << " partial=" << audit.partial_addresses
                      << " known_gap=" << audit.known_gap_addresses
                      << " rejected=" << audit.rejected_addresses
                      << " unmapped=" << audit.unmapped_addresses
                      << " unresolved_poll_guard_loops=" << audit.unresolved_poll_guard_loops
                      << '\n';
        }
    }
    if (failures != 0u) return 2;
    if ((fail_on_gap && definite_gaps != 0u) || (strict && strict_failures != 0u)) return 2;
    return 0;
}

int export_analysis_graph(const std::filesystem::path& path,
                          const std::optional<std::filesystem::path>& override_path,
                          const std::string_view command) {
    const auto project = katana::io::load_project(path);
    std::optional<katana::analysis::AnalysisOverrides> overrides;
    if (override_path.has_value()) {
        overrides = katana::analysis::parse_analysis_overrides(*override_path);
    }
    const auto analysis = katana::analysis::analyze_control_flow(
        project.image, overrides.has_value() ? &*overrides : nullptr);
    const bool call_graph = command.starts_with("callgraph-");
    const auto graph = call_graph ? katana::analysis::build_call_graph(analysis)
                                  : katana::analysis::build_control_flow_graph(analysis);
    if (command.ends_with("-json")) {
        std::cout << attach_execution_profile_json(
            katana::analysis::serialize_analysis_graph_json(graph), project.execution_profile);
    } else {
        std::cout << "// "
                  << katana::io::format_project_execution_profile_text(project.execution_profile)
                  << '\n'
                  << katana::analysis::serialize_analysis_graph_dot(graph);
    }
    return 0;
}

int diagnose_firmware(const std::filesystem::path& path,
                      const katana::platform::FirmwareImageKind kind,
                      const katana::platform::FirmwareDiagnosticOptions& options) {
    const auto report = katana::platform::inspect_firmware_file(path, kind, options);
    std::cout << katana::platform::format_firmware_diagnostic_json(report);
    return katana::cli::exit_status(report.valid() ? katana::cli::ExitCode::Success
                                                   : katana::cli::ExitCode::ProcessingFailure);
}

std::optional<std::string> pc_relative_disassembly_annotation(
    const katana::io::ExecutableImage& image,
    const katana::sh4::DisassemblyLine& line) {
    using Kind = katana::sh4::InstructionKind;
    const auto kind = line.instruction.kind;
    std::uint32_t literal_address = 0u;
    std::size_t width = 0u;
    if (kind == Kind::MovLongLoadPcRelative) {
        literal_address = ((line.address + 4u) & ~3u) +
                          static_cast<std::uint32_t>(line.instruction.displacement);
        width = 4u;
    } else if (kind == Kind::MovWordLoadPcRelative) {
        literal_address = line.address + 4u +
                          static_cast<std::uint32_t>(line.instruction.displacement);
        width = 2u;
    } else if (kind == Kind::MoveAddressPcRelative) {
        const auto value = ((line.address + 4u) & ~3u) +
                           static_cast<std::uint32_t>(line.instruction.displacement);
        std::ostringstream annotation;
        annotation << "mova=0x" << std::hex << std::uppercase << std::setw(8)
                   << std::setfill('0') << value;
        return annotation.str();
    } else {
        return std::nullopt;
    }

    const auto* segment = image.find_segment(literal_address, width);
    if (segment == nullptr) {
        return std::nullopt;
    }
    const auto offset = segment->byte_offset(literal_address);
    if (!offset.has_value()) {
        return std::nullopt;
    }

    std::uint32_t value = segment->bytes[*offset];
    value |= static_cast<std::uint32_t>(segment->bytes[*offset + 1u]) << 8u;
    if (width == 4u) {
        value |= static_cast<std::uint32_t>(segment->bytes[*offset + 2u]) << 16u;
        value |= static_cast<std::uint32_t>(segment->bytes[*offset + 3u]) << 24u;
    }

    std::ostringstream annotation;
    annotation << "literal@0x" << std::hex << std::uppercase << std::setw(8)
               << std::setfill('0') << literal_address << "=0x" << std::setw(width * 2u)
               << value;
    return annotation.str();
}

int disassemble_file(const std::filesystem::path& path,
                     const std::uint32_t base_address,
                     const std::uint32_t file_offset = 0u,
                     const std::optional<std::uint32_t> requested_byte_count = std::nullopt) {
    katana::io::RawBinaryLoadOptions options;
    options.base_address = base_address;
    auto image = katana::io::load_raw_binary(path, options);
    const auto source_byte_count = image.segments().front().bytes.size();

    if ((file_offset & 1u) != 0u) {
        throw std::invalid_argument("Der Disassembly-Dateioffset muss 2-Byte-ausgerichtet sein.");
    }
    if (file_offset >= source_byte_count) {
        throw std::invalid_argument("Der Disassembly-Dateioffset liegt ausserhalb der Datei.");
    }

    const auto available_byte_count = source_byte_count - file_offset;
    const auto selected_byte_count = requested_byte_count.has_value()
                                         ? static_cast<std::size_t>(*requested_byte_count)
                                         : available_byte_count;
    if (selected_byte_count == 0u || selected_byte_count > available_byte_count) {
        throw std::invalid_argument("Der Disassembly-Bereich liegt ausserhalb der Datei.");
    }
    if ((selected_byte_count & 1u) != 0u) {
        throw std::invalid_argument("Die Disassembly-Byteanzahl muss durch 2 teilbar sein.");
    }

    const auto range_address = static_cast<std::uint64_t>(base_address) + file_offset;
    const auto range_end = range_address + selected_byte_count;
    if (range_address > std::numeric_limits<std::uint32_t>::max() ||
        range_end > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u) {
        throw std::invalid_argument("Der Disassembly-Bereich ueberlaeuft den 32-Bit-Gastadressraum.");
    }

    if (file_offset != 0u || selected_byte_count != source_byte_count) {
        const auto& source_segment = image.segments().front();
        std::vector<std::uint8_t> selected_bytes(
            source_segment.bytes.begin() + static_cast<std::ptrdiff_t>(file_offset),
            source_segment.bytes.begin() +
                static_cast<std::ptrdiff_t>(file_offset + selected_byte_count));
        katana::io::ExecutableImage selected_image(path);
        katana::io::ImageSegment selected_segment{
            ".raw-range",
            static_cast<std::uint32_t>(range_address),
            file_offset,
            selected_byte_count,
            katana::io::SegmentKind::Code,
            katana::io::SegmentPermissions{true, false, true},
            std::move(selected_bytes)};
        selected_segment.source_kind = katana::io::ImageSourceKind::RawBinary;
        selected_segment.local_source_name = path.filename().string();
        selected_image.add_segment(std::move(selected_segment));
        image = std::move(selected_image);
    }
    const auto lines = katana::sh4::disassemble(image);

    std::size_t unknown_count = 0;
    std::size_t control_flow_count = 0;
    std::size_t delay_slot_count = 0;

    std::cout << "Datei:         " << path.string() << '\n'
              << "Dateigroesse:  " << std::dec << source_byte_count << " Bytes\n"
              << "Dateioffset:   0x" << std::hex << std::uppercase << file_offset << '\n'
              << "Bereich:       " << std::dec << selected_byte_count << " Bytes\n"
              << "Basisadresse:  0x" << std::hex << std::uppercase << std::setw(8)
              << std::setfill('0') << static_cast<std::uint32_t>(range_address) << "\n\n";

    for (const auto& line : lines) {
        if (!line.instruction.is_known()) {
            ++unknown_count;
        }

        if (line.instruction.changes_control_flow()) {
            ++control_flow_count;
        }

        if (line.is_delay_slot) {
            ++delay_slot_count;
        }

        print_address(line.address);

        std::cout << "  " << std::setw(4) << line.opcode << "  " << format_disassembly_text(line);
        if (const auto annotation = pc_relative_disassembly_annotation(image, line);
            annotation.has_value()) {
            std::cout << "  ; " << *annotation;
        }
        std::cout << '\n';
    }

    std::cout << "\nInstruktionen:         " << std::dec << lines.size()
              << "\nKontrollfluss:         " << control_flow_count
              << "\nMarkierte Delay Slots: " << delay_slot_count
              << "\nUnbekannte Opcodes:    " << unknown_count << '\n';

    return 0;
}

int analyze_blocks(const std::filesystem::path& path, const std::uint32_t base_address) {
    katana::io::RawBinaryLoadOptions options;
    options.base_address = base_address;
    options.entry_point = base_address;
    const auto image = katana::io::load_raw_binary(path, options);
    const auto lines = katana::analysis::analyze_reachable_code(image).instructions;
    const auto blocks = katana::analysis::build_basic_blocks(lines);

    std::cout << "Datei:         " << path.string() << '\n'
              << "Dateigroesse:  " << std::dec << image.segments()[0].bytes.size() << " Bytes\n"
              << "Basic Blocks:  " << blocks.size() << "\n\n";

    for (const auto& block : blocks) {
        std::cout << "Block " << std::dec << block.id << ": ";

        print_address(block.start_address);
        std::cout << " - ";
        print_address(block.end_address);
        std::cout << '\n';

        for (const auto& line : block.lines) {
            std::cout << "  ";
            print_address(line.address);

            std::cout << "  " << std::setw(4) << line.opcode << "  "
                      << format_disassembly_text(line) << '\n';
        }

        std::cout << "  Nachfolger: ";

        if (block.successors.empty() && !block.has_indirect_successor) {
            std::cout << "keine";
        } else {
            bool first = true;

            for (const auto successor : block.successors) {
                if (!first) {
                    std::cout << ", ";
                }

                print_address(successor);
                first = false;
            }

            if (block.has_indirect_successor) {
                if (!first) {
                    std::cout << ", ";
                }

                std::cout << "indirekt";
            }
        }

        std::cout << "\n\n";
    }

    return 0;
}

int analyze_functions(const std::filesystem::path& path,
                      const std::uint32_t entry_address,
                      const std::uint32_t base_address) {
    katana::io::RawBinaryLoadOptions options;
    options.base_address = base_address;
    options.entry_point = entry_address;
    const auto image = katana::io::load_raw_binary(path, options);
    const auto lines = katana::analysis::analyze_reachable_code(image).instructions;

    const std::array<std::uint32_t, 1> seeds = {entry_address};

    const auto functions = katana::analysis::discover_functions(lines, seeds);

    std::cout << "Datei:         " << path.string() << '\n'
              << "Dateigroesse:  " << std::dec << image.segments()[0].bytes.size() << " Bytes\n"
              << "Einstieg:      ";

    print_address(entry_address);

    std::cout << "\nFunktionen:    " << std::dec << functions.size() << "\n\n";

    for (const auto& function : functions) {
        std::cout << "Funktion " << std::dec << function.id << ": ";

        print_address(function.entry_address);
        std::cout << '\n';

        std::cout << "  Basic Blocks: ";

        if (function.block_addresses.empty()) {
            std::cout << "keine";
        } else {
            for (std::size_t index = 0; index < function.block_addresses.size(); ++index) {
                if (index != 0u) {
                    std::cout << ", ";
                }

                print_address(function.block_addresses[index]);
            }
        }

        std::cout << "\n  Direkte Aufrufe: ";

        if (function.direct_callees.empty()) {
            std::cout << "keine";
        } else {
            for (std::size_t index = 0; index < function.direct_callees.size(); ++index) {
                if (index != 0u) {
                    std::cout << ", ";
                }

                print_address(function.direct_callees[index]);
            }
        }

        std::cout << "\n  Indirekte Aufrufe: ";

        if (function.indirect_call_sites.empty()) {
            std::cout << "keine";
        } else {
            for (std::size_t index = 0; index < function.indirect_call_sites.size(); ++index) {
                if (index != 0u) {
                    std::cout << ", ";
                }

                print_address(function.indirect_call_sites[index]);
            }
        }

        std::cout << "\n\n";
    }

    return 0;
}

int analyze_ir(const std::filesystem::path& path,
               const std::uint32_t entry_address,
               const std::uint32_t base_address,
               const bool json,
               const std::optional<std::filesystem::path>& override_path) {
    std::optional<katana::io::ProjectManifest> execution_profile;
    const auto program =
        build_ir_program(path, entry_address, base_address, override_path, &execution_profile);

    if (json) {
        auto report = katana::ir::emit_ir_json(program);
        std::cout << (execution_profile
                          ? attach_execution_profile_json(std::move(report), *execution_profile)
                          : report);
    } else {
        std::cout << katana::ir::emit_ir_text(program);
        if (execution_profile) {
            std::cout << katana::io::format_project_execution_profile_text(*execution_profile)
                      << '\n';
        }
    }

    return 0;
}

int emit_cpp(const std::filesystem::path& input_path,
             const std::uint32_t entry_address,
             const std::filesystem::path& output_path,
             const std::uint32_t base_address,
             katana::ir::OptimizationOptions optimization_options,
             const std::optional<std::filesystem::path>& dump_prefix,
             const std::optional<std::filesystem::path>& override_path) {
    std::optional<katana::io::ProjectManifest> execution_profile;
    auto program = build_ir_program(
        input_path, entry_address, base_address, override_path, &execution_profile);
    if (execution_profile) require_cpp_profile_capabilities(*execution_profile);

    const auto before_optimization =
        dump_prefix ? katana::ir::emit_ir_text(program) : std::string{};
    optimization_options.capture_dumps = dump_prefix.has_value();
    const auto optimization_report = katana::ir::optimize_program(program, optimization_options);

    if (dump_prefix) {
        const auto write_dump = [](const std::filesystem::path& path, const std::string& contents) {
            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path());
            }
            std::ofstream output(path, std::ios::binary);
            if (!output) {
                throw katana::io::InputOutputError("Der IR-Dump konnte nicht geoeffnet werden.");
            }
            output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (!output) {
                throw katana::io::InputOutputError("Der IR-Dump konnte nicht gespeichert werden.");
            }
        };
        write_dump(dump_prefix->string() + ".before.ir", before_optimization);
        write_dump(dump_prefix->string() + ".after.ir", katana::ir::emit_ir_text(program));
    }

    const auto source = katana::codegen::emit_cpp_program(program, entry_address);

    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    std::ofstream output(output_path, std::ios::binary);

    if (!output) {
        throw katana::io::InputOutputError("Die Ausgabedatei konnte nicht geoeffnet werden.");
    }

    output.write(source.data(), static_cast<std::streamsize>(source.size()));

    if (!output) {
        throw katana::io::InputOutputError(
            "Der generierte C++-Code konnte nicht gespeichert werden.");
    }

    std::cout << "C++-Code erzeugt: " << output_path.string() << '\n'
              << "Funktionen:       " << std::dec << program.size() << '\n'
              << "Zeichen:          " << source.size() << '\n'
              << "Optimierungen:    " << optimization_report.total_changes << '\n';

    return 0;
}

int emit_phase6_probe_source(const std::filesystem::path& gdi_path,
                             const std::filesystem::path& output_path) {
    const auto disc = katana::platform::load_dreamcast_gdi_boot(gdi_path);
    const auto image = katana::platform::make_dreamcast_disc_executable(disc);
    const auto analysis = katana::analysis::analyze_control_flow(image);
    if (!analysis.recursive.diagnostics.empty()) {
        throw std::runtime_error("Der Phase-6-Bootblock enthaelt eine unbekannte Instruktion.");
    }
    const auto program = katana::ir::lower_program(analysis);
    const auto function =
        std::find_if(program.begin(), program.end(), [](const katana::ir::Function& value) {
            return value.entry_address == katana::platform::dreamcast_disc_boot_address;
        });
    if (function == program.end()) {
        throw std::runtime_error("Der Phase-6-Programmeinstieg wurde nicht analysiert.");
    }
    const auto block = std::find_if(
        function->blocks.begin(), function->blocks.end(), [](const katana::ir::BasicBlock& value) {
            return value.start_address == katana::platform::dreamcast_disc_boot_address;
        });
    if (block == function->blocks.end() || block->instructions.empty()) {
        throw std::runtime_error("Der Phase-6-Einstiegsblock ist leer oder fehlt.");
    }
    if (katana::codegen::block_requires_call_dispatch(*block)) {
        throw std::runtime_error(
            "Der Phase-6-Einstiegsblock braucht fuer diese Probe bereits einen Call-Dispatch.");
    }

    katana::ir::Function probe;
    probe.entry_address = function->entry_address;
    probe.blocks.push_back(*block);
    probe.blocks.front().successors.clear();
    const std::array<katana::ir::Function, 1u> probe_program = {std::move(probe)};
    auto source = katana::codegen::emit_cpp_program(probe_program,
                                                    katana::platform::dreamcast_disc_boot_address);
    source += R"cpp(

#include "katana/platform/phase6_gate.hpp"
#include <exception>
#include <filesystem>
#include <iostream>

int main(const int argc, const char* const* argv) {
    if (argc != 3) {
        std::cerr << "Phase-6-Probe erwartet GDI-Quelle und Berichtsausgabe.\n";
        return 2;
    }
    try {
        const auto report = katana::platform::run_phase6_gate(
            std::filesystem::path(argv[1]),
            katana_generated::run,
)cpp";
    source += std::to_string(block->instructions.size());
    source += R"cpp(u
        );
        katana::platform::write_phase6_gate_report(report, std::filesystem::path(argv[2]));
        std::cout << report.checkpoint << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Phase-6-Gate fehlgeschlagen: " << error.what() << '\n';
        return 1;
    }
}
)cpp";

    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw katana::io::InputOutputError(
            "Die lokale Phase-6-Probe konnte nicht geoeffnet werden.");
    }
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    if (!output) {
        throw katana::io::InputOutputError(
            "Die lokale Phase-6-Probe konnte nicht geschrieben werden.");
    }
    std::cout << "Lokale, temporaere Phase-6-Blockprobe erzeugt.\n";
    return 0;
}

std::optional<std::string> configured_environment_value(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t value_size = 0u;
    if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
    if (result.empty()) return std::nullopt;
    return result;
#else
    const auto* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::string(value);
#endif
}

std::filesystem::path discover_source_root_for_protection() {
    std::vector<std::filesystem::path> starts{std::filesystem::current_path()};
#ifdef _WIN32
    std::wstring executable(32'768u, L'\0');
    const auto length =
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length != 0u && length < executable.size()) {
        executable.resize(length);
        starts.push_back(std::filesystem::path(executable).parent_path());
    }
#else
    std::error_code link_error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", link_error);
    if (!link_error) starts.push_back(executable.parent_path());
#endif
    for (auto start : starts) {
        for (;;) {
            if (std::filesystem::exists(start / ".git") &&
                std::filesystem::exists(start / "include" / "katana") &&
                std::filesystem::exists(start / "CMakeLists.txt"))
                return std::filesystem::canonical(start);
            const auto parent = start.parent_path();
            if (parent.empty() || parent == start) break;
            start = parent;
        }
    }
    return {};
}

struct RuntimeBuildBinding {
    std::filesystem::path package_prefix;
    std::filesystem::path build_targets_file;
    std::filesystem::path source_root;
    std::string build_configuration;
    bool multi_config = false;
    bool msbuild_generator = false;
};

bool is_runtime_source_root(const std::filesystem::path& root) {
    return std::filesystem::exists(root / "CMakeLists.txt") &&
           std::filesystem::exists(
               root / "include" / "katana" / "runtime" / "aot_runtime_abi.hpp");
}

bool is_runtime_package_prefix(const std::filesystem::path& prefix) {
    if (!std::filesystem::exists(
            prefix / "include" / "katana" / "runtime" / "aot_runtime_abi.hpp"))
        return false;
    return std::filesystem::exists(
               prefix / "lib" / "cmake" / "KatanaRecomp" /
               "KatanaRecompConfig.cmake") ||
           std::filesystem::exists(
               prefix / "lib64" / "cmake" / "KatanaRecomp" /
               "KatanaRecompConfig.cmake") ||
           std::filesystem::exists(
               prefix / "share" / "KatanaRecomp" / "KatanaRecompConfig.cmake");
}

bool is_runtime_build_targets_file(
    const std::filesystem::path& targets_file) {
    std::error_code status_error;
    const auto status =
        std::filesystem::symlink_status(
            targets_file, status_error);
    return !status_error &&
           std::filesystem::is_regular_file(status) &&
           !std::filesystem::is_symlink(status) &&
           targets_file.filename() ==
                "KatanaRuntimeBuildTargets.cmake";
}

struct RuntimeBuildTreeProfile final {
    std::string configuration;
    bool multi_config = false;
    bool msbuild_generator = false;
};

std::optional<std::string> cmake_cache_value(
    const std::filesystem::path& cache_path,
    const std::string_view key) {
    std::ifstream cache(cache_path, std::ios::binary);
    if (!cache)
        throw std::invalid_argument(
            "Katana-Buildtree besitzt keinen lesbaren CMakeCache.txt.");

    std::optional<std::string> result;
    std::string line;
    while (std::getline(cache, line)) {
        if (line.size() <= key.size() ||
            !line.starts_with(key) ||
            line[key.size()] != ':')
            continue;
        const auto assignment = line.find('=', key.size() + 1u);
        if (assignment == std::string::npos)
            throw std::invalid_argument(
                "Katana-Buildtree besitzt einen ungueltigen CMakeCache-Eintrag.");
        if (result)
            throw std::invalid_argument(
                "Katana-Buildtree besitzt einen mehrdeutigen CMakeCache-Eintrag.");
        result = line.substr(assignment + 1u);
        if (!result->empty() && result->back() == '\r')
            result->pop_back();
    }
    if (!cache.eof())
        throw std::invalid_argument(
            "Katana-Buildtree-CMakeCache konnte nicht vollstaendig gelesen werden.");
    return result;
}

std::string uppercase_ascii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    return value;
}

std::optional<std::string> preferred_optimized_configuration(
    const std::vector<std::string>& configurations) {
    constexpr std::array preferred{
        std::string_view("RELWITHDEBINFO"),
        std::string_view("RELEASE"),
        std::string_view("MINSIZEREL")};
    for (const auto expected : preferred) {
        for (const auto& configuration : configurations) {
            if (uppercase_ascii(configuration) == expected)
                return configuration;
        }
    }
    return std::nullopt;
}

bool runtime_build_tree_is_multi_config(
    const std::filesystem::path& build_root) {
    const auto profile_path =
        build_root / "KatanaRuntimeBuildProfile.txt";
    std::error_code profile_status_error;
    const auto profile_status =
        std::filesystem::symlink_status(
            profile_path, profile_status_error);
    if (profile_status_error ||
        !std::filesystem::is_regular_file(profile_status) ||
        std::filesystem::is_symlink(profile_status))
        throw std::invalid_argument(
            "Katana-Buildtree-Export besitzt kein regulaeres "
            "KatanaRuntimeBuildProfile.txt.");

    std::ifstream profile(profile_path, std::ios::binary);
    if (!profile)
        throw std::invalid_argument(
            "Katana-Buildtree-Profil konnte nicht gelesen werden.");
    std::optional<std::string> schema;
    std::optional<std::string> multi_config;
    std::string line;
    while (std::getline(profile, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto assignment = line.find('=');
        if (assignment == std::string::npos)
            throw std::invalid_argument(
                "Katana-Buildtree-Profil ist ungueltig.");
        const auto key = line.substr(0u, assignment);
        const auto value = line.substr(assignment + 1u);
        auto* destination =
            key == "schema"
                ? &schema
                : key == "multi_config"
                      ? &multi_config
                      : nullptr;
        if (destination == nullptr || *destination)
            throw std::invalid_argument(
                "Katana-Buildtree-Profil ist unbekannt oder mehrdeutig.");
        *destination = value;
    }
    if (!profile.eof() ||
        schema != "katana-runtime-build-profile-v1" ||
        !multi_config ||
        (*multi_config != "0" && *multi_config != "1"))
        throw std::invalid_argument(
            "Katana-Buildtree-Profil ist unvollstaendig oder inkompatibel.");
    return *multi_config == "1";
}

RuntimeBuildTreeProfile require_optimized_runtime_build_tree(
    const std::filesystem::path& targets_file) {
    const auto build_root = targets_file.parent_path();
    const auto multi_config =
        runtime_build_tree_is_multi_config(build_root);
    const auto cache_path = build_root / "CMakeCache.txt";
    std::error_code cache_status_error;
    const auto cache_status =
        std::filesystem::symlink_status(cache_path, cache_status_error);
    if (cache_status_error ||
        !std::filesystem::is_regular_file(cache_status) ||
        std::filesystem::is_symlink(cache_status))
        throw std::invalid_argument(
            "Katana-Buildtree-Export besitzt keinen regulaeren CMakeCache.txt.");
    const auto generator =
        cmake_cache_value(cache_path, "CMAKE_GENERATOR");
    if (!generator || generator->empty())
        throw std::invalid_argument(
            "Katana-Buildtree besitzt keinen eindeutigen CMake-Generator.");
#ifdef _WIN32
    const bool msbuild_generator =
        generator->starts_with("Visual Studio ");
#else
    const bool msbuild_generator = false;
#endif

    std::vector<std::string> configuration_types;
    if (const auto configured_types =
            cmake_cache_value(cache_path, "CMAKE_CONFIGURATION_TYPES")) {
        std::size_t begin = 0u;
        while (begin <= configured_types->size()) {
            const auto end = configured_types->find(';', begin);
            const auto count =
                end == std::string::npos
                    ? configured_types->size() - begin
                    : end - begin;
            auto configuration = configured_types->substr(begin, count);
            const auto first = configuration.find_first_not_of(" \t");
            if (first != std::string::npos) {
                const auto last = configuration.find_last_not_of(" \t");
                configuration =
                    configuration.substr(first, last - first + 1u);
                configuration_types.push_back(std::move(configuration));
            }
            if (end == std::string::npos) break;
            begin = end + 1u;
        }
    }

    if (multi_config) {
        if (configuration_types.empty())
            throw std::invalid_argument(
                "Katana-Multi-Config-Buildtree besitzt keine "
                "CMAKE_CONFIGURATION_TYPES.");
        const auto optimized =
            preferred_optimized_configuration(configuration_types);
        if (!optimized)
            throw std::invalid_argument(
                "Katana-Multi-Config-Buildtree besitzt keine optimierte "
                "Konfiguration; RelWithDebInfo, Release oder MinSizeRel "
                "muss in CMAKE_CONFIGURATION_TYPES enthalten sein.");
        return {*optimized, true, msbuild_generator};
    }

    const auto build_type =
        cmake_cache_value(cache_path, "CMAKE_BUILD_TYPE");
    const std::vector<std::string> configured_build_type =
        build_type && !build_type->empty()
            ? std::vector<std::string>{*build_type}
            : std::vector<std::string>{};
    const auto optimized =
        preferred_optimized_configuration(configured_build_type);
    if (!optimized)
        throw std::invalid_argument(
            "Katana-Single-Config-Buildtree ist nicht optimiert "
            "(CMAKE_BUILD_TYPE=" +
            (build_type && !build_type->empty()
                 ? *build_type
                 : std::string("<leer>")) +
            "); verwende einen separaten Buildtree mit RelWithDebInfo, "
            "Release oder MinSizeRel.");
    return {*optimized, false, msbuild_generator};
}

RuntimeBuildBinding runtime_build_tree_binding(
    const std::filesystem::path& targets_file) {
    const auto profile =
        require_optimized_runtime_build_tree(targets_file);
    return {
        {},
        targets_file,
        {},
        profile.configuration,
        profile.multi_config,
        profile.msbuild_generator};
}

RuntimeBuildBinding
discover_runtime_binding_for_build(const std::filesystem::path& source_root) {
    const auto configured_prefix =
        configured_environment_value("KATANA_RUNTIME_PREFIX");
    const auto configured_build_targets =
        configured_environment_value("KATANA_RUNTIME_BUILD_TARGETS");
    const auto configured_source_root =
        configured_environment_value("KATANA_RUNTIME_ROOT");
    const auto configured_binding_count =
        static_cast<unsigned>(configured_prefix.has_value()) +
        static_cast<unsigned>(configured_build_targets.has_value()) +
        static_cast<unsigned>(configured_source_root.has_value());
    if (configured_binding_count > 1u)
        throw std::invalid_argument(
            "Konfiguriere genau eine Runtime-Umgebungsbindung aus "
            "KATANA_RUNTIME_PREFIX, KATANA_RUNTIME_BUILD_TARGETS oder "
            "KATANA_RUNTIME_ROOT.");

    if (configured_prefix) {
        const auto prefix =
            std::filesystem::absolute(*configured_prefix).lexically_normal();
        if (is_runtime_package_prefix(prefix))
            return {prefix, {}, {}};
        throw std::invalid_argument(
            "KATANA_RUNTIME_PREFIX bezeichnet kein installiertes KatanaRecomp-Runtime-Paket.");
    }
    if (configured_build_targets) {
        const auto targets =
            std::filesystem::absolute(*configured_build_targets)
                .lexically_normal();
        if (is_runtime_build_targets_file(targets))
            return runtime_build_tree_binding(targets);
        throw std::invalid_argument(
            "KATANA_RUNTIME_BUILD_TARGETS bezeichnet keinen "
            "gueltigen Katana-Buildtree-Export.");
    }
    if (configured_source_root) {
        const auto root =
            std::filesystem::absolute(*configured_source_root).lexically_normal();
        if (is_runtime_source_root(root))
            return {{}, {}, root};
        throw std::invalid_argument("KATANA_RUNTIME_ROOT bezeichnet kein kompatibles Runtime-SDK.");
    }

    std::filesystem::path executable;
#ifdef _WIN32
    std::wstring buffer(32'768u, L'\0');
    const auto length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length != 0u && length < buffer.size()) {
        buffer.resize(length);
        executable = std::filesystem::path(buffer);
    }
#else
    std::error_code link_error;
    executable = std::filesystem::read_symlink("/proc/self/exe", link_error);
#endif
    if (!executable.empty()) {
        const auto installed_prefix =
            executable.parent_path().parent_path().lexically_normal();
        if (is_runtime_package_prefix(installed_prefix))
            return {installed_prefix, {}, {}};

        const std::array build_targets_candidates{
            executable.parent_path() /
                "KatanaRuntimeBuildTargets.cmake",
            executable.parent_path().parent_path() /
                "KatanaRuntimeBuildTargets.cmake"};
        for (const auto& build_targets :
             build_targets_candidates) {
            if (is_runtime_build_targets_file(build_targets))
                return runtime_build_tree_binding(build_targets);
        }

        const auto packaged_sdk = executable.parent_path() / "runtime-sdk";
        if (is_runtime_package_prefix(packaged_sdk))
            return {packaged_sdk, {}, {}};
        if (is_runtime_source_root(packaged_sdk))
            return {{}, {}, packaged_sdk};
    }
    if (!source_root.empty())
        return {{}, {}, source_root};

    throw std::runtime_error(
        "Runtime-SDK fuer Portbuild fehlt; KATANA_RUNTIME_PREFIX, "
        "KATANA_RUNTIME_BUILD_TARGETS oder KATANA_RUNTIME_ROOT "
        "kann es explizit angeben.");
}

std::string normalized_host_command(const std::string& command) {
#ifdef _WIN32
    // Windows transports this raw command in a dedicated, deduplicated child
    // environment value. cmd expands that outer value once; '%' embedded in
    // the resulting command is therefore not recursively expanded, while
    // /v:off preserves literal '!'.
    return command;
#else
    return command;
#endif
}

#ifdef _WIN32
[[nodiscard]] bool regular_reparse_free_windows_path(
    const std::filesystem::path& path) noexcept {
    try {
        const auto normalized =
            std::filesystem::absolute(path).lexically_normal();
        const auto root = normalized.root_path();
        if (root.empty()) return false;
        auto current = root;
        const auto relative = normalized.lexically_relative(root);
        for (const auto& component : relative) {
            if (component == "." || component == "..") return false;
            current /= component;
            const auto attributes = GetFileAttributesW(current.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
                return false;
        }
        std::error_code status_error;
        const auto status =
            std::filesystem::symlink_status(normalized, status_error);
        if (status_error || !std::filesystem::is_regular_file(status) ||
            std::filesystem::is_symlink(status))
            return false;
        const auto handle = CreateFileW(
            normalized.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION identity{};
        FILE_ATTRIBUTE_TAG_INFO final_attributes{};
        const auto valid =
            GetFileType(handle) == FILE_TYPE_DISK &&
            GetFileInformationByHandle(handle, &identity) &&
            GetFileInformationByHandleEx(
                handle,
                FileAttributeTagInfo,
                &final_attributes,
                sizeof(final_attributes)) &&
            identity.nNumberOfLinks == 1u &&
            (final_attributes.FileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY |
              FILE_ATTRIBUTE_REPARSE_POINT)) == 0u;
        static_cast<void>(CloseHandle(handle));
        return valid;
    } catch (...) {
        return false;
    }
}

struct WindowsConfiguredHostTools final {
    std::string compiler;
    std::string archiver;
    std::string linker;
};

[[nodiscard]] WindowsConfiguredHostTools
configured_windows_host_tools(
    const std::filesystem::path& cache_path) {
    if (!regular_reparse_free_windows_path(cache_path))
        throw std::runtime_error(
            "Port-Hostbuild-CMakeCache ist keine sichere regulaere "
            "Datei.");
    const auto require_tool = [&](const std::string_view key) {
        const auto cached = cmake_cache_value(cache_path, key);
        if (!cached || cached->empty())
            throw std::runtime_error(
                "Port-Hostbuild besitzt kein gebundenes Tool fuer " +
                std::string(key) + '.');
        if (cached->find_first_of("\"\r\n%^&|<>") !=
            std::string::npos)
            throw std::runtime_error(
                "Port-Hostbuild-Toolpfad ist nicht sicher an die "
                "Wrapper-Umgebung bindbar.");
        const auto tool =
            std::filesystem::path(*cached).lexically_normal();
        if (!tool.is_absolute() ||
            !regular_reparse_free_windows_path(tool))
            throw std::runtime_error(
                "Port-Hostbuild-Tool ist keine sichere regulaere "
                "Datei: " + std::string(key));
        return tool.string();
    };
    return {
        require_tool("CMAKE_CXX_COMPILER"),
        require_tool("CMAKE_AR"),
        require_tool("CMAKE_LINKER")};
}

[[nodiscard]] std::filesystem::path
discover_msvc_developer_environment_script() {
    std::vector<std::filesystem::path> roots;
    for (const auto* variable : {"ProgramFiles(x86)", "ProgramFiles"}) {
        if (const auto configured =
                configured_environment_value(variable))
            roots.emplace_back(*configured);
    }
    constexpr std::array<std::string_view, 2u> versions{
        "2022", "2019"};
    constexpr std::array<std::string_view, 4u> editions{
        "BuildTools", "Community", "Professional", "Enterprise"};
    for (const auto& root : roots) {
        for (const auto version : versions) {
            for (const auto edition : editions) {
                const auto candidate =
                    root / "Microsoft Visual Studio" / version /
                    edition / "Common7" / "Tools" /
                    "VsDevCmd.bat";
                if (regular_reparse_free_windows_path(candidate))
                    return std::filesystem::canonical(candidate);
            }
        }
    }
    throw std::runtime_error(
        "Vollstaendige MSVC-Developer-Umgebung fehlt: "
        "kein sicherer VsDevCmd.bat aus Visual Studio 2019/2022 gefunden.");
}

[[nodiscard]] std::string prepare_windows_host_command(
    std::string command,
    const bool needs_msvc_environment) {
    std::string prefix;
    if (needs_msvc_environment &&
        (!configured_environment_value("INCLUDE") ||
         !configured_environment_value("LIB"))) {
        const auto script =
            discover_msvc_developer_environment_script();
        const auto script_text = script.string();
        if (script_text.find('"') != std::string::npos)
            throw std::runtime_error(
                "VsDevCmd-Pfad ist nicht sicher quotierbar.");
        // This is the top-level /c command, not a batch-file caller. Direct
        // invocation returns to the remaining && command without CALL's
        // additional percent-expansion pass.
        prefix = "\"" + script_text +
                 "\" -arch=x64 -host_arch=x64 >nul && ";
    }
    return prefix + command;
}

[[nodiscard]] bool windows_msvc_mp_token(
    std::string_view token) noexcept {
    if (token.size() >= 2u && token.front() == '"' &&
        token.back() == '"') {
        token.remove_prefix(1u);
        token.remove_suffix(1u);
    }
    if (token.size() < 3u ||
        (token.front() != '/' && token.front() != '-') ||
        std::toupper(static_cast<unsigned char>(token[1])) != 'M' ||
        std::toupper(static_cast<unsigned char>(token[2])) != 'P')
        return false;
    return std::all_of(
        token.begin() + 3,
        token.end(),
        [](const unsigned char character) {
            return std::isdigit(character) != 0;
        });
}

[[nodiscard]] std::string strip_windows_msvc_mp_options(
    const std::string_view value) {
    std::vector<std::string_view> retained;
    std::size_t cursor = 0u;
    while (cursor < value.size()) {
        while (cursor < value.size() &&
               std::isspace(
                   static_cast<unsigned char>(value[cursor])))
            ++cursor;
        if (cursor == value.size()) break;
        const auto begin = cursor;
        bool quoted = false;
        for (; cursor < value.size(); ++cursor) {
            const auto character = value[cursor];
            if (character == '"') quoted = !quoted;
            if (!quoted &&
                std::isspace(
                    static_cast<unsigned char>(character)))
                break;
        }
        if (quoted)
            throw std::invalid_argument(
                "MSVC-Optionsvariable besitzt ein nicht geschlossenes "
                "Anfuehrungszeichen.");
        const auto token = value.substr(begin, cursor - begin);
        if (!windows_msvc_mp_token(token)) retained.push_back(token);
    }
    std::string normalized;
    for (const auto token : retained) {
        if (!normalized.empty()) normalized += ' ';
        normalized.append(token);
    }
    return normalized;
}

[[nodiscard]] std::vector<char>
windows_child_environment(
    const std::optional<std::size_t> cl_jobs,
    const std::string_view raw_host_command) {
    constexpr std::string_view raw_command_name{
        "KATANA_RAW_HOST_COMMAND"};
    constexpr std::string_view mspdbsrv_options_name{
        "_MSPDBSRV_"};
    constexpr std::string_view mspdbsrv_endpoint_name{
        "_MSPDBSRV_ENDPOINT_"};
    constexpr std::size_t maximum_raw_command_bytes = 24u * 1'024u;
    constexpr std::size_t maximum_environment_block_bytes = 32'767u;
    if (cl_jobs && *cl_jobs > 256u)
        throw std::invalid_argument(
            "Exaktes MSVC-Compilerbudget ist ungueltig.");
    if (raw_host_command.empty() ||
        raw_host_command.size() > maximum_raw_command_bytes ||
        raw_host_command.find('\0') != std::string_view::npos)
        throw std::invalid_argument(
            "Windows-Hostkommando ist leer, enthaelt NUL oder "
            "ueberschreitet das sichere Transportlimit.");
    const auto environment = GetEnvironmentStringsA();
    if (environment == nullptr)
        throw std::runtime_error(
            "Windows-Hostumgebung konnte nicht gelesen werden.");
    std::vector<std::string> entries;
    try {
        for (auto cursor = environment; *cursor != '\0';) {
            const auto size = std::strlen(cursor);
            entries.emplace_back(cursor, size);
            cursor += size + 1u;
        }
    } catch (...) {
        FreeEnvironmentStringsA(environment);
        throw;
    }
    FreeEnvironmentStringsA(environment);

    const auto variable_matches = [](
        const std::string_view entry,
        const std::string_view name) noexcept {
        if (entry.size() <= name.size() ||
            entry[name.size()] != '=')
            return false;
        return std::equal(
            name.begin(), name.end(), entry.begin(),
            [](const unsigned char left,
               const unsigned char right) {
                return std::toupper(left) ==
                       std::toupper(right);
            });
    };
    std::string cl_value;
    std::string trailing_cl_value;
    bool cl_seen = false;
    bool trailing_cl_seen = false;
    bool path_seen = false;
    std::string path_value;
    std::vector<std::string> filtered;
    filtered.reserve(entries.size() + 5u);
    for (auto& entry : entries) {
        // Never inherit a spoofed transport value. Windows environment names
        // are case-insensitive, so remove every spelling before binding the
        // exact command owned by this supervision attempt.
        if (variable_matches(entry, raw_command_name) ||
            variable_matches(entry, mspdbsrv_options_name) ||
            variable_matches(entry, mspdbsrv_endpoint_name))
            continue;
        if (variable_matches(entry, "PATH")) {
            if (!path_seen) {
                path_seen = true;
                path_value = std::string_view(entry).substr(5u);
            }
            continue;
        }
        if (cl_jobs && variable_matches(entry, "CL")) {
            if (cl_seen)
                throw std::runtime_error(
                    "Windows-Hostumgebung besitzt mehrere CL-Werte.");
            cl_seen = true;
            cl_value = strip_windows_msvc_mp_options(
                std::string_view(entry).substr(3u));
            continue;
        }
        if (cl_jobs && variable_matches(entry, "_CL_")) {
            if (trailing_cl_seen)
                throw std::runtime_error(
                    "Windows-Hostumgebung besitzt mehrere _CL_-Werte.");
            trailing_cl_seen = true;
            trailing_cl_value = strip_windows_msvc_mp_options(
                std::string_view(entry).substr(5u));
            continue;
        }
        filtered.push_back(std::move(entry));
    }
    if (path_seen) {
        if (const auto effective_path =
                configured_environment_value("PATH"))
            path_value = *effective_path;
        filtered.push_back("PATH=" + path_value);
    }
    if (cl_jobs) {
        filtered.push_back("CL=" + cl_value);
        if (*cl_jobs != 0u) {
            if (!trailing_cl_value.empty()) trailing_cl_value += ' ';
            trailing_cl_value +=
                "/MP" + std::to_string(*cl_jobs);
        }
        filtered.push_back("_CL_=" + trailing_cl_value);
    }
    filtered.push_back(
        std::string(raw_command_name) + '=' +
        std::string(raw_host_command));
    std::random_device endpoint_random;
    std::ostringstream endpoint_seed;
    endpoint_seed
        << GetCurrentProcessId() << ':' << GetCurrentThreadId() << ':'
        << std::chrono::steady_clock::now().time_since_epoch().count()
        << ':' << endpoint_random() << ':' << endpoint_random()
        << ':' << endpoint_random() << ':' << endpoint_random();
    filtered.push_back(
        std::string(mspdbsrv_options_name) + "=-shutdowntime 1");
    filtered.push_back(
        std::string(mspdbsrv_endpoint_name) + "=katana-" +
        katana::io::sha256_bytes(endpoint_seed.str()).substr(0u, 32u));
    std::sort(
        filtered.begin(), filtered.end(),
        [](const std::string& left, const std::string& right) {
            return std::lexicographical_compare(
                left.begin(), left.end(),
                right.begin(), right.end(),
                [](const unsigned char first,
                   const unsigned char second) {
                    return std::toupper(first) <
                           std::toupper(second);
                });
        });
    std::vector<char> block;
    const auto total_size = std::accumulate(
        filtered.begin(), filtered.end(), std::size_t{1u},
        [](const std::size_t total, const std::string& entry) {
            return total + entry.size() + 1u;
        });
    if (total_size > maximum_environment_block_bytes)
        throw std::invalid_argument(
            "Windows-Hostumgebung ueberschreitet mit dem atomaren "
            "Rohkommando das CreateProcess-Limit.");
    block.reserve(total_size);
    for (const auto& entry : filtered) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back('\0');
    }
    block.push_back('\0');
    return block;
}
#endif

[[nodiscard]] std::filesystem::path
current_process_executable_path() {
    std::filesystem::path executable;
#ifdef _WIN32
    std::vector<wchar_t> buffer(32'768u, L'\0');
    const auto length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0u || length >= buffer.size())
        throw std::runtime_error(
            "Pfad des laufenden Katana-Werkzeugs konnte nicht gelesen "
            "werden.");
    executable = std::filesystem::path(
        std::wstring_view(buffer.data(), length));
    if (!regular_reparse_free_windows_path(executable))
        throw std::runtime_error(
            "Laufendes Katana-Werkzeug ist kein sicherer regulaerer "
            "Windows-Pfad.");
#elif defined(__APPLE__)
    std::uint32_t required = 0u;
    static_cast<void>(_NSGetExecutablePath(nullptr, &required));
    if (required == 0u)
        throw std::runtime_error(
            "Pfad des laufenden Katana-Werkzeugs konnte nicht gelesen "
            "werden.");
    std::vector<char> buffer(required, '\0');
    if (_NSGetExecutablePath(buffer.data(), &required) != 0)
        throw std::runtime_error(
            "Pfad des laufenden Katana-Werkzeugs ist instabil.");
    executable = buffer.data();
#else
    std::array<char, 32'768u> buffer{};
    const auto length = ::readlink(
        "/proc/self/exe", buffer.data(), buffer.size() - 1u);
    if (length <= 0 ||
        static_cast<std::size_t>(length) >= buffer.size() - 1u)
        throw std::runtime_error(
            "Pfad des laufenden Katana-Werkzeugs konnte nicht gelesen "
            "werden.");
    executable = std::string_view(
        buffer.data(), static_cast<std::size_t>(length));
#endif
    std::error_code canonical_error;
    const auto canonical =
        std::filesystem::canonical(executable, canonical_error);
    std::error_code status_error;
    const auto status =
        std::filesystem::symlink_status(canonical, status_error);
    if (canonical_error || status_error ||
        !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status))
        throw std::runtime_error(
            "Laufendes Katana-Werkzeug besitzt keine sichere "
            "Dateiidentitaet.");
    return canonical;
}

struct SupervisedHostCommandResult final {
    int exit_code = 1;
    bool exit_code_available = false;
    bool timed_out = false;
    bool interrupted = false;
    std::optional<int> forwarded_signal;
    bool process_tree_quiescent = false;
    bool post_exit_descendants_terminated = false;
};

class SupervisedHostCommandTelemetryAttempt final {
  public:
    SupervisedHostCommandTelemetryAttempt(
        katana::cli::PortBuildTelemetryRecorder* telemetry,
        const std::string_view phase,
        const SupervisedHostCommandResult& result)
        : telemetry_(telemetry), result_(result) {
        if (telemetry_ != nullptr)
            observation_.stage = std::string(
                phase.empty()
                    ? std::string_view("host-command")
                    : phase);
    }

    ~SupervisedHostCommandTelemetryAttempt() noexcept {
        if (telemetry_ == nullptr) return;
        if (result_.exit_code_available)
            observation_.host_exit_code = result_.exit_code;
        observation_.timed_out = result_.timed_out;
        observation_.interrupted = result_.interrupted;
        observation_.forwarded_signal = result_.forwarded_signal;
        observation_.process_tree_quiescent =
            result_.process_tree_quiescent;
#ifdef _WIN32
        observation_.process_tree_scope =
            result_.post_exit_descendants_terminated
                ? "job-object-tree-post-exit-drain"
                : "job-object-tree";
        observation_.process_tree_query_complete =
            result_.process_tree_quiescent;
#elif defined(__linux__)
        observation_.process_tree_scope =
            "subreaper-descendant-tree";
        observation_.process_tree_query_complete =
            result_.process_tree_quiescent;
#else
        observation_.process_tree_scope = "process-group-only";
        observation_.process_tree_query_complete = false;
#endif
        static_cast<void>(telemetry_->record_host_command(
            std::move(observation_)));
    }

    SupervisedHostCommandTelemetryAttempt(
        const SupervisedHostCommandTelemetryAttempt&) = delete;
    SupervisedHostCommandTelemetryAttempt& operator=(
        const SupervisedHostCommandTelemetryAttempt&) = delete;

  private:
    katana::cli::PortBuildTelemetryRecorder* telemetry_ = nullptr;
    const SupervisedHostCommandResult& result_;
    katana::cli::PortBuildHostCommandObservation observation_;
};

inline constexpr auto maximum_port_host_command_runtime =
    std::chrono::minutes(20);

inline constexpr auto windows_port_host_post_exit_grace =
    std::chrono::milliseconds(500);

using PortHostCommandTimeout =
    std::optional<std::chrono::milliseconds>;

PortHostCommandTimeout configured_port_host_command_runtime(
    const std::string_view stage) {
    const auto configured_timeout = []() -> PortHostCommandTimeout {
        const auto value =
            configured_environment_value(
                "KATANA_PORT_HOST_COMMAND_TIMEOUT_MS");
        if (!value)
            return std::chrono::duration_cast<
                std::chrono::milliseconds>(
                maximum_port_host_command_runtime);
        if (*value == "unlimited") return std::nullopt;
        std::uint64_t milliseconds = 0u;
        const auto conversion = std::from_chars(
            value->data(),
            value->data() + value->size(),
            milliseconds,
            10);
        if (conversion.ec != std::errc{} ||
            conversion.ptr != value->data() + value->size())
            throw std::invalid_argument(
                "KATANA_PORT_HOST_COMMAND_TIMEOUT_MS muss 'unlimited' "
                "oder eine Millisekundenzahl sein.");
        const auto maximum_wait_milliseconds =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    maximum_port_host_command_runtime)
                    .count());
        if (milliseconds == 0u ||
            milliseconds > maximum_wait_milliseconds)
            throw std::invalid_argument(
                "KATANA_PORT_HOST_COMMAND_TIMEOUT_MS muss 'unlimited' "
                "oder zwischen 1 und 1200000 liegen.");
        return std::chrono::milliseconds(milliseconds);
    }();
    const auto test_stage =
        configured_environment_value(
            "KATANA_PORT_HOST_COMMAND_TEST_TIMEOUT_STAGE");
    const auto test_timeout =
        configured_environment_value(
            "KATANA_PORT_HOST_COMMAND_TEST_TIMEOUT_MS");
    if (!test_stage && !test_timeout) return configured_timeout;
    if (!test_stage || !test_timeout ||
        (*test_stage != "configure" &&
         *test_stage != "runtime-sdk-build" &&
         *test_stage != "host-build"))
        throw std::invalid_argument(
            "KATANA_PORT_HOST_COMMAND_TEST_TIMEOUT_STAGE/_MS "
            "bilden keinen gueltigen Testvertrag.");
    std::size_t parsed = 0u;
    const auto milliseconds =
        std::stoull(*test_timeout, &parsed, 10);
    if (parsed != test_timeout->size() ||
        milliseconds == 0u || milliseconds > 10'000u)
        throw std::invalid_argument(
            "KATANA_PORT_HOST_COMMAND_TEST_TIMEOUT_MS ist ungueltig.");
    if (*test_stage != stage) return configured_timeout;
    return std::chrono::milliseconds(milliseconds);
}

#ifdef _WIN32
enum class WindowsJobQuiescenceResult {
    Quiescent,
    TerminationFailed,
    ProcessWaitFailed,
    JobQueryFailed,
    DeadlineExpired,
};

struct WindowsJobEmptyProbe final {
    bool query_succeeded = false;
    bool empty = false;
};

WindowsJobEmptyProbe query_windows_job_empty(const HANDLE job) noexcept {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(
            job,
            JobObjectBasicAccountingInformation,
            &accounting,
            sizeof(accounting),
            nullptr))
        return {};
    if (accounting.ActiveProcesses != 0u)
        return {true, false};

    JOBOBJECT_BASIC_PROCESS_ID_LIST process_ids{};
    if (!QueryInformationJobObject(
            job,
            JobObjectBasicProcessIdList,
            &process_ids,
            sizeof(process_ids),
            nullptr)) {
        // A fixed one-entry buffer is intentionally sufficient for the empty
        // proof. ERROR_MORE_DATA proves that at least one process remains.
        if (GetLastError() == ERROR_MORE_DATA)
            return {true, false};
        return {};
    }
    return {
        true,
        process_ids.NumberOfAssignedProcesses == 0u &&
            process_ids.NumberOfProcessIdsInList == 0u};
}

WindowsJobQuiescenceResult terminate_windows_job_and_wait(
    const HANDLE job,
    const HANDLE root_process,
    const DWORD exit_code) noexcept {
    const auto initial_probe = query_windows_job_empty(job);
    if (!initial_probe.query_succeeded)
        return WindowsJobQuiescenceResult::JobQueryFailed;
    const auto initial_root_wait =
        WaitForSingleObject(root_process, 0u);
    if (initial_root_wait == WAIT_FAILED)
        return WindowsJobQuiescenceResult::ProcessWaitFailed;
    if (initial_probe.empty &&
        initial_root_wait == WAIT_OBJECT_0)
        return WindowsJobQuiescenceResult::Quiescent;

    const auto termination_requested =
        TerminateJobObject(job, exit_code) != FALSE;
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    for (;;) {
        const auto root_wait =
            WaitForSingleObject(root_process, 0u);
        if (root_wait == WAIT_FAILED)
            return WindowsJobQuiescenceResult::ProcessWaitFailed;
        const auto probe = query_windows_job_empty(job);
        if (!probe.query_succeeded)
            return WindowsJobQuiescenceResult::JobQueryFailed;
        if (root_wait == WAIT_OBJECT_0 && probe.empty)
            return WindowsJobQuiescenceResult::Quiescent;
        if (!termination_requested)
            return WindowsJobQuiescenceResult::TerminationFailed;
        if (std::chrono::steady_clock::now() >= deadline)
            return WindowsJobQuiescenceResult::DeadlineExpired;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(10));
    }
}

bool terminate_windows_process_and_wait(
    const HANDLE process,
    const DWORD exit_code) noexcept {
    const auto initial_wait = WaitForSingleObject(process, 0u);
    if (initial_wait == WAIT_OBJECT_0) return true;
    if (initial_wait == WAIT_FAILED) return false;
    if (!TerminateProcess(process, exit_code)) {
        const auto raced_wait = WaitForSingleObject(process, 0u);
        if (raced_wait == WAIT_OBJECT_0) return true;
        return false;
    }
    return WaitForSingleObject(process, 5'000u) ==
           WAIT_OBJECT_0;
}
#else

std::mutex posix_host_signal_mutex;
volatile sig_atomic_t posix_host_pending_signal = 0;

void capture_posix_host_signal(const int signal) noexcept {
    if (posix_host_pending_signal == 0)
        posix_host_pending_signal = signal;
}

class ScopedPosixHostSignals final {
  public:
    ScopedPosixHostSignals()
        : lock_(posix_host_signal_mutex) {
        posix_host_pending_signal = 0;
        struct sigaction action {};
        action.sa_handler = capture_posix_host_signal;
        ::sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        if (::sigaction(SIGINT, &action, &previous_interrupt_) != 0)
            throw std::runtime_error(
                "SIGINT-Aufsicht fuer Port-Hostprozess konnte nicht "
                "installiert werden.");
        interrupt_installed_ = true;
        if (::sigaction(SIGTERM, &action, &previous_terminate_) != 0) {
            static_cast<void>(
                ::sigaction(SIGINT, &previous_interrupt_, nullptr));
            interrupt_installed_ = false;
            throw std::runtime_error(
                "SIGTERM-Aufsicht fuer Port-Hostprozess konnte nicht "
                "installiert werden.");
        }
        terminate_installed_ = true;
        action.sa_handler = SIG_IGN;
        if (::sigaction(SIGPIPE, &action, &previous_pipe_) != 0) {
            static_cast<void>(
                ::sigaction(SIGTERM, &previous_terminate_, nullptr));
            static_cast<void>(
                ::sigaction(SIGINT, &previous_interrupt_, nullptr));
            terminate_installed_ = false;
            interrupt_installed_ = false;
            throw std::runtime_error(
                "SIGPIPE-Aufsicht fuer Port-Hostprozess konnte nicht "
                "installiert werden.");
        }
        pipe_installed_ = true;
    }

    ~ScopedPosixHostSignals() noexcept {
        if (pipe_installed_)
            static_cast<void>(
                ::sigaction(SIGPIPE, &previous_pipe_, nullptr));
        if (terminate_installed_)
            static_cast<void>(
                ::sigaction(SIGTERM, &previous_terminate_, nullptr));
        if (interrupt_installed_)
            static_cast<void>(
                ::sigaction(SIGINT, &previous_interrupt_, nullptr));
        posix_host_pending_signal = 0;
    }

    ScopedPosixHostSignals(const ScopedPosixHostSignals&) = delete;
    ScopedPosixHostSignals& operator=(
        const ScopedPosixHostSignals&) = delete;

    [[nodiscard]] int pending_signal() const noexcept {
        return static_cast<int>(posix_host_pending_signal);
    }

    static void restore_defaults_in_child() noexcept {
        struct sigaction action {};
        action.sa_handler = SIG_DFL;
        ::sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        static_cast<void>(::sigaction(SIGINT, &action, nullptr));
        static_cast<void>(::sigaction(SIGTERM, &action, nullptr));
        static_cast<void>(::sigaction(SIGPIPE, &action, nullptr));
    }

  private:
    std::unique_lock<std::mutex> lock_;
    struct sigaction previous_interrupt_ {};
    struct sigaction previous_terminate_ {};
    struct sigaction previous_pipe_ {};
    bool interrupt_installed_ = false;
    bool terminate_installed_ = false;
    bool pipe_installed_ = false;
};

#if defined(__linux__)
class ScopedLinuxChildSubreaper final {
  public:
    ScopedLinuxChildSubreaper() {
        if (::prctl(
                PR_GET_CHILD_SUBREAPER,
                &previous_,
                0,
                0,
                0) != 0)
            throw std::runtime_error(
                "Linux-Subreaper-Zustand fuer den Hostprozessbaum "
                "konnte nicht gelesen werden.");
        if (previous_ == 0 &&
            ::prctl(
                PR_SET_CHILD_SUBREAPER,
                1,
                0,
                0,
                0) != 0)
            throw std::runtime_error(
                "Linux-Subreaper fuer den Hostprozessbaum konnte "
                "nicht aktiviert werden.");
        restore_ = previous_ == 0;
    }

    ~ScopedLinuxChildSubreaper() noexcept {
        if (restore_)
            static_cast<void>(::prctl(
                PR_SET_CHILD_SUBREAPER,
                previous_,
                0,
                0,
                0));
    }

    ScopedLinuxChildSubreaper(
        const ScopedLinuxChildSubreaper&) = delete;
    ScopedLinuxChildSubreaper& operator=(
        const ScopedLinuxChildSubreaper&) = delete;

  private:
    int previous_ = 0;
    bool restore_ = false;
};

struct LinuxChildrenSnapshot final {
    std::vector<pid_t> children;
    bool process_present = true;
    bool complete = true;
};

[[nodiscard]] bool linux_process_exists(
    const pid_t process) noexcept {
    for (;;) {
        if (::kill(process, 0) == 0 || errno == EPERM) return true;
        if (errno == EINTR) continue;
        return false;
    }
}

[[nodiscard]] LinuxChildrenSnapshot
capture_linux_children(const pid_t process) noexcept {
    LinuxChildrenSnapshot result;
    try {
        const auto task_root =
            std::filesystem::path("/proc") /
            std::to_string(process) / "task";
        std::error_code iteration_error;
        const std::filesystem::directory_iterator end;
        for (std::filesystem::directory_iterator task(
                 task_root, iteration_error);
             !iteration_error && task != end;
             task.increment(iteration_error)) {
            const auto task_name =
                task->path().filename().string();
            std::int64_t task_id = 0;
            const auto parsed = std::from_chars(
                task_name.data(),
                task_name.data() + task_name.size(),
                task_id);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != task_name.data() + task_name.size() ||
                task_id <= 0)
                continue;
            const auto children_path = task->path() / "children";
            std::ifstream children_file(
                children_path, std::ios::binary);
            if (!children_file) {
                std::error_code task_status_error;
                const auto task_status =
                    std::filesystem::symlink_status(
                        task->path(), task_status_error);
                if (!task_status_error &&
                    task_status.type() !=
                        std::filesystem::file_type::not_found) {
                    result.complete = false;
                    return result;
                }
                continue;
            }
            std::string child_text;
            while (children_file >> child_text) {
                std::int64_t child = 0;
                const auto child_parse = std::from_chars(
                    child_text.data(),
                    child_text.data() + child_text.size(),
                    child);
                if (child_parse.ec != std::errc{} ||
                    child_parse.ptr !=
                        child_text.data() + child_text.size() ||
                    child <= 0 ||
                    child > static_cast<std::int64_t>(
                                std::numeric_limits<pid_t>::max())) {
                    result.complete = false;
                    return result;
                }
                result.children.push_back(
                    static_cast<pid_t>(child));
            }
            if (!children_file.eof()) {
                result.complete = false;
                return result;
            }
        }
        if (iteration_error) {
            result.process_present =
                linux_process_exists(process);
            result.complete = !result.process_present;
            return result;
        }
        std::sort(
            result.children.begin(), result.children.end());
        result.children.erase(
            std::unique(
                result.children.begin(), result.children.end()),
            result.children.end());
        return result;
    } catch (...) {
        result.process_present = linux_process_exists(process);
        result.complete = !result.process_present;
        return result;
    }
}

[[nodiscard]] std::vector<pid_t>
capture_linux_baseline_children() {
    const auto snapshot = capture_linux_children(::getpid());
    if (!snapshot.complete || !snapshot.process_present)
        throw std::runtime_error(
            "Linux-Kindprozessbasis konnte nicht vollstaendig "
            "erfasst werden.");
    return snapshot.children;
}

enum class LinuxDescendantState {
    Empty,
    Present,
    Failed,
};

[[nodiscard]] LinuxDescendantState
probe_linux_descendants(
    const pid_t root,
    const std::vector<pid_t>& baseline_children,
    std::vector<pid_t>* const descendant_processes = nullptr) noexcept {
    try {
        constexpr std::size_t maximum_descendants = 65'536u;
        std::vector<pid_t> queue{root};
        std::unordered_set<pid_t> discovered{root};
        bool descendant_seen = false;
        const auto enqueue_direct_children = [&]() {
            const auto direct =
                capture_linux_children(::getpid());
            if (!direct.complete || !direct.process_present)
                return false;
            if (std::find(
                    direct.children.begin(),
                    direct.children.end(),
                    root) == direct.children.end())
                return false;
            for (const auto child : direct.children) {
                if (child == root ||
                    std::binary_search(
                        baseline_children.begin(),
                        baseline_children.end(),
                        child))
                    continue;
                descendant_seen = true;
                if (discovered.insert(child).second) {
                    queue.push_back(child);
                    if (descendant_processes != nullptr)
                        descendant_processes->push_back(child);
                }
            }
            return true;
        };
        if (!enqueue_direct_children())
            return LinuxDescendantState::Failed;
        for (std::size_t index = 0u; index < queue.size(); ++index) {
            if (queue.size() > maximum_descendants)
                return LinuxDescendantState::Failed;
            const auto children =
                capture_linux_children(queue[index]);
            if (!children.complete)
                return LinuxDescendantState::Failed;
            if (!children.process_present) continue;
            for (const auto child : children.children) {
                descendant_seen = true;
                if (discovered.insert(child).second) {
                    queue.push_back(child);
                    if (descendant_processes != nullptr)
                        descendant_processes->push_back(child);
                }
            }
        }
        // A process can exit and be adopted by this subreaper while its old
        // parent is traversed. A second direct-child snapshot closes that
        // race; any newly observed child forces another supervisor round.
        if (!enqueue_direct_children())
            return LinuxDescendantState::Failed;
        return descendant_seen
                   ? LinuxDescendantState::Present
                   : LinuxDescendantState::Empty;
    } catch (...) {
        return LinuxDescendantState::Failed;
    }
}
#endif

enum class PosixRootState {
    Running,
    Terminal,
    Failed,
};

enum class PosixGroupState {
    RootOnly,
    OtherMembers,
    Failed,
};

enum class PosixTreeQuiescenceState {
    Pending,
    Quiescent,
    Failed,
};

#if defined(__linux__)
[[nodiscard]] bool parse_linux_process_group(
    const std::string_view stat,
    std::int64_t& process_group) noexcept {
    try {
        const auto closing_name = stat.rfind(')');
        if (closing_name == std::string_view::npos ||
            closing_name + 3u >= stat.size())
            return false;
        std::istringstream fields(
            std::string(stat.substr(closing_name + 2u)));
        char state = 0;
        std::int64_t parent = 0;
        return static_cast<bool>(
            fields >> state >> parent >> process_group);
    } catch (...) {
        return false;
    }
}
#endif

[[nodiscard]] PosixGroupState probe_posix_process_group(
    const pid_t process_group,
    const pid_t root) noexcept {
    try {
#if defined(__linux__)
        bool root_seen = false;
        std::error_code iteration_error;
        const std::filesystem::directory_iterator end;
        for (std::filesystem::directory_iterator entry(
                 "/proc", iteration_error);
             !iteration_error && entry != end;
             entry.increment(iteration_error)) {
            const auto name = entry->path().filename().string();
            if (name.empty()) continue;
            std::int64_t parsed_pid = 0;
            const auto parsed = std::from_chars(
                name.data(), name.data() + name.size(), parsed_pid);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != name.data() + name.size() ||
                parsed_pid <= 0 ||
                parsed_pid >
                    static_cast<std::int64_t>(
                        std::numeric_limits<pid_t>::max()))
                continue;
            const auto candidate_pid =
                static_cast<pid_t>(parsed_pid);
            const auto stat_path = entry->path() / "stat";
            std::ifstream stat_file(stat_path, std::ios::binary);
            std::string stat;
            std::int64_t observed_group = 0;
            const auto stat_resolved =
                stat_file &&
                static_cast<bool>(std::getline(stat_file, stat)) &&
                parse_linux_process_group(stat, observed_group);
            if (!stat_resolved) {
                pid_t fallback_group = -1;
                do {
                    fallback_group = ::getpgid(candidate_pid);
                } while (fallback_group < 0 && errno == EINTR);
                if (fallback_group < 0) {
                    // A process disappearing between readdir/open/getpgid is
                    // ordinary /proc churn and cannot have remained a member
                    // of the group being proved empty.
                    if (errno == ESRCH) continue;
                    return PosixGroupState::Failed;
                }
                observed_group =
                    static_cast<std::int64_t>(fallback_group);
            }
            if (observed_group !=
                static_cast<std::int64_t>(process_group))
                continue;
            if (parsed_pid == static_cast<std::int64_t>(root)) {
                root_seen = true;
                continue;
            }
            return PosixGroupState::OtherMembers;
        }
        if (iteration_error || !root_seen)
            return PosixGroupState::Failed;
        return PosixGroupState::RootOnly;
#elif defined(__APPLE__)
        const int query[] = {
            CTL_KERN, KERN_PROC, KERN_PROC_PGRP, process_group};
        std::size_t byte_count = 0u;
        if (::sysctl(
                const_cast<int*>(query),
                4u,
                nullptr,
                &byte_count,
                nullptr,
                0u) != 0)
            return PosixGroupState::Failed;
        std::vector<kinfo_proc> processes(
            byte_count / sizeof(kinfo_proc) + 1u);
        byte_count = processes.size() * sizeof(kinfo_proc);
        if (::sysctl(
                const_cast<int*>(query),
                4u,
                processes.data(),
                &byte_count,
                nullptr,
                0u) != 0 ||
            byte_count % sizeof(kinfo_proc) != 0u)
            return PosixGroupState::Failed;
        bool root_seen = false;
        const auto count = byte_count / sizeof(kinfo_proc);
        for (std::size_t index = 0u; index < count; ++index) {
            const auto pid = processes[index].kp_proc.p_pid;
            if (pid == root) {
                root_seen = true;
                continue;
            }
            return PosixGroupState::OtherMembers;
        }
        return root_seen
                   ? PosixGroupState::RootOnly
                   : PosixGroupState::Failed;
#else
        static_cast<void>(process_group);
        static_cast<void>(root);
        return PosixGroupState::Failed;
#endif
    } catch (...) {
        return PosixGroupState::Failed;
    }
}

// Owns the forked child only while the two-way launch handshake still
// prevents exec and descendant creation. In this narrow state direct-PID
// kill/wait is a complete process-tree cleanup proof; after the commit byte,
// ownership must first move to PosixChildSupervisor.
class PosixUncommittedChildGuard final {
  public:
    explicit PosixUncommittedChildGuard(const pid_t child) noexcept
        : child_(child) {}

    ~PosixUncommittedChildGuard() noexcept {
        if (owned_)
            static_cast<void>(terminate_and_reap());
    }

    PosixUncommittedChildGuard(
        const PosixUncommittedChildGuard&) = delete;
    PosixUncommittedChildGuard& operator=(
        const PosixUncommittedChildGuard&) = delete;

    [[nodiscard]] bool terminate_and_reap() noexcept {
        if (!owned_) return true;
        for (;;) {
            if (::kill(child_, SIGKILL) == 0 || errno == ESRCH)
                break;
            if (errno == EINTR) continue;
            break;
        }
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        for (;;) {
            int status = 0;
            const auto waited =
                ::waitpid(child_, &status, WNOHANG);
            if (waited == child_ ||
                (waited < 0 && errno == ECHILD)) {
                owned_ = false;
                return true;
            }
            if (waited < 0 && errno != EINTR)
                return false;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(10));
        }
    }

    void release() noexcept { owned_ = false; }

  private:
    pid_t child_ = -1;
    bool owned_ = true;
};

class PosixChildSupervisor final {
  public:
    PosixChildSupervisor(
        const pid_t child,
        katana::cli::PortBuildTelemetryRecorder* telemetry,
        const std::string_view telemetry_phase,
        std::vector<pid_t> baseline_children = {}) noexcept
        : child_(child),
          telemetry_(telemetry),
          telemetry_phase_(telemetry_phase)
#if defined(__linux__)
          , baseline_children_(std::move(baseline_children))
#endif
          {
#if !defined(__linux__)
        static_cast<void>(baseline_children);
#endif
        if (telemetry_ != nullptr)
            telemetry_->register_posix_process_group(
                static_cast<std::int64_t>(child_));
    }

    ~PosixChildSupervisor() noexcept {
        if (!reaped_)
            static_cast<void>(terminate_and_reap(SIGKILL));
        finish_telemetry();
    }

    PosixChildSupervisor(const PosixChildSupervisor&) = delete;
    PosixChildSupervisor& operator=(
        const PosixChildSupervisor&) = delete;

    [[nodiscard]] PosixRootState observe_root() noexcept {
        if (root_terminal_) return PosixRootState::Terminal;
        for (;;) {
            siginfo_t information {};
            if (::waitid(
                    P_PID,
                    static_cast<id_t>(child_),
                    &information,
                    WEXITED | WNOHANG | WNOWAIT) == 0) {
                if (information.si_pid == 0)
                    return PosixRootState::Running;
                if (information.si_pid != child_)
                    return PosixRootState::Failed;
                root_terminal_ = true;
                return PosixRootState::Terminal;
            }
            if (errno == EINTR) continue;
            return PosixRootState::Failed;
        }
    }

    [[nodiscard]] bool refresh_descendant_telemetry() noexcept {
#if defined(__linux__)
        if (telemetry_ == nullptr || !telemetry_->enabled())
            return true;
        std::vector<pid_t> descendants;
        const auto state = probe_linux_descendants(
            child_, baseline_children_, &descendants);
        if (state == LinuxDescendantState::Failed) return false;
        publish_linux_descendants(descendants);
#endif
        return true;
    }

    [[nodiscard]] bool terminate_and_reap(
        const int initial_signal) noexcept {
        if (reaped_) return process_tree_quiescent_;
        const auto initial_group_signal_sent =
            signal_group(initial_signal);
#if defined(__linux__)
        const auto initial_descendant_signal_sent =
            signal_linux_descendants(initial_signal);
#else
        constexpr bool initial_descendant_signal_sent = false;
#endif
        if (initial_signal == SIGKILL &&
            (initial_group_signal_sent ||
             initial_descendant_signal_sent))
            hard_group_kill_sent_ = true;
        const auto grace_deadline =
            std::chrono::steady_clock::now() +
            (initial_signal == SIGKILL
                 ? std::chrono::milliseconds(0)
                 : std::chrono::seconds(2));
        while (std::chrono::steady_clock::now() <= grace_deadline) {
            if (try_quiescent_reap() ==
                PosixTreeQuiescenceState::Quiescent)
                return true;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(10));
        }

        // The unreaped direct child remains the PGID identity anchor while
        // SIGKILL is issued and while OS process-table evidence is sampled.
        // Consequently this can never target a recycled unrelated group.
        if (signal_group(SIGKILL))
            hard_group_kill_sent_ = true;
#if defined(__linux__)
        if (signal_linux_descendants(SIGKILL))
            hard_group_kill_sent_ = true;
#endif
        static_cast<void>(signal_root(SIGKILL));
        const auto kill_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() <= kill_deadline) {
            if (try_quiescent_reap() ==
                PosixTreeQuiescenceState::Quiescent)
                return true;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(10));
        }
        return false;
    }

    [[nodiscard]] bool process_tree_quiescent() const noexcept {
        return process_tree_quiescent_ && reaped_;
    }

    // A successful command is complete only after its direct child is
    // terminal and every remaining member of the anchored process group has
    // exited naturally. This probe never signals the group.
    [[nodiscard]] PosixTreeQuiescenceState
    try_natural_quiescent_reap() noexcept {
        return try_quiescent_reap();
    }

    [[nodiscard]] int exit_code() const noexcept {
        if (!reaped_) return 1;
        if (WIFEXITED(status_)) return WEXITSTATUS(status_);
        if (WIFSIGNALED(status_))
            return 128 + WTERMSIG(status_);
        return 1;
    }

    void finish_telemetry() noexcept {
        if (telemetry_finished_ || telemetry_ == nullptr) return;
        telemetry_finished_ = true;
        if (usage_available_)
            record_final_usage();
        telemetry_->clear_process_tree();
        telemetry_->sample_resources(telemetry_phase_);
    }

  private:
#if defined(__linux__)
    [[nodiscard]] bool signal_linux_descendants(
        const int signal) noexcept {
        std::vector<pid_t> descendants;
        const auto state = probe_linux_descendants(
            child_, baseline_children_, &descendants);
        if (state == LinuxDescendantState::Failed) return false;
        publish_linux_descendants(descendants);
        for (const auto descendant : descendants) {
            for (;;) {
                if (::kill(descendant, signal) == 0 ||
                    errno == ESRCH)
                    break;
                if (errno == EINTR) continue;
                return false;
            }
        }
        return true;
    }

    void publish_linux_descendants(
        const std::span<const pid_t> descendants) noexcept {
        if (telemetry_ == nullptr) return;
        try {
            std::vector<std::int64_t> telemetry_processes;
            telemetry_processes.reserve(descendants.size());
            for (const auto process : descendants)
                telemetry_processes.push_back(
                    static_cast<std::int64_t>(process));
            telemetry_->update_posix_process_tree_members(
                telemetry_processes);
        } catch (...) {
            static_cast<void>(
                telemetry_->mark_upstream_incomplete(
                    "posix-descendant-telemetry-allocation-failed"));
        }
    }

    [[nodiscard]] bool reap_terminal_linux_adoptees(
        bool& reaped_any) noexcept {
        reaped_any = false;
        const auto direct = capture_linux_children(::getpid());
        if (!direct.complete || !direct.process_present)
            return false;
        for (const auto process : direct.children) {
            if (process == child_ ||
                std::binary_search(
                    baseline_children_.begin(),
                    baseline_children_.end(),
                    process))
                continue;
            for (;;) {
                int status = 0;
                struct rusage usage {};
                const auto waited =
                    ::wait4(process, &status, WNOHANG, &usage);
                if (waited == process) {
                    accumulate_final_usage(usage);
                    reaped_any = true;
                    break;
                }
                if (waited == 0 ||
                    (waited < 0 && errno == ECHILD))
                    break;
                if (waited < 0 && errno == EINTR) continue;
                return false;
            }
        }
        return true;
    }
#endif

    [[nodiscard]] bool signal_group(const int signal) noexcept {
        for (;;) {
            if (::kill(-child_, signal) == 0) return true;
            if (errno == EINTR) continue;
            return false;
        }
    }

    [[nodiscard]] bool signal_root(const int signal) noexcept {
        for (;;) {
            if (::kill(child_, signal) == 0) return true;
            if (errno == EINTR) continue;
            return errno == ESRCH && root_terminal_;
        }
    }

    [[nodiscard]] PosixTreeQuiescenceState
    try_quiescent_reap() noexcept {
        const auto root_state = observe_root();
        if (root_state == PosixRootState::Failed)
            return PosixTreeQuiescenceState::Failed;
        if (root_state != PosixRootState::Terminal &&
            !hard_group_kill_sent_)
            return PosixTreeQuiescenceState::Pending;
        const auto group_state =
            probe_posix_process_group(child_, child_);
        if (group_state == PosixGroupState::Failed)
            return PosixTreeQuiescenceState::Failed;
        if (group_state != PosixGroupState::RootOnly)
            return PosixTreeQuiescenceState::Pending;
#if defined(__linux__)
        bool adopted_reaped = false;
        if (!reap_terminal_linux_adoptees(adopted_reaped))
            return PosixTreeQuiescenceState::Failed;
        std::vector<pid_t> descendant_processes;
        const auto descendants = probe_linux_descendants(
            child_, baseline_children_, &descendant_processes);
        if (descendants == LinuxDescendantState::Failed)
            return PosixTreeQuiescenceState::Failed;
        publish_linux_descendants(descendant_processes);
        if (adopted_reaped ||
            descendants == LinuxDescendantState::Present) {
            empty_descendant_observations_ = 0u;
            return PosixTreeQuiescenceState::Pending;
        }
        // Two consecutive complete empty snapshots prevent an adoption race
        // between the process-group proof and the final root reap.
        if (++empty_descendant_observations_ < 2u)
            return PosixTreeQuiescenceState::Pending;
#endif
        // A waitid failure cannot silently leak a process tree. Once SIGKILL
        // was successfully delivered to the still anchored group and the OS
        // proves only the direct child remains, wait4(WNOHANG) may safely
        // observe/reap it. The ordinary terminal path stays blocking and both
        // paths release the PID/PGID anchor only after the group proof.
        const auto wait_options =
            root_state == PosixRootState::Terminal ? 0 : WNOHANG;
        for (;;) {
            struct rusage usage {};
            const auto waited =
                ::wait4(child_, &status_, wait_options, &usage);
            if (waited == child_) {
                accumulate_final_usage(usage);
                reaped_ = true;
                process_tree_quiescent_ = true;
                return PosixTreeQuiescenceState::Quiescent;
            }
            if (waited < 0 && errno == EINTR) continue;
            if (waited == 0)
                return PosixTreeQuiescenceState::Pending;
            return PosixTreeQuiescenceState::Failed;
        }
    }

    static void accumulate_usage_time(
        timeval& destination,
        const timeval source) noexcept {
        if (source.tv_sec < 0 || source.tv_usec < 0) return;
        using Seconds = decltype(destination.tv_sec);
        constexpr auto microseconds_per_second = 1'000'000;
        const auto maximum_seconds =
            std::numeric_limits<Seconds>::max();
        if (destination.tv_sec < 0 || destination.tv_usec < 0) {
            destination = {};
        }
        const auto carry =
            (destination.tv_usec + source.tv_usec) /
            microseconds_per_second;
        const auto remaining =
            (destination.tv_usec + source.tv_usec) %
            microseconds_per_second;
        if (source.tv_sec > maximum_seconds - destination.tv_sec ||
            carry > maximum_seconds - destination.tv_sec -
                        source.tv_sec) {
            destination.tv_sec = maximum_seconds;
            destination.tv_usec =
                microseconds_per_second - 1;
            return;
        }
        destination.tv_sec += source.tv_sec + carry;
        destination.tv_usec = remaining;
    }

    [[nodiscard]] static long accumulate_usage_counter(
        const long destination,
        const long source) noexcept {
        const auto normalized_destination =
            std::max<long>(0, destination);
        const auto normalized_source =
            std::max<long>(0, source);
        if (normalized_source >
            std::numeric_limits<long>::max() -
                normalized_destination)
            return std::numeric_limits<long>::max();
        return normalized_destination + normalized_source;
    }

    void accumulate_final_usage(
        const struct rusage& usage) noexcept {
        if (!usage_available_) {
            final_usage_ = usage;
            usage_available_ = true;
            return;
        }
        accumulate_usage_time(
            final_usage_.ru_utime, usage.ru_utime);
        accumulate_usage_time(
            final_usage_.ru_stime, usage.ru_stime);
        final_usage_.ru_minflt = accumulate_usage_counter(
            final_usage_.ru_minflt, usage.ru_minflt);
        final_usage_.ru_majflt = accumulate_usage_counter(
            final_usage_.ru_majflt, usage.ru_majflt);
        final_usage_.ru_inblock = accumulate_usage_counter(
            final_usage_.ru_inblock, usage.ru_inblock);
        final_usage_.ru_oublock = accumulate_usage_counter(
            final_usage_.ru_oublock, usage.ru_oublock);
        final_usage_.ru_maxrss = std::max<long>(
            final_usage_.ru_maxrss, usage.ru_maxrss);
    }

    void record_final_usage() noexcept {
        const auto milliseconds = [](const timeval value) noexcept {
            if (value.tv_sec < 0 || value.tv_usec < 0)
                return std::uint64_t{0u};
            const auto seconds =
                static_cast<std::uint64_t>(value.tv_sec);
            const auto microseconds =
                static_cast<std::uint64_t>(value.tv_usec);
            if (seconds >
                (std::numeric_limits<std::uint64_t>::max() -
                 microseconds / 1'000u) /
                    1'000u)
                return std::numeric_limits<std::uint64_t>::max();
            return seconds * 1'000u + microseconds / 1'000u;
        };
        katana::cli::PortBuildPosixProcessTreeFinalSample sample;
        sample.cpu_available = true;
        sample.user_cpu_ms = milliseconds(final_usage_.ru_utime);
        sample.kernel_cpu_ms = milliseconds(final_usage_.ru_stime);
        sample.faults_available = true;
        sample.page_faults =
            static_cast<std::uint64_t>(
                std::max<long>(0, final_usage_.ru_minflt)) +
            static_cast<std::uint64_t>(
                std::max<long>(0, final_usage_.ru_majflt));
        sample.io_blocks_available = true;
        sample.io_input_blocks =
            static_cast<std::uint64_t>(
                std::max<long>(0, final_usage_.ru_inblock));
        sample.io_output_blocks =
            static_cast<std::uint64_t>(
                std::max<long>(0, final_usage_.ru_oublock));
        if (final_usage_.ru_maxrss > 0) {
            sample.working_set_peak_available = true;
            const auto resident = static_cast<std::uint64_t>(
                final_usage_.ru_maxrss);
#if defined(__APPLE__)
            sample.working_set_peak_bytes = resident;
#else
            sample.working_set_peak_bytes =
                resident >
                        std::numeric_limits<std::uint64_t>::max() /
                            1'024u
                    ? std::numeric_limits<std::uint64_t>::max()
                    : resident * 1'024u;
#endif
        }
        telemetry_->record_posix_process_tree_final_sample(sample);
    }

    pid_t child_ = -1;
    katana::cli::PortBuildTelemetryRecorder* telemetry_ = nullptr;
    std::string telemetry_phase_;
    int status_ = 0;
    struct rusage final_usage_ {};
    bool root_terminal_ = false;
    bool hard_group_kill_sent_ = false;
    bool process_tree_quiescent_ = false;
    bool reaped_ = false;
    bool usage_available_ = false;
    bool telemetry_finished_ = false;
#if defined(__linux__)
    std::vector<pid_t> baseline_children_;
    std::size_t empty_descendant_observations_ = 0u;
#endif
};
#endif

SupervisedHostCommandResult run_supervised_host_command(
    const std::string& command,
    const PortHostCommandTimeout timeout,
    katana::cli::PortBuildTelemetryRecorder* telemetry = nullptr,
    const std::string_view telemetry_phase = {},
    // nullopt preserves CL/_CL_; zero strips all /MP options for an external
    // scheduler such as Ninja; N strips conflicts and appends exact /MPN as
    // the final _CL_ option seen by cl.exe.
    const std::optional<std::size_t> windows_cl_jobs =
        std::nullopt,
    const std::function<void()>& supervision_heartbeat = {}) {
    if (timeout &&
        (timeout->count() <= 0 ||
         *timeout > maximum_port_host_command_runtime))
        throw std::invalid_argument(
            "Port-Hostprozess braucht 'unlimited' oder ein Zeitlimit von "
            "hoechstens 20 Minuten.");
    SupervisedHostCommandResult result;
    const SupervisedHostCommandTelemetryAttempt telemetry_attempt(
        telemetry, telemetry_phase, result);
#ifdef _WIN32
    // Until CreateProcess succeeds there is no child tree to drain. Once a
    // suspended child exists every exit path must replace this trivial proof
    // with a checked job/direct-process quiescence proof.
    result.process_tree_quiescent = true;
    const auto clear_telemetry_process_tree = [&]() noexcept {
        if (telemetry == nullptr) return;
        telemetry->clear_process_tree();
        telemetry->sample_resources(telemetry_phase);
    };
    const auto job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr)
        throw std::runtime_error(
            "Port-Hostprozess-Job konnte nicht erstellt werden.");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits))) {
        CloseHandle(job);
        throw std::runtime_error(
            "Port-Hostprozess-Job konnte nicht begrenzt werden.");
    }

    auto command_line = std::string(
        "cmd.exe /d /v:off /s /c "
        "\"%KATANA_RAW_HOST_COMMAND%\"");
    const auto child_environment =
        windows_child_environment(windows_cl_jobs, command);
    std::array<HANDLE, 3u> inherited_standard_handles{
        INVALID_HANDLE_VALUE,
        INVALID_HANDLE_VALUE,
        INVALID_HANDLE_VALUE};
    const auto close_inherited_standard_handles = [&]() noexcept {
        for (auto& handle : inherited_standard_handles) {
            if (handle != nullptr &&
                handle != INVALID_HANDLE_VALUE)
                static_cast<void>(CloseHandle(handle));
            handle = INVALID_HANDLE_VALUE;
        }
    };
    const auto inheritable_standard_handle = [](
        const DWORD identifier,
        const DWORD fallback_access) noexcept -> HANDLE {
        const auto parent = GetCurrentProcess();
        const auto source = GetStdHandle(identifier);
        HANDLE duplicate = INVALID_HANDLE_VALUE;
        if (source != nullptr && source != INVALID_HANDLE_VALUE &&
            DuplicateHandle(
                parent,
                source,
                parent,
                &duplicate,
                0u,
                TRUE,
                DUPLICATE_SAME_ACCESS))
            return duplicate;
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        return CreateFileW(
            L"NUL",
            fallback_access,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    };
    inherited_standard_handles[0] =
        inheritable_standard_handle(
            STD_INPUT_HANDLE, GENERIC_READ);
    inherited_standard_handles[1] =
        inheritable_standard_handle(
            STD_OUTPUT_HANDLE, GENERIC_WRITE);
    inherited_standard_handles[2] =
        inheritable_standard_handle(
            STD_ERROR_HANDLE, GENERIC_WRITE);
    if (std::any_of(
            inherited_standard_handles.begin(),
            inherited_standard_handles.end(),
            [](const HANDLE handle) {
                return handle == nullptr ||
                       handle == INVALID_HANDLE_VALUE;
            })) {
        close_inherited_standard_handles();
        CloseHandle(job);
        throw std::runtime_error(
            "Port-Hostprozess-Standardhandles konnten nicht sicher "
            "gebunden werden.");
    }
    SIZE_T attribute_bytes = 0u;
    static_cast<void>(InitializeProcThreadAttributeList(
        nullptr, 1u, 0u, &attribute_bytes));
    if (attribute_bytes == 0u) {
        close_inherited_standard_handles();
        CloseHandle(job);
        throw std::runtime_error(
            "Port-Hostprozess-Handle-Whitelist konnte nicht bemessen "
            "werden.");
    }
    std::vector<std::byte> attribute_storage(attribute_bytes);
    auto* const attributes =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            attribute_storage.data());
    if (!InitializeProcThreadAttributeList(
            attributes, 1u, 0u, &attribute_bytes)) {
        close_inherited_standard_handles();
        CloseHandle(job);
        throw std::runtime_error(
            "Port-Hostprozess-Handle-Whitelist konnte nicht "
            "initialisiert werden.");
    }
    if (!UpdateProcThreadAttribute(
            attributes,
            0u,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_standard_handles.data(),
            inherited_standard_handles.size() * sizeof(HANDLE),
            nullptr,
            nullptr)) {
        DeleteProcThreadAttributeList(attributes);
        close_inherited_standard_handles();
        CloseHandle(job);
        throw std::runtime_error(
            "Port-Hostprozess-Handle-Whitelist konnte nicht erstellt "
            "werden.");
    }
    STARTUPINFOEXA startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inherited_standard_handles[0];
    startup.StartupInfo.hStdOutput = inherited_standard_handles[1];
    startup.StartupInfo.hStdError = inherited_standard_handles[2];
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    const auto process_created =
        CreateProcessA(nullptr,
                       command_line.data(),
                       nullptr,
                       nullptr,
                       TRUE,
                       CREATE_NO_WINDOW | CREATE_SUSPENDED |
                           EXTENDED_STARTUPINFO_PRESENT,
                       static_cast<void*>(
                           const_cast<char*>(
                               child_environment.data())),
                       nullptr,
                       &startup.StartupInfo,
                       &process) != FALSE;
    DeleteProcThreadAttributeList(attributes);
    close_inherited_standard_handles();
    if (!process_created) {
        CloseHandle(job);
        throw std::runtime_error(
            "Port-Hostprozess konnte nicht gestartet werden.");
    }
    result.process_tree_quiescent = false;
    const auto close_supervision_handles = [&]() noexcept {
        clear_telemetry_process_tree();
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
    };
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        const auto process_quiescent =
            terminate_windows_process_and_wait(
                process.hProcess,
                125u);
        close_supervision_handles();
        if (!process_quiescent)
            throw std::runtime_error(
                "Port-Hostprozess konnte nach fehlgeschlagener "
                "Jobbindung nicht sicher beendet werden.");
        result.process_tree_quiescent = true;
        throw std::runtime_error(
            "Port-Hostprozess konnte nicht an seinen Prozessbaum-Job "
            "gebunden werden.");
    }
    if (telemetry != nullptr)
        telemetry->register_windows_job(
            reinterpret_cast<std::uintptr_t>(job));
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        const auto quiescence =
            terminate_windows_job_and_wait(
                job,
                process.hProcess,
                125u);
        close_supervision_handles();
        if (quiescence !=
            WindowsJobQuiescenceResult::Quiescent)
            throw std::runtime_error(
                "Port-Hostprozess konnte nach fehlgeschlagenem Start "
                "nicht sicher beendet werden.");
        result.process_tree_quiescent = true;
        throw std::runtime_error(
            "Port-Hostprozess konnte nicht sicher gestartet werden.");
    }

    const auto deadline =
        timeout
            ? std::optional(
                  std::chrono::steady_clock::now() +
                  *timeout)
            : std::nullopt;
    bool root_terminal = false;
    DWORD root_exit_code = 1u;
    std::optional<std::chrono::steady_clock::time_point>
        root_descendant_grace_deadline;
    for (;;) {
        if (supervision_heartbeat) supervision_heartbeat();
        const auto root_wait =
            WaitForSingleObject(process.hProcess, 0u);
        if (root_wait == WAIT_FAILED ||
            (root_wait != WAIT_TIMEOUT &&
             root_wait != WAIT_OBJECT_0)) {
            const auto quiescence =
                terminate_windows_job_and_wait(
                    job,
                    process.hProcess,
                    125u);
            close_supervision_handles();
            if (quiescence !=
                WindowsJobQuiescenceResult::Quiescent)
                throw std::runtime_error(
                    "Port-Hostprozessaufsicht schlug fehl und der "
                    "Prozessbaum konnte nicht nachweislich geleert "
                    "werden.");
            result.process_tree_quiescent = true;
            throw std::runtime_error(
                "Port-Hostprozess konnte nicht sicher beaufsichtigt "
                "werden; sein Prozessbaum wurde vollstaendig beendet.");
        }

        if (root_wait == WAIT_OBJECT_0 && !root_terminal) {
            if (!GetExitCodeProcess(
                    process.hProcess, &root_exit_code)) {
                const auto quiescence =
                    terminate_windows_job_and_wait(
                        job,
                        process.hProcess,
                        125u);
                close_supervision_handles();
                if (quiescence !=
                    WindowsJobQuiescenceResult::Quiescent)
                    throw std::runtime_error(
                        "Port-Hostprozessstatus war unlesbar und sein "
                        "Prozessbaum konnte nicht nachweislich geleert "
                        "werden.");
                result.process_tree_quiescent = true;
                throw std::runtime_error(
                    "Port-Hostprozessstatus konnte nicht gelesen "
                    "werden; sein Prozessbaum wurde beendet.");
            }
            root_terminal = true;
            root_descendant_grace_deadline =
                std::chrono::steady_clock::now() +
                windows_port_host_post_exit_grace;
        }

        const auto empty = query_windows_job_empty(job);
        if (!empty.query_succeeded) {
            const auto quiescence =
                terminate_windows_job_and_wait(
                    job,
                    process.hProcess,
                    125u);
            close_supervision_handles();
            if (quiescence !=
                WindowsJobQuiescenceResult::Quiescent)
                throw std::runtime_error(
                    "Port-Hostprozessbaumstatus war unlesbar und der "
                    "Prozessbaum konnte nicht nachweislich geleert "
                    "werden.");
            result.process_tree_quiescent = true;
            throw std::runtime_error(
                "Port-Hostprozessbaumstatus konnte nicht gelesen "
                "werden; der Prozessbaum wurde beendet.");
        }
        if (root_terminal && empty.empty) {
            result.exit_code = static_cast<int>(root_exit_code);
            result.exit_code_available = true;
            result.process_tree_quiescent = true;
            break;
        }

        if (root_terminal && root_descendant_grace_deadline &&
            std::chrono::steady_clock::now() >=
                *root_descendant_grace_deadline) {
            const auto quiescence =
                terminate_windows_job_and_wait(
                    job,
                    process.hProcess,
                    125u);
            if (quiescence !=
                WindowsJobQuiescenceResult::Quiescent) {
                close_supervision_handles();
                throw std::runtime_error(
                    "Nach Ende des Port-Hostprozesses verbliebene "
                    "Hilfsprozesse konnten nicht nachweislich geleert "
                    "werden.");
            }
            result.exit_code = static_cast<int>(root_exit_code);
            result.exit_code_available = true;
            result.process_tree_quiescent = true;
            result.post_exit_descendants_terminated = true;
            break;
        }

        if (deadline &&
            std::chrono::steady_clock::now() >= *deadline) {
            result.exit_code = 124;
            result.exit_code_available = true;
            result.timed_out = true;
            const auto quiescence =
                terminate_windows_job_and_wait(
                    job,
                    process.hProcess,
                    124u);
            if (quiescence !=
                WindowsJobQuiescenceResult::Quiescent) {
                close_supervision_handles();
                throw std::runtime_error(
                    "Port-Hostprozess-Timeout konnte den Prozessbaum "
                    "nicht nachweislich leeren.");
            }
            result.process_tree_quiescent = true;
            break;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(10));
    }
    close_supervision_handles();
    return result;
#else
    if (windows_cl_jobs)
        throw std::invalid_argument(
            "MSVC-Compilerbudget wurde auf einer Nicht-Windows-Plattform "
            "angefordert.");
    result.process_tree_quiescent = true;
    ScopedPosixHostSignals forwarding_signals;
#if defined(__linux__)
    ScopedLinuxChildSubreaper child_subreaper;
    auto baseline_children = capture_linux_baseline_children();
#else
    std::vector<pid_t> baseline_children;
#endif
    int launch_ready_pipe[2] = {-1, -1};
    int launch_commit_pipe[2] = {-1, -1};
    if (::pipe(launch_ready_pipe) != 0)
        throw std::runtime_error(
            "Port-Hostprozess-Startvertrag konnte nicht erstellt "
            "werden.");
    if (::pipe(launch_commit_pipe) != 0) {
        static_cast<void>(::close(launch_ready_pipe[0]));
        static_cast<void>(::close(launch_ready_pipe[1]));
        throw std::runtime_error(
            "Port-Hostprozess-Commitvertrag konnte nicht erstellt "
            "werden.");
    }
    const auto close_launch_pipes = [&]() noexcept {
        for (auto& descriptor : launch_ready_pipe) {
            if (descriptor < 0) continue;
            static_cast<void>(::close(descriptor));
            descriptor = -1;
        }
        for (auto& descriptor : launch_commit_pipe) {
            if (descriptor < 0) continue;
            static_cast<void>(::close(descriptor));
            descriptor = -1;
        }
    };
    const std::array launch_descriptors{
        launch_ready_pipe[0],
        launch_ready_pipe[1],
        launch_commit_pipe[0],
        launch_commit_pipe[1]};
    for (const auto descriptor : launch_descriptors) {
        const auto flags = ::fcntl(descriptor, F_GETFD);
        if (flags < 0 ||
            ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
            close_launch_pipes();
            throw std::runtime_error(
                "Port-Hostprozess-Startvertrag konnte nicht "
                "close-on-exec gebunden werden.");
        }
    }
    const auto child = ::fork();
    if (child < 0) {
        close_launch_pipes();
        throw std::runtime_error(
            "Port-Hostprozess konnte nicht gestartet werden.");
    }
    if (child == 0) {
        static_cast<void>(::close(launch_ready_pipe[0]));
        static_cast<void>(::close(launch_commit_pipe[1]));
        ScopedPosixHostSignals::restore_defaults_in_child();
        const auto group_ready = ::setpgid(0, 0) == 0;
        const char launch_state = group_ready ? 'R' : 'E';
        ssize_t written = -1;
        do {
            written = ::write(
                launch_ready_pipe[1], &launch_state, 1u);
        } while (written < 0 && errno == EINTR);
        static_cast<void>(::close(launch_ready_pipe[1]));
        if (!group_ready || written != 1) {
            static_cast<void>(::close(launch_commit_pipe[0]));
            ::_exit(126);
        }
        char launch_commit = 0;
        ssize_t commit_read = -1;
        do {
            commit_read = ::read(
                launch_commit_pipe[0], &launch_commit, 1u);
        } while (commit_read < 0 && errno == EINTR);
        static_cast<void>(::close(launch_commit_pipe[0]));
        if (commit_read != 1 || launch_commit != 'C')
            ::_exit(126);
        ::execl("/bin/sh",
                "sh",
                "-c",
                command.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    result.process_tree_quiescent = false;
    PosixUncommittedChildGuard uncommitted_child(child);
    static_cast<void>(::close(launch_ready_pipe[1]));
    launch_ready_pipe[1] = -1;
    static_cast<void>(::close(launch_commit_pipe[0]));
    launch_commit_pipe[0] = -1;
    char launch_state = 0;
    ssize_t launch_read = -1;
    do {
        launch_read = ::read(
            launch_ready_pipe[0], &launch_state, 1u);
    } while (launch_read < 0 && errno == EINTR);
    if (launch_read != 1 || launch_state != 'R') {
        close_launch_pipes();
        if (!uncommitted_child.terminate_and_reap())
            throw std::runtime_error(
                "Port-Hostprozess konnte vor dem Exec-Commit nicht "
                "nachweislich beendet und reap-ed werden.");
        result.process_tree_quiescent = true;
        throw std::runtime_error(
            "Port-Hostprozess konnte keine nachweislich eigene "
            "Prozessgruppe vorbereiten.");
    }

    PosixChildSupervisor supervisor(
        child,
        telemetry,
        telemetry_phase,
        std::move(baseline_children));
    uncommitted_child.release();
    const char launch_commit = 'C';
    ssize_t commit_written = -1;
    do {
        commit_written = ::write(
            launch_commit_pipe[1], &launch_commit, 1u);
    } while (commit_written < 0 && errno == EINTR);
    close_launch_pipes();
    if (commit_written != 1) {
        if (!supervisor.terminate_and_reap(SIGKILL))
            throw std::runtime_error(
                "Port-Hostprozess konnte nach fehlgeschlagenem "
                "Exec-Commit nicht nachweislich beendet und reap-ed "
                "werden.");
        result.process_tree_quiescent = true;
        throw std::runtime_error(
            "Port-Hostprozess-Exec-Commit konnte nicht zugestellt "
            "werden.");
    }

    const auto deadline =
        timeout
            ? std::optional(
                  std::chrono::steady_clock::now() +
                  *timeout)
            : std::nullopt;
    for (;;) {
        if (!supervisor.refresh_descendant_telemetry()) {
            if (!supervisor.terminate_and_reap(SIGKILL))
                throw std::runtime_error(
                    "Linux-Descendant-Telemetrie schlug fehl; auch "
                    "der Failsafe konnte den Prozessbaum nicht "
                    "nachweislich leeren.");
            result.process_tree_quiescent = true;
            throw std::runtime_error(
                "Linux-Descendant-Telemetrie konnte den aktiven "
                "Prozessbaum nicht vollstaendig erfassen.");
        }
        if (supervision_heartbeat) supervision_heartbeat();
        const auto forwarded_signal =
            forwarding_signals.pending_signal();
        if (forwarded_signal == SIGINT ||
            forwarded_signal == SIGTERM) {
            result.exit_code = 128 + forwarded_signal;
            result.exit_code_available = true;
            result.interrupted = true;
            result.forwarded_signal = forwarded_signal;
            if (!supervisor.terminate_and_reap(
                    forwarded_signal))
                throw std::runtime_error(
                    "Unterbrechung konnte den Port-Hostprozessbaum "
                    "nicht nachweislich leeren und reap-en.");
            break;
        }

        const auto root_state = supervisor.observe_root();
        if (root_state == PosixRootState::Failed) {
            if (!supervisor.terminate_and_reap(SIGKILL))
                throw std::runtime_error(
                    "Port-Hostprozessaufsicht schlug fehl; auch der "
                    "Failsafe konnte den Prozessbaum nicht "
                    "nachweislich leeren und reap-en.");
            result.process_tree_quiescent = true;
            throw std::runtime_error(
                "Port-Hostprozess konnte nicht sicher beaufsichtigt "
                "werden; sein Prozessbaum wurde vollstaendig beendet.");
        }
        if (root_state == PosixRootState::Terminal) {
            // The direct child intentionally remains waitable via WNOWAIT
            // until all remaining PGID members are gone naturally. Only the
            // overall deadline or an external interruption may signal them.
            const auto quiescence =
                supervisor.try_natural_quiescent_reap();
            if (quiescence ==
                PosixTreeQuiescenceState::Failed) {
                if (!supervisor.terminate_and_reap(SIGKILL))
                    throw std::runtime_error(
                        "Port-Hostprozessaufsicht verlor ihren "
                        "Quieszenzbeweis; auch der Failsafe konnte den "
                        "Prozessbaum nicht sicher leeren.");
                result.process_tree_quiescent = true;
                throw std::runtime_error(
                    "Port-Hostprozessbaum konnte nicht sicher "
                    "beaufsichtigt werden; er wurde vollstaendig "
                    "beendet.");
            }
            if (quiescence ==
                PosixTreeQuiescenceState::Quiescent) {
                result.exit_code = supervisor.exit_code();
                result.exit_code_available = true;
                break;
            }
        }
        if (deadline &&
            std::chrono::steady_clock::now() >= *deadline) {
            result.exit_code = 124;
            result.exit_code_available = true;
            result.timed_out = true;
            if (!supervisor.terminate_and_reap(SIGTERM))
                throw std::runtime_error(
                    "Port-Hostprozess-Timeout konnte den Prozessbaum "
                    "nicht nachweislich leeren und reap-en.");
            break;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(20));
    }
    result.process_tree_quiescent =
        supervisor.process_tree_quiescent();
    supervisor.finish_telemetry();
    if (!result.process_tree_quiescent)
        throw std::runtime_error(
            "Port-Hostprozessbaum besitzt keinen beweisbar "
            "quieszenten Endzustand.");
    return result;
#endif
}

struct HostCompileBudget final {
    std::size_t requested = 1u;
    std::size_t effective = 1u;
};

HostCompileBudget configured_host_compile_budget() {
    constexpr std::size_t maximum_jobs = 256u;
    const auto detected = std::clamp<std::size_t>(
        std::max(1u, std::thread::hardware_concurrency()),
        1u,
        maximum_jobs);
    const auto configured = configured_environment_value(
        "KATANA_HOST_COMPILE_JOBS");
    const auto legacy = configured_environment_value(
        "KATANA_HOST_BUILD_JOBS");
    const auto parse = [](const std::string& value,
                          const std::string_view name) {
        std::size_t jobs = 0u;
        const auto conversion = std::from_chars(
            value.data(), value.data() + value.size(), jobs, 10);
        if (conversion.ec != std::errc{} ||
            conversion.ptr != value.data() + value.size() ||
            jobs == 0u || jobs > maximum_jobs)
            throw std::invalid_argument(
                std::string(name) + " ist ungueltig.");
        return jobs;
    };
    if (configured && legacy &&
        parse(*configured, "KATANA_HOST_COMPILE_JOBS") !=
            parse(*legacy, "KATANA_HOST_BUILD_JOBS"))
        throw std::invalid_argument(
            "KATANA_HOST_COMPILE_JOBS und das Legacy-Alias "
            "KATANA_HOST_BUILD_JOBS widersprechen sich.");
    const auto requested = configured
                               ? parse(
                                     *configured,
                                     "KATANA_HOST_COMPILE_JOBS")
                               : legacy
                                     ? parse(
                                           *legacy,
                                           "KATANA_HOST_BUILD_JOBS")
                                     : detected;
    return {requested, std::min(requested, detected)};
}

bool valid_port_target_name(const std::string_view value) noexcept {
    if (value.empty() ||
        !std::isalpha(static_cast<unsigned char>(value.front())))
        return false;
    const auto valid_character = [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '_' ||
               character == '-';
    };
    return std::all_of(value.begin(), value.end(), valid_character) &&
           value != "katana-recomp" && value != "katana_runtime" &&
           value != "katana_core" && value != "katana_generated" &&
           value != "all" && value != "clean" && value != "install" &&
           value != "test" && value != "help" &&
           value != "rebuild_cache";
}

bool unsafe_port_filesystem_link(
    const std::filesystem::path& path,
    const std::filesystem::file_status status) noexcept {
    if (std::filesystem::is_symlink(status)) return true;
#ifdef _WIN32
    try {
        auto inspected = std::filesystem::absolute(path).lexically_normal();
        inspected.make_preferred();
        const auto native = inspected.native();
        if (native.starts_with(LR"(\\.\)")) return true;
        if (!native.starts_with(LR"(\\?\)")) {
            inspected = native.starts_with(LR"(\\)")
                ? std::filesystem::path(
                      std::wstring(LR"(\\?\UNC\)") + native.substr(2u))
                : std::filesystem::path(
                      std::wstring(LR"(\\?\)") + native);
        }
        const auto attributes = GetFileAttributesW(inspected.c_str());
        return attributes == INVALID_FILE_ATTRIBUTES ||
               (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u;
    } catch (...) {
        return true;
    }
#else
    static_cast<void>(path);
    return false;
#endif
}

bool safe_regular_port_directory_exists(const std::filesystem::path& root,
                                         const std::string_view description) {
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(root, status_error);
    if (status_error == std::errc::no_such_file_or_directory ||
        (!status_error &&
         status.type() ==
             std::filesystem::file_type::not_found))
        return false;
    if (status_error)
        throw std::runtime_error(std::string(description) +
                                 " konnte nicht sicher geprueft werden.");
    if (!std::filesystem::is_directory(status) ||
        unsafe_port_filesystem_link(root, status))
        throw std::runtime_error(std::string(description) +
                                  " ist kein sicherer regulaerer Ordner.");
    return true;
}

void require_safe_port_tree(
    const std::filesystem::path& root,
    const std::string_view description) {
    if (!safe_regular_port_directory_exists(root, description))
        throw std::runtime_error(
            std::string(description) + " fehlt.");
    for (std::filesystem::recursive_directory_iterator iterator(root),
         end;
         iterator != end;
         ++iterator) {
        std::error_code status_error;
        const auto status =
            iterator->symlink_status(status_error);
        if (status_error ||
            unsafe_port_filesystem_link(iterator->path(), status) ||
            (!std::filesystem::is_directory(status) &&
             !std::filesystem::is_regular_file(status)))
            throw std::runtime_error(
                std::string(description) +
                " enthaelt einen unsicheren Dateisystemeintrag.");
    }
}

void remove_safe_port_tree(
    const std::filesystem::path& root,
    const std::string_view description) {
    if (!safe_regular_port_directory_exists(root, description))
        return;
    require_safe_port_tree(root, description);
    std::error_code remove_error;
    static_cast<void>(
        std::filesystem::remove_all(root, remove_error));
    if (remove_error)
        throw std::filesystem::filesystem_error(
            std::string(description) +
                " konnte nicht entfernt werden.",
            root,
            remove_error);
}

bool safe_port_directory_chain_exists(
    const std::filesystem::path& root,
    const std::filesystem::path& directory,
    const std::string_view description) {
    const auto normalized_root =
        std::filesystem::absolute(root).lexically_normal();
    const auto normalized_directory =
        std::filesystem::absolute(directory).lexically_normal();
    const auto relative =
        normalized_directory.lexically_relative(normalized_root);
    if ((relative.empty() && normalized_directory != normalized_root) ||
        relative.is_absolute() ||
        (!relative.empty() && *relative.begin() == ".."))
        throw std::runtime_error(
            std::string(description) +
            " liegt ausserhalb des privaten Port-Arbeitsverzeichnisses.");
    if (!safe_regular_port_directory_exists(normalized_root, description))
        return false;
    auto current = normalized_root;
    for (const auto& component : relative) {
        if (component.empty() || component == ".") continue;
        current /= component;
        if (!safe_regular_port_directory_exists(current, description))
            return false;
    }
    return true;
}

void ensure_safe_port_directory_chain(
    const std::filesystem::path& root,
    const std::filesystem::path& directory,
    const std::string_view description) {
    const auto normalized_root =
        std::filesystem::absolute(root).lexically_normal();
    const auto normalized_directory =
        std::filesystem::absolute(directory).lexically_normal();
    const auto relative =
        normalized_directory.lexically_relative(normalized_root);
    if ((relative.empty() && normalized_directory != normalized_root) ||
        relative.is_absolute() ||
        (!relative.empty() && *relative.begin() == "..") ||
        !safe_regular_port_directory_exists(normalized_root, description))
        throw std::runtime_error(
            std::string(description) +
            " besitzt keinen sicheren privaten Port-Stamm.");
    auto current = normalized_root;
    for (const auto& component : relative) {
        if (component.empty() || component == ".") continue;
        current /= component;
        if (safe_regular_port_directory_exists(current, description))
            continue;
        std::error_code create_error;
        if (!std::filesystem::create_directory(current, create_error) &&
            create_error)
            throw std::filesystem::filesystem_error(
                std::string(description) + " konnte nicht erstellt werden.",
                current,
                create_error);
        if (!safe_regular_port_directory_exists(current, description))
            throw std::runtime_error(
                std::string(description) +
                " wurde nicht als sicherer Ordner erstellt.");
    }
}

void ensure_safe_absolute_directory_chain(
    const std::filesystem::path& directory,
    const std::string_view description) {
    const auto normalized =
        std::filesystem::absolute(directory).lexically_normal();
    if (normalized.empty() || normalized.root_path().empty())
        throw std::runtime_error(
            std::string(description) +
            " besitzt keinen absoluten Dateisystemanker.");
    auto current = normalized.root_path();
    const auto require_safe_current =
        [&](const std::filesystem::path& candidate) {
            std::error_code status_error;
            const auto status =
                std::filesystem::symlink_status(
                    candidate, status_error);
            if (status_error ||
                !std::filesystem::is_directory(status) ||
                unsafe_port_filesystem_link(candidate, status))
                throw std::runtime_error(
                    std::string(description) +
                    " enthaelt eine unsichere Elternkomponente.");
        };
    require_safe_current(current);
    for (const auto& component : normalized.relative_path()) {
        if (component.empty() || component == ".") continue;
        if (component == "..")
            throw std::runtime_error(
                std::string(description) +
                " verlaesst seinen absoluten Dateisystemanker.");
        current /= component;
        std::error_code status_error;
        auto status =
            std::filesystem::symlink_status(current, status_error);
        const bool missing =
            status_error == std::errc::no_such_file_or_directory ||
            (!status_error &&
             status.type() ==
                 std::filesystem::file_type::not_found);
        if (missing) {
            status_error.clear();
            if (!std::filesystem::create_directory(
                    current, status_error) &&
                status_error)
                throw std::filesystem::filesystem_error(
                    std::string(description) +
                        " konnte eine Elternkomponente nicht erstellen.",
                    current,
                    status_error);
            status =
                std::filesystem::symlink_status(
                    current, status_error);
        }
        if (status_error ||
            !std::filesystem::is_directory(status) ||
            unsafe_port_filesystem_link(current, status))
            throw std::runtime_error(
                std::string(description) +
                " enthaelt eine unsichere Elternkomponente.");
    }
}

void require_safe_replaceable_port_file(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::string_view description) {
    if (!safe_port_directory_chain_exists(
            root, path.parent_path(), description))
        throw std::runtime_error(
            std::string(description) +
            " besitzt keinen sicheren Zielordner.");
    std::error_code status_error;
    const auto status =
        std::filesystem::symlink_status(path, status_error);
    if (status_error == std::errc::no_such_file_or_directory ||
        (!status_error &&
         status.type() ==
             std::filesystem::file_type::not_found))
        return;
    if (status_error ||
        !std::filesystem::is_regular_file(status) ||
        unsafe_port_filesystem_link(path, status))
        throw std::runtime_error(
            std::string(description) +
            " ist keine sicher ersetzbare regulaere Datei.");
}

void reset_host_build_event_root(
    const std::filesystem::path& port_root,
    const std::filesystem::path& event_root) {
    remove_safe_port_tree(
        event_root, "Hostbuild-Ereignisverzeichnis");
    ensure_safe_port_directory_chain(
        port_root,
        event_root,
        "Hostbuild-Ereignisverzeichnis");
}

#ifdef _WIN32
struct WindowsHostToolWrappers final {
    std::filesystem::path directory;
    std::filesystem::path compiler;
    std::filesystem::path archiver;
    std::filesystem::path linker;
};

[[nodiscard]] WindowsHostToolWrappers
prepare_windows_host_tool_wrappers(
    const std::filesystem::path& port_root,
    const std::filesystem::path& directory) {
    ensure_safe_port_directory_chain(
        port_root, directory, "Windows-Hosttool-Wrapper");
    const auto source = current_process_executable_path();
    const auto source_identity =
        katana::io::capture_input_provenance(
            "host-tool-wrapper-source", source);
    const auto prepare = [&](const std::string_view name) {
        const auto target = directory / std::string(name);
        require_safe_replaceable_port_file(
            port_root, target, "Windows-Hosttool-Wrapper");
        bool current = false;
        std::error_code status_error;
        const auto status =
            std::filesystem::symlink_status(target, status_error);
        if (!status_error && std::filesystem::is_regular_file(status) &&
            regular_reparse_free_windows_path(target)) {
            const auto target_identity =
                katana::io::capture_input_provenance(
                    "host-tool-wrapper-target", target);
            current =
                target_identity.size == source_identity.size &&
                target_identity.sha256 == source_identity.sha256;
        } else if (status_error !=
                       std::errc::no_such_file_or_directory &&
                   (status_error ||
                    status.type() !=
                        std::filesystem::file_type::not_found)) {
            throw std::runtime_error(
                "Windows-Hosttool-Wrapper konnte nicht sicher "
                "geprueft werden.");
        }
        if (!current) {
            std::filesystem::copy_file(
                source,
                target,
                std::filesystem::copy_options::overwrite_existing);
            if (!regular_reparse_free_windows_path(target))
                throw std::runtime_error(
                    "Windows-Hosttool-Wrapper wurde nicht als sichere "
                    "regulaere Datei erzeugt.");
            const auto copied_identity =
                katana::io::capture_input_provenance(
                    "host-tool-wrapper-copy", target);
            if (copied_identity.size != source_identity.size ||
                copied_identity.sha256 != source_identity.sha256)
                throw std::runtime_error(
                    "Windows-Hosttool-Wrapper ist nicht byteidentisch.");
        }
        return target;
    };
    return {
        directory,
        prepare("katana-host-cl-wrapper.exe"),
        prepare("katana-host-archive-wrapper.exe"),
        prepare("katana-host-link-wrapper.exe")};
}
#else
[[nodiscard]] std::filesystem::path
prepare_posix_host_archive_wrapper(
    const std::filesystem::path& port_root,
    const std::filesystem::path& directory) {
    ensure_safe_port_directory_chain(
        port_root, directory, "POSIX-Hostarchive-Wrapper");
    const auto source = current_process_executable_path();
    const auto target = directory / "katana-host-archive-wrapper";
    require_safe_replaceable_port_file(
        port_root, target, "POSIX-Hostarchive-Wrapper");
    const auto source_identity =
        katana::io::capture_input_provenance(
            "host-archive-wrapper-source", source);
    bool current = false;
    std::error_code status_error;
    const auto status =
        std::filesystem::symlink_status(target, status_error);
    if (!status_error && std::filesystem::is_regular_file(status) &&
        !std::filesystem::is_symlink(status)) {
        struct stat identity {};
        if (::lstat(target.c_str(), &identity) != 0 ||
            !S_ISREG(identity.st_mode) || identity.st_nlink != 1)
            throw std::runtime_error(
                "POSIX-Hostarchive-Wrapper besitzt keine sichere "
                "Dateiidentitaet.");
        const auto target_identity =
            katana::io::capture_input_provenance(
                "host-archive-wrapper-target", target);
        current = target_identity.size == source_identity.size &&
                  target_identity.sha256 == source_identity.sha256;
    } else if (status_error !=
                   std::errc::no_such_file_or_directory &&
               (status_error ||
                status.type() !=
                    std::filesystem::file_type::not_found)) {
        throw std::runtime_error(
            "POSIX-Hostarchive-Wrapper konnte nicht sicher geprueft "
            "werden.");
    }
    if (!current) {
        std::filesystem::copy_file(
            source,
            target,
            std::filesystem::copy_options::overwrite_existing);
        std::filesystem::permissions(
            target,
            std::filesystem::status(source).permissions());
        const auto copied_identity =
            katana::io::capture_input_provenance(
                "host-archive-wrapper-copy", target);
        if (copied_identity.size != source_identity.size ||
            copied_identity.sha256 != source_identity.sha256)
            throw std::runtime_error(
                "POSIX-Hostarchive-Wrapper ist nicht byteidentisch.");
    }
    return target;
}
#endif

class ExclusivePortExportLock final {
  public:
    explicit ExclusivePortExportLock(const std::filesystem::path& workspace)
        : lock_path_(workspace.string() + ".lock") {
        ensure_safe_absolute_directory_chain(
            lock_path_.parent_path(), "Port-Exportpfad-Sperre");
#ifdef _WIN32
        handle_ = CreateFileW(lock_path_.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              0u,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error == ERROR_SHARING_VIOLATION ||
                error == ERROR_LOCK_VIOLATION)
                throw std::runtime_error(
                    "Port-Exportpfad wird bereits von einem "
                    "anderen Export verwendet.");
            throw std::runtime_error(
                "Port-Exportpfad-Sperre konnte nicht geoeffnet werden "
                "(Win32-Fehler " + std::to_string(error) + ").");
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(
                handle_,
                FileAttributeTagInfo,
                &attributes,
                sizeof(attributes)) ||
            (attributes.FileAttributes &
             (FILE_ATTRIBUTE_REPARSE_POINT |
              FILE_ATTRIBUTE_DIRECTORY)) != 0u) {
            static_cast<void>(CloseHandle(handle_));
            handle_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "Port-Exportpfad-Sperre ist kein sicheres regulaeres "
                "Artefakt.");
        }
#else
        auto flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        descriptor_ = ::open(lock_path_.c_str(), flags, 0600);
        if (descriptor_ < 0)
            throw std::runtime_error(
                "Port-Exportpfad-Sperre konnte nicht geoeffnet werden.");
        struct stat lock_status {};
        if (::fstat(descriptor_, &lock_status) != 0 ||
            !S_ISREG(lock_status.st_mode)) {
            static_cast<void>(::close(descriptor_));
            descriptor_ = -1;
            throw std::runtime_error(
                "Port-Exportpfad-Sperre ist kein sicheres regulaeres "
                "Artefakt.");
        }
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const auto error = errno;
            static_cast<void>(::close(descriptor_));
            descriptor_ = -1;
            if (error == EWOULDBLOCK || error == EAGAIN)
                throw std::runtime_error(
                    "Port-Exportpfad wird bereits von einem "
                    "anderen Export verwendet.");
            throw std::runtime_error(
                "Port-Exportpfad-Sperre konnte nicht aktiviert werden.");
        }
#endif
    }

    ~ExclusivePortExportLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE)
            static_cast<void>(CloseHandle(handle_));
#else
        if (descriptor_ >= 0) static_cast<void>(::close(descriptor_));
#endif
    }

    ExclusivePortExportLock(const ExclusivePortExportLock&) = delete;
    ExclusivePortExportLock& operator=(const ExclusivePortExportLock&) = delete;

  private:
    std::filesystem::path lock_path_;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

struct PortPublishOutputPaths final {
    std::filesystem::path output;
    std::filesystem::path lock_base;
    std::filesystem::path journal;
    std::string output_identity;
};

struct ActivePortPublishTransaction final {
    PortPublishOutputPaths output_paths;
    std::string token;
    std::string owner_document;
    std::filesystem::path root;
    std::filesystem::path owner_marker;
    std::filesystem::path state;
    std::filesystem::path journal_staging;
    std::filesystem::path stage;
    std::filesystem::path backup;
};

std::filesystem::path canonical_port_publish_output(
    const std::filesystem::path& absolute_output) {
    std::error_code canonical_error;
    const auto canonical_parent =
        std::filesystem::canonical(
            absolute_output.parent_path(), canonical_error);
    if (canonical_error)
        throw std::runtime_error(
            "Port-Ausgabeelternpfad konnte nicht physisch aufgeloest "
            "werden.");
    return (canonical_parent / absolute_output.filename()).lexically_normal();
}

std::string port_publish_output_identity(
    const std::filesystem::path& output) {
#ifdef _WIN32
    const auto parent = output.parent_path();
    const auto parent_handle =
        CreateFileW(parent.c_str(),
                    FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS |
                        FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr);
    if (parent_handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error(
            "Port-Ausgabeelternpfad konnte nicht physisch identifiziert "
            "werden.");
    BY_HANDLE_FILE_INFORMATION information{};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    const auto information_ok =
        GetFileInformationByHandle(parent_handle, &information) != FALSE;
    const auto attributes_ok =
        GetFileInformationByHandleEx(
            parent_handle,
            FileAttributeTagInfo,
            &attributes,
            sizeof(attributes)) != FALSE;
    static_cast<void>(CloseHandle(parent_handle));
    if (!information_ok || !attributes_ok ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
        throw std::runtime_error(
            "Port-Ausgabeelternpfad besitzt keine sichere physische "
            "Identitaet.");

    auto filename = output.filename().native();
    const auto folded_size =
        LCMapStringEx(LOCALE_NAME_INVARIANT,
                      LCMAP_LOWERCASE,
                      filename.data(),
                      static_cast<int>(filename.size()),
                      nullptr,
                      0,
                      nullptr,
                      nullptr,
                      0);
    if (folded_size <= 0)
        throw std::runtime_error(
            "Port-Ausgabename konnte nicht kanonisch normalisiert werden.");
    std::wstring folded(static_cast<std::size_t>(folded_size), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT,
                      LCMAP_LOWERCASE,
                      filename.data(),
                      static_cast<int>(filename.size()),
                      folded.data(),
                      folded_size,
                      nullptr,
                      nullptr,
                      0) != folded_size)
        throw std::runtime_error(
            "Port-Ausgabename konnte nicht kanonisch normalisiert werden.");

    std::ostringstream identity;
    identity << "windows-parent-file-id-v1:"
             << std::hex << std::setfill('0')
             << std::setw(8) << information.dwVolumeSerialNumber << ':'
             << std::setw(8) << information.nFileIndexHigh
             << std::setw(8) << information.nFileIndexLow << ':';
    for (const auto character : folded)
        identity << std::setw(4)
                 << static_cast<std::uint32_t>(
                        static_cast<std::uint16_t>(character));
    return katana::io::sha256_bytes(identity.str());
#else
    return katana::io::sha256_bytes(output.generic_string());
#endif
}

PortPublishOutputPaths port_publish_output_paths(
    const std::filesystem::path& absolute_output) {
    PortPublishOutputPaths paths;
    paths.output = canonical_port_publish_output(absolute_output);
    paths.lock_base =
        std::filesystem::path(paths.output.string() + ".katana-publish");
    paths.journal =
        std::filesystem::path(
            paths.output.string() + ".katana-publish-transaction");
    paths.output_identity = port_publish_output_identity(paths.output);
    return paths;
}

bool safe_regular_port_file_exists(
    const std::filesystem::path& path,
    const std::string_view description) {
    std::error_code status_error;
    const auto status =
        std::filesystem::symlink_status(path, status_error);
    if (status_error == std::errc::no_such_file_or_directory ||
        (!status_error &&
         status.type() == std::filesystem::file_type::not_found))
        return false;
    if (status_error ||
        !std::filesystem::is_regular_file(status) ||
        unsafe_port_filesystem_link(path, status))
        throw std::runtime_error(
            std::string(description) +
            " ist kein sicheres regulaeres Dateiartefakt.");
    return true;
}

struct CompilerCacheBinding final {
    std::string launcher;
    std::optional<std::filesystem::path> managed_storage;
};

[[nodiscard]] bool same_existing_path(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    try {
        std::error_code equivalent_error;
        const auto equivalent =
            std::filesystem::equivalent(left, right, equivalent_error);
        if (!equivalent_error) return equivalent;
        std::error_code left_error;
        std::error_code right_error;
        const auto left_canonical =
            std::filesystem::weakly_canonical(left, left_error);
        const auto right_canonical =
            std::filesystem::weakly_canonical(right, right_error);
        if (!left_error && !right_error)
            return left_canonical == right_canonical;
        return left.lexically_normal() == right.lexically_normal();
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool is_katana_host_build_launcher(
    const std::string_view launcher) {
    auto filename = std::filesystem::path(launcher).filename().string();
    std::transform(
        filename.begin(), filename.end(), filename.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return filename == "katana-recomp" ||
           filename == "katana-recomp.exe" ||
           filename == "katana-host-cl-wrapper" ||
           filename == "katana-host-cl-wrapper.exe" ||
           filename == "katana-host-link-wrapper" ||
           filename == "katana-host-link-wrapper.exe";
}

[[nodiscard]] bool compiler_cache_would_recurse(
    const std::string_view launcher) {
    if (launcher.empty() || is_katana_host_build_launcher(launcher))
        return true;
    return same_existing_path(
        std::filesystem::path(launcher),
        current_process_executable_path());
}

#ifdef _WIN32
[[nodiscard]] bool prepare_managed_compiler_cache_storage(
    const std::filesystem::path& storage) {
    std::error_code storage_error;
    std::filesystem::create_directories(storage, storage_error);
    if (storage_error) return false;
    const auto status =
        std::filesystem::symlink_status(storage, storage_error);
    return !storage_error &&
           status.type() == std::filesystem::file_type::directory &&
           !std::filesystem::is_symlink(status);
}
#endif

std::optional<CompilerCacheBinding> configured_compiler_cache_binding() {
    if (const auto configured =
            configured_environment_value("KATANA_COMPILER_CACHE"))
        return CompilerCacheBinding{*configured, std::nullopt};
    if (const auto configured =
            configured_environment_value("CMAKE_CXX_COMPILER_LAUNCHER"))
        return CompilerCacheBinding{*configured, std::nullopt};
    if (!katana::build_contract::configured_compiler_launcher.empty())
        return CompilerCacheBinding{
            std::string(
                katana::build_contract::configured_compiler_launcher),
            std::nullopt};
#ifdef _WIN32
    // The desktop dependency installer places the freely redistributable
    // sccache binary and its persistent object store below LOCALAPPDATA.
    // Discover that managed installation for ordinary CLI/double-click
    // exports too; requiring an environment variable made the generated
    // project report a cache while the instrumented compile wrapper still
    // invoked cl.exe directly.
    if (const auto local_app_data =
            configured_environment_value("LOCALAPPDATA")) {
        const auto managed_root =
            std::filesystem::path(*local_app_data) / "KatanaRecomp";
        constexpr std::string_view managed_sccache_version = "0.17.0";
        const auto versioned_directory =
            managed_root / "tools" /
            ("sccache-v" + std::string(managed_sccache_version));
        const auto target_directory =
            "sccache-v" + std::string(managed_sccache_version) +
            "-x86_64-pc-windows-msvc";
        // Keep the managed layout deterministic, but accept both the
        // unpacked release directory used by the installer and its flat
        // equivalent used by older local dependency bundles.
        const std::array<std::filesystem::path, 3u> launchers{
            versioned_directory / target_directory / "sccache.exe",
            versioned_directory / "sccache.exe",
            managed_root / "tools" / target_directory / "sccache.exe"};
        for (const auto& launcher : launchers) {
            if (safe_regular_port_file_exists(
                    launcher, "Verwalteter MSVC-Objektcache"))
                return CompilerCacheBinding{
                    launcher.generic_string(),
                    managed_root / "compiler-cache" /
                        ("sccache-v" +
                         std::string(managed_sccache_version))};
        }
    }
#endif
    return std::nullopt;
}

std::string read_safe_small_port_file(
    const std::filesystem::path& path,
    const std::size_t maximum_size,
    const std::string_view description) {
#ifdef _WIN32
    // Never perform a path-based size check followed by a second path-based
    // open.  The handle is opened without following a reparse point and all
    // size/attribute checks are made against that same handle.
    const auto handle = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error(
            std::string(description) +
            " konnte nicht sicher geoeffnet werden.");
    struct ScopedReadHandle final {
        HANDLE value = INVALID_HANDLE_VALUE;

        ~ScopedReadHandle() noexcept {
            if (value != INVALID_HANDLE_VALUE)
                static_cast<void>(CloseHandle(value));
        }
    };
    const ScopedReadHandle scoped_handle{handle};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    LARGE_INTEGER initial_size{};
    if (!GetFileInformationByHandleEx(
            handle,
            FileAttributeTagInfo,
            &attributes,
            sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
        !GetFileSizeEx(handle, &initial_size) || initial_size.QuadPart < 0 ||
        static_cast<unsigned long long>(initial_size.QuadPart) >
            static_cast<unsigned long long>(maximum_size) ||
        static_cast<unsigned long long>(initial_size.QuadPart) >
            static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error(
            std::string(description) +
            " besitzt keine sichere begrenzte Groesse.");
    }
    const auto size = static_cast<std::size_t>(initial_size.QuadPart);
    std::string document(size, '\0');
    std::size_t offset = 0u;
    while (offset < size) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            size - offset, static_cast<std::size_t>(1u << 20u)));
        DWORD read = 0u;
        if (!ReadFile(
                handle,
                document.data() + offset,
                chunk,
                &read,
                nullptr) ||
            read != chunk) {
            throw std::runtime_error(
                std::string(description) +
                " konnte nicht vollstaendig gelesen werden.");
        }
        offset += read;
    }
    LARGE_INTEGER final_size{};
    FILE_ATTRIBUTE_TAG_INFO final_attributes{};
    const bool stable =
        GetFileSizeEx(handle, &final_size) != FALSE &&
        GetFileInformationByHandleEx(
            handle,
            FileAttributeTagInfo,
            &final_attributes,
            sizeof(final_attributes)) != FALSE &&
        final_size.QuadPart == initial_size.QuadPart &&
        (final_attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u &&
        (final_attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u;
    if (!stable)
        throw std::runtime_error(
            std::string(description) +
            " wurde waehrend des Lesens veraendert.");
    return document;
#else
#if !defined(O_NOFOLLOW)
    // A portable fallback which cannot promise no-follow semantics must not
    // silently weaken this CLI's artifact boundary.
    throw std::runtime_error(
        std::string(description) +
        " kann auf diesem System ohne O_NOFOLLOW nicht sicher gelesen werden.");
#else
    int flags = O_RDONLY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0)
        throw std::runtime_error(
            std::string(description) +
            " konnte nicht sicher geoeffnet werden.");
    struct ScopedReadDescriptor final {
        int value = -1;

        ~ScopedReadDescriptor() noexcept {
            if (value >= 0)
                static_cast<void>(::close(value));
        }
    };
    const ScopedReadDescriptor scoped_descriptor{descriptor};
    struct stat initial_status{};
    if (::fstat(descriptor, &initial_status) != 0 ||
        !S_ISREG(initial_status.st_mode) || initial_status.st_size < 0 ||
        static_cast<std::uintmax_t>(initial_status.st_size) > maximum_size ||
        static_cast<std::uintmax_t>(initial_status.st_size) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error(
            std::string(description) +
            " besitzt keine sichere begrenzte Groesse.");
    }
    const auto size = static_cast<std::size_t>(initial_status.st_size);
    std::string document(size, '\0');
    std::size_t offset = 0u;
    while (offset < size) {
        const auto read = ::read(
            descriptor,
            document.data() + offset,
            std::min<std::size_t>(size - offset, static_cast<std::size_t>(1u << 20u)));
        if (read < 0 && errno == EINTR) continue;
        if (read <= 0) {
            throw std::runtime_error(
                std::string(description) +
                " konnte nicht vollstaendig gelesen werden.");
        }
        offset += static_cast<std::size_t>(read);
    }
    struct stat final_status{};
    const bool stable =
        ::fstat(descriptor, &final_status) == 0 &&
        S_ISREG(final_status.st_mode) &&
        final_status.st_size == initial_status.st_size;
    if (!stable)
        throw std::runtime_error(
            std::string(description) +
            " wurde waehrend des Lesens veraendert.");
    return document;
#endif
#endif
}

void write_exclusive_safe_port_file(
    const std::filesystem::path& path,
    const std::string_view document,
    const std::string_view description) {
#ifdef _WIN32
    auto handle =
        CreateFileW(path.c_str(),
                    GENERIC_READ | GENERIC_WRITE,
                    0u,
                    nullptr,
                    CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error(
            std::string(description) +
            " konnte nicht exklusiv erstellt werden.");
    try {
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(
                handle,
                FileAttributeTagInfo,
                &attributes,
                sizeof(attributes)) ||
            (attributes.FileAttributes &
             (FILE_ATTRIBUTE_REPARSE_POINT |
              FILE_ATTRIBUTE_DIRECTORY)) != 0u)
            throw std::runtime_error(
                std::string(description) +
                " wurde nicht als sicheres regulaeres Dateiartefakt "
                "erstellt.");
        std::size_t offset = 0u;
        while (offset < document.size()) {
            const auto remaining = document.size() - offset;
            const auto chunk =
                static_cast<DWORD>(
                    std::min<std::size_t>(
                        remaining,
                        std::numeric_limits<DWORD>::max()));
            DWORD written = 0u;
            if (!WriteFile(handle,
                           document.data() + offset,
                           chunk,
                           &written,
                           nullptr) ||
                written != chunk)
                throw std::runtime_error(
                    std::string(description) +
                    " konnte nicht vollstaendig geschrieben werden.");
            offset += written;
        }
        if (!FlushFileBuffers(handle))
            throw std::runtime_error(
                std::string(description) +
                " konnte nicht dauerhaft geschrieben werden.");
        static_cast<void>(CloseHandle(handle));
        handle = INVALID_HANDLE_VALUE;
    } catch (...) {
        if (handle != INVALID_HANDLE_VALUE)
            static_cast<void>(CloseHandle(handle));
        static_cast<void>(DeleteFileW(path.c_str()));
        throw;
    }
#else
    auto flags = O_CREAT | O_EXCL | O_WRONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    auto descriptor = ::open(path.c_str(), flags, 0600);
    if (descriptor < 0)
        throw std::runtime_error(
            std::string(description) +
            " konnte nicht exklusiv erstellt werden.");
    try {
        struct stat status {};
        if (::fstat(descriptor, &status) != 0 ||
            !S_ISREG(status.st_mode))
            throw std::runtime_error(
                std::string(description) +
                " wurde nicht als sicheres regulaeres Dateiartefakt "
                "erstellt.");
        std::size_t offset = 0u;
        while (offset < document.size()) {
            const auto written =
                ::write(descriptor,
                        document.data() + offset,
                        document.size() - offset);
            if (written <= 0)
                throw std::runtime_error(
                    std::string(description) +
                    " konnte nicht vollstaendig geschrieben werden.");
            offset += static_cast<std::size_t>(written);
        }
        if (::fsync(descriptor) != 0)
            throw std::runtime_error(
                std::string(description) +
                " konnte nicht dauerhaft geschrieben werden.");
        if (::close(descriptor) != 0) {
            descriptor = -1;
            throw std::runtime_error(
                std::string(description) +
                " konnte nicht sicher geschlossen werden.");
        }
        descriptor = -1;
    } catch (...) {
        if (descriptor >= 0) static_cast<void>(::close(descriptor));
        static_cast<void>(::unlink(path.c_str()));
        throw;
    }
#endif
}

void remove_owned_safe_port_file(
    const std::filesystem::path& path,
    const std::string_view expected_document,
    const std::string_view description) {
    const auto document =
        read_safe_small_port_file(path, 1024u, description);
    if (document != expected_document)
        throw std::runtime_error(
            std::string(description) +
            " ist nicht transaktionseigen und bleibt unangetastet.");
    std::error_code remove_error;
    if (!std::filesystem::remove(path, remove_error) || remove_error)
        throw std::filesystem::filesystem_error(
            std::string(description) +
                " konnte nicht entfernt werden.",
            path,
            remove_error);
}

std::string port_publish_owner_document(
    const PortPublishOutputPaths& paths,
    const std::string_view token) {
    return "KATANA_PORT_PUBLISH_TRANSACTION_V1\n"
           "output-sha256=" +
           paths.output_identity +
           "\ntoken=" + std::string(token) + "\n";
}

std::optional<std::string> parse_port_publish_journal_token(
    const std::string_view document,
    const PortPublishOutputPaths& paths) {
    constexpr std::string_view header =
        "KATANA_PORT_PUBLISH_TRANSACTION_V1\n";
    const auto digest_line =
        "output-sha256=" + paths.output_identity + "\n";
    if (!document.starts_with(header) ||
        !document.substr(header.size()).starts_with(digest_line))
        return std::nullopt;
    const auto token_line =
        document.substr(header.size() + digest_line.size());
    constexpr std::string_view token_prefix = "token=";
    if (!token_line.starts_with(token_prefix) ||
        token_line.size() != token_prefix.size() + 32u + 1u ||
        token_line.back() != '\n')
        return std::nullopt;
    const auto token =
        token_line.substr(token_prefix.size(), 32u);
    if (!std::all_of(
            token.begin(),
            token.end(),
            [](const unsigned char character) {
                return std::isdigit(character) != 0 ||
                       (character >= 'a' && character <= 'f');
            }))
        return std::nullopt;
    return std::string(token);
}

ActivePortPublishTransaction active_port_publish_transaction(
    const PortPublishOutputPaths& paths,
    const std::string& token) {
    ActivePortPublishTransaction transaction;
    transaction.output_paths = paths;
    transaction.token = token;
    transaction.owner_document =
        port_publish_owner_document(paths, token);
    transaction.root =
        std::filesystem::path(
            paths.output.string() +
            ".katana-publish-transaction." + token);
    transaction.owner_marker = transaction.root / "owner";
    transaction.state = transaction.root / "state";
    transaction.journal_staging =
        std::filesystem::path(
            paths.journal.string() + ".new." + token);
    transaction.stage = transaction.root / "stage";
    transaction.backup = transaction.root / "backup";
    return transaction;
}

void install_port_publish_journal(
    const ActivePortPublishTransaction& transaction) {
    write_exclusive_safe_port_file(
        transaction.journal_staging,
        transaction.owner_document,
        "Vorbereiteter Port-Publish-Transaktionsmarker");
#ifdef _WIN32
    if (!MoveFileExW(
            transaction.journal_staging.c_str(),
            transaction.output_paths.journal.c_str(),
            MOVEFILE_WRITE_THROUGH)) {
        try {
            remove_owned_safe_port_file(
                transaction.journal_staging,
                transaction.owner_document,
                "Vorbereiteter Port-Publish-Transaktionsmarker");
        } catch (...) {
        }
        throw std::runtime_error(
            "Port-Publish-Transaktionsmarker konnte nicht atomar "
            "installiert werden.");
    }
#else
    if (::link(
            transaction.journal_staging.c_str(),
            transaction.output_paths.journal.c_str()) != 0) {
        try {
            remove_owned_safe_port_file(
                transaction.journal_staging,
                transaction.owner_document,
                "Vorbereiteter Port-Publish-Transaktionsmarker");
        } catch (...) {
        }
        throw std::runtime_error(
            "Port-Publish-Transaktionsmarker konnte nicht atomar "
            "installiert werden.");
    }
    try {
        remove_owned_safe_port_file(
            transaction.journal_staging,
            transaction.owner_document,
            "Installierter Port-Publish-Transaktionsmarker");
    } catch (...) {
        // Der vollstaendige, atomar verlinkte Journalmarker bleibt
        // autoritativ. Ein zweiter Hardlink ist kein Grund, den aktiven
        // Publishzustand zu verwerfen.
    }
#endif
}

std::string new_port_publish_transaction_token(
    const PortPublishOutputPaths& paths) {
    std::random_device random;
    std::ostringstream seed;
    seed << paths.output_identity << ':'
         << std::chrono::steady_clock::now().time_since_epoch().count()
         << ':' << random() << ':' << random() << ':' << random()
         << ':' << random();
#ifdef _WIN32
    seed << ':' << GetCurrentProcessId();
#else
    seed << ':' << ::getpid();
#endif
    return katana::io::sha256_bytes(seed.str()).substr(0u, 32u);
}

// Keeps the previous committed analysis generation recoverable until the
// session-ledger commit has succeeded. Every path is explicit, in the same
// directory and already protected by the analyze-port sibling lock.
class AnalysisArtifactRollback final {
  public:
    explicit AnalysisArtifactRollback(
        const std::filesystem::path& root)
        : root_(root) {
        PortPublishOutputPaths token_input;
        token_input.output = root_ / "analysis-generation";
        token_input.output_identity = katana::io::sha256_bytes(
            token_input.output.lexically_normal().generic_string());
        token_ = new_port_publish_transaction_token(token_input);
    }

    void prepare(const std::filesystem::path& path,
                 const std::string_view description) {
        require_safe_replaceable_port_file(root_, path, description);
        Entry entry;
        entry.path = path;
        entry.backup = path.parent_path() /
            (path.filename().string() + ".katana-backup." + token_);
        require_safe_replaceable_port_file(
            root_, entry.backup, "Analyseartefakt-Rollbackdatei");
        if (safe_regular_port_file_exists(
                entry.backup, "Analyseartefakt-Rollbackdatei"))
            throw std::runtime_error(
                "Analyseartefakt-Rollbackdatei kollidiert.");
        entry.existed = safe_regular_port_file_exists(path, description);
        entries_.push_back(entry);
        if (!entry.existed) return;
        std::error_code move_error;
        std::filesystem::rename(path, entry.backup, move_error);
        if (move_error) {
            entries_.pop_back();
            throw std::filesystem::filesystem_error(
                std::string(description) +
                    " konnte nicht fuer den Rollback gesichert werden.",
                path, entry.backup, move_error);
        }
    }

    void commit() noexcept {
        committed_ = true;
        for (const auto& entry : entries_) {
            if (!entry.existed) continue;
            std::error_code cleanup_error;
            static_cast<void>(
                std::filesystem::remove(entry.backup, cleanup_error));
        }
    }

    ~AnalysisArtifactRollback() noexcept {
        if (committed_) return;
        for (auto iterator = entries_.rbegin();
             iterator != entries_.rend(); ++iterator) {
            std::error_code status_error;
            const auto status = std::filesystem::symlink_status(
                iterator->path, status_error);
            if (!status_error &&
                std::filesystem::is_regular_file(status) &&
                !unsafe_port_filesystem_link(iterator->path, status)) {
                std::error_code remove_error;
                static_cast<void>(std::filesystem::remove(
                    iterator->path, remove_error));
            }
            if (!iterator->existed) continue;
            std::error_code restore_error;
            std::filesystem::rename(
                iterator->backup, iterator->path, restore_error);
        }
    }

    AnalysisArtifactRollback(const AnalysisArtifactRollback&) = delete;
    AnalysisArtifactRollback& operator=(
        const AnalysisArtifactRollback&) = delete;

  private:
    struct Entry final {
        std::filesystem::path path;
        std::filesystem::path backup;
        bool existed = false;
    };

    std::filesystem::path root_;
    std::string token_;
    std::vector<Entry> entries_;
    bool committed_ = false;
};

void write_atomic_analysis_file(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::span<const std::uint8_t> bytes,
    const std::string_view description) {
    require_safe_replaceable_port_file(root, path, description);
    PortPublishOutputPaths token_input;
    token_input.output = path;
    token_input.output_identity = katana::io::sha256_bytes(
        path.lexically_normal().generic_string());
    const auto token = new_port_publish_transaction_token(token_input);
    const auto temporary = path.parent_path() /
        (path.filename().string() + ".katana-tmp." + token);
    const auto document = std::string_view(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    write_exclusive_safe_port_file(
        temporary, document, "Temporaeres Analyseartefakt");
    try {
#ifdef _WIN32
        if (!MoveFileExW(
                temporary.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error(
                std::string(description) +
                " konnte nicht atomar publiziert werden.");
#else
        std::error_code replace_error;
        std::filesystem::rename(temporary, path, replace_error);
        if (replace_error)
            throw std::runtime_error(
                std::string(description) +
                " konnte nicht atomar publiziert werden.");
#endif
    } catch (...) {
        std::error_code cleanup_error;
        static_cast<void>(
            std::filesystem::remove(temporary, cleanup_error));
        throw;
    }
}

void write_atomic_analysis_file(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::string_view document,
    const std::string_view description) {
    write_atomic_analysis_file(
        root,
        path,
        std::span(
            reinterpret_cast<const std::uint8_t*>(document.data()),
            document.size()),
        description);
}

void write_port_publish_state(
    const ActivePortPublishTransaction& transaction,
    const std::string_view state) {
    static constexpr std::array<std::string_view, 5u> allowed{
        "prepared",
        "old-moved",
        "new-published",
        "user-data-preserved",
        "committed"};
    if (std::find(allowed.begin(), allowed.end(), state) == allowed.end())
        throw std::logic_error(
            "Port-Publish-Transaktionszustand ist ungueltig.");
    require_safe_replaceable_port_file(
        transaction.root,
        transaction.state,
        "Port-Publish-Transaktionszustand");
    std::ofstream output(
        transaction.state,
        std::ios::binary | std::ios::trunc);
    output << state << '\n' << std::flush;
    if (!output)
        throw std::runtime_error(
            "Port-Publish-Transaktionszustand konnte nicht geschrieben "
            "werden.");
}

void validate_owned_port_publish_transaction_tree(
    const ActivePortPublishTransaction& transaction) {
    require_safe_port_tree(
        transaction.root,
        "Port-Publish-Transaktionsbaum");
    bool owner_seen = false;
    bool owner_complete = false;
    bool transaction_payload_seen = false;
    for (const auto& entry :
         std::filesystem::directory_iterator(transaction.root)) {
        const auto name = entry.path().filename().generic_string();
        if (name == "owner") {
            owner_seen = true;
            const auto owner =
                read_safe_small_port_file(
                    entry.path(),
                    1024u,
                    "Port-Publish-Transaktionseigentuemer");
            owner_complete =
                owner == transaction.owner_document;
            if (!owner_complete &&
                !transaction.owner_document.starts_with(owner))
                throw std::runtime_error(
                    "Port-Publish-Transaktionsbaum besitzt keinen "
                    "passenden Eigentuemermarker und bleibt "
                    "unangetastet.");
            continue;
        }
        if (name == "state") {
            transaction_payload_seen = true;
            static_cast<void>(
                read_safe_small_port_file(
                    entry.path(),
                    128u,
                    "Port-Publish-Transaktionszustand"));
            continue;
        }
        if (name == "stage" || name == "backup") {
            transaction_payload_seen = true;
            static_cast<void>(
                safe_regular_port_directory_exists(
                    entry.path(),
                    "Port-Publish-Transaktionsunterordner"));
            continue;
        }
        throw std::runtime_error(
            "Port-Publish-Transaktionsbaum enthaelt einen fremden "
            "Eintrag und bleibt unangetastet.");
    }
    if ((!owner_seen || !owner_complete) &&
        transaction_payload_seen)
        throw std::runtime_error(
            "Port-Publish-Transaktionsbaum besitzt keinen "
            "vollstaendigen Eigentuemermarker und bleibt "
            "unangetastet.");
}

std::optional<ActivePortPublishTransaction>
load_owned_port_publish_transaction(
    const PortPublishOutputPaths& paths) {
    if (!safe_regular_port_file_exists(
            paths.journal,
            "Port-Publish-Transaktionsmarker"))
        return std::nullopt;
    const auto journal =
        read_safe_small_port_file(
            paths.journal,
            1024u,
            "Port-Publish-Transaktionsmarker");
    const auto token =
        parse_port_publish_journal_token(journal, paths);
    if (!token)
        throw std::runtime_error(
            "Port-Publish-Transaktionsmarker ist fremd oder ungueltig "
            "und bleibt unangetastet.");
    auto transaction =
        active_port_publish_transaction(paths, *token);
    if (!safe_regular_port_directory_exists(
            transaction.root,
            "Port-Publish-Transaktionsbaum")) {
        remove_owned_safe_port_file(
            paths.journal,
            transaction.owner_document,
            "Verwaister Port-Publish-Transaktionsmarker");
        return std::nullopt;
    }
    validate_owned_port_publish_transaction_tree(transaction);
    return transaction;
}

bool output_has_owned_port_publish_marker(
    const ActivePortPublishTransaction& transaction) {
    const auto marker =
        transaction.output_paths.output / ".katana-publish-owner";
    if (!safe_regular_port_file_exists(
            marker,
            "Publizierter Port-Transaktionsmarker"))
        return false;
    const auto document =
        read_safe_small_port_file(
            marker,
            1024u,
            "Publizierter Port-Transaktionsmarker");
    if (document != transaction.owner_document)
        throw std::runtime_error(
            "Bestehendes Portpaket besitzt einen fremden "
            "Transaktionsmarker und bleibt unangetastet.");
    return true;
}

void remove_owned_output_publish_marker(
    const ActivePortPublishTransaction& transaction) {
    remove_owned_safe_port_file(
        transaction.output_paths.output / ".katana-publish-owner",
        transaction.owner_document,
        "Publizierter Port-Transaktionsmarker");
}

void cleanup_owned_port_publish_transaction(
    const ActivePortPublishTransaction& transaction) {
    validate_owned_port_publish_transaction_tree(transaction);
    if (safe_regular_port_directory_exists(
            transaction.stage,
            "Port-Publish-Staging") ||
        safe_regular_port_directory_exists(
            transaction.backup,
            "Port-Publish-Backup"))
        throw std::runtime_error(
            "Port-Publish-Transaktion ist vor der Bereinigung nicht leer.");
    remove_safe_port_tree(
        transaction.root,
        "Abgeschlossene Port-Publish-Transaktion");
    remove_owned_safe_port_file(
        transaction.output_paths.journal,
        transaction.owner_document,
        "Abgeschlossener Port-Publish-Transaktionsmarker");
}

void recover_port_publish_transaction(
    const PortPublishOutputPaths& paths) {
    auto loaded = load_owned_port_publish_transaction(paths);
    if (!loaded) return;
    auto& transaction = *loaded;
    const auto output_exists =
        safe_regular_port_directory_exists(
            paths.output,
            "Port-Publish-Recovery-Ausgabe");
    auto backup_exists =
        safe_regular_port_directory_exists(
            transaction.backup,
            "Port-Publish-Recovery-Backup");
    const auto output_is_owned =
        output_exists &&
        output_has_owned_port_publish_marker(transaction);

    if (backup_exists && !output_exists) {
        std::filesystem::rename(
            transaction.backup,
            paths.output);
        backup_exists = false;
    } else if (backup_exists && output_is_owned) {
        katana::codegen::preserve_local_port_user_data(
            transaction.backup,
            paths.output);
        write_port_publish_state(
            transaction,
            "user-data-preserved");
        remove_safe_port_tree(
            transaction.backup,
            "Gerettetes Port-Publish-Backup");
        backup_exists = false;
    } else if (backup_exists) {
        throw std::runtime_error(
            "Port-Publish-Recovery fand neben dem gesicherten Altport "
            "einen fremden Ausgabeordner; beide bleiben unangetastet.");
    }

    if (safe_regular_port_directory_exists(
            transaction.stage,
            "Verwaistes Port-Publish-Staging"))
        remove_safe_port_tree(
            transaction.stage,
            "Verwaistes Port-Publish-Staging");

    if (output_is_owned) {
        write_port_publish_state(transaction, "committed");
        remove_owned_output_publish_marker(transaction);
    }
    if (backup_exists)
        throw std::logic_error(
            "Port-Publish-Recovery verlor den Backupzustand.");
    cleanup_owned_port_publish_transaction(transaction);
}

ActivePortPublishTransaction begin_port_publish_transaction(
    const PortPublishOutputPaths& paths) {
    if (safe_regular_port_file_exists(
            paths.journal,
            "Port-Publish-Transaktionsmarker"))
        throw std::runtime_error(
            "Port-Publish-Transaktionsmarker ist bereits vorhanden.");
    const auto token = new_port_publish_transaction_token(paths);
    auto transaction =
        active_port_publish_transaction(paths, token);
    std::error_code status_error;
    const auto root_status =
        std::filesystem::symlink_status(
            transaction.root,
            status_error);
    if (status_error != std::errc::no_such_file_or_directory &&
        (status_error ||
         root_status.type() !=
             std::filesystem::file_type::not_found))
        throw std::runtime_error(
            "Neuer Port-Publish-Transaktionsbaum kollidiert mit einem "
            "fremden Dateisystemeintrag.");
    install_port_publish_journal(transaction);
    std::error_code create_error;
    if (!std::filesystem::create_directory(
            transaction.root,
            create_error) ||
        create_error)
        throw std::filesystem::filesystem_error(
            "Port-Publish-Transaktionsbaum konnte nicht erstellt werden.",
            transaction.root,
            create_error);
    if (!safe_regular_port_directory_exists(
            transaction.root,
            "Port-Publish-Transaktionsbaum"))
        throw std::runtime_error(
            "Port-Publish-Transaktionsbaum wurde nicht sicher erstellt.");
    write_exclusive_safe_port_file(
        transaction.owner_marker,
        transaction.owner_document,
        "Port-Publish-Transaktionseigentuemer");
    write_port_publish_state(transaction, "prepared");
    return transaction;
}

void maybe_hold_port_publish_lock_for_test() {
    const auto configured_duration =
        configured_environment_value(
            "KATANA_PORT_PUBLISH_TEST_HOLD_LOCK_MS");
    const auto configured_release_file =
        configured_environment_value(
            "KATANA_PORT_PUBLISH_TEST_RELEASE_FILE");
    if (!configured_duration && !configured_release_file) return;
    if (configured_duration && configured_release_file)
        throw std::invalid_argument(
            "Port-Publish-Lock-Test darf nur einen Freigabemodus "
            "verwenden.");
    std::cout << "KATANA_PORT_PUBLISH_TEST_OUTPUT_LOCK_HELD\n"
              << std::flush;
    if (configured_release_file) {
        const auto release_file =
            std::filesystem::absolute(*configured_release_file)
                .lexically_normal();
        if (release_file.empty() ||
            release_file == release_file.root_path() ||
            release_file.filename().empty())
            throw std::invalid_argument(
                "KATANA_PORT_PUBLISH_TEST_RELEASE_FILE ist "
                "ungueltig.");
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(120);
        while (std::chrono::steady_clock::now() < deadline) {
            std::error_code status_error;
            const auto status =
                std::filesystem::symlink_status(
                    release_file, status_error);
            const bool missing =
                status_error ==
                    std::errc::no_such_file_or_directory ||
                (!status_error &&
                 status.type() ==
                     std::filesystem::file_type::not_found);
            if (!status_error &&
                std::filesystem::is_regular_file(status) &&
                !unsafe_port_filesystem_link(
                    release_file, status))
                return;
            if (!missing)
                throw std::runtime_error(
                    "Port-Publish-Lock-Testfreigabe ist kein "
                    "sicheres regulaeres Dateiartefakt.");
            std::this_thread::sleep_for(
                std::chrono::milliseconds(10));
        }
        throw std::runtime_error(
            "Port-Publish-Lock-Testfreigabe wurde nicht rechtzeitig "
            "signalisiert.");
    }
    std::size_t parsed = 0u;
    const auto duration =
        std::stoull(*configured_duration, &parsed, 10);
    if (parsed != configured_duration->size() || duration == 0u ||
        duration > 10000u)
        throw std::invalid_argument(
            "KATANA_PORT_PUBLISH_TEST_HOLD_LOCK_MS ist ungueltig.");
    std::this_thread::sleep_for(
        std::chrono::milliseconds(duration));
}

bool exit_after_port_publish_recovery_for_test() {
    const auto configured =
        configured_environment_value(
            "KATANA_PORT_PUBLISH_TEST_EXIT_AFTER_RECOVERY");
    if (!configured) return false;
    if (*configured != "1")
        throw std::invalid_argument(
            "KATANA_PORT_PUBLISH_TEST_EXIT_AFTER_RECOVERY ist "
            "ungueltig.");
    std::cout << "KATANA_PORT_PUBLISH_TEST_RECOVERY_COMPLETE\n"
              << std::flush;
    return true;
}

void maybe_crash_port_publish_for_test(
    const std::string_view point) {
    const auto configured =
        configured_environment_value(
            "KATANA_PORT_PUBLISH_TEST_CRASH_POINT");
    if (!configured || *configured != point) return;
    std::cerr << "KATANA_PORT_PUBLISH_TEST_CRASH point="
              << point << '\n' << std::flush;
    std::cout.flush();
    std::_Exit(86);
}

bool remove_new_failed_port_host_build_state(
    const std::filesystem::path& port_root,
    const std::filesystem::path& build_root,
    const bool reusable_state_existed_before_configure) {
    const auto normalized_port = std::filesystem::absolute(port_root).lexically_normal();
    const auto normalized_build = std::filesystem::absolute(build_root).lexically_normal();
    const auto build_name = normalized_build.filename().generic_string();
    if (normalized_build.parent_path() != normalized_port || build_name.size() <= 6u ||
        !build_name.starts_with("build-"))
        throw std::runtime_error(
            "Fehlgeschlagener Hostbuild besitzt keinen sicher abgeleiteten Buildpfad.");
    if (!safe_regular_port_directory_exists(
            normalized_build,
            "Fehlgeschlagener CMake-Configure-Zustand"))
        return false;
    std::error_code canonical_error;
    const auto resolved_port = std::filesystem::canonical(normalized_port, canonical_error);
    if (canonical_error)
        throw std::runtime_error(
            "Portwurzel fuer Configure-Bereinigung konnte nicht aufgeloest werden.");
    const auto resolved_build = std::filesystem::canonical(normalized_build, canonical_error);
    if (canonical_error || resolved_build.parent_path() != resolved_port)
        throw std::runtime_error(
            "Configure-Bereinigung wuerde den sicheren Portbuildpfad verlassen.");
    // CMake and Ninja keep the expensive object graph below this directory.
    // A failed reconfigure has not produced a runnable product and the next
    // successful configure deterministically regenerates its control files;
    // deleting a previously valid tree here merely turns a cheap correction
    // into a full cold build.  Only discard a directory first created by this
    // failed invocation, where no reusable object state can exist.
    if (reusable_state_existed_before_configure) return false;
    remove_safe_port_tree(
        normalized_build,
        "Fehlgeschlagener neuer CMake-Configure-Zustand");
    return true;
}

#ifdef _WIN32
void require_optimized_msvc_configuration(
    const std::filesystem::path& build_root,
    const std::string_view configuration) {
    std::ifstream cache(build_root / "CMakeCache.txt", std::ios::binary);
    if (!cache)
        throw std::runtime_error(
            "CMakeCache fehlt nach erfolgreicher Hostbuild-Konfiguration.");
    auto configuration_upper = std::string(configuration);
    std::transform(configuration_upper.begin(),
                   configuration_upper.end(),
                   configuration_upper.begin(),
                   [](const unsigned char value) {
                       return static_cast<char>(std::toupper(value));
                   });
    const auto entry = "CMAKE_CXX_FLAGS_" + configuration_upper + ':';
    std::string flags;
    std::string line;
    while (std::getline(cache, line)) {
        if (!line.starts_with(entry)) continue;
        const auto assignment = line.find('=');
        if (assignment != std::string::npos) flags = line.substr(assignment + 1u);
        break;
    }
    std::transform(flags.begin(), flags.end(), flags.begin(), [](const unsigned char value) {
        return static_cast<char>(std::toupper(value));
    });
    const auto contains = [&flags](const std::string_view option) {
        return flags.find(option) != std::string::npos;
    };
    const bool disabled = contains("/OD") || contains("-O0");
    const bool enabled = contains("/O1") || contains("/O2") || contains("/OX") ||
                         contains("-O1") || contains("-O2") || contains("-O3") ||
                         contains("-OFAST");
    if (flags.empty() || disabled || !enabled)
        throw std::runtime_error(
            "MSVC-" + std::string(configuration) +
            "-Configure besitzt keine wirksame Optimierung.");
}
#endif

bool path_is_within(const std::filesystem::path& path,
                    const std::filesystem::path& root) {
    const auto normalized_path = path.lexically_normal();
    const auto normalized_root = root.lexically_normal();
    if (normalized_path.empty() || normalized_root.empty())
        return false;
    auto path_component = normalized_path.begin();
    auto root_component = normalized_root.begin();
    for (; root_component != normalized_root.end();
         ++root_component, ++path_component) {
        if (path_component == normalized_path.end())
            return false;
#ifdef _WIN32
        const auto left = path_component->native();
        const auto right = root_component->native();
        if (CompareStringOrdinal(left.c_str(),
                                 static_cast<int>(left.size()),
                                 right.c_str(),
                                 static_cast<int>(right.size()),
                                 TRUE) != CSTR_EQUAL)
            return false;
#else
        if (*path_component != *root_component)
            return false;
#endif
    }
    return true;
}

bool port_paths_alias(const std::filesystem::path& left,
                      const std::filesystem::path& right) {
    if (left.empty() || right.empty()) return false;
    const auto normalized_left =
        std::filesystem::absolute(left).lexically_normal();
    const auto normalized_right =
        std::filesystem::absolute(right).lexically_normal();
    if (path_is_within(normalized_left, normalized_right) &&
        path_is_within(normalized_right, normalized_left))
        return true;

    std::error_code equivalent_error;
    const auto equivalent =
        std::filesystem::equivalent(normalized_left,
                                    normalized_right,
                                    equivalent_error);
    return !equivalent_error && equivalent;
}

void require_telemetry_path_disjoint_from_file(
    const std::filesystem::path& telemetry_path,
    const std::filesystem::path& protected_path,
    const std::string_view description) {
    if (!port_paths_alias(telemetry_path, protected_path)) return;
    throw std::invalid_argument(
        "--telemetry-jsonl darf " + std::string(description) +
        " weder direkt noch ueber einen Hardlink ueberschreiben.");
}

void require_telemetry_path_outside_tree(
    const std::filesystem::path& telemetry_path,
    const std::filesystem::path& protected_root,
    const std::string_view description) {
    if (protected_root.empty() ||
        !path_is_within(
            std::filesystem::absolute(telemetry_path).lexically_normal(),
            std::filesystem::absolute(protected_root).lexically_normal()))
        return;
    throw std::invalid_argument(
        "--telemetry-jsonl muss ausserhalb von " +
        std::string(description) + " liegen.");
}

void require_native_port_definition_path_disjoint_from_path(
    const std::filesystem::path& native_port_definition_path,
    const std::filesystem::path& protected_path,
    const std::string_view description) {
    if (native_port_definition_path.empty() || protected_path.empty()) return;
    const auto native_path =
        std::filesystem::absolute(native_port_definition_path).lexically_normal();
    const auto protected_absolute =
        std::filesystem::absolute(protected_path).lexically_normal();
    if (!port_paths_alias(native_path, protected_absolute) &&
        !path_is_within(native_path, protected_absolute) &&
        !path_is_within(protected_absolute, native_path))
        return;
    throw std::invalid_argument(
        "--native-port-definition darf " + std::string(description) +
        " weder direkt noch ueber einen Hardlink aliasieren.");
}

inline constexpr std::uintmax_t maximum_port_export_cache_state_bytes =
    4u * 1024u;
inline constexpr std::string_view generated_artifact_manifest_name =
    ".katana-generated-artifacts";
inline constexpr std::string_view generated_artifact_manifest_header =
    "katana-codegen-artifacts-v1";

struct CachedPortExport {
    std::string key;
    std::string source_kind;
    std::string tree_identity;
    std::string recipe_identity;
    std::size_t functions = 0u;
    std::size_t partitions = 0u;
};

bool valid_cache_digest(const std::string_view value) noexcept {
    return value.size() == 64u &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isdigit(character) != 0 ||
                      (character >= 'a' && character <= 'f');
           });
}

bool valid_generated_artifact_relative_path(
    const std::filesystem::path& relative,
    const std::string_view source_text) {
    const auto normalized = relative.lexically_normal();
    if (normalized.empty() || normalized.is_absolute() ||
        normalized.generic_string() != source_text ||
        normalized.generic_string() == generated_artifact_manifest_name)
        return false;
    return std::none_of(
        normalized.begin(), normalized.end(), [](const auto& component) {
            return component == "." || component == "..";
        });
}

std::optional<std::vector<std::filesystem::path>>
read_generated_artifact_manifest(const std::filesystem::path& generated_root) {
    const auto manifest = generated_root / generated_artifact_manifest_name;
    std::error_code status_error;
    const auto status =
        std::filesystem::symlink_status(manifest, status_error);
    if (status_error || !std::filesystem::is_regular_file(status) ||
        unsafe_port_filesystem_link(manifest, status))
        return std::nullopt;
    std::ifstream input(manifest, std::ios::binary);
    std::string line;
    if (!input || !std::getline(input, line) ||
        line != generated_artifact_manifest_header)
        return std::nullopt;
    std::vector<std::filesystem::path> listed;
    std::string previous;
    while (std::getline(input, line)) {
        if (line.empty() ||
            !valid_generated_artifact_relative_path(
                std::filesystem::path(line), line) ||
            (!previous.empty() && previous >= line))
            return std::nullopt;
        previous = line;
        listed.emplace_back(line);
    }
    if (!input.eof()) return std::nullopt;
    return listed;
}

std::optional<std::vector<std::filesystem::path>>
collect_generated_artifact_files(const std::filesystem::path& generated_root) {
    if (!safe_regular_port_directory_exists(
            generated_root,
            "Content-addressed Portcodegen-Verzeichnis"))
        return std::nullopt;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator iterator(generated_root), end;
         iterator != end;
         ++iterator) {
        std::error_code status_error;
        const auto status = iterator->symlink_status(status_error);
        if (status_error ||
            unsafe_port_filesystem_link(iterator->path(), status))
            return std::nullopt;
        if (std::filesystem::is_directory(status)) continue;
        if (!std::filesystem::is_regular_file(status))
            return std::nullopt;
        const auto relative = iterator->path().lexically_relative(generated_root);
        if (relative.empty() || relative.is_absolute() ||
            *relative.begin() == "..")
            return std::nullopt;
        files.push_back(relative.lexically_normal());
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.generic_string() < right.generic_string();
    });
    return files;
}

std::optional<std::vector<std::filesystem::path>>
validated_generated_artifact_files(const std::filesystem::path& root) {
    const auto generated_root = root / "generated";
    const auto listed = read_generated_artifact_manifest(generated_root);
    const auto actual = collect_generated_artifact_files(generated_root);
    if (!listed || !actual) return std::nullopt;
    auto expected = *listed;
    expected.emplace_back(generated_artifact_manifest_name);
    std::sort(
        expected.begin(), expected.end(), [](const auto& left, const auto& right) {
            return left.generic_string() < right.generic_string();
        });
    if (expected != *actual) return std::nullopt;
    std::vector<std::filesystem::path> absolute_files;
    absolute_files.reserve(actual->size());
    for (const auto& relative : *actual)
        absolute_files.push_back(generated_root / relative);
    return absolute_files;
}

std::optional<std::vector<std::filesystem::path>>
validated_generated_source_files(const std::filesystem::path& root) {
    const auto source_root = root / "src";
    const auto actual = collect_generated_artifact_files(source_root);
    if (!actual) return std::nullopt;
    const std::vector<std::filesystem::path> expected{"main.cpp"};
    if (*actual != expected) return std::nullopt;
    return std::vector<std::filesystem::path>{source_root / expected.front()};
}

std::optional<std::uint64_t>
validated_native_host_translation_unit_supplement(
    const std::filesystem::path& build_root) {
    const auto plan = build_root / ".katana-native-host-build-plan";
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(plan, status_error);
    if (status_error || !std::filesystem::is_regular_file(status) ||
        unsafe_port_filesystem_link(plan, status))
        return std::nullopt;
    std::error_code size_error;
    const auto size = std::filesystem::file_size(plan, size_error);
    if (size_error || size == 0u || size > 256u) return std::nullopt;
    std::ifstream input(plan, std::ios::binary);
    std::string header;
    std::string value;
    std::string trailing;
    if (!input ||
        !std::getline(input, header) ||
        !std::getline(input, value) ||
        std::getline(input, trailing) ||
        !input.eof())
        return std::nullopt;
    const auto strip_windows_carriage_return = [](std::string& line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
    };
    strip_windows_carriage_return(header);
    strip_windows_carriage_return(value);
    if (header != "katana-native-host-build-plan-v1")
        return std::nullopt;
    constexpr std::string_view prefix =
        "translation_unit_supplement=";
    if (!value.starts_with(prefix)) return std::nullopt;
    const auto digits = std::string_view(value).substr(prefix.size());
    std::uint64_t count = 0u;
    const auto conversion = std::from_chars(
        digits.data(), digits.data() + digits.size(), count, 10);
    if (digits.empty() || conversion.ec != std::errc{} ||
        conversion.ptr != digits.data() + digits.size() ||
        count == 0u || count > 65'536u)
        return std::nullopt;
    return count;
}

std::string read_port_distribution_text(
    const std::filesystem::path& path,
    const std::size_t maximum_size = 1024u * 1024u) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (size_error || size > maximum_size)
        throw std::runtime_error(
            "Portdistributionsmanifest ist unlesbar oder zu gross.");
    std::ifstream input(path, std::ios::binary);
    std::string content(static_cast<std::size_t>(size), '\0');
    if (!input)
        throw std::runtime_error(
            "Portdistributionsmanifest konnte nicht geoeffnet werden.");
    if (!content.empty())
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!input || input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error(
            "Portdistributionsmanifest konnte nicht vollstaendig gelesen werden.");
    return content;
}

std::string disc_install_manifest_document(
    const katana::runtime::DiscInstallRecipe& recipe,
    const std::string_view recipe_sha256,
    const std::string_view executable_name,
    const std::string_view executable_sha256) {
    return "{\"schema\":\"katana-disc-install\",\"version\":1,"
           "\"job_generation\":\"" +
           recipe.job_generation + "\",\"content_identity\":\"" +
           recipe.content_identity +
           "\",\"artifacts\":[{\"role\":\"disc_install_recipe\",\"path\":"
           "\"game.katana-install\",\"sha256\":\"" +
           std::string(recipe_sha256) +
           "\"},{\"role\":\"host_executable\",\"path\":\"../" +
           std::string(executable_name) + "\",\"sha256\":\"" +
           std::string(executable_sha256) + "\"}]}\n";
}

struct PortRuntimeDependency final {
    std::filesystem::path relative_path;
    std::uint64_t size = 0u;
    std::string sha256;

    friend bool operator==(const PortRuntimeDependency&,
                           const PortRuntimeDependency&) = default;
};

struct PortRuntimeDependencySnapshot final {
    std::vector<PortRuntimeDependency> files;
    bool redistribution_ready = true;
};

PortRuntimeDependencySnapshot capture_port_runtime_dependencies(
    const std::filesystem::path& directory) {
    PortRuntimeDependencySnapshot snapshot;
#ifdef _WIN32
    constexpr std::array<std::string_view, 8u> required_names{
        "avformat-62.dll",
        "avcodec-62.dll",
        "avutil-60.dll",
        "swresample-6.dll",
        "swscale-9.dll",
        "FFmpeg-LGPL.txt",
        "FFmpeg-NOTICE.txt",
        "FFmpeg-BUILD-CONFIGURATION.txt"};
    constexpr std::string_view source_name =
        "FFmpeg-Corresponding-Source.zip";
    constexpr std::string_view development_name =
        "FFmpeg-DEVELOPMENT-ONLY.txt";

    const auto capture = [&](const std::string_view name) {
        const auto path = directory / std::string(name);
        std::error_code status_error;
        const auto status =
            std::filesystem::symlink_status(path, status_error);
        if (status_error || !std::filesystem::is_regular_file(status) ||
            unsafe_port_filesystem_link(path, status))
            throw std::runtime_error(
                "Native Runtimeabhaengigkeit fehlt oder ist unsicher: " +
                std::string(name));
        const auto provenance =
            katana::io::capture_input_provenance(
                "native-runtime-dependency", path);
        if (provenance.size == 0u || provenance.sha256.size() != 64u)
            throw std::runtime_error(
                "Native Runtimeabhaengigkeit besitzt keine sichere Identitaet: " +
                std::string(name));
        snapshot.files.push_back(
            {std::filesystem::path(name),
             provenance.size,
             provenance.sha256});
    };

    for (const auto name : required_names) capture(name);
    const auto safe_optional = [&](const std::string_view name) {
        const auto path = directory / std::string(name);
        std::error_code status_error;
        const auto status =
            std::filesystem::symlink_status(path, status_error);
        if (status_error == std::errc::no_such_file_or_directory ||
            (!status_error &&
             status.type() == std::filesystem::file_type::not_found))
            return false;
        if (status_error || !std::filesystem::is_regular_file(status) ||
            unsafe_port_filesystem_link(path, status))
            throw std::runtime_error(
                "Optionale Runtimeabhaengigkeit ist unsicher: " +
                std::string(name));
        return true;
    };
    const auto has_source = safe_optional(source_name);
    const auto has_development_marker = safe_optional(development_name);
    if (has_source == has_development_marker)
        throw std::runtime_error(
            "FFmpeg-Runtime braucht exakt Quellbundle oder Development-Marker.");
    snapshot.redistribution_ready = has_source;
    capture(has_source ? source_name : development_name);
#else
    static_cast<void>(directory);
#endif
    std::sort(
        snapshot.files.begin(), snapshot.files.end(),
        [](const auto& left, const auto& right) {
            return left.relative_path.generic_string() <
                   right.relative_path.generic_string();
        });
    return snapshot;
}

PortRuntimeDependencySnapshot publish_port_runtime_dependencies(
    const std::filesystem::path& source_directory,
    const std::filesystem::path& output_root) {
    const auto source =
        capture_port_runtime_dependencies(source_directory);
#ifdef _WIN32
    constexpr std::array<std::string_view, 2u> alternatives{
        "FFmpeg-Corresponding-Source.zip",
        "FFmpeg-DEVELOPMENT-ONLY.txt"};
    for (const auto name : alternatives) {
        const auto target = output_root / std::string(name);
        require_safe_replaceable_port_file(
            output_root, target, "Veraltete Runtimeabhaengigkeit");
        if (std::none_of(
                source.files.begin(), source.files.end(),
                [&](const auto& file) {
                    return file.relative_path == name;
                })) {
            std::error_code remove_error;
            std::filesystem::remove(target, remove_error);
            if (remove_error &&
                remove_error != std::errc::no_such_file_or_directory)
                throw std::filesystem::filesystem_error(
                    "Veraltete Runtimeabhaengigkeit konnte nicht entfernt werden.",
                    target,
                    remove_error);
        }
    }
    for (const auto& dependency : source.files) {
        const auto source_path =
            source_directory / dependency.relative_path;
        const auto target = output_root / dependency.relative_path;
        require_safe_replaceable_port_file(
            output_root, target, "Publizierte Runtimeabhaengigkeit");
        std::filesystem::copy_file(
            source_path,
            target,
            std::filesystem::copy_options::overwrite_existing);
    }
#else
    static_cast<void>(output_root);
#endif
    const auto published =
        capture_port_runtime_dependencies(output_root);
    if (published.files != source.files ||
        published.redistribution_ready != source.redistribution_ready)
        throw std::runtime_error(
            "Publizierte Runtimeabhaengigkeiten sind nicht byteidentisch.");
    return published;
}

std::string runtime_dependency_manifest_document(
    const katana::runtime::DiscInstallRecipe& recipe,
    const std::string_view runtime_profile,
    const PortRuntimeDependencySnapshot& dependencies) {
    std::ostringstream document;
    document << "{\"schema\":\"katana-runtime-dependencies\",\"version\":3,"
                "\"linkage\":\"static-aot-plus-shared-native-media\","
                "\"runtime_profile\":"
             << katana::io::quote_json(runtime_profile)
             << ",\"job_generation\":"
             << katana::io::quote_json(recipe.job_generation)
             << ",\"redistribution_ready\":"
             << (dependencies.redistribution_ready ? "true" : "false")
             << ",\"files\":[";
    for (std::size_t index = 0u; index < dependencies.files.size(); ++index) {
        if (index != 0u) document << ',';
        const auto& file = dependencies.files[index];
        document << "{\"path\":"
                 << katana::io::quote_json(
                        file.relative_path.generic_string())
                 << ",\"size\":" << file.size
                 << ",\"sha256\":"
                 << katana::io::quote_json(file.sha256) << '}';
    }
    document << "]}\n";
    return document.str();
}

std::string declared_port_distribution_target_name(
    const std::filesystem::path& source) {
    const auto metadata =
        source / "generated" / "metadata" /
        "port-project.json";
    if (!safe_regular_port_file_exists(
            metadata,
            "Portprojekt-Metadaten"))
        throw std::runtime_error(
            "Bestehende Portdistribution besitzt keine "
            "targetgebundenen Metadaten.");
    const auto document =
        read_port_distribution_text(metadata);
    constexpr std::string_view field{"\"target_name\":"};
    const auto field_position = document.find(field);
    if (field_position == std::string::npos ||
        document.find(field, field_position + field.size()) !=
            std::string::npos)
        throw std::runtime_error(
            "Bestehende Portdistribution besitzt keine eindeutige "
            "Targetbindung.");
    const auto value_begin = field_position + field.size();
    if (value_begin >= document.size() ||
        document[value_begin] != '"')
        throw std::runtime_error(
            "Bestehende Portdistribution besitzt eine ungueltige "
            "Targetbindung.");
    const auto value_end =
        document.find('"', value_begin + 1u);
    if (value_end == std::string::npos)
        throw std::runtime_error(
            "Bestehende Portdistribution besitzt eine unvollstaendige "
            "Targetbindung.");
    const auto target_name =
        document.substr(
            value_begin + 1u,
            value_end - value_begin - 1u);
    if (!valid_port_target_name(target_name))
        throw std::runtime_error(
            "Bestehende Portdistribution besitzt keinen sicheren "
            "Targetnamen.");
    return target_name;
}

bool private_port_workspace_path(const std::filesystem::path& relative) {
    if (relative.empty()) return false;
    const auto top_level = relative.begin()->generic_string();
    return top_level == "user-data" ||
           top_level == ".katana-codegen-cache" ||
           top_level == "build" || top_level.starts_with("build-");
}

void copy_validated_port_distribution(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::string_view target_name,
    const katana::runtime::DiscInstallRecipe& expected_recipe,
    const std::string_view expected_runtime_profile,
    const bool copy_files = true) {
    if (!safe_regular_port_directory_exists(source, "Portquelle")) return;
    if (copy_files &&
        safe_regular_port_directory_exists(destination, "Portkopieziel"))
        throw std::runtime_error("Portkopieziel existiert bereits.");

    std::vector<std::filesystem::path> allowed;
    constexpr std::array<std::string_view, 7u> required_files{
        "CMakeLists.txt",
        ".gitignore",
        "INSTALL_ORIGINAL_DISC.txt",
        "run-product-gate.ps1",
        "content/game.katana-install",
        "content/game.katana-install.json",
        "runtime/runtime-dependencies.json"};
    for (const auto relative : required_files) {
        allowed.emplace_back(relative);
    }
    auto executable_relative = std::filesystem::path(target_name);
#ifdef _WIN32
    executable_relative += ".exe";
#endif
    allowed.push_back(executable_relative);

    const auto generated_files = validated_generated_artifact_files(source);
    const auto generated_sources = validated_generated_source_files(source);
    if (!generated_files || !generated_sources)
        throw std::runtime_error(
            "Portquelle besitzt kein exaktes generiertes Artefaktinventar.");
    for (const auto& path : *generated_files)
        allowed.push_back(path.lexically_relative(source));
    for (const auto& path : *generated_sources)
        allowed.push_back(path.lexically_relative(source));
    const auto runtime_dependencies =
        capture_port_runtime_dependencies(source);
    for (const auto& dependency : runtime_dependencies.files)
        allowed.push_back(dependency.relative_path);
    std::sort(allowed.begin(), allowed.end(), [](const auto& left, const auto& right) {
        return left.generic_string() < right.generic_string();
    });
    if (std::adjacent_find(allowed.begin(), allowed.end()) != allowed.end())
        throw std::logic_error(
            "Portdistributions-Allowlist enthaelt doppelte Pfade.");

    for (const auto& relative : allowed) {
        const auto path = source / relative;
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(path, status_error);
        if (status_error || !std::filesystem::is_regular_file(status) ||
            unsafe_port_filesystem_link(path, status))
            throw std::runtime_error(
                "Portdistributionsdatei fehlt oder ist nicht regulaer: " +
                relative.generic_string());
    }

    const auto recipe_path = source / "content" / "game.katana-install";
    const auto actual_recipe =
        katana::runtime::parse_disc_install_recipe(recipe_path);
    if (katana::runtime::format_disc_install_recipe(actual_recipe) !=
        katana::runtime::format_disc_install_recipe(expected_recipe))
        throw std::runtime_error(
            "Portdistributions-Recipe besitzt eine falsche Identitaet.");
    const auto recipe_sha256 =
        katana::io::capture_input_provenance(
            "disc-install-recipe", recipe_path)
            .sha256;
    const auto executable_path = source / executable_relative;
    const auto executable_sha256 =
        katana::io::capture_input_provenance(
            "host-executable", executable_path)
            .sha256;
    const auto expected_install_manifest =
        disc_install_manifest_document(
            expected_recipe,
            recipe_sha256,
            executable_relative.generic_string(),
            executable_sha256);
    if (read_port_distribution_text(
            source / "content" / "game.katana-install.json") !=
        expected_install_manifest)
        throw std::runtime_error(
            "Portdistributions-Installationsmanifest ist nicht exakt "
            "an Recipe und Hostprogramm gebunden.");
    if (read_port_distribution_text(
            source / "runtime" / "runtime-dependencies.json") !=
        runtime_dependency_manifest_document(
            expected_recipe,
            expected_runtime_profile,
            runtime_dependencies))
        throw std::runtime_error(
            "Portdistributions-Runtimevertrag ist nicht die sichere "
            "statische Dateiliste.");

    for (std::filesystem::recursive_directory_iterator iterator(source), end;
         iterator != end;
         ++iterator) {
        const auto relative =
            iterator->path().lexically_relative(source).lexically_normal();
        std::error_code status_error;
        const auto status = iterator->symlink_status(status_error);
        if (status_error ||
            unsafe_port_filesystem_link(iterator->path(), status))
            throw std::runtime_error(
                "Portquelle enthaelt einen unsicheren Dateisystemeintrag.");
        if (private_port_workspace_path(relative)) {
            if (std::filesystem::is_directory(status))
                iterator.disable_recursion_pending();
            continue;
        }
        if (std::filesystem::is_directory(status)) continue;
        if (!std::filesystem::is_regular_file(status))
            throw std::runtime_error(
                "Portquelle enthaelt einen nicht regulaeren Eintrag.");
        if (!std::binary_search(
                allowed.begin(),
                allowed.end(),
                relative,
                [](const auto& left, const auto& right) {
                    return left.generic_string() < right.generic_string();
                }))
            throw std::runtime_error(
                "Portquelle enthaelt eine nicht distributionsgebundene Datei: " +
                relative.generic_string());
    }

    if (!copy_files) return;
    std::filesystem::create_directories(destination);
    if (!safe_regular_port_directory_exists(
            destination, "Portkopieziel"))
        throw std::runtime_error(
            "Portkopieziel wurde nicht als sicherer Ordner erstellt.");
    for (const auto& relative : allowed) {
        const auto source_path = source / relative;
        const auto target = destination / relative;
        std::error_code status_error;
        const auto status =
            std::filesystem::symlink_status(source_path, status_error);
        if (status_error || !std::filesystem::is_regular_file(status) ||
            unsafe_port_filesystem_link(source_path, status))
            throw std::runtime_error(
                "Portdistributionsdatei wurde waehrend des Publish "
                "ungueltig: " +
                relative.generic_string());
        ensure_safe_port_directory_chain(
            destination,
            target.parent_path(),
            "Portkopieziel");
        require_safe_replaceable_port_file(
            destination, target, "Portkopiezieldatei");
        std::filesystem::copy_file(
            source_path,
            target,
            std::filesystem::copy_options::none);
        std::error_code permission_error;
        std::filesystem::permissions(
            target,
            status.permissions(),
            std::filesystem::perm_options::replace,
            permission_error);
        if (permission_error)
            throw std::runtime_error(
                "Portdateirechte konnten nicht uebernommen werden.");
        std::error_code timestamp_error;
        std::filesystem::last_write_time(
            target,
            std::filesystem::last_write_time(source_path),
            timestamp_error);
        if (timestamp_error)
            throw std::runtime_error(
                "Portdatei-Zeitstempel konnte nicht uebernommen werden.");
    }
}

[[nodiscard]] bool regular_file_contains_token(
    const std::filesystem::path& path,
    const std::string_view token) {
    constexpr std::size_t chunk_bytes = 1024u * 1024u;
    constexpr std::size_t maximum_token_bytes = 512u;
    if (token.empty() || token.size() > maximum_token_bytes)
        return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::vector<char> chunk(chunk_bytes);
    std::string tail;
    tail.reserve(token.size() - 1u);
    while (input) {
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto read = input.gcount();
        if (read <= 0) break;
        std::string scan;
        scan.reserve(tail.size() + static_cast<std::size_t>(read));
        scan.append(tail);
        scan.append(chunk.data(), static_cast<std::size_t>(read));
        if (scan.find(token) != std::string::npos) return true;
        const auto retained =
            std::min(token.size() - 1u, scan.size());
        tail.assign(scan.end() - static_cast<std::ptrdiff_t>(retained),
                    scan.end());
    }
    return !input.bad() && tail.find(token) != std::string::npos;
}

void remove_invalid_generated_artifact_tree(
    const std::filesystem::path& workspace) {
    const auto normalized_workspace =
        std::filesystem::absolute(workspace).lexically_normal();
    const auto generated_root = (normalized_workspace / "generated").lexically_normal();
    if (generated_root.parent_path() != normalized_workspace ||
        generated_root.filename() != "generated")
        throw std::runtime_error("Ungueltiger Portcodegen-Bereinigungspfad.");
    if (!safe_regular_port_directory_exists(
            generated_root,
            "Ungueltige Portcodegen-Ausgabe"))
        return;
    remove_safe_port_tree(
        generated_root, "Ungueltige Portcodegen-Ausgabe");
}

void remove_invalid_generated_source_tree(
    const std::filesystem::path& workspace) {
    const auto normalized_workspace =
        std::filesystem::absolute(workspace).lexically_normal();
    const auto source_root = (normalized_workspace / "src").lexically_normal();
    if (source_root.parent_path() != normalized_workspace ||
        source_root.filename() != "src")
        throw std::runtime_error("Ungueltiger Portquell-Bereinigungspfad.");
    if (!safe_regular_port_directory_exists(
            source_root,
            "Ungueltige generierte Portquelle"))
        return;
    remove_safe_port_tree(
        source_root, "Ungueltige generierte Portquelle");
}

void reconcile_generated_artifacts_after_cache_miss(
    const std::filesystem::path& workspace) {
    const auto generated_root = workspace / "generated";
    if (!safe_regular_port_directory_exists(
            generated_root,
            "Content-addressed Portcodegen-Verzeichnis"))
        return;
    const auto listed = read_generated_artifact_manifest(generated_root);
    const auto actual = collect_generated_artifact_files(generated_root);
    if (!listed || !actual) {
        remove_invalid_generated_artifact_tree(workspace);
        return;
    }
    auto expected = *listed;
    expected.emplace_back(generated_artifact_manifest_name);
    std::sort(
        expected.begin(), expected.end(), [](const auto& left, const auto& right) {
            return left.generic_string() < right.generic_string();
        });
    std::vector<std::filesystem::path> injected;
    std::set_difference(actual->begin(),
                        actual->end(),
                        expected.begin(),
                        expected.end(),
                        std::back_inserter(injected),
                        [](const auto& left, const auto& right) {
                            return left.generic_string() <
                                   right.generic_string();
                        });
    for (const auto& relative : injected) {
        const auto target = (generated_root / relative).lexically_normal();
        if (target.parent_path().empty() ||
            !path_is_within(target, generated_root))
            throw std::runtime_error(
                "Injiziertes Portcodegen-Artefakt verlaesst das Ausgabeziel.");
        std::error_code remove_error;
        if (!std::filesystem::remove(target, remove_error) || remove_error)
            throw std::runtime_error(
                "Injiziertes Portcodegen-Artefakt konnte nicht entfernt werden.");
    }
}

void reconcile_generated_sources_after_cache_miss(
    const std::filesystem::path& workspace) {
    const auto source_root = workspace / "src";
    if (!safe_regular_port_directory_exists(
            source_root,
            "Content-addressed generierter Portquellordner"))
        return;
    if (!validated_generated_source_files(workspace))
        remove_invalid_generated_source_tree(workspace);
}

std::optional<std::string> port_codegen_tree_identity(
    const std::filesystem::path& root,
    const katana::ProgressReporter& progress,
    const std::string_view progress_label) {
    const auto skipped_progress = [&]() {
        auto scope = progress.begin(
            katana::ProgressOperation::ProgramValidation,
            katana::ProgressUnit::Files,
            std::nullopt,
            std::string(progress_label));
        scope.skipped();
        return std::optional<std::string>{};
    };
    std::vector<std::filesystem::path> files;
    constexpr std::array<std::string_view, 5u> required_files{
        "CMakeLists.txt",
        ".gitignore",
        "INSTALL_ORIGINAL_DISC.txt",
        "run-product-gate.ps1",
        "content/game.katana-install"};
    for (const auto relative : required_files) {
        const auto path = root / relative;
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(path, status_error);
        if (status_error || !std::filesystem::is_regular_file(status) ||
            unsafe_port_filesystem_link(path, status))
            return skipped_progress();
        files.push_back(path);
    }
    const auto generated_files =
        validated_generated_artifact_files(root);
    if (!generated_files) return skipped_progress();
    files.insert(files.end(),
                 generated_files->begin(),
                 generated_files->end());
    const auto generated_sources =
        validated_generated_source_files(root);
    if (!generated_sources) return skipped_progress();
    files.insert(files.end(),
                 generated_sources->begin(),
                 generated_sources->end());
    std::sort(files.begin(), files.end(), [&root](const auto& left, const auto& right) {
        return left.lexically_relative(root).generic_string() <
               right.lexically_relative(root).generic_string();
    });
    auto identity_progress = progress.begin(
        katana::ProgressOperation::ProgramValidation,
        katana::ProgressUnit::Files,
        files.size(),
        std::string(progress_label));
    std::ostringstream identity;
    identity << "katana-port-codegen-tree-v3;";
    for (const auto& file : files) {
        const auto relative = file.lexically_relative(root).generic_string();
        const auto provenance =
            katana::io::capture_input_provenance("port-codegen-cache", file);
        identity << relative << ':' << provenance.size << ':' << provenance.sha256 << ';';
        identity_progress.advance(1u);
    }
    auto result = katana::io::sha256_bytes(identity.str());
    identity_progress.complete();
    return result;
}

void append_port_export_cache_field(std::ostringstream& output,
                                    const std::string_view value) {
    output << value.size() << ':' << value << ';';
}

std::filesystem::path port_export_workspace_root(
    const std::filesystem::path& output_parent) {
    if (const auto configured =
            configured_environment_value(
                "KATANA_PORT_WORKSPACE_ROOT")) {
        const auto root =
            std::filesystem::path(*configured).lexically_normal();
        if (!root.is_absolute())
            throw std::invalid_argument(
                "KATANA_PORT_WORKSPACE_ROOT muss absolut sein.");
        return root;
    }
#ifdef _WIN32
    if (const auto local_app_data =
            configured_environment_value("LOCALAPPDATA"))
        return std::filesystem::path(*local_app_data) /
               "KatanaRecomp" / "port-workspaces";
#else
    if (const auto xdg_cache =
            configured_environment_value("XDG_CACHE_HOME"))
        return std::filesystem::path(*xdg_cache) /
               "KatanaRecomp" / "port-workspaces";
    if (const auto user_home =
            configured_environment_value("HOME"))
        return std::filesystem::path(*user_home) /
               ".cache" / "KatanaRecomp" / "port-workspaces";
#endif
    // Minimal environments without a per-user cache root remain functional.
    // The content-addressed key still makes sibling output paths share work.
    return output_parent;
}

std::filesystem::path port_export_cache_path(
    const std::filesystem::path& workspace,
    const std::string_view source_kind,
    const std::string_view key) {
    return workspace / ".katana-codegen-cache" / "whole-export" /
           ("port-" + std::string(source_kind) + '-' + std::string(key) + ".state");
}

std::optional<CachedPortExport>
load_cached_port_export(
    const std::filesystem::path& workspace,
    const std::string_view expected_source_kind,
    const std::string_view expected_key,
    const katana::runtime::DiscInstallRecipe& expected_recipe,
    const katana::ProgressReporter& progress) {
    const auto state_path =
        port_export_cache_path(workspace, expected_source_kind, expected_key);
    if (!safe_port_directory_chain_exists(
            workspace,
            state_path.parent_path(),
            "Content-addressed Portexport-Cache"))
        return std::nullopt;
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(state_path, status_error);
    if (status_error == std::errc::no_such_file_or_directory ||
        (!status_error &&
         status.type() ==
             std::filesystem::file_type::not_found))
        return std::nullopt;
    if (status_error || !std::filesystem::is_regular_file(status) ||
        unsafe_port_filesystem_link(state_path, status))
        return std::nullopt;
    const auto state_bytes =
        std::filesystem::file_size(state_path, status_error);
    if (status_error ||
        state_bytes > maximum_port_export_cache_state_bytes)
        return std::nullopt;
    std::ifstream state_input(state_path, std::ios::binary | std::ios::ate);
    if (!state_input || state_input.tellg() < 0 ||
        static_cast<std::uintmax_t>(state_input.tellg()) != state_bytes)
        return std::nullopt;
    std::string state_content(
        static_cast<std::size_t>(state_bytes), '\0');
    state_input.seekg(0, std::ios::beg);
    if (!state_content.empty())
        state_input.read(
            state_content.data(),
            static_cast<std::streamsize>(state_content.size()));
    if (!state_input ||
        state_input.peek() != std::char_traits<char>::eof())
        return std::nullopt;
    state_input.close();
    const auto final_status =
        std::filesystem::symlink_status(state_path, status_error);
    if (status_error ||
        !std::filesystem::is_regular_file(final_status) ||
        unsafe_port_filesystem_link(state_path, final_status) ||
        std::filesystem::file_size(state_path, status_error) != state_bytes ||
        status_error ||
        !safe_port_directory_chain_exists(
            workspace,
            state_path.parent_path(),
            "Content-addressed Portexport-Cache"))
        return std::nullopt;
    std::istringstream input(state_content);
    std::string magic;
    std::uint32_t version = 0u;
    CachedPortExport cached;
    std::string key_name;
    std::string source_name;
    std::string tree_name;
    std::string recipe_name;
    std::string functions_name;
    std::string partitions_name;
    if (!(input >> magic >> version >> key_name >> cached.key >>
          source_name >> cached.source_kind >>
          tree_name >> cached.tree_identity >>
          recipe_name >> cached.recipe_identity >>
          functions_name >> cached.functions >>
          partitions_name >> cached.partitions))
        return std::nullopt;
    input >> std::ws;
    if (!input.eof() ||
        magic != "KATANA_PORT_EXPORT_STATE" ||
        version != port_export_cache_version ||
        key_name != "key" || source_name != "source" ||
        tree_name != "tree" || recipe_name != "recipe" ||
        functions_name != "functions" || partitions_name != "partitions" ||
        cached.key != expected_key ||
        cached.source_kind != expected_source_kind ||
        cached.recipe_identity != port_export_recipe_identity(expected_recipe) ||
        !valid_cache_digest(cached.key) ||
        !valid_cache_digest(cached.tree_identity) ||
        !valid_cache_digest(cached.recipe_identity) ||
        cached.functions == 0u || cached.partitions == 0u)
        return std::nullopt;
    const auto actual_tree = port_codegen_tree_identity(
        workspace,
        progress,
        "whole-export-cache-tree-load");
    if (!actual_tree || *actual_tree != cached.tree_identity)
        return std::nullopt;
    const auto recipe_path = workspace / "content" / "game.katana-install";
    try {
        const auto actual_recipe =
            katana::runtime::parse_disc_install_recipe(recipe_path);
        if (katana::runtime::format_disc_install_recipe(actual_recipe) !=
            katana::runtime::format_disc_install_recipe(expected_recipe))
            return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    return cached;
}

void store_cached_port_export(
    const std::filesystem::path& workspace,
    const std::string_view source_kind,
    const std::string_view key,
    const katana::runtime::DiscInstallRecipe& expected_recipe,
    const katana::codegen::PortExportResult& report,
    const katana::ProgressReporter& progress) {
    if (report.job_generation != expected_recipe.job_generation ||
        report.content_identity != expected_recipe.content_identity ||
        report.disc_tracks != expected_recipe.tracks.size())
        throw std::runtime_error(
            "Content-addressed Portexport besitzt eine unerwartete Disc-Bindung.");
    const auto tree_identity = port_codegen_tree_identity(
        workspace,
        progress,
        "whole-export-cache-tree-store");
    if (!tree_identity)
        throw std::runtime_error(
            "Content-addressed Portcodegen-Ausgabe ist nach Export unvollstaendig.");
    try {
        const auto state_path =
            port_export_cache_path(workspace, source_kind, key);
        ensure_safe_port_directory_chain(
            workspace,
            state_path.parent_path(),
            "Content-addressed Portexport-Cache");
        const auto temporary =
            std::filesystem::path(state_path.string() + ".tmp");
        std::ostringstream state_content;
        state_content << "KATANA_PORT_EXPORT_STATE "
                      << port_export_cache_version << '\n'
                      << "key " << key << '\n'
                      << "source " << source_kind << '\n'
                      << "tree " << *tree_identity << '\n'
                      << "recipe "
                      << port_export_recipe_identity(expected_recipe)
                      << '\n'
                      << "functions " << report.functions << '\n'
                      << "partitions " << report.partitions << '\n';
        const auto serialized_state = state_content.str();
        if (serialized_state.size() >
            maximum_port_export_cache_state_bytes)
            throw std::runtime_error(
                "Content-addressed Portexport-Status ueberschreitet sein "
                "Bytebudget.");
        std::error_code replace_error;
        const auto temporary_status =
            std::filesystem::symlink_status(temporary, replace_error);
        if (replace_error == std::errc::no_such_file_or_directory ||
            (!replace_error &&
             temporary_status.type() ==
                 std::filesystem::file_type::not_found)) {
            replace_error.clear();
        } else if (
            replace_error ||
            !std::filesystem::is_regular_file(temporary_status) ||
            unsafe_port_filesystem_link(temporary, temporary_status) ||
            !std::filesystem::remove(temporary, replace_error) ||
            replace_error) {
            throw std::runtime_error(
                "Temporaerer Portexport-Status ist kein sicher "
                "ersetzbares Artefakt.");
        }
        std::ofstream output(
            temporary, std::ios::binary | std::ios::trunc);
        output.write(
            serialized_state.data(),
            static_cast<std::streamsize>(serialized_state.size()));
        output.flush();
        if (!output)
            throw std::runtime_error(
                "Content-addressed Portexport-Status konnte nicht "
                "geschrieben werden.");
        output.close();
        const auto previous_status =
            std::filesystem::symlink_status(state_path, replace_error);
        if (replace_error == std::errc::no_such_file_or_directory ||
            (!replace_error &&
             previous_status.type() ==
                 std::filesystem::file_type::not_found)) {
            replace_error.clear();
        } else if (
            replace_error ||
            !std::filesystem::is_regular_file(previous_status) ||
            unsafe_port_filesystem_link(state_path, previous_status) ||
            !std::filesystem::remove(state_path, replace_error) ||
            replace_error) {
            throw std::runtime_error(
                "Alter content-addressed Portexport-Status konnte nicht "
                "sicher ersetzt werden.");
        }
        ensure_safe_port_directory_chain(
            workspace,
            state_path.parent_path(),
            "Content-addressed Portexport-Cache");
        std::filesystem::rename(temporary, state_path, replace_error);
        if (replace_error)
            throw std::runtime_error(
                "Content-addressed Portexport-Status konnte nicht "
                "publiziert werden.");
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& exception) {
        std::cerr
            << "KATANA_PORT_CACHE_PUBLISH_SKIPPED whole-export: "
            << exception.what() << '\n'
            << std::flush;
    }
}

int extract_boot_executable_artifact(
    const std::filesystem::path& gdi_path,
    const std::filesystem::path& output_path) {
    if (gdi_path.empty() || output_path.empty())
        throw std::invalid_argument(
            "extract-boot-executable braucht GDI und Ausgabeordner.");
    const auto absolute_output =
        std::filesystem::absolute(output_path).lexically_normal();
    const auto source_root = discover_source_root_for_protection();
    if (!source_root.empty()) {
        std::error_code canonical_error;
        const auto resolved_output =
            std::filesystem::weakly_canonical(
                absolute_output, canonical_error);
        if (canonical_error)
            throw std::runtime_error(
                "Privater Boot-Artefaktpfad konnte nicht aufgeloest werden.");
        if (path_is_within(
                resolved_output, std::filesystem::canonical(source_root)))
            throw std::invalid_argument(
                "Private Dreamcast-Bootbytes duerfen nicht im KatanaRecomp-Quellbaum liegen.");
    }
    const auto artifact =
        katana::platform::extract_dreamcast_boot_executable_artifact(
            gdi_path, absolute_output);
    std::cout << "Privates Boot-Executable-Artefakt verifiziert: "
              << artifact.manifest_path.string() << '\n'
              << "Bootdatei: " << artifact.metadata.boot_file_name << '\n'
              << "Boot-SHA-256: " << artifact.boot_sha256 << '\n'
              << "Content-Identitaet: "
              << artifact.install_recipe.content_identity << '\n'
              << "Retailbytes im Repository: 0\n";
    return 0;
}

using RuntimeImagePayloadArgument =
    std::pair<std::string, std::filesystem::path>;
using NativeBootstrapWritePayloadArgument =
    std::pair<std::uint32_t, std::filesystem::path>;

using LatentAotEntryHintArgument = katana::codegen::LatentAotEntryHint;
using LatentAotDiscoveryModeArgument =
    katana::codegen::LatentAotDiscoveryMode;
using PortAnalysisMode = katana::codegen::PortAnalysisMode;

constexpr std::uint64_t latent_aot_entry_disc_sector_size = 2048u;
constexpr std::size_t maximum_latent_aot_entry_hint_arguments = 1024u;
constexpr std::uintmax_t maximum_latent_aot_entry_file_bytes = 1024u * 1024u;
constexpr std::size_t maximum_latent_aot_entry_file_line_bytes = 512u;
constexpr std::size_t maximum_native_aot_resume_entry_arguments = 4096u;

LatentAotDiscoveryModeArgument parse_latent_aot_discovery_mode(
    const std::string_view text) {
    if (text == "heuristic")
        return LatentAotDiscoveryModeArgument::HintsAndHeuristics;
    if (text == "exact-only")
        return LatentAotDiscoveryModeArgument::ExactOnly;
    throw std::invalid_argument(
        "--latent-aot-mode erwartet heuristic oder exact-only.");
}

PortAnalysisMode parse_port_analysis_mode(const std::string_view text) {
    if (text == "platform") return PortAnalysisMode::PlatformAbi;
    if (text == "runtime-only")
        return PortAnalysisMode::ConservativeRuntimeOnly;
    throw std::invalid_argument(
        "--analysis-mode erwartet platform oder runtime-only.");
}

std::uint32_t parse_native_aot_resume_entry(const std::string_view text) {
    if (text.size() < 3u || text.size() > 10u ||
        !(text.starts_with("0x") || text.starts_with("0X")))
        throw std::invalid_argument(
            "--native-aot-resume-entry erwartet eine 32-Bit-Hexadresse mit 0x-Praefix.");
    std::uint32_t address = 0u;
    const auto conversion = std::from_chars(
        text.data() + 2u, text.data() + text.size(), address, 16);
    if (conversion.ec != std::errc{} ||
        conversion.ptr != text.data() + text.size() || (address & 1u) != 0u)
        throw std::invalid_argument(
            "--native-aot-resume-entry besitzt keine gerade 32-Bit-Hexadresse.");
    return address;
}

std::uint32_t parse_native_bootstrap_write_address(
    const std::string_view text) {
    if (text.size() < 3u || text.size() > 10u ||
        !(text.starts_with("0x") || text.starts_with("0X")))
        throw std::invalid_argument(
            "--native-bootstrap-write-payload erwartet eine "
            "32-Bit-Hexadresse mit 0x-Praefix.");
    std::uint32_t address = 0u;
    const auto conversion = std::from_chars(
        text.data() + 2u, text.data() + text.size(), address, 16);
    if (conversion.ec != std::errc{} ||
        conversion.ptr != text.data() + text.size())
        throw std::invalid_argument(
            "--native-bootstrap-write-payload besitzt keine "
            "32-Bit-Hexadresse.");
    return address;
}

std::string_view port_analysis_mode_identity(const PortAnalysisMode mode) {
    switch (mode) {
    case PortAnalysisMode::PlatformAbi:
        return "platform-abi";
    case PortAnalysisMode::ConservativeRuntimeOnly:
        return "conservative-runtime-only";
    default:
        throw std::invalid_argument("Ungueltiger Port-Analysemodus.");
    }
}

bool valid_latent_aot_entry_identity(const std::string_view identity) noexcept {
    constexpr std::string_view prefix{"sha256:"};
    if (identity.size() != prefix.size() + 64u || !identity.starts_with(prefix))
        return false;
    const auto digest = identity.substr(prefix.size());
    return std::all_of(digest.begin(), digest.end(), [](const char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

bool valid_latent_aot_entry_hint(
    const LatentAotEntryHintArgument& hint) noexcept {
    return valid_latent_aot_entry_identity(hint.byte_identity) &&
           (hint.disc_byte_offset % latent_aot_entry_disc_sector_size) == 0u &&
           hint.byte_size != 0u &&
           hint.byte_size <=
               katana::runtime::maximum_native_aot_template_extent &&
           (hint.module_relative_offset & 1u) == 0u &&
           hint.disc_byte_offset <=
               std::numeric_limits<std::uint64_t>::max() - hint.byte_size;
}

std::uint64_t parse_latent_aot_entry_integer(const std::string_view text,
                                             const std::string_view field_name) {
    auto digits = text;
    std::uint64_t base = 10u;
    if (digits.starts_with("0x")) {
        digits.remove_prefix(2u);
        base = 16u;
    }
    if (digits.empty())
        throw std::invalid_argument(
            "--latent-aot-entry besitzt kein " + std::string(field_name) + ".");
    std::uint64_t value = 0u;
    for (const auto character : digits) {
        std::uint64_t digit = 0u;
        if (character >= '0' && character <= '9')
            digit = static_cast<std::uint64_t>(character - '0');
        else if (base == 16u && character >= 'a' && character <= 'f')
            digit = static_cast<std::uint64_t>(character - 'a' + 10u);
        else if (base == 16u && character >= 'A' && character <= 'F')
            digit = static_cast<std::uint64_t>(character - 'A' + 10u);
        else
            throw std::invalid_argument(
                "--latent-aot-entry besitzt ein ungueltiges " +
                std::string(field_name) + ".");
        if (digit >= base ||
            value > (std::numeric_limits<std::uint64_t>::max() - digit) / base)
            throw std::invalid_argument(
                "--latent-aot-entry besitzt ein zu grosses " +
                std::string(field_name) + ".");
        value = value * base + digit;
    }
    return value;
}

LatentAotEntryHintArgument parse_latent_aot_entry_hint(const std::string_view text) {
    const auto at = text.find('@');
    if (at == std::string_view::npos || at == 0u ||
        text.find('@', at + 1u) != std::string_view::npos)
        throw std::invalid_argument(
            "--latent-aot-entry erwartet "
            "sha256:<64-lowerhex>@<disc-byte-offset>:<encoded-byte-size>:"
            "<module-relative-offset>.");
    const auto identity = text.substr(0u, at);
    const auto fields = text.substr(at + 1u);
    const auto first_separator = fields.find(':');
    const auto second_separator =
        first_separator == std::string_view::npos
            ? std::string_view::npos
            : fields.find(':', first_separator + 1u);
    if (first_separator == 0u || second_separator == std::string_view::npos ||
        second_separator == first_separator + 1u ||
        second_separator + 1u >= fields.size() ||
        fields.find(':', second_separator + 1u) != std::string_view::npos ||
        !valid_latent_aot_entry_identity(identity))
        throw std::invalid_argument(
            "--latent-aot-entry erwartet "
            "sha256:<64-lowerhex>@<disc-byte-offset>:<encoded-byte-size>:"
            "<module-relative-offset>.");
    const auto disc_byte_offset = parse_latent_aot_entry_integer(
        fields.substr(0u, first_separator), "Disc-Byteoffset");
    const auto byte_size = parse_latent_aot_entry_integer(
        fields.substr(first_separator + 1u, second_separator - first_separator - 1u),
        "Modulgroesse");
    const auto module_relative_offset = parse_latent_aot_entry_integer(
        fields.substr(second_separator + 1u), "Modulentryoffset");
    if ((disc_byte_offset % latent_aot_entry_disc_sector_size) != 0u ||
        byte_size == 0u ||
        byte_size > std::numeric_limits<std::uint32_t>::max() ||
        module_relative_offset > std::numeric_limits<std::uint32_t>::max() ||
        (module_relative_offset & 1u) != 0u ||
        disc_byte_offset > std::numeric_limits<std::uint64_t>::max() - byte_size)
        throw std::invalid_argument(
            "--latent-aot-entry besitzt eine ungueltige Modulbindung.");
    return {std::string(identity),
            disc_byte_offset,
            static_cast<std::uint32_t>(byte_size),
            static_cast<std::uint32_t>(module_relative_offset)};
}

std::vector<LatentAotEntryHintArgument> load_latent_aot_entry_hint_file(
    const std::filesystem::path& path) {
    if (path.empty())
        throw std::invalid_argument(
            "--latent-aot-entry-file besitzt keinen Dateipfad.");

    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path, status_error);
    if (status_error || !std::filesystem::is_regular_file(status) ||
        unsafe_port_filesystem_link(path, status))
        throw std::invalid_argument(
            "--latent-aot-entry-file muss eine regulaere Nicht-Symlink-Datei sein.");
    const auto canonical = std::filesystem::canonical(path, status_error);
    if (status_error)
        throw std::invalid_argument(
            "--latent-aot-entry-file kann nicht kanonisiert werden.");
    const auto byte_size = std::filesystem::file_size(canonical, status_error);
    if (status_error || byte_size == 0u ||
        byte_size > maximum_latent_aot_entry_file_bytes)
        throw std::invalid_argument(
            "--latent-aot-entry-file ist leer oder ueberschreitet 1 MiB.");

    std::ifstream input(canonical, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != static_cast<std::streamoff>(byte_size))
        throw std::runtime_error(
            "--latent-aot-entry-file kann nicht stabil geoeffnet werden.");
    std::string contents(static_cast<std::size_t>(byte_size), '\0');
    input.seekg(0, std::ios::beg);
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!input)
        throw std::runtime_error(
            "--latent-aot-entry-file kann nicht gelesen werden.");
    input.close();

    const auto final_status =
        std::filesystem::symlink_status(canonical, status_error);
    if (status_error || !std::filesystem::is_regular_file(final_status) ||
        unsafe_port_filesystem_link(canonical, final_status) ||
        std::filesystem::file_size(canonical, status_error) != byte_size ||
        status_error)
        throw std::runtime_error(
            "--latent-aot-entry-file wurde waehrend des Lesens veraendert.");
    if (contents.find('\0') != std::string::npos)
        throw std::invalid_argument(
            "--latent-aot-entry-file enthaelt ein ungueltiges NUL-Byte.");

    std::vector<LatentAotEntryHintArgument> hints;
    std::size_t line_number = 0u;
    std::size_t line_begin = 0u;
    while (line_begin <= contents.size()) {
        ++line_number;
        const auto line_end = contents.find('\n', line_begin);
        const auto line_length =
            (line_end == std::string::npos ? contents.size() : line_end) -
            line_begin;
        if (line_length > maximum_latent_aot_entry_file_line_bytes)
            throw std::invalid_argument(
                "--latent-aot-entry-file Zeile " +
                std::to_string(line_number) + " ist zu lang.");
        auto line = std::string_view(contents).substr(line_begin, line_length);
        const auto ascii_space = [](const char character) noexcept {
            return character == ' ' || character == '\t' || character == '\r';
        };
        while (!line.empty() && ascii_space(line.front())) line.remove_prefix(1u);
        while (!line.empty() && ascii_space(line.back())) line.remove_suffix(1u);
        if (!line.empty() && !line.starts_with('#')) {
            if (hints.size() >= maximum_latent_aot_entry_hint_arguments)
                throw std::invalid_argument(
                    "--latent-aot-entry-file ueberschreitet das Hintbudget.");
            try {
                hints.push_back(parse_latent_aot_entry_hint(line));
            } catch (const std::invalid_argument& error) {
                throw std::invalid_argument(
                    "--latent-aot-entry-file Zeile " +
                    std::to_string(line_number) + ": " + error.what());
            }
        }
        if (line_end == std::string::npos) break;
        line_begin = line_end + 1u;
    }
    if (hints.empty())
        throw std::invalid_argument(
            "--latent-aot-entry-file enthaelt keine Entry-Hints.");
    return hints;
}

bool latent_aot_entry_hint_less(const LatentAotEntryHintArgument& left,
                                const LatentAotEntryHintArgument& right) noexcept {
    if (left.byte_identity != right.byte_identity)
        return left.byte_identity < right.byte_identity;
    if (left.disc_byte_offset != right.disc_byte_offset)
        return left.disc_byte_offset < right.disc_byte_offset;
    if (left.byte_size != right.byte_size) return left.byte_size < right.byte_size;
    return left.module_relative_offset < right.module_relative_offset;
}

std::vector<LatentAotEntryHintArgument> normalize_latent_aot_entry_hints(
    std::vector<LatentAotEntryHintArgument> hints) {
    if (hints.size() > maximum_latent_aot_entry_hint_arguments)
        throw std::invalid_argument("--latent-aot-entry ueberschreitet das Hintbudget.");
    if (std::any_of(hints.begin(), hints.end(),
                    [](const auto& hint) {
                        return !valid_latent_aot_entry_hint(hint);
                    }))
        throw std::invalid_argument(
            "--latent-aot-entry besitzt eine ungueltige Modulbindung.");
    std::sort(hints.begin(), hints.end(), latent_aot_entry_hint_less);
    hints.erase(std::unique(hints.begin(), hints.end()), hints.end());
    return hints;
}

std::string latent_aot_entry_hint_identity(
    const std::vector<LatentAotEntryHintArgument>& hints,
    const LatentAotDiscoveryModeArgument discovery_mode) {
    std::ostringstream identity;
    append_port_export_cache_field(identity, "katana-latent-aot-entry-hints-v2");
    switch (discovery_mode) {
    case LatentAotDiscoveryModeArgument::HintsAndHeuristics:
        append_port_export_cache_field(identity, "heuristic");
        break;
    case LatentAotDiscoveryModeArgument::ExactOnly:
        append_port_export_cache_field(identity, "exact-only");
        break;
    default:
        throw std::invalid_argument(
            "Latent-AOT-Discoverymodus ist ungueltig.");
    }
    for (const auto& hint : hints) {
        append_port_export_cache_field(identity, hint.byte_identity);
        append_port_export_cache_field(identity, std::to_string(hint.disc_byte_offset));
        append_port_export_cache_field(identity, std::to_string(hint.byte_size));
        append_port_export_cache_field(
            identity, std::to_string(hint.module_relative_offset));
    }
    return katana::io::sha256_bytes(identity.str());
}

std::vector<std::uint8_t> load_runtime_image_payload(
    const std::filesystem::path& path,
    const std::uint32_t expected_size,
    const std::filesystem::path& source_root) {
    if (path.empty() || expected_size == 0u)
        throw std::invalid_argument(
            "Runtime-Image-Payloadpfad oder erwartete Groesse ist leer.");
    std::error_code status_error;
    const auto status =
        std::filesystem::symlink_status(path, status_error);
    if (status_error)
        throw std::invalid_argument(
            "Runtime-Image-Payloadstatus konnte nicht gelesen werden: " +
            path.string() + ": " + status_error.message());
    if (!std::filesystem::is_regular_file(status))
        throw std::invalid_argument(
            "Runtime-Image-Payload ist keine regulaere Datei: " +
            path.string());
    if (unsafe_port_filesystem_link(path, status))
        throw std::invalid_argument(
            "Runtime-Image-Payload besitzt ein unsicheres Reparse-Attribut: " +
            path.string());
    const auto canonical =
        std::filesystem::canonical(path, status_error);
    if (status_error)
        throw std::invalid_argument(
            "Runtime-Image-Payloadpfad kann nicht kanonisiert werden.");
    if (!source_root.empty() &&
        path_is_within(
            canonical,
            std::filesystem::canonical(source_root)))
        throw std::invalid_argument(
            "Private Runtime-Image-Payloadbytes duerfen nicht im "
            "KatanaRecomp-Quellbaum liegen.");
    const auto actual_size =
        std::filesystem::file_size(canonical, status_error);
    if (actual_size != expected_size || status_error)
        throw std::invalid_argument(
            "Gebundene Payloadgroesse passt nicht zum Deskriptor: "
            "expected=" +
            std::to_string(expected_size) + ", actual=" +
            (status_error ? std::string{"unavailable"}
                          : std::to_string(actual_size)) +
            '.');
    std::ifstream input(canonical, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() !=
                      static_cast<std::streamoff>(expected_size))
        throw std::runtime_error(
            "Runtime-Image-Payload kann nicht stabil geoeffnet werden.");
    std::vector<std::uint8_t> bytes(expected_size);
    input.seekg(0, std::ios::beg);
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!input)
        throw std::runtime_error(
            "Runtime-Image-Payload kann nicht gelesen werden.");
    input.close();
    const auto final_status =
        std::filesystem::symlink_status(canonical, status_error);
    if (status_error || !std::filesystem::is_regular_file(final_status) ||
        unsafe_port_filesystem_link(canonical, final_status) ||
        std::filesystem::file_size(canonical, status_error) !=
            expected_size ||
        status_error)
        throw std::runtime_error(
            "Runtime-Image-Payload wurde waehrend des Lesens veraendert.");
    return bytes;
}

constexpr std::size_t maximum_runtime_frontier_import_bytes =
    16u * 1024u * 1024u;
constexpr std::size_t maximum_runtime_frontier_line_bytes = 16u * 1024u;
constexpr std::size_t maximum_agent_session_ledger_bytes =
    4u * 1024u * 1024u;
constexpr std::size_t maximum_agent_session_line_bytes = 8192u;

struct RuntimeFrontierObservation final {
    std::string reason;
    std::uint32_t pc = 0u;
    std::uint32_t pr = 0u;
    std::uint32_t runtime_target = 0u;
    std::uint32_t source_address = 0u;
    std::uint32_t callsite = 0u;
    std::uint32_t exit_kind = 0u;
    std::uint32_t dispatch_site_class = 0u;
    std::uint32_t active_instruction = 0u;
    std::uint32_t active_block = 0u;
    std::uint32_t active_block_size = 0u;
    std::uint32_t pointer_value = 0u;
};

struct AgentIterationDelta final {
    std::size_t resolved_frontiers = 0u;
    std::size_t new_frontiers = 0u;
    std::size_t new_runtime_hints = 0u;
    std::size_t routed_to_runtime_frontiers = 0u;
    std::size_t routed_to_static_frontiers = 0u;
    std::size_t static_actionable_before = 0u;
    std::size_t static_actionable_after = 0u;
    std::size_t proof_upgrades = 0u;
    std::size_t proof_downgrades = 0u;
    std::size_t new_incomplete_roots = 0u;
    std::size_t resolved_incomplete_roots = 0u;
};

katana::agent::ExecutableMaterializationWorld load_agent_world(
    const std::filesystem::path& path,
    std::string* artifact_sha256 = nullptr);

enum class StrictJsonValueKind : std::uint8_t {
    String,
    Number,
    Boolean,
    Null,
};

struct StrictJsonField final {
    std::string_view key;
    StrictJsonValueKind kind = StrictJsonValueKind::Null;
    std::string_view string_value;
    std::uint64_t number = 0u;
    bool negative = false;
    bool boolean = false;
};

struct StrictJsonObject final {
    std::array<StrictJsonField, 32u> fields{};
    std::size_t count = 0u;
};

[[nodiscard]] bool strict_json_space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

void strict_json_skip_space(const std::string_view text, std::size_t& cursor) {
    while (cursor < text.size() && strict_json_space(text[cursor])) ++cursor;
}

[[nodiscard]] const StrictJsonField* strict_json_field(
    const StrictJsonObject& object,
    const std::string_view key) noexcept {
    const auto found = std::find_if(
        object.fields.begin(), object.fields.begin() + object.count,
        [&](const auto& field) { return field.key == key; });
    return found == object.fields.begin() + object.count ? nullptr : &*found;
}

void parse_strict_json_object(
    const std::string_view text,
    StrictJsonObject& output) {
    output = {};
    std::size_t cursor = 0u;
    strict_json_skip_space(text, cursor);
    if (cursor == text.size() || text[cursor++] != '{')
        throw std::invalid_argument("Striktes JSON-Objekt beginnt nicht mit '{'.");
    strict_json_skip_space(text, cursor);
    if (cursor < text.size() && text[cursor] == '}') {
        ++cursor;
        strict_json_skip_space(text, cursor);
        if (cursor != text.size())
            throw std::invalid_argument("Striktes JSON-Objekt besitzt Restdaten.");
        return;
    }
    while (cursor < text.size()) {
        if (output.count >= output.fields.size() || cursor >= text.size() ||
            text[cursor++] != '"')
            throw std::invalid_argument("Striktes JSON besitzt einen ungueltigen Schluessel.");
        const auto key_begin = cursor;
        while (cursor < text.size() && text[cursor] != '"') {
            const auto byte = static_cast<unsigned char>(text[cursor]);
            if (text[cursor] == '\\' || byte < 0x20u)
                throw std::invalid_argument("Striktes JSON besitzt einen nicht unterstuetzten String-Escape.");
            ++cursor;
        }
        if (cursor >= text.size())
            throw std::invalid_argument("Striktes JSON besitzt einen offenen Schluessel.");
        const auto key = text.substr(key_begin, cursor - key_begin);
        ++cursor;
        if (strict_json_field(output, key) != nullptr)
            throw std::invalid_argument("Striktes JSON besitzt einen doppelten Schluessel.");
        strict_json_skip_space(text, cursor);
        if (cursor >= text.size() || text[cursor++] != ':')
            throw std::invalid_argument("Striktes JSON besitzt keinen Schluesseltrenner.");
        strict_json_skip_space(text, cursor);
        auto& field = output.fields[output.count++];
        field.key = key;
        if (cursor >= text.size())
            throw std::invalid_argument("Striktes JSON besitzt keinen Wert.");
        if (text[cursor] == '"') {
            ++cursor;
            const auto value_begin = cursor;
            while (cursor < text.size() && text[cursor] != '"') {
                const auto byte = static_cast<unsigned char>(text[cursor]);
                if (text[cursor] == '\\' || byte < 0x20u)
                    throw std::invalid_argument("Striktes JSON besitzt einen nicht unterstuetzten Wert-Escape.");
                ++cursor;
            }
            if (cursor >= text.size())
                throw std::invalid_argument("Striktes JSON besitzt einen offenen Stringwert.");
            field.kind = StrictJsonValueKind::String;
            field.string_value = text.substr(value_begin, cursor - value_begin);
            ++cursor;
        } else if (text[cursor] == 't' &&
                   text.substr(cursor, 4u) == "true") {
            field.kind = StrictJsonValueKind::Boolean;
            field.boolean = true;
            cursor += 4u;
        } else if (text[cursor] == 'f' &&
                   text.substr(cursor, 5u) == "false") {
            field.kind = StrictJsonValueKind::Boolean;
            field.boolean = false;
            cursor += 5u;
        } else if (text[cursor] == 'n' &&
                   text.substr(cursor, 4u) == "null") {
            field.kind = StrictJsonValueKind::Null;
            cursor += 4u;
        } else {
            if (text[cursor] == '-') {
                field.negative = true;
                ++cursor;
            }
            const auto digits_begin = cursor;
            while (cursor < text.size() && text[cursor] >= '0' &&
                   text[cursor] <= '9')
                ++cursor;
            if (cursor == digits_begin ||
                (text[digits_begin] == '0' && cursor - digits_begin > 1u))
                throw std::invalid_argument("Striktes JSON besitzt eine ungueltige Zahl.");
            std::uint64_t value = 0u;
            const auto conversion = std::from_chars(
                text.data() + digits_begin,
                text.data() + cursor,
                value,
                10);
            if (conversion.ec != std::errc{} ||
                conversion.ptr != text.data() + cursor)
                throw std::invalid_argument("Striktes JSON besitzt eine ueberlaufende Zahl.");
            field.kind = StrictJsonValueKind::Number;
            field.number = value;
        }
        strict_json_skip_space(text, cursor);
        if (cursor >= text.size())
            throw std::invalid_argument("Striktes JSON endet unvollstaendig.");
        if (text[cursor] == '}') {
            ++cursor;
            strict_json_skip_space(text, cursor);
            if (cursor != text.size())
                throw std::invalid_argument("Striktes JSON besitzt Restdaten.");
            return;
        }
        if (text[cursor++] != ',')
            throw std::invalid_argument("Striktes JSON besitzt keinen Datensatztrenner.");
        strict_json_skip_space(text, cursor);
    }
    throw std::invalid_argument("Striktes JSON endet ohne Abschluss.");
}

std::uint32_t strict_json_u32(
    const StrictJsonObject& object,
    const std::string_view key) {
    const auto* field = strict_json_field(object, key);
    if (field == nullptr || field->kind != StrictJsonValueKind::Number ||
        field->negative || field->number > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Striktes JSON besitzt kein gueltiges u32-Feld " + std::string(key) + '.');
    return static_cast<std::uint32_t>(field->number);
}

std::uint64_t strict_json_u64(
    const StrictJsonObject& object,
    const std::string_view key) {
    const auto* field = strict_json_field(object, key);
    if (field == nullptr || field->kind != StrictJsonValueKind::Number ||
        field->negative)
        throw std::invalid_argument("Striktes JSON besitzt kein gueltiges u64-Feld " + std::string(key) + '.');
    return field->number;
}

bool strict_json_bool(
    const StrictJsonObject& object,
    const std::string_view key) {
    const auto* field = strict_json_field(object, key);
    if (field == nullptr || field->kind != StrictJsonValueKind::Boolean)
        throw std::invalid_argument("Striktes JSON besitzt kein gueltiges Bool-Feld " + std::string(key) + '.');
    return field->boolean;
}

std::string strict_json_identifier(
    const StrictJsonObject& object,
    const std::string_view key,
    const std::size_t maximum_length,
    const bool reason_identifier = false) {
    const auto* field = strict_json_field(object, key);
    if (field == nullptr || field->kind != StrictJsonValueKind::String ||
        field->string_value.empty() || field->string_value.size() > maximum_length)
        throw std::invalid_argument("Striktes JSON besitzt keinen begrenzten Bezeichner " + std::string(key) + '.');
    std::string result(field->string_value);
    for (const auto character : result) {
        const auto byte = static_cast<unsigned char>(character);
        if (reason_identifier) {
            if (!std::isalnum(byte) && character != '-' && character != '_' && character != '.')
                throw std::invalid_argument("Runtime-Frontier-Grund ist kein stabiler Bezeichner.");
        } else if (std::isspace(byte) || byte < 0x20u || character == '/' ||
                   character == '\\' || character == '"') {
            throw std::invalid_argument("Identitaetsfeld enthaelt unzulaessige Zeichen.");
        }
    }
    return result;
}

void require_strict_json_keys(
    const StrictJsonObject& object,
    const std::span<const std::string_view> expected) {
    if (object.count != expected.size())
        throw std::invalid_argument("Striktes JSON besitzt unerwartete oder fehlende Felder.");
    for (const auto key : expected)
        if (strict_json_field(object, key) == nullptr)
            throw std::invalid_argument("Striktes JSON besitzt ein fehlendes Feld " + std::string(key) + '.');
}

struct RuntimeFrontierImportBinding final {
    katana::codegen::NativeDiscAnalysisArtifactIdentity identity;
};

struct RuntimeFrontierImport final {
    RuntimeFrontierImportBinding binding;
    std::vector<RuntimeFrontierObservation> observations;
};

RuntimeFrontierImport load_runtime_frontier_import(
    const std::filesystem::path& path) {
    constexpr std::string_view binding_prefix{"KATANA_RUNTIME_FRONTIER_BINDING "};
    constexpr std::string_view event_prefix{"KATANA_RUNTIME_FRONTIER "};
    static constexpr std::array<std::string_view, 18u> binding_keys{
        "version", "analysis_artifact_key", "content_identity",
        "boot_byte_identity", "project_identity",
        "analysis_contract_identity", "image_analysis_key",
        "game_project_identity", "native_port_identity",
        "native_port_artifact_identity",
        "analysis_implementation_identity",
        "analysis_cache_implementation_identity",
        "codegen_implementation_identity", "analyzer_abi", "backend_abi",
        "analysis_mode", "disc_volume_start_lba", "disc_extent_lba_bias"};
    static constexpr std::array<std::string_view, 13u> event_keys{
        "version", "reason", "pc", "pr", "runtime_target", "source_address",
        "callsite", "exit_kind", "dispatch_site_class", "active_instruction",
        "active_block", "active_block_size", "pointer_value"};
    const auto document = read_safe_small_port_file(
        path, maximum_runtime_frontier_import_bytes, "Runtime-Frontier-Log");
    RuntimeFrontierImport result;
    bool binding_seen = false;
    bool event_seen = false;
    std::size_t cursor = 0u;
    while (cursor < document.size()) {
        const auto newline = document.find('\n', cursor);
        auto line = std::string_view(document).substr(
            cursor,
            newline == std::string::npos ? document.size() - cursor : newline - cursor);
        cursor = newline == std::string::npos ? document.size() : newline + 1u;
        if (line.size() > maximum_runtime_frontier_line_bytes)
            throw std::invalid_argument("Runtime-Frontier-Zeile ueberschreitet ihr festes Budget.");
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1u);
        if (line.empty())
            throw std::invalid_argument("Runtime-Frontier besitzt eine leere Zeile.");
        if (line.starts_with(binding_prefix)) {
            if (binding_seen || event_seen)
                throw std::invalid_argument("Runtime-Frontier-Binding steht nicht exakt am Anfang.");
            StrictJsonObject object;
            parse_strict_json_object(line.substr(binding_prefix.size()), object);
            require_strict_json_keys(object, binding_keys);
            if (strict_json_u32(object, "version") != 2u)
                throw std::invalid_argument("Runtime-Frontier-Binding besitzt eine unbekannte Version.");
            auto& identity = result.binding.identity;
            identity.key = strict_json_identifier(
                object, "analysis_artifact_key", 512u);
            identity.content_identity = strict_json_identifier(
                object, "content_identity", 512u);
            identity.boot_byte_identity = strict_json_identifier(
                object, "boot_byte_identity", 512u);
            identity.project_identity = strict_json_identifier(
                object, "project_identity", 512u);
            identity.analysis_contract_identity = strict_json_identifier(
                object, "analysis_contract_identity", 512u);
            identity.image_analysis_key = strict_json_identifier(
                object, "image_analysis_key", 512u);
            identity.game_project_identity = strict_json_identifier(
                object, "game_project_identity", 512u);
            identity.native_port_identity = strict_json_identifier(
                object, "native_port_identity", 512u);
            identity.native_port_artifact_identity = strict_json_identifier(
                object, "native_port_artifact_identity", 512u);
            identity.analysis_implementation_identity = strict_json_identifier(
                object, "analysis_implementation_identity", 512u);
            identity.analysis_cache_implementation_identity =
                strict_json_identifier(
                    object, "analysis_cache_implementation_identity", 512u);
            identity.codegen_implementation_identity = strict_json_identifier(
                object, "codegen_implementation_identity", 512u);
            identity.analyzer_abi = strict_json_u32(object, "analyzer_abi");
            identity.backend_abi = strict_json_u32(object, "backend_abi");
            identity.analysis_mode = strict_json_u32(object, "analysis_mode");
            identity.disc_volume_start_lba = strict_json_u32(
                object, "disc_volume_start_lba");
            identity.disc_extent_lba_bias = strict_json_u32(
                object, "disc_extent_lba_bias");
            if (katana::codegen::native_disc_analysis_artifact_identity_key(
                    identity) != identity.key)
                throw std::invalid_argument(
                    "Runtime-Frontier-Binding besitzt keinen kanonischen "
                    "Analyseartefaktschluessel.");
            binding_seen = true;
            continue;
        }
        if (!line.starts_with(event_prefix) || !binding_seen)
            throw std::invalid_argument("Runtime-Frontier besitzt eine unbekannte oder ungebundene Zeile.");
        StrictJsonObject object;
        parse_strict_json_object(line.substr(event_prefix.size()), object);
        require_strict_json_keys(object, event_keys);
        if (strict_json_u32(object, "version") != 1u)
            throw std::invalid_argument("Runtime-Frontier besitzt kein gueltiges v1-Envelope.");
        RuntimeFrontierObservation observation;
        observation.reason = strict_json_identifier(object, "reason", 128u, true);
        observation.pc = strict_json_u32(object, "pc");
        observation.pr = strict_json_u32(object, "pr");
        observation.runtime_target = strict_json_u32(object, "runtime_target");
        observation.source_address = strict_json_u32(object, "source_address");
        observation.callsite = strict_json_u32(object, "callsite");
        observation.exit_kind = strict_json_u32(object, "exit_kind");
        observation.dispatch_site_class = strict_json_u32(object, "dispatch_site_class");
        observation.active_instruction = strict_json_u32(object, "active_instruction");
        observation.active_block = strict_json_u32(object, "active_block");
        observation.active_block_size = strict_json_u32(object, "active_block_size");
        observation.pointer_value = strict_json_u32(object, "pointer_value");
        if (observation.runtime_target == 0u || observation.source_address == 0u ||
            observation.pointer_value != observation.runtime_target ||
            (observation.active_block_size != 0u && observation.active_block == 0u))
            throw std::invalid_argument("Runtime-Frontier verletzt seinen Ziel-/Pointervertrag.");
        result.observations.push_back(std::move(observation));
        event_seen = true;
        if (result.observations.size() > katana::agent::materialization_world_max_frontier)
            throw std::invalid_argument("Runtime-Frontier-Import ueberschreitet sein Eintragsbudget.");
    }
    if (!binding_seen || !event_seen)
        throw std::invalid_argument("Runtime-Frontier-Log besitzt keine vollstaendige v2-Bindung/v1-Evidence.");
    const auto key = [](const RuntimeFrontierObservation& item) {
        return std::tie(item.reason, item.source_address, item.callsite,
                        item.runtime_target, item.pc, item.pr, item.exit_kind,
                        item.dispatch_site_class, item.active_instruction,
                        item.active_block, item.active_block_size, item.pointer_value);
    };
    std::sort(result.observations.begin(), result.observations.end(),
              [&](const auto& left, const auto& right) { return key(left) < key(right); });
    result.observations.erase(
        std::unique(result.observations.begin(), result.observations.end(),
                    [&](const auto& left, const auto& right) { return key(left) == key(right); }),
        result.observations.end());
    return result;
}

std::string agent_hex_address(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(8) << std::setfill('0')
           << value;
    return output.str();
}

struct RuntimeHintRecord final {
    std::string family;
    std::string owner;
    std::string site;
    std::string observation;
};

std::vector<RuntimeHintRecord> retained_runtime_hints(
    const katana::agent::ExecutableMaterializationWorld& world) {
    std::vector<RuntimeHintRecord> result;
    for (const auto& frontier : world.frontier()) {
        if (frontier.state != katana::agent::FrontierState::ObservedHint ||
            frontier.proof !=
                katana::agent::FrontierProof::RuntimeObservation)
            continue;
        const auto prefix =
            "runtime:" + frontier.family + '|' + frontier.owner + '|' +
            frontier.site + '|';
        bool matched = false;
        for (const auto evidence_id : frontier.evidence) {
            const auto candidate = std::find_if(
                world.evidence().begin(),
                world.evidence().end(),
                [&](const auto& evidence) {
                    return evidence.id == evidence_id &&
                           evidence.kind ==
                               katana::agent::EvidenceKind::RuntimeObservation &&
                           evidence.canonical_identity.starts_with(prefix);
                });
            if (candidate == world.evidence().end()) continue;
            matched = true;
            result.push_back(
                {frontier.family,
                 frontier.owner,
                 frontier.site,
                 candidate->canonical_identity.substr(prefix.size())});
        }
        if (!matched)
            throw std::invalid_argument(
                "Materialization-World besitzt ungebundene Runtime-Evidence.");
    }
    return result;
}

RuntimeHintRecord runtime_hint_record(
    const RuntimeFrontierObservation& item) {
    RuntimeHintRecord result;
    result.family = "runtime-frontier:" + item.reason;
    result.owner = "source-target:" + agent_hex_address(item.source_address);
    result.site = "callsite:" + agent_hex_address(item.callsite) +
                  "|runtime-target:" + agent_hex_address(item.runtime_target);
    std::ostringstream observation;
    observation << "pc=" << agent_hex_address(item.pc)
                << ";pr=" << agent_hex_address(item.pr)
                << ";source=" << agent_hex_address(item.source_address)
                << ";runtime=" << agent_hex_address(item.runtime_target)
                << ";callsite=" << agent_hex_address(item.callsite)
                << ";exit-kind=" << item.exit_kind
                << ";site-class=" << item.dispatch_site_class
                << ";active-instruction="
                << agent_hex_address(item.active_instruction)
                << ";active-block=" << agent_hex_address(item.active_block)
                << ";active-block-size=" << item.active_block_size;
    result.observation = observation.str();
    return result;
}

[[nodiscard]] bool has_immutable_agent_evidence(
    const katana::agent::ExecutableMaterializationWorld& world,
    const katana::agent::EvidenceKind kind,
    const std::string_view canonical_identity) noexcept {
    const auto found = std::find_if(
        world.evidence().begin(), world.evidence().end(),
        [&](const auto& evidence) {
            return evidence.kind == kind && evidence.immutable &&
                   evidence.canonical_identity == canonical_identity;
        });
    return found != world.evidence().end();
}

[[nodiscard]] bool materialization_world_matches_analysis_identity(
    const katana::agent::ExecutableMaterializationWorld& world,
    const katana::codegen::NativeDiscAnalysisArtifactIdentity& identity) noexcept {
    // The world format predates the aggregate artifact-key field.  Bind every
    // identity that the world does carry, and reject legacy worlds which omit
    // any member of this set instead of retaining their runtime hints by
    // filename or revision alone.
    if (!has_immutable_agent_evidence(
            world,
            katana::agent::EvidenceKind::IdentityBinding,
            "primary-image:" + identity.boot_byte_identity) ||
        !has_immutable_agent_evidence(
            world,
            katana::agent::EvidenceKind::StaticAnalysis,
            "analysis-contract:" + identity.analysis_contract_identity) ||
        !has_immutable_agent_evidence(
            world,
            katana::agent::EvidenceKind::IdentityBinding,
            "native-port:" + identity.native_port_identity + "|" +
                identity.native_port_artifact_identity))
        return false;
    const auto image = std::find_if(
        world.nodes().begin(), world.nodes().end(),
        [&](const auto& node) {
            return node.kind == katana::agent::MaterializationNodeKind::Image &&
                   node.canonical_identity ==
                       "image:" + identity.boot_byte_identity &&
                   node.source_identity == identity.content_identity;
        });
    const auto provider = std::find_if(
        world.nodes().begin(), world.nodes().end(),
        [&](const auto& node) {
            return node.kind == katana::agent::MaterializationNodeKind::Provider &&
                   node.canonical_identity ==
                       "provider:" + identity.native_port_identity &&
                   node.source_identity == identity.native_port_artifact_identity;
        });
    return image != world.nodes().end() && provider != world.nodes().end();
}

void validate_runtime_frontier_import_binding(
    const RuntimeFrontierImport& imported,
    const katana::codegen::NativeDiscAnalysisResult& analyzed) {
    const auto& expected = analyzed.analysis_artifact_identity;
    const auto& actual = imported.binding.identity;
    if (actual != expected ||
        katana::codegen::native_disc_analysis_artifact_identity_key(actual) !=
            expected.key)
        throw std::invalid_argument(
            "Runtime-Frontier-Bindung passt nicht exakt zur aktuellen "
            "Analyseidentitaet.");
}

std::string serialize_agent_world_json_owned(
    const katana::agent::ExecutableMaterializationWorld& world) {
    constexpr std::size_t initial_capacity = 1u * 1024u * 1024u;
    std::vector<char> buffer(initial_capacity);
    for (;;) {
        const auto serialized =
            katana::agent::serialize_agent_world_json(world, buffer);
        if (serialized.complete && !serialized.truncated &&
            serialized.bytes != 0u)
            return std::string(buffer.data(), serialized.bytes);
        if (buffer.size() ==
            katana::agent::materialization_world_max_binary_artifact_bytes)
            throw std::runtime_error(
                "Materialization-World-JSON ueberschritt die feste "
                "Artefaktgrenze.");
        buffer.resize(std::min(
            katana::agent::materialization_world_max_binary_artifact_bytes,
            buffer.size() * 2u));
    }
}

std::vector<std::uint8_t> serialize_agent_world_binary_owned(
    const katana::agent::ExecutableMaterializationWorld& world) {
    constexpr std::size_t initial_capacity = 1u * 1024u * 1024u;
    std::vector<std::uint8_t> buffer(initial_capacity);
    for (;;) {
        const auto serialized =
            katana::agent::serialize_agent_world_binary(world, buffer);
        if (serialized.complete && !serialized.truncated &&
            serialized.bytes != 0u) {
            buffer.resize(serialized.bytes);
            return buffer;
        }
        if (buffer.size() ==
            katana::agent::materialization_world_max_binary_artifact_bytes)
            throw std::runtime_error(
                "Materialization-World ueberschritt die feste "
                "Artefaktgrenze.");
        buffer.resize(std::min(
            katana::agent::materialization_world_max_binary_artifact_bytes,
            buffer.size() * 2u));
    }
}

std::size_t refresh_agent_artifacts(
    katana::codegen::NativeDiscAnalysisResult& analyzed,
    const std::optional<katana::agent::ExecutableMaterializationWorld>&
        previous_world,
    const std::span<const RuntimeFrontierObservation> observations) {
    katana::agent::ExecutableMaterializationWorld world;
    if (!katana::agent::parse_agent_world_binary(
            analyzed.materialization_world_artifact_bytes, world))
        throw std::runtime_error(
            "Autoritativer Materialization-World konnte nicht erneut "
            "validiert werden.");
    std::vector<RuntimeHintRecord> hints;
    if (previous_world.has_value())
        hints = retained_runtime_hints(*previous_world);
    auto previous_hints = hints;
    hints.reserve(hints.size() + observations.size());
    for (const auto& observation : observations)
        hints.push_back(runtime_hint_record(observation));
    const auto key = [](const RuntimeHintRecord& item) {
        return std::tie(
            item.family, item.owner, item.site, item.observation);
    };
    std::sort(
        hints.begin(), hints.end(),
        [&](const auto& left, const auto& right) {
            return key(left) < key(right);
        });
    hints.erase(
        std::unique(
            hints.begin(), hints.end(),
            [&](const auto& left, const auto& right) {
                return key(left) == key(right);
            }),
        hints.end());
    std::sort(
        previous_hints.begin(), previous_hints.end(),
        [&](const auto& left, const auto& right) {
            return key(left) < key(right);
        });
    previous_hints.erase(
        std::unique(
            previous_hints.begin(), previous_hints.end(),
            [&](const auto& left, const auto& right) {
                return key(left) == key(right);
            }),
        previous_hints.end());
    std::size_t accepted_observations = 0u;
    for (const auto& observation : observations) {
        const auto record = runtime_hint_record(observation);
        if (!std::binary_search(
                previous_hints.begin(), previous_hints.end(), record,
                [&](const auto& left, const auto& right) {
                    return key(left) < key(right);
                }))
            ++accepted_observations;
    }
    for (const auto& hint : hints) {
        if (!world.add_runtime_hint(
                hint.family, hint.owner, hint.site, hint.observation))
            throw std::runtime_error(
                std::string("Runtime-Evidence konnte nicht fail-closed in "
                            "den Materialization-World uebernommen werden: ") +
                katana::agent::world_model_error_name(world.last_error()));
    }
    if (!world.validate())
        throw std::runtime_error(
            "Materialization-World ist nach Runtime-Evidence ungueltig.");
    const auto decision = katana::agent::evaluate_agent_decision(world);
    analyzed.agent_decision =
        katana::agent::agent_decision_kind_name(decision.kind);
    analyzed.agent_decision_reason.assign(decision.reason);
    analyzed.agent_decision_focus = decision.focus.value;
    analyzed.agent_actionable_frontier = decision.actionable_frontier;

    analyzed.materialization_world_json =
        serialize_agent_world_json_owned(world);
    analyzed.materialization_world_artifact_bytes =
        serialize_agent_world_binary_owned(world);
    return accepted_observations;
}

const katana::agent::FrontierEntry* find_agent_frontier(
    const katana::agent::ExecutableMaterializationWorld& world,
    const katana::agent::StableId id) noexcept {
    const auto found = std::find_if(
        world.frontier().begin(), world.frontier().end(),
        [&](const auto& entry) { return entry.id == id; });
    return found == world.frontier().end() ? nullptr : &*found;
}

const katana::agent::MaterializationNode* find_agent_node(
    const katana::agent::ExecutableMaterializationWorld& world,
    const katana::agent::StableId id) noexcept {
    const auto found = std::find_if(
        world.nodes().begin(), world.nodes().end(),
        [&](const auto& entry) { return entry.id == id; });
    return found == world.nodes().end() ? nullptr : &*found;
}

unsigned agent_frontier_proof_rank(
    const katana::agent::FrontierProof proof) noexcept {
    using katana::agent::FrontierProof;
    switch (proof) {
    case FrontierProof::None:
    case FrontierProof::RuntimeObservation:
        return 0u;
    case FrontierProof::StaticDisassembly:
        return 1u;
    case FrontierProof::StaticAnalyzer:
        return 2u;
    case FrontierProof::IdentityBound:
    case FrontierProof::ExplicitRejection:
        return 3u;
    }
    return 0u;
}

unsigned agent_node_proof_rank(
    const katana::agent::ProofClass proof) noexcept {
    using katana::agent::ProofClass;
    switch (proof) {
    case ProofClass::UnknownMaterialization:
        return 0u;
    case ProofClass::GuardedPartial:
        return 1u;
    case ProofClass::GuardedComplete:
        return 2u;
    case ProofClass::ProvenExact:
    case ProofClass::NativeReplaced:
    case ProofClass::Rejected:
        return 3u;
    }
    return 0u;
}

bool unresolved_agent_frontier(
    const katana::agent::FrontierEntry& entry) noexcept {
    return entry.state != katana::agent::FrontierState::Closed &&
           !entry.static_complete &&
           entry.proof != katana::agent::FrontierProof::ExplicitRejection;
}

bool runtime_agent_frontier(
    const katana::agent::FrontierEntry& entry) noexcept {
    return entry.runtime_evidence_required ||
           entry.state == katana::agent::FrontierState::ObservedHint ||
           entry.proof == katana::agent::FrontierProof::RuntimeObservation;
}

bool static_actionable_agent_frontier(
    const katana::agent::FrontierEntry& entry) noexcept {
    return unresolved_agent_frontier(entry) &&
           !runtime_agent_frontier(entry);
}

AgentIterationDelta measure_agent_iteration(
    const katana::agent::ExecutableMaterializationWorld& before,
    const katana::agent::ExecutableMaterializationWorld& after) {
    AgentIterationDelta result;
    for (const auto& previous : before.frontier()) {
        if (static_actionable_agent_frontier(previous))
            ++result.static_actionable_before;
        const auto* current = find_agent_frontier(after, previous.id);
        if (unresolved_agent_frontier(previous) &&
            (current == nullptr || !unresolved_agent_frontier(*current)))
            ++result.resolved_frontiers;
        if (current == nullptr) continue;
        if (static_actionable_agent_frontier(previous) &&
            unresolved_agent_frontier(*current) &&
            runtime_agent_frontier(*current))
            ++result.routed_to_runtime_frontiers;
        if (unresolved_agent_frontier(previous) &&
            runtime_agent_frontier(previous) &&
            static_actionable_agent_frontier(*current))
            ++result.routed_to_static_frontiers;
        const auto previous_rank = agent_frontier_proof_rank(previous.proof);
        const auto current_rank = agent_frontier_proof_rank(current->proof);
        if ((!previous.static_complete && current->static_complete) ||
            current_rank > previous_rank)
            ++result.proof_upgrades;
        if ((previous.static_complete && !current->static_complete) ||
            current_rank < previous_rank)
            ++result.proof_downgrades;
    }
    for (const auto& current : after.frontier()) {
        if (static_actionable_agent_frontier(current))
            ++result.static_actionable_after;
        if (find_agent_frontier(before, current.id) == nullptr &&
            unresolved_agent_frontier(current)) {
            if (current.state ==
                katana::agent::FrontierState::ObservedHint)
                ++result.new_runtime_hints;
            else
                ++result.new_frontiers;
        }
    }
    for (const auto& current : after.frontier()) {
        const auto* previous = find_agent_frontier(before, current.id);
        if (previous == nullptr ||
            current.state != katana::agent::FrontierState::ObservedHint ||
            current.proof != katana::agent::FrontierProof::RuntimeObservation)
            continue;
        for (const auto evidence : current.evidence)
            if (std::find(
                    previous->evidence.begin(), previous->evidence.end(),
                    evidence) == previous->evidence.end())
                ++result.new_runtime_hints;
    }
    for (const auto& previous : before.nodes()) {
        const auto* current = find_agent_node(after, previous.id);
        if (current == nullptr) continue;
        const auto previous_rank = agent_node_proof_rank(previous.proof);
        const auto current_rank = agent_node_proof_rank(current->proof);
        if ((previous.completeness != katana::agent::Completeness::Complete &&
             current->completeness == katana::agent::Completeness::Complete) ||
            current_rank > previous_rank)
            ++result.proof_upgrades;
        if ((previous.completeness == katana::agent::Completeness::Complete &&
             current->completeness != katana::agent::Completeness::Complete) ||
            current_rank < previous_rank)
            ++result.proof_downgrades;
        if (previous.kind == katana::agent::MaterializationNodeKind::AnalysisRoot) {
            const bool was_incomplete =
                previous.completeness != katana::agent::Completeness::Complete;
            const bool is_incomplete =
                current->completeness != katana::agent::Completeness::Complete;
            if (!was_incomplete && is_incomplete)
                ++result.new_incomplete_roots;
            if (was_incomplete && !is_incomplete)
                ++result.resolved_incomplete_roots;
        }
    }
    for (const auto& current : after.nodes())
        if (current.kind ==
                katana::agent::MaterializationNodeKind::AnalysisRoot &&
            current.completeness != katana::agent::Completeness::Complete &&
            find_agent_node(before, current.id) == nullptr)
            ++result.new_incomplete_roots;
    return result;
}

struct AgentSessionLedgerState final {
    std::optional<std::uint64_t> previous_wall_time_ms;
    std::string analysis_artifact_id;
    std::string producer_identity;
    std::string analysis_session_contract_identity;
    std::string materialization_world_sha256;
    std::string materialization_world_json_sha256;
    std::string analysis_report_sha256;
    std::optional<std::string> analysis_archive_sha256;
    bool committed_generation = false;
};

std::string agent_session_producer_identity(
    const katana::cli::PortExportImplementationIdentities& identities) {
    std::ostringstream material;
    const auto append = [&](const std::string_view value) {
        material << value.size() << ':' << value << ';';
    };
    append("katana-agent-session-producer-v2");
    append(KATANA_RECOMP_VERSION);
    append(identities.analysis);
    append(identities.analysis_cache);
    append(identities.codegen);
    append(identities.whole_export);
    append(katana::build_contract::
               materialization_world_component_identity);
    append(std::to_string(
        katana::codegen::native_disc_analysis_artifact_schema_version));
    append(std::to_string(
        katana::agent::materialization_world_schema_version));
    append(std::to_string(
        katana::agent::materialization_world_binary_schema_version));
    return katana::io::sha256_bytes(material.str());
}

std::string agent_analysis_session_contract_identity(
    const katana::codegen::NativeDiscAnalysisArtifactIdentity& identity,
    const katana::codegen::PortExportOptions& options) {
    std::ostringstream material;
    const auto append = [&](const std::string_view value) {
        material << value.size() << ':' << value << ';';
    };
    const auto append_value = [&](const std::uint64_t value) {
        material << 'i' << value << ';';
    };
    append("katana-agent-analysis-session-contract-v1");
    append(identity.content_identity);
    append(identity.boot_byte_identity);
    append(identity.project_identity);
    append(identity.game_project_identity);
    append(identity.native_port_identity);
    append(identity.native_port_artifact_identity);
    append_value(identity.analysis_mode);
    append_value(identity.disc_volume_start_lba);
    append_value(identity.disc_extent_lba_bias);
    append(options.console_profile);
    append_value(static_cast<std::underlying_type_t<
        katana::codegen::LatentAotDiscoveryMode>>(
        options.latent_aot_discovery_mode));

    std::vector<katana::codegen::LatentAotEntryHint> hints(
        options.latent_aot_entry_hints.begin(),
        options.latent_aot_entry_hints.end());
    std::sort(hints.begin(), hints.end(), [](const auto& left, const auto& right) {
        return std::tie(left.byte_identity,
                        left.disc_byte_offset,
                        left.byte_size,
                        left.module_relative_offset) <
               std::tie(right.byte_identity,
                        right.disc_byte_offset,
                        right.byte_size,
                        right.module_relative_offset);
    });
    append_value(hints.size());
    for (const auto& hint : hints) {
        append(hint.byte_identity);
        append_value(hint.disc_byte_offset);
        append_value(hint.byte_size);
        append_value(hint.module_relative_offset);
    }

    std::vector<std::uint32_t> resume_entries(
        options.native_aot_resume_entries.begin(),
        options.native_aot_resume_entries.end());
    std::sort(resume_entries.begin(), resume_entries.end());
    resume_entries.erase(
        std::unique(resume_entries.begin(), resume_entries.end()),
        resume_entries.end());
    append_value(resume_entries.size());
    for (const auto entry : resume_entries) append_value(entry);

    std::vector<std::pair<std::string, std::string>> runtime_payloads;
    runtime_payloads.reserve(
        options.game_project_runtime_image_payloads.size());
    for (const auto& payload : options.game_project_runtime_image_payloads) {
        runtime_payloads.emplace_back(
            payload.image_id,
            katana::io::sha256_bytes(std::string_view(
                reinterpret_cast<const char*>(payload.bytes.data()),
                payload.bytes.size())));
    }
    std::sort(runtime_payloads.begin(), runtime_payloads.end());
    append_value(runtime_payloads.size());
    for (const auto& [id, hash] : runtime_payloads) {
        append(id);
        append(hash);
    }

    std::vector<std::pair<std::uint32_t, std::string>> bootstrap_payloads;
    bootstrap_payloads.reserve(
        options.native_port_bootstrap_write_payloads.size());
    for (const auto& payload : options.native_port_bootstrap_write_payloads) {
        bootstrap_payloads.emplace_back(
            payload.guest_address,
            katana::io::sha256_bytes(std::string_view(
                reinterpret_cast<const char*>(payload.bytes.data()),
                payload.bytes.size())));
    }
    std::sort(bootstrap_payloads.begin(), bootstrap_payloads.end());
    append_value(bootstrap_payloads.size());
    for (const auto& [address, hash] : bootstrap_payloads) {
        append_value(address);
        append(hash);
    }
    return katana::io::sha256_bytes(material.str());
}

struct AgentAnalysisModuleAuthority final {
    std::string id;
    std::string byte_identity;
    std::uint32_t byte_size = 0u;
    std::uint32_t source_address = 0u;
    std::vector<katana::codegen::PreparedLatentAotSourceBinding>
        source_bindings;
    std::vector<std::uint32_t> entry_offsets;
    std::vector<std::uint32_t> instruction_addresses;
};

struct AgentAnalysisAuthorityBaseline final {
    std::vector<std::uint32_t> external_primary_roots;
    std::vector<std::uint32_t> native_resume_entries;
    std::vector<std::uint32_t> primary_function_entries;
    std::vector<std::uint32_t> primary_instruction_addresses;
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>
        primary_call_edges;
    std::vector<AgentAnalysisModuleAuthority> modules;
    katana::codegen::NativeDiscProgramIndexCheckpoint program_index;
    bool guarded_inventory_complete = false;
    bool native_hardware_closure_complete = false;
    bool replacement_reachability_proven = false;
    bool backend_admitted = false;
};

std::vector<std::uint32_t> latent_module_instruction_addresses(
    const katana::codegen::PreparedLatentAotModule& module) {
    std::vector<std::uint32_t> addresses;
    for (const auto& function : module.program)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                addresses.push_back(instruction.source_address);
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(
        std::unique(addresses.begin(), addresses.end()), addresses.end());
    return addresses;
}

AgentAnalysisAuthorityBaseline capture_agent_analysis_authority(
    const katana::codegen::NativeDiscAnalysisArtifact& artifact) {
    AgentAnalysisAuthorityBaseline result;
    result.external_primary_roots = artifact.external_primary_roots;
    result.native_resume_entries = artifact.native_resume_entries;
    result.primary_function_entries.reserve(
        artifact.primary.lowered_program.size());
    for (const auto& function : artifact.primary.lowered_program)
        result.primary_function_entries.push_back(function.entry_address);
    std::sort(
        result.primary_function_entries.begin(),
        result.primary_function_entries.end());
    result.primary_function_entries.erase(
        std::unique(result.primary_function_entries.begin(),
                    result.primary_function_entries.end()),
        result.primary_function_entries.end());
    result.primary_instruction_addresses.reserve(
        artifact.primary.analysis.recursive.instructions.size());
    for (const auto& instruction :
         artifact.primary.analysis.recursive.instructions)
        result.primary_instruction_addresses.push_back(instruction.address);
    std::sort(result.primary_instruction_addresses.begin(),
              result.primary_instruction_addresses.end());
    result.primary_instruction_addresses.erase(
        std::unique(result.primary_instruction_addresses.begin(),
                    result.primary_instruction_addresses.end()),
        result.primary_instruction_addresses.end());
    for (const auto& edge : artifact.primary.call_graph.edges)
        if (edge.target.has_value())
            result.primary_call_edges.emplace_back(
                edge.source, *edge.target, edge.callsite);
    std::sort(
        result.primary_call_edges.begin(), result.primary_call_edges.end());
    result.primary_call_edges.erase(
        std::unique(result.primary_call_edges.begin(),
                    result.primary_call_edges.end()),
        result.primary_call_edges.end());
    result.modules.reserve(artifact.latent.modules.size());
    for (const auto& module : artifact.latent.modules) {
        result.modules.push_back({
            module.id,
            module.byte_identity,
            module.byte_size,
            module.source_address,
            module.source_bindings,
            module.entry_offsets,
            latent_module_instruction_addresses(module)});
    }
    result.guarded_inventory_complete = artifact.guarded_inventory_complete;
    result.native_hardware_closure_complete =
        artifact.native_hardware_closure_complete;
    result.replacement_reachability_proven =
        artifact.replacement_reachability_proven;
    result.backend_admitted = artifact.backend_admitted;
    result.program_index = artifact.native_port_program_index;
    return result;
}

template <typename Value>
bool sorted_authority_subset(const std::vector<Value>& required,
                             const std::vector<Value>& candidate) {
    return std::includes(
        candidate.begin(), candidate.end(), required.begin(), required.end());
}

bool program_index_adjacency_subset(
    const std::vector<katana::codegen::NativeDiscProgramIndexAdjacency>&
        required,
    const std::vector<katana::codegen::NativeDiscProgramIndexAdjacency>&
        candidate) {
    return std::all_of(
        required.begin(), required.end(), [&](const auto& entry) {
            const auto found = std::lower_bound(
                candidate.begin(),
                candidate.end(),
                entry.address,
                [](const auto& value, const std::uint32_t address) {
                    return value.address < address;
                });
            return found != candidate.end() &&
                   found->address == entry.address &&
                   sorted_authority_subset(
                       entry.related_addresses,
                       found->related_addresses);
        });
}

std::optional<std::string_view> agent_analysis_authority_rejection(
    const AgentAnalysisAuthorityBaseline& baseline,
    const katana::codegen::NativeDiscAnalysisArtifact& candidate,
    const AgentIterationDelta& world_delta) {
    if (!sorted_authority_subset(
            baseline.external_primary_roots,
            candidate.external_primary_roots))
        return "external-primary-roots-lost";
    if (!sorted_authority_subset(
            baseline.native_resume_entries,
            candidate.native_resume_entries))
        return "native-resume-entries-lost";

    std::vector<std::uint32_t> candidate_function_entries;
    candidate_function_entries.reserve(candidate.primary.lowered_program.size());
    for (const auto& function : candidate.primary.lowered_program)
        candidate_function_entries.push_back(function.entry_address);
    std::sort(candidate_function_entries.begin(), candidate_function_entries.end());
    candidate_function_entries.erase(
        std::unique(candidate_function_entries.begin(),
                    candidate_function_entries.end()),
        candidate_function_entries.end());
    if (!sorted_authority_subset(
            baseline.primary_function_entries, candidate_function_entries))
        return "primary-functions-lost";

    std::vector<std::uint32_t> candidate_instruction_addresses;
    candidate_instruction_addresses.reserve(
        candidate.primary.analysis.recursive.instructions.size());
    for (const auto& instruction :
         candidate.primary.analysis.recursive.instructions)
        candidate_instruction_addresses.push_back(instruction.address);
    std::sort(candidate_instruction_addresses.begin(),
              candidate_instruction_addresses.end());
    candidate_instruction_addresses.erase(
        std::unique(candidate_instruction_addresses.begin(),
                    candidate_instruction_addresses.end()),
        candidate_instruction_addresses.end());
    if (!sorted_authority_subset(
            baseline.primary_instruction_addresses,
            candidate_instruction_addresses))
        return "primary-instructions-lost";

    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>
        candidate_call_edges;
    for (const auto& edge : candidate.primary.call_graph.edges)
        if (edge.target.has_value())
            candidate_call_edges.emplace_back(
                edge.source, *edge.target, edge.callsite);
    std::sort(candidate_call_edges.begin(), candidate_call_edges.end());
    candidate_call_edges.erase(
        std::unique(candidate_call_edges.begin(),
                    candidate_call_edges.end()),
        candidate_call_edges.end());
    if (!sorted_authority_subset(
            baseline.primary_call_edges, candidate_call_edges))
        return "primary-call-edges-lost";

    for (const auto& required : baseline.modules) {
        const auto found = std::find_if(
            candidate.latent.modules.begin(),
            candidate.latent.modules.end(),
            [&](const auto& module) {
                return module.id == required.id &&
                       module.byte_identity == required.byte_identity &&
                       module.byte_size == required.byte_size &&
                       module.source_address == required.source_address;
            });
        if (found == candidate.latent.modules.end() ||
            !std::all_of(
                required.source_bindings.begin(),
                required.source_bindings.end(),
                [&](const auto& binding) {
                    return std::find(
                               found->source_bindings.begin(),
                               found->source_bindings.end(),
                               binding) != found->source_bindings.end();
                }) ||
            !sorted_authority_subset(
                required.entry_offsets, found->entry_offsets) ||
            !sorted_authority_subset(
                required.instruction_addresses,
                latent_module_instruction_addresses(*found)))
            return "latent-materialization-lost";
    }
    if (!program_index_adjacency_subset(
            baseline.program_index.incoming_edge_sources,
            candidate.native_port_program_index.incoming_edge_sources) ||
        !program_index_adjacency_subset(
            baseline.program_index.incoming_instruction_addresses,
            candidate.native_port_program_index
                .incoming_instruction_addresses) ||
        !program_index_adjacency_subset(
            baseline.program_index.outgoing_function_entries,
            candidate.native_port_program_index
                .outgoing_function_entries) ||
        !sorted_authority_subset(
            baseline.program_index.incomplete_outgoing_function_entries,
            candidate.native_port_program_index
                .incomplete_outgoing_function_entries) ||
        !sorted_authority_subset(
            baseline.program_index.seed_entries,
            candidate.native_port_program_index.seed_entries))
        return "native-program-index-facts-lost";
    if ((baseline.guarded_inventory_complete &&
         !candidate.guarded_inventory_complete) ||
        (baseline.native_hardware_closure_complete &&
         !candidate.native_hardware_closure_complete) ||
        (baseline.replacement_reachability_proven &&
         !candidate.replacement_reachability_proven) ||
        (baseline.backend_admitted && !candidate.backend_admitted) ||
        world_delta.proof_downgrades != 0u)
        return "proof-downgrade";
    return std::nullopt;
}

std::optional<std::string> active_pending_agent_analysis_slot(
    const std::filesystem::path& pending_root) {
    const auto active_path = pending_root / "active-slot.txt";
    if (!safe_regular_port_file_exists(
            active_path, "Aktiver Analyse-Kandidatenslot"))
        return std::nullopt;
    const auto active = read_safe_small_port_file(
        active_path, 32u, "Aktiver Analyse-Kandidatenslot");
    if (active == "slot-0\n") return std::string("slot-0");
    if (active == "slot-1\n") return std::string("slot-1");
    throw std::runtime_error(
        "Aktiver Analyse-Kandidatenslot ist ungueltig.");
}

void write_pending_agent_analysis_candidate_manifest_at_slot(
    const std::filesystem::path& slot_root,
    const std::string_view slot,
    const katana::codegen::NativeDiscAnalysisResult& analyzed,
    const std::string_view authority,
    const std::string_view reason,
    const std::string_view analysis_session_contract_identity) {
    static constexpr std::array<std::string_view, 3u> allowed_authorities{
        "unvalidated", "rejected", "validated"};
    if (std::find(
            allowed_authorities.begin(),
            allowed_authorities.end(),
            authority) == allowed_authorities.end() ||
        reason.empty() || reason.size() > 1024u)
        throw std::invalid_argument(
            "Analyse-Kandidatenmanifest besitzt einen ungueltigen Zustand.");
    const auto manifest_path = slot_root / "candidate.json";
    auto world_json = analyzed.materialization_world_json;
    world_json.push_back('\n');
    const auto archive_sha256 = katana::io::sha256_bytes(
        std::string_view(
            reinterpret_cast<const char*>(
                analyzed.analysis_artifact_bytes.data()),
            analyzed.analysis_artifact_bytes.size()));
    const auto world_sha256 = katana::io::sha256_bytes(
        std::string_view(
            reinterpret_cast<const char*>(
                analyzed.materialization_world_artifact_bytes.data()),
            analyzed.materialization_world_artifact_bytes.size()));
    std::ostringstream manifest;
    manifest
        << "{\"schema\":3,\"kind\":\"katana-pending-analysis-candidate\""
        << ",\"slot\":" << katana::io::quote_json(slot)
        << ",\"authority\":" << katana::io::quote_json(authority)
        << ",\"reason\":" << katana::io::quote_json(reason)
        << ",\"analysis_artifact_id\":"
        << katana::io::quote_json(
               analyzed.analysis_artifact_identity.key)
        << ",\"analysis_session_contract_identity\":"
        << katana::io::quote_json(
               analysis_session_contract_identity)
        << ",\"analysis_archive_sha256\":"
        << katana::io::quote_json(archive_sha256)
        << ",\"materialization_world_sha256\":"
        << katana::io::quote_json(world_sha256)
        << ",\"materialization_world_json_sha256\":"
        << katana::io::quote_json(
               katana::io::sha256_bytes(world_json))
        << "}\n";
    write_atomic_analysis_file(
        slot_root,
        manifest_path,
        manifest.str(),
        "Analyse-Kandidatenmanifest");
}

void write_pending_agent_analysis_candidate_manifest(
    const std::filesystem::path& output_root,
    const katana::codegen::NativeDiscAnalysisResult& analyzed,
    const std::string_view authority,
    const std::string_view reason,
    const std::string_view analysis_session_contract_identity) {
    const auto pending_root =
        output_root / ".katana" / "agent" / "pending-analysis";
    ensure_safe_port_directory_chain(
        output_root,
        pending_root,
        "Nichtautoritative Analyse-Kandidatengeneration");
    const auto active = active_pending_agent_analysis_slot(pending_root);
    if (!active.has_value())
        throw std::runtime_error(
            "Analyse-Kandidatenmanifest besitzt keinen aktiven Slot.");
    const auto slot_root = pending_root / *active;
    if (!safe_regular_port_directory_exists(
            slot_root, "Aktiver Analyse-Kandidatenslot"))
        throw std::runtime_error(
            "Aktiver Analyse-Kandidatenslot fehlt.");
    write_pending_agent_analysis_candidate_manifest_at_slot(
        slot_root,
        *active,
        analyzed,
        authority,
        reason,
        analysis_session_contract_identity);
}

void publish_pending_agent_analysis_candidate(
    const std::filesystem::path& output_root,
    const katana::codegen::NativeDiscAnalysisResult& analyzed,
    const std::string_view analysis_session_contract_identity) {
    const auto pending_root =
        output_root / ".katana" / "agent" / "pending-analysis";
    ensure_safe_port_directory_chain(
        output_root,
        pending_root,
        "Nichtautoritative Analyse-Kandidatengeneration");
    const auto active = active_pending_agent_analysis_slot(pending_root);
    const std::string_view target_slot =
        active == std::optional<std::string>{"slot-0"}
            ? std::string_view("slot-1")
            : std::string_view("slot-0");
    const auto slot_root = pending_root / target_slot;
    ensure_safe_port_directory_chain(
        pending_root,
        slot_root,
        "Nichtautoritativer Analyse-Kandidatenslot");
    const auto archive_path =
        slot_root / "native-disc-analysis.katana-analysis";
    const auto world_path =
        slot_root / "materialization-world.katana-world";
    const auto world_json_path =
        slot_root / "materialization-world.json";
    // Build a complete generation in the inactive slot.  The single active
    // pointer replacement below is the only commit: until then a crash leaves
    // the preceding complete slot authoritative and untouched.
    write_atomic_analysis_file(
        slot_root,
        archive_path,
        analyzed.analysis_artifact_bytes,
        "Nichtautoratives Analysearchiv");
    write_atomic_analysis_file(
        slot_root,
        world_path,
        analyzed.materialization_world_artifact_bytes,
        "Nichtautoritativer Materialization-World");
    auto world_json = analyzed.materialization_world_json;
    world_json.push_back('\n');
    write_atomic_analysis_file(
        slot_root,
        world_json_path,
        world_json,
        "Nichtautoritativer Materialization-World-JSON");
    write_pending_agent_analysis_candidate_manifest_at_slot(
        slot_root,
        target_slot,
        analyzed,
        "unvalidated",
        "authority-validation-pending",
        analysis_session_contract_identity);
    const auto active_path = pending_root / "active-slot.txt";
    const auto active_document = std::string(target_slot) + "\n";
    write_atomic_analysis_file(
        pending_root,
        active_path,
        active_document,
        "Aktiver Analyse-Kandidatenslot");
}

bool same_noop_analysis_manifest_identity(
    katana::codegen::NativeDiscAnalysisArtifactIdentity committed,
    katana::codegen::NativeDiscAnalysisArtifactIdentity current) {
    // Schema-6 checkpoints deliberately omit downstream codegen identity.
    // The terminal ledger producer identity binds the current codegen/World
    // contract independently.
    committed.codegen_implementation_identity.clear();
    current.codegen_implementation_identity.clear();
    return committed == current;
}

AgentSessionLedgerState validate_agent_session_ledger(
    const std::string_view ledger) {
    AgentSessionLedgerState state;
    if (ledger.empty()) return state;
    if (ledger.back() != '\n')
        throw std::invalid_argument(
            "Agent-Session-Ledger endet nicht an einer Datensatzgrenze.");
    static constexpr std::array<std::string_view, 21u> schema_one_keys{
        "schema", "kind", "task_id", "analysis_artifact_id", "commit",
        "result", "resolved_frontiers", "new_frontiers", "new_runtime_hints",
        "proof_upgrades", "proof_downgrades", "new_incomplete_roots",
        "resolved_incomplete_roots", "analysis_wall_time_ms",
        "analysis_wall_time_delta_ms", "boot_analysis_cache_hit",
        "boot_analysis_pipeline_runs", "latent_root_seed_cache_hit",
        "analysis_artifact_cache_hit", "runtime_frontiers_imported", "next_action"};
    static constexpr std::array<std::string_view, 22u> schema_two_keys{
        "schema", "kind", "task_id", "analysis_artifact_id", "commit",
        "result", "resolved_frontiers", "new_frontiers", "new_runtime_hints",
        "proof_upgrades", "proof_downgrades", "new_incomplete_roots",
        "resolved_incomplete_roots", "timing_kind",
        "telemetry_analysis_wall_time_ms", "telemetry_analysis_wall_time_delta_ms",
        "boot_analysis_cache_hit", "boot_analysis_pipeline_runs",
        "latent_root_seed_cache_hit", "analysis_artifact_cache_hit",
        "runtime_frontiers_imported", "next_action"};
    static constexpr std::array<std::string_view, 26u> schema_three_keys{
        "schema", "kind", "task_id", "analysis_artifact_id", "commit",
        "result", "resolved_frontiers", "new_frontiers", "new_runtime_hints",
        "proof_upgrades", "proof_downgrades", "new_incomplete_roots",
        "resolved_incomplete_roots", "timing_kind",
        "telemetry_analysis_wall_time_ms", "telemetry_analysis_wall_time_delta_ms",
        "boot_analysis_cache_hit", "boot_analysis_pipeline_runs",
        "latent_root_seed_cache_hit", "analysis_artifact_cache_hit",
        "runtime_frontiers_imported", "next_action",
        "materialization_world_sha256", "materialization_world_json_sha256",
        "analysis_report_sha256", "analysis_archive_sha256"};
    static constexpr std::array<std::string_view, 27u> schema_four_keys{
        "schema", "kind", "task_id", "analysis_artifact_id", "commit",
        "result", "resolved_frontiers", "new_frontiers", "new_runtime_hints",
        "proof_upgrades", "proof_downgrades", "new_incomplete_roots",
        "resolved_incomplete_roots", "timing_kind",
        "telemetry_analysis_wall_time_ms", "telemetry_analysis_wall_time_delta_ms",
        "boot_analysis_cache_hit", "boot_analysis_pipeline_runs",
        "latent_root_seed_cache_hit", "analysis_artifact_cache_hit",
        "runtime_frontiers_imported", "next_action", "producer_identity",
        "materialization_world_sha256", "materialization_world_json_sha256",
        "analysis_report_sha256", "analysis_archive_sha256"};
    static constexpr std::array<std::string_view, 28u> schema_five_keys{
        "schema", "kind", "task_id", "analysis_artifact_id", "commit",
        "result", "resolved_frontiers", "new_frontiers", "new_runtime_hints",
        "proof_upgrades", "proof_downgrades", "new_incomplete_roots",
        "resolved_incomplete_roots", "timing_kind",
        "telemetry_analysis_wall_time_ms", "telemetry_analysis_wall_time_delta_ms",
        "boot_analysis_cache_hit", "boot_analysis_pipeline_runs",
        "latent_root_seed_cache_hit", "analysis_artifact_cache_hit",
        "runtime_frontiers_imported", "next_action", "producer_identity",
        "analysis_session_contract_identity", "materialization_world_sha256",
        "materialization_world_json_sha256", "analysis_report_sha256",
        "analysis_archive_sha256"};
    static constexpr std::array<std::string_view, 8u> numeric_keys{
        "task_id", "resolved_frontiers", "new_frontiers", "new_runtime_hints",
        "proof_upgrades", "proof_downgrades", "new_incomplete_roots",
        "resolved_incomplete_roots"};
    std::size_t cursor = 0u;
    while (cursor < ledger.size()) {
        const auto newline = ledger.find('\n', cursor);
        if (newline == std::string_view::npos)
            throw std::invalid_argument(
                "Agent-Session-Ledger besitzt eine unvollstaendige Zeile.");
        const auto line = ledger.substr(cursor, newline - cursor);
        cursor = newline + 1u;
        if (line.empty() || line.size() > maximum_agent_session_line_bytes)
            throw std::invalid_argument(
                "Agent-Session-Ledger besitzt eine leere oder zu lange Zeile.");
        StrictJsonObject object;
        parse_strict_json_object(line, object);
        const auto schema = strict_json_u32(object, "schema");
        state.committed_generation = false;
        state.producer_identity.clear();
        state.analysis_session_contract_identity.clear();
        state.materialization_world_sha256.clear();
        state.materialization_world_json_sha256.clear();
        state.analysis_report_sha256.clear();
        state.analysis_archive_sha256.reset();
        if (schema == 1u) {
            require_strict_json_keys(object, schema_one_keys);
            state.previous_wall_time_ms =
                strict_json_u64(object, "analysis_wall_time_ms");
            const auto* delta = strict_json_field(object, "analysis_wall_time_delta_ms");
            if (delta == nullptr ||
                (delta->kind != StrictJsonValueKind::Number &&
                 delta->kind != StrictJsonValueKind::Null))
                throw std::invalid_argument("Agent-Session-Ledger besitzt ein ungueltiges Zeitdelta.");
        } else if (schema == 2u) {
            require_strict_json_keys(object, schema_two_keys);
            if (strict_json_identifier(object, "timing_kind", 64u) !=
                "nondeterministic-telemetry")
                throw std::invalid_argument("Agent-Session-Ledger besitzt eine ungueltige Zeitsemantik.");
            state.previous_wall_time_ms =
                strict_json_u64(object, "telemetry_analysis_wall_time_ms");
            const auto* delta = strict_json_field(object, "telemetry_analysis_wall_time_delta_ms");
            if (delta == nullptr ||
                (delta->kind != StrictJsonValueKind::Number &&
                 delta->kind != StrictJsonValueKind::Null))
                throw std::invalid_argument("Agent-Session-Ledger besitzt ein ungueltiges Zeitdelta.");
        } else if (schema == 3u || schema == 4u || schema == 5u) {
            require_strict_json_keys(
                object,
                schema == 3u
                    ? std::span<const std::string_view>(schema_three_keys)
                    : schema == 4u
                        ? std::span<const std::string_view>(schema_four_keys)
                        : std::span<const std::string_view>(schema_five_keys));
            if (strict_json_identifier(object, "timing_kind", 64u) !=
                "nondeterministic-telemetry")
                throw std::invalid_argument(
                    "Agent-Session-Ledger besitzt eine ungueltige Zeitsemantik.");
            state.previous_wall_time_ms = strict_json_u64(
                object, "telemetry_analysis_wall_time_ms");
            const auto* delta = strict_json_field(
                object, "telemetry_analysis_wall_time_delta_ms");
            if (delta == nullptr ||
                (delta->kind != StrictJsonValueKind::Number &&
                 delta->kind != StrictJsonValueKind::Null))
                throw std::invalid_argument(
                    "Agent-Session-Ledger besitzt ein ungueltiges Zeitdelta.");
            state.materialization_world_sha256 = strict_json_identifier(
                object, "materialization_world_sha256", 64u);
            state.materialization_world_json_sha256 = strict_json_identifier(
                object, "materialization_world_json_sha256", 64u);
            state.analysis_report_sha256 = strict_json_identifier(
                object, "analysis_report_sha256", 64u);
            const auto* archive = strict_json_field(
                object, "analysis_archive_sha256");
            if (archive == nullptr ||
                (archive->kind != StrictJsonValueKind::String &&
                 archive->kind != StrictJsonValueKind::Null))
                throw std::invalid_argument(
                    "Agent-Session-Ledger besitzt eine ungueltige "
                    "Analysearchividentitaet.");
            if (archive->kind == StrictJsonValueKind::String) {
                state.analysis_archive_sha256 = strict_json_identifier(
                    object, "analysis_archive_sha256", 64u);
            } else {
                state.analysis_archive_sha256.reset();
            }
            if (schema >= 4u)
                state.producer_identity = strict_json_identifier(
                    object, "producer_identity", 64u);
            if (schema == 5u)
                state.analysis_session_contract_identity =
                    strict_json_identifier(
                        object,
                        "analysis_session_contract_identity",
                        64u);
            state.committed_generation = true;
        } else {
            throw std::invalid_argument("Agent-Session-Ledger besitzt eine unbekannte Schema-Version.");
        }
        if (strict_json_identifier(object, "kind", 64u) != "katana-agent-session")
            throw std::invalid_argument("Agent-Session-Ledger besitzt einen ungueltigen Datensatztyp.");
        state.analysis_artifact_id =
            strict_json_identifier(object, "analysis_artifact_id", 512u);
        static_cast<void>(strict_json_identifier(object, "commit", 256u));
        static_cast<void>(strict_json_identifier(object, "result", 64u));
        static_cast<void>(strict_json_identifier(object, "next_action", 128u));
        for (const auto key : numeric_keys)
            static_cast<void>(strict_json_u64(object, key));
        static_cast<void>(strict_json_u64(object, "boot_analysis_pipeline_runs"));
        static_cast<void>(strict_json_u64(object, "runtime_frontiers_imported"));
        static_cast<void>(strict_json_bool(object, "boot_analysis_cache_hit"));
        static_cast<void>(strict_json_bool(object, "latent_root_seed_cache_hit"));
        static_cast<void>(strict_json_bool(object, "analysis_artifact_cache_hit"));
    }
    if (!state.previous_wall_time_ms.has_value())
        throw std::invalid_argument(
            "Agent-Session-Ledger besitzt keinen messbaren Zeitwert.");
    return state;
}

struct CommittedAgentGeneration final {
    katana::agent::ExecutableMaterializationWorld world;
    std::string analysis_artifact_id;
    std::string analysis_archive;
    std::string producer_identity;
    std::string analysis_session_contract_identity;
};

CommittedAgentGeneration load_committed_agent_generation(
    const std::filesystem::path& output_root) {
    const auto ledger = read_safe_small_port_file(
        output_root / ".katana" / "agent" / "session.jsonl",
        maximum_agent_session_ledger_bytes,
        "Agent-Session-Ledger");
    const auto state = validate_agent_session_ledger(ledger);
    if (!state.committed_generation || state.analysis_artifact_id.empty())
        throw std::invalid_argument(
            "analyze-port --resume verweigert ein nicht transaktional "
            "publiziertes Agent-Artefakt.");

    std::string world_sha256;
    auto world = load_agent_world(
        output_root / "materialization-world.katana-world",
        &world_sha256);
    const auto world_json = read_safe_small_port_file(
        output_root / "materialization-world.json",
        katana::agent::materialization_world_max_binary_artifact_bytes,
        "Materialization-World-JSON");
    const auto report = read_safe_small_port_file(
        output_root / "native-disc-analysis.json",
        maximum_agent_session_ledger_bytes,
        "NativeDisc-Analysebericht");
    if (world_sha256 != state.materialization_world_sha256 ||
        katana::io::sha256_bytes(world_json) !=
            state.materialization_world_json_sha256 ||
        katana::io::sha256_bytes(report) != state.analysis_report_sha256)
        throw std::invalid_argument(
            "analyze-port --resume verweigert eine unvollstaendige oder "
            "nachtraeglich veraenderte Analysepublikation.");

    std::string archive;
    if (state.analysis_archive_sha256.has_value()) {
        archive = read_safe_small_port_file(
            output_root / "native-disc-analysis.katana-analysis",
            katana::codegen::maximum_native_disc_analysis_artifact_bytes,
            "NativeDisc-Analysearchiv");
        if (katana::io::sha256_bytes(archive) !=
            *state.analysis_archive_sha256)
            throw std::invalid_argument(
                "analyze-port --resume verweigert ein veraendertes "
                "NativeDisc-Analysearchiv.");
    }
    return {
        std::move(world), state.analysis_artifact_id, std::move(archive),
        state.producer_identity,
        state.analysis_session_contract_identity};
}

void append_agent_session_ledger(
    const std::filesystem::path& output_root,
    const katana::codegen::NativeDiscAnalysisResult& analyzed,
    const std::optional<katana::agent::ExecutableMaterializationWorld>&
        previous_world,
    const katana::agent::ExecutableMaterializationWorld& current_world,
    const std::uint64_t analysis_wall_time_ms,
    const std::size_t imported_runtime_frontiers,
    const std::string_view materialization_world_sha256,
    const std::string_view materialization_world_json_sha256,
    const std::string_view analysis_report_sha256,
    const std::optional<std::string_view> analysis_archive_sha256,
    const std::string_view producer_identity,
    const std::string_view analysis_session_contract_identity) {
    const auto agent_root = output_root / ".katana" / "agent";
    ensure_safe_port_directory_chain(
        output_root, agent_root, "Privates Agent-Session-Verzeichnis");
    const auto ledger_path = agent_root / "session.jsonl";
    std::string ledger;
    if (safe_regular_port_file_exists(
            ledger_path, "Agent-Session-Ledger"))
        ledger = read_safe_small_port_file(
            ledger_path,
            maximum_agent_session_ledger_bytes,
            "Agent-Session-Ledger");
    const auto prior_state = validate_agent_session_ledger(ledger);
    const auto prior_wall_time = prior_state.previous_wall_time_ms;
    if (prior_wall_time.has_value() &&
        *prior_wall_time >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument(
            "Agent-Session-Ledger besitzt einen ueberlaufenden Zeitwert.");
    AgentIterationDelta delta;
    std::uint64_t task_id = analyzed.agent_decision_focus;
    if (previous_world.has_value()) {
        delta = measure_agent_iteration(*previous_world, current_world);
        task_id = katana::agent::evaluate_agent_decision(*previous_world)
                      .focus.value;
    }
    const auto result = !previous_world.has_value()
        ? "initial"
        : delta.proof_downgrades != 0u || delta.new_frontiers != 0u ||
                  delta.new_incomplete_roots != 0u
            ? "regression"
            : delta.resolved_frontiers != 0u ||
                      delta.proof_upgrades != 0u
                ? "improved"
                : "no_progress";
    std::ostringstream line;
    line << "{\"schema\":5,\"kind\":\"katana-agent-session\""
         << ",\"task_id\":" << task_id
         << ",\"analysis_artifact_id\":"
         << katana::io::quote_json(analyzed.analysis_artifact_identity.key)
         << ",\"commit\":"
         << katana::io::quote_json(
                katana::build_contract::katana_git_commit)
         << ",\"result\":" << katana::io::quote_json(result)
         << ",\"resolved_frontiers\":" << delta.resolved_frontiers
         << ",\"new_frontiers\":" << delta.new_frontiers
         << ",\"new_runtime_hints\":" << delta.new_runtime_hints
         << ",\"proof_upgrades\":" << delta.proof_upgrades
         << ",\"proof_downgrades\":" << delta.proof_downgrades
         << ",\"new_incomplete_roots\":"
         << delta.new_incomplete_roots
         << ",\"resolved_incomplete_roots\":"
         << delta.resolved_incomplete_roots
         << ",\"timing_kind\":\"nondeterministic-telemetry\""
         << ",\"telemetry_analysis_wall_time_ms\":"
         << analysis_wall_time_ms
         << ",\"telemetry_analysis_wall_time_delta_ms\":";
    if (prior_wall_time.has_value())
        line << static_cast<std::int64_t>(analysis_wall_time_ms) -
                    static_cast<std::int64_t>(*prior_wall_time);
    else
        line << "null";
    line << ",\"boot_analysis_cache_hit\":"
         << (analyzed.boot_analysis_cache_hit ? "true" : "false")
         << ",\"boot_analysis_pipeline_runs\":"
         << analyzed.boot_analysis_pipeline_runs
         << ",\"latent_root_seed_cache_hit\":"
         << (analyzed.latent_primary_root_seed_cache_hit ? "true" : "false")
         << ",\"analysis_artifact_cache_hit\":"
         << (analyzed.analysis_artifact_cache_hit ? "true" : "false")
         << ",\"runtime_frontiers_imported\":"
         << imported_runtime_frontiers
         << ",\"next_action\":"
         << katana::io::quote_json(analyzed.agent_decision)
         << ",\"producer_identity\":"
         << katana::io::quote_json(producer_identity)
         << ",\"analysis_session_contract_identity\":"
         << katana::io::quote_json(analysis_session_contract_identity)
         << ",\"materialization_world_sha256\":"
         << katana::io::quote_json(materialization_world_sha256)
         << ",\"materialization_world_json_sha256\":"
         << katana::io::quote_json(materialization_world_json_sha256)
         << ",\"analysis_report_sha256\":"
         << katana::io::quote_json(analysis_report_sha256)
         << ",\"analysis_archive_sha256\":";
    if (analysis_archive_sha256.has_value())
        line << katana::io::quote_json(*analysis_archive_sha256);
    else
        line << "null";
    line
         << "}\n";
    const auto record = line.str();
    if (record.size() > maximum_agent_session_line_bytes ||
        ledger.size() > maximum_agent_session_ledger_bytes -
                            std::min(
                                record.size(),
                                maximum_agent_session_ledger_bytes))
        throw std::runtime_error(
            "Agent-Session-Ledger ueberschreitet sein festes Budget.");
    ledger.append(record);
    write_atomic_analysis_file(
        output_root,
        ledger_path,
        ledger,
        "Agent-Session-Ledger");
}

std::size_t resolved_runtime_jobs() noexcept {
    constexpr std::size_t maximum_jobs = 64u;
    const auto detected = std::clamp<std::size_t>(
        std::max(1u, std::thread::hardware_concurrency()),
        1u,
        maximum_jobs);
    const auto configured =
        configured_environment_value("KATANA_RUNTIME_JOBS");
    if (!configured) return detected;
    std::size_t requested = 0u;
    const auto conversion = std::from_chars(
        configured->data(),
        configured->data() + configured->size(),
        requested,
        10);
    if (conversion.ec != std::errc{} ||
        conversion.ptr !=
            configured->data() + configured->size() ||
        requested == 0u)
        return detected;
    return std::clamp<std::size_t>(requested, 1u, detected);
}

int export_port_project(const std::filesystem::path& source_path,
                        const std::filesystem::path& output_path,
                        const std::string& target_name,
                        const bool diagnostic_partial = false,
                        const std::string& console_profile = "japan-ntsc",
                        const bool boot_executable_artifact = false,
                        const std::optional<std::filesystem::path>&
                            game_project_path = std::nullopt,
                        const std::optional<std::filesystem::path>&
                            native_port_definition_path = std::nullopt,
                        const std::optional<std::filesystem::path>&
                            game_entry_handoff_path = std::nullopt,
                        const std::vector<RuntimeImagePayloadArgument>&
                            runtime_image_payload_arguments = {},
                        const std::vector<NativeBootstrapWritePayloadArgument>&
                            bootstrap_write_payload_arguments = {},
                        const std::vector<LatentAotEntryHintArgument>&
                            latent_aot_entry_hints = {},
                         const LatentAotDiscoveryModeArgument
                             latent_aot_discovery_mode =
                                 LatentAotDiscoveryModeArgument::
                                      HintsAndHeuristics,
                         const std::vector<std::uint32_t>&
                             native_aot_resume_entries = {},
                          const std::optional<std::filesystem::path>&
                              telemetry_jsonl_path = std::nullopt,
                         const bool detailed_analysis_telemetry = false,
                         const PortAnalysisMode analysis_mode =
                            PortAnalysisMode::PlatformAbi,
                        const bool analysis_only = false,
                        const bool resume_analysis = false,
                        const bool refresh_analysis = false,
                        const std::optional<std::filesystem::path>&
                            runtime_frontier_import_path = std::nullopt) {
    if (!valid_port_target_name(target_name))
        throw std::invalid_argument(
            "--target-name ist kein sicherer CMake-Targetname.");
    const auto host_compile_budget =
        configured_host_compile_budget();
    const auto normalized_latent_aot_entry_hints =
        normalize_latent_aot_entry_hints(latent_aot_entry_hints);
    if (!normalized_latent_aot_entry_hints.empty() &&
        (diagnostic_partial || boot_executable_artifact))
        throw std::invalid_argument(
            "--latent-aot-entry ist ausschliesslich fuer vollstaendige "
            "NativeDisc-Produktports erlaubt.");
    if (analysis_only && boot_executable_artifact)
        throw std::invalid_argument(
            "analyze-port akzeptiert nur eine NativeDisc-GDI-Quelle.");
    if ((resume_analysis || runtime_frontier_import_path.has_value()) &&
        !analysis_only)
        throw std::invalid_argument(
            "--resume und --import-runtime-frontier sind ausschliesslich "
            "fuer analyze-port erlaubt.");
    if (refresh_analysis && (!analysis_only || !resume_analysis))
        throw std::invalid_argument(
            "--refresh-analysis braucht einen expliziten "
            "analyze-port --resume-Lauf.");
    if (runtime_frontier_import_path.has_value() && !resume_analysis)
        throw std::invalid_argument(
            "--import-runtime-frontier braucht einen expliziten "
            "analyze-port --resume-Lauf.");
    const auto analysis_mode_identity =
        port_analysis_mode_identity(analysis_mode);
    if (analysis_mode == PortAnalysisMode::ConservativeRuntimeOnly &&
        (diagnostic_partial || !game_project_path.has_value()))
        throw std::invalid_argument(
            "--analysis-mode runtime-only braucht einen vollstaendigen "
            "Produktport mit --game-project.");
    if (!native_aot_resume_entries.empty() &&
        (analysis_mode != PortAnalysisMode::ConservativeRuntimeOnly ||
         diagnostic_partial || boot_executable_artifact ||
         !game_project_path.has_value()))
        throw std::invalid_argument(
            "--native-aot-resume-entry braucht einen vollstaendigen "
            "RuntimeOnly-NativeDisc-Port mit --game-project.");
    const auto source_root = discover_source_root_for_protection();
    const auto runtime_binding = discover_runtime_binding_for_build(source_root);
    const auto absolute_output = std::filesystem::absolute(output_path).lexically_normal();
    if (absolute_output == absolute_output.root_path() ||
        absolute_output.filename().empty())
        throw std::invalid_argument(
            "Port-Ausgabe darf kein Dateisystemstamm sein.");
    ensure_safe_absolute_directory_chain(
        absolute_output.parent_path(), "Port-Ausgabeelternpfad");
    if (!diagnostic_partial && !native_port_definition_path.has_value())
        throw std::invalid_argument(
            "Produktports brauchen --native-port-definition.");
    if (diagnostic_partial && native_port_definition_path.has_value())
        throw std::invalid_argument(
            "--native-port-definition ist ausschliesslich fuer "
            "vollstaendige Produktports erlaubt.");
    const auto publish_paths =
        port_publish_output_paths(absolute_output);
    std::shared_ptr<katana::runtime::NativePortArtifact>
        verified_native_port;
    std::optional<std::filesystem::path> native_adapter_source_dir;
    if (native_port_definition_path.has_value()) {
        verified_native_port =
            katana::runtime::NativePortArtifact::load(
                *native_port_definition_path);
        const auto& native_definition_path =
            verified_native_port->canonical_path();
        native_adapter_source_dir = native_definition_path.parent_path();
        ensure_safe_absolute_directory_chain(
            *native_adapter_source_dir,
            "Privater Native-Port-Adapter");
        const auto adapter_cmake =
            *native_adapter_source_dir / "CMakeLists.txt";
        std::error_code adapter_cmake_error;
        const auto adapter_cmake_status =
            std::filesystem::symlink_status(
                adapter_cmake, adapter_cmake_error);
        if (adapter_cmake_error ||
            !std::filesystem::is_regular_file(adapter_cmake_status) ||
            unsafe_port_filesystem_link(
                adapter_cmake, adapter_cmake_status))
            throw std::invalid_argument(
                "Der Ordner der Native-Port-Definition muss einen sicheren "
                "privaten CMakeLists.txt-Titeladapter enthalten.");
        if (!source_root.empty() &&
            path_is_within(
                native_definition_path,
                std::filesystem::absolute(source_root).lexically_normal()))
            throw std::invalid_argument(
                "Native-Port-Definition muss ausserhalb des "
                "KatanaRecomp-Quellbaums liegen.");
        require_native_port_definition_path_disjoint_from_path(
            native_definition_path, source_path, "die Portquelle");
        require_native_port_definition_path_disjoint_from_path(
            native_definition_path, publish_paths.output,
            "das publizierte Portpaket");
        require_native_port_definition_path_disjoint_from_path(
            native_definition_path, publish_paths.lock_base,
            "die Port-Publish-Sperre");
        require_native_port_definition_path_disjoint_from_path(
            native_definition_path, publish_paths.journal,
            "das Port-Publish-Journal");
        if (game_project_path.has_value())
            require_native_port_definition_path_disjoint_from_path(
                native_definition_path, *game_project_path,
                "das Game-Project");
        if (game_entry_handoff_path.has_value())
            require_native_port_definition_path_disjoint_from_path(
                native_definition_path, *game_entry_handoff_path,
                "den Game-Entry-Handoff");
        for (const auto& [image_id, payload_path] :
             runtime_image_payload_arguments) {
            static_cast<void>(image_id);
            require_native_port_definition_path_disjoint_from_path(
                native_definition_path, payload_path,
                "ein Runtime-Image-Payload");
        }
        for (const auto& [guest_address, payload_path] :
             bootstrap_write_payload_arguments) {
            static_cast<void>(guest_address);
            require_native_port_definition_path_disjoint_from_path(
                native_definition_path, payload_path,
                "ein Bootstrap-Write-Payload");
        }
    }
    std::optional<std::filesystem::path>
        normalized_telemetry_jsonl_path;
    if (telemetry_jsonl_path.has_value()) {
        const auto telemetry_path =
            std::filesystem::absolute(*telemetry_jsonl_path).
                lexically_normal();
        if (telemetry_path.empty() ||
            telemetry_path == telemetry_path.root_path() ||
            telemetry_path.filename().empty() ||
            path_is_within(telemetry_path, absolute_output))
            throw std::invalid_argument(
                "--telemetry-jsonl muss eine Datei ausserhalb des "
                "publizierten Portpakets bezeichnen.");
        ensure_safe_absolute_directory_chain(
            telemetry_path.parent_path(),
            "Portbuild-Telemetriepfad");
        std::error_code telemetry_status_error;
        const auto telemetry_status =
            std::filesystem::symlink_status(
                telemetry_path, telemetry_status_error);
        const bool telemetry_missing =
            telemetry_status_error ==
                std::errc::no_such_file_or_directory ||
            (!telemetry_status_error &&
             telemetry_status.type() ==
                 std::filesystem::file_type::not_found);
        if (!telemetry_missing &&
            (telemetry_status_error ||
             !std::filesystem::is_regular_file(
                 telemetry_status) ||
             unsafe_port_filesystem_link(
                 telemetry_path, telemetry_status)))
            throw std::invalid_argument(
                "--telemetry-jsonl ist keine sicher ersetzbare "
                "regulaere Datei.");

        const auto telemetry_writer_lock =
            katana::cli::port_build_telemetry_writer_lock_path(
                telemetry_path);

        require_telemetry_path_disjoint_from_file(
            telemetry_path,
            source_path,
            "die Portquelle");
        require_telemetry_path_outside_tree(
            telemetry_path,
            publish_paths.output,
            "dem publizierten Portpaket");
        require_telemetry_path_disjoint_from_file(
            telemetry_path,
            std::filesystem::path(
                publish_paths.lock_base.string() +
                ".lock"),
            "die Port-Publish-Sperre");
        require_telemetry_path_outside_tree(
            telemetry_path,
            publish_paths.journal,
            "dem Port-Publish-Journal");
        require_telemetry_path_outside_tree(
            telemetry_path,
            source_root,
            "dem KatanaRecomp-Quellbaum");
        require_telemetry_path_outside_tree(
            telemetry_path,
            runtime_binding.package_prefix,
            "dem Runtime-Paket");
        require_telemetry_path_outside_tree(
            telemetry_path,
            runtime_binding.source_root,
            "dem Runtime-Quellbaum");
        require_telemetry_path_outside_tree(
            telemetry_path,
            port_export_workspace_root(
                absolute_output.parent_path()),
            "dem globalen Port-Arbeitscache");
        if (!runtime_binding.build_targets_file.empty()) {
            require_telemetry_path_outside_tree(
                telemetry_path,
                runtime_binding.build_targets_file.parent_path(),
                "dem Runtime-Buildbaum");
        }
        if (game_project_path.has_value())
            require_telemetry_path_disjoint_from_file(
                telemetry_path,
                *game_project_path,
                "das Game-Project");
        if (game_entry_handoff_path.has_value())
            require_telemetry_path_disjoint_from_file(
                telemetry_path,
                *game_entry_handoff_path,
                "den Game-Entry-Handoff");
        if (verified_native_port)
            require_telemetry_path_disjoint_from_file(
                telemetry_path,
                verified_native_port->canonical_path(),
                "die Native-Port-Definition");
        for (const auto& [image_id, payload_path] :
             runtime_image_payload_arguments) {
            static_cast<void>(image_id);
            require_telemetry_path_disjoint_from_file(
                telemetry_path,
                payload_path,
                "ein Runtime-Image-Payload");
        }
        if (!boot_executable_artifact) {
            const auto descriptor =
                katana::runtime::parse_gdi_descriptor(source_path);
            require_telemetry_path_disjoint_from_file(
                telemetry_path,
                descriptor.resolved_path,
                "den GDI-Descriptor");
            require_telemetry_path_disjoint_from_file(
                telemetry_writer_lock,
                descriptor.resolved_path,
                "den GDI-Descriptor");
            for (const auto& track : descriptor.tracks)
            {
                require_telemetry_path_disjoint_from_file(
                    telemetry_path,
                    track.resolved_path,
                    "einen GDI-Track");
                require_telemetry_path_disjoint_from_file(
                    telemetry_writer_lock,
                    track.resolved_path,
                    "einen GDI-Track");
            }
        }

        // The persistent sibling lock is opened with exclusive ownership for
        // the recorder lifetime. Protect it with exactly the same alias and
        // tree boundaries as the JSONL payload itself; otherwise a crafted
        // lock pathname could pin or alias an input/publish artifact before
        // the recorder ever touches the requested output.
        require_telemetry_path_disjoint_from_file(
            telemetry_writer_lock,
            telemetry_path,
            "das Portbuild-Telemetrieziel");
        require_telemetry_path_disjoint_from_file(
            telemetry_writer_lock,
            source_path,
            "die Portquelle");
        require_telemetry_path_outside_tree(
            telemetry_writer_lock,
            publish_paths.output,
            "dem publizierten Portpaket");
        require_telemetry_path_disjoint_from_file(
            telemetry_writer_lock,
            std::filesystem::path(
                publish_paths.lock_base.string() +
                ".lock"),
            "die Port-Publish-Sperre");
        require_telemetry_path_outside_tree(
            telemetry_writer_lock,
            publish_paths.journal,
            "dem Port-Publish-Journal");
        require_telemetry_path_outside_tree(
            telemetry_writer_lock,
            source_root,
            "dem KatanaRecomp-Quellbaum");
        require_telemetry_path_outside_tree(
            telemetry_writer_lock,
            runtime_binding.package_prefix,
            "dem Runtime-Paket");
        require_telemetry_path_outside_tree(
            telemetry_writer_lock,
            runtime_binding.source_root,
            "dem Runtime-Quellbaum");
        require_telemetry_path_outside_tree(
            telemetry_writer_lock,
            port_export_workspace_root(
                absolute_output.parent_path()),
            "dem globalen Port-Arbeitscache");
        if (!runtime_binding.build_targets_file.empty()) {
            require_telemetry_path_outside_tree(
                telemetry_writer_lock,
                runtime_binding.build_targets_file.parent_path(),
                "dem Runtime-Buildbaum");
        }
        if (game_project_path.has_value())
            require_telemetry_path_disjoint_from_file(
                telemetry_writer_lock,
                *game_project_path,
                "das Game-Project");
        if (game_entry_handoff_path.has_value())
            require_telemetry_path_disjoint_from_file(
                telemetry_writer_lock,
                *game_entry_handoff_path,
                "den Game-Entry-Handoff");
        if (verified_native_port)
            require_telemetry_path_disjoint_from_file(
                telemetry_writer_lock,
                verified_native_port->canonical_path(),
                "die Native-Port-Definition");
        for (const auto& [image_id, payload_path] :
             runtime_image_payload_arguments) {
            static_cast<void>(image_id);
            require_telemetry_path_disjoint_from_file(
                telemetry_writer_lock,
                payload_path,
                "ein Runtime-Image-Payload");
        }
        normalized_telemetry_jsonl_path = telemetry_path;
    }
    if (!source_root.empty()) {
        const auto relative_to_source = absolute_output.lexically_relative(source_root);
        if (!relative_to_source.empty() && !relative_to_source.is_absolute() &&
            *relative_to_source.begin() != "..") {
            throw std::invalid_argument(
                "Port-Ausgabe muss ausserhalb des KatanaRecomp-Quellbaums liegen.");
        }
    }
    const auto port_runtime_profile =
        configured_environment_value("KATANA_PORT_RUNTIME_PROFILE")
            .value_or(diagnostic_partial
                          ? "diagnostic-interpreter"
                          : "native-port");
    if (diagnostic_partial) {
        if (port_runtime_profile != "diagnostic-interpreter")
            throw std::invalid_argument(
                "Diagnoseports brauchen KATANA_PORT_RUNTIME_PROFILE="
                "diagnostic-interpreter.");
    } else if (port_runtime_profile != "native-port") {
        throw std::invalid_argument(
            "Produktports brauchen KATANA_PORT_RUNTIME_PROFILE=native-port; "
            "der historische Geraetepfad ist kein Exportprofil.");
    }
    katana::cli::PortBuildTelemetryOptions telemetry_options;
    telemetry_options.jsonl_path =
        normalized_telemetry_jsonl_path;
    // analyze-port intentionally stops before host configuration, so there is
    // no CMake-resolved child environment to bind.  Its progress and phase
    // telemetry remain mandatory when requested; only the build-only
    // post-configure record is outside this command's contract.
    telemetry_options.require_resolved_environment = !analysis_only;
    telemetry_options.require_phase_timings =
        normalized_telemetry_jsonl_path.has_value();
    telemetry_options.build_profile =
        configured_environment_value("KATANA_PORT_BUILD_PROFILE")
            .value_or("bringup");
    telemetry_options.runtime_profile = port_runtime_profile;
    telemetry_options.host_compile_jobs_requested =
        host_compile_budget.requested;
    telemetry_options.host_compile_jobs_effective =
        host_compile_budget.effective;
    telemetry_options.job_kind =
        analysis_only
            ? "analyze-port"
            : boot_executable_artifact
            ? (diagnostic_partial
                   ? "probe-port-executable"
                   : "port-executable")
            : (diagnostic_partial ? "probe-port" : "native-disc-port");
    const auto build_job_kind = telemetry_options.job_kind;
    katana::cli::PortBuildTelemetryRecorder
        build_telemetry(std::move(telemetry_options));
    if (normalized_telemetry_jsonl_path.has_value() &&
        !build_telemetry.enabled())
        throw katana::cli::Error(
            katana::cli::ExitCode::InputOutput,
            "Explizit angeforderte Portbuild-Telemetrie konnte "
            "nicht initialisiert werden.");
    PortPhaseTimingRecorder phase_timings(&build_telemetry);
    const katana::ProgressReporter port_progress(
        [&build_telemetry](
            const katana::ProgressEvent& event) {
            build_telemetry.observe_progress(event);
            observe_structured_progress(event);
        },
        std::chrono::milliseconds(250),
        std::chrono::seconds(1));
    auto port_build_progress =
        port_progress.begin(
            katana::ProgressOperation::PortBuild,
            katana::ProgressUnit::Steps,
            1u,
            build_job_kind);
    PortBuildTelemetryRun telemetry_run(
        build_telemetry,
        phase_timings,
        port_progress,
        port_build_progress,
        normalized_telemetry_jsonl_path.has_value());
    try {
        phase_timings.transition("setup");
    const auto shell_quote = [](const std::filesystem::path& path) {
        const auto text = path.string();
#ifdef _WIN32
        if (text.find('"') != std::string::npos) {
            throw std::invalid_argument("Hostbuildpfad enthaelt ein Anfuehrungszeichen.");
        }
        return '"' + text + '"';
#else
        std::string quoted = "'";
        for (const auto character : text) {
            character == '\'' ? quoted += "'\\''" : quoted += character;
        }
        return quoted + "'";
#endif
    };
    std::unique_ptr<ExclusivePortExportLock> publish_lock;
    if (!analysis_only) {
        publish_lock = std::make_unique<ExclusivePortExportLock>(
            publish_paths.lock_base);
        maybe_hold_port_publish_lock_for_test();
        recover_port_publish_transaction(publish_paths);
        if (exit_after_port_publish_recovery_for_test()) {
            telemetry_run.complete();
            return 0;
        }
    }
    if (!analysis_only)
        static_cast<void>(
            safe_regular_port_directory_exists(
                publish_paths.output, "Bestehendes Portpaket"));
    std::optional<katana::platform::DreamcastBootExecutableArtifact>
        verified_boot_artifact;
    std::optional<katana::platform::DreamcastDiscBoot>
        verified_native_disc;
    std::optional<katana::runtime::DiscInstallRecipe>
        verified_install_recipe;
    std::shared_ptr<katana::runtime::GameEntryHandoffArtifact>
        verified_game_entry_handoff;
    std::shared_ptr<katana::runtime::GameProjectArtifact>
        verified_game_project;
    std::optional<katana::runtime::GameProjectDefinition>
        resolved_game_project;
    std::vector<std::vector<std::uint8_t>>
        runtime_image_payload_storage;
    std::vector<katana::codegen::GameProjectRuntimeImagePayload>
        runtime_image_payloads;
    std::vector<std::vector<std::uint8_t>>
        bootstrap_write_payload_storage;
    std::vector<katana::codegen::NativePortBootstrapWritePayload>
        bootstrap_write_payloads;
    std::optional<std::string> whole_export_cache_key;
    std::string whole_export_source_kind;
    std::uint32_t whole_export_source_contract_version = 0u;
    std::string whole_export_boot_file_name;
    std::uint32_t whole_export_entry_address = 0u;
    std::string native_port_artifact_identity;
    std::uint32_t native_port_artifact_format_version_for_cache = 0u;
    if (game_project_path.has_value() && diagnostic_partial)
        throw std::invalid_argument(
            "--game-project ist ausschliesslich fuer vollstaendige Produktports erlaubt.");
    if (game_entry_handoff_path.has_value() &&
        (!boot_executable_artifact || diagnostic_partial))
        throw std::invalid_argument(
            "--game-entry-handoff ist ausschliesslich fuer "
            "port-executable-Produktports erlaubt.");
    if (!runtime_image_payload_arguments.empty() &&
        (!game_project_path.has_value() || diagnostic_partial))
        throw std::invalid_argument(
            "--runtime-image-payload braucht einen vollstaendigen "
            "Produktport mit --game-project.");
    if (!bootstrap_write_payload_arguments.empty() &&
        (!verified_native_port || diagnostic_partial))
        throw std::invalid_argument(
            "--native-bootstrap-write-payload braucht einen "
            "vollstaendigen Produktport mit --native-port-definition.");
    phase_timings.transition("analysis-codegen");
    std::cout << "KATANA_PORT_PHASE analysis-codegen\n";
    std::cout << std::flush;
    if (game_project_path.has_value()) {
        verified_game_project =
            katana::runtime::GameProjectArtifact::load(
                *game_project_path);
        resolved_game_project = verified_game_project->definition();
    }
    if (verified_native_port) {
        native_port_artifact_identity =
            verified_native_port->artifact_identity();
        native_port_artifact_format_version_for_cache =
            katana::runtime::native_port_artifact_format_version;
    }
    const auto implementation_identities =
        port_export_implementation_identities(
            native_port_artifact_identity,
            native_port_artifact_format_version_for_cache);
    if (boot_executable_artifact) {
        verified_boot_artifact =
            katana::platform::load_dreamcast_boot_executable_artifact(source_path);
        verified_install_recipe = verified_boot_artifact->install_recipe;
        whole_export_source_kind = "boot-executable";
        whole_export_source_contract_version = verified_boot_artifact->version;
        whole_export_boot_file_name =
            verified_boot_artifact->metadata.boot_file_name;
        whole_export_entry_address = verified_boot_artifact->entry_address;
        if (game_entry_handoff_path.has_value()) {
            verified_game_entry_handoff =
                katana::runtime::GameEntryHandoffArtifact::load(
                    *game_entry_handoff_path);
            const auto& descriptor =
                verified_game_entry_handoff->descriptor();
            const auto& binding = descriptor.binding;
            if (descriptor.completeness !=
                    katana::runtime::GameEntryHandoffCompleteness::
                        CompletePlatform ||
                binding.executable.content_identity !=
                    verified_boot_artifact->install_recipe.content_identity ||
                binding.executable.boot_file_name !=
                    verified_boot_artifact->metadata.boot_file_name ||
                binding.executable.boot_byte_identity !=
                    "sha256:" + verified_boot_artifact->boot_sha256 ||
                katana::runtime::dreamcast_console_profile_name(
                    binding.console_profile) != console_profile)
                throw std::invalid_argument(
                    "Game-entry handoff passt nicht exakt zum "
                    "Boot-Executable-Produktport.");
            if (verified_boot_artifact->boot_file.size() >
                std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument(
                    "Boot-Executable ist fuer den Game-entry handoff zu gross.");
            const std::array allowed_entry_ranges{
                katana::runtime::GameEntryCodeRange{
                    verified_boot_artifact->entry_address,
                    static_cast<std::uint32_t>(
                        verified_boot_artifact->boot_file.size())}};
            katana::runtime::GameEntryHandoffRequest request;
            request.expected_binding = binding;
            request.allowed_entry_ranges = allowed_entry_ranges;
            request.memory_layout = {
                static_cast<std::uint32_t>(
                    katana::runtime::dreamcast_main_ram_size),
                static_cast<std::uint32_t>(
                    katana::runtime::dreamcast_vram_size),
                static_cast<std::uint32_t>(
                    katana::runtime::dreamcast_aica_ram_size)};
            request.required_devices =
                katana::runtime::dreamcast_game_entry_required_devices_v2;
            request.required_completeness =
                katana::runtime::GameEntryHandoffCompleteness::
                    CompletePlatform;
            static_cast<void>(
                katana::runtime::validate_and_stage_game_entry_handoff(
                    request, verified_game_entry_handoff->provider()));

            if (!resolved_game_project.has_value())
                resolved_game_project.emplace();
            auto& definition = *resolved_game_project;
            if (!verified_game_project) {
                definition.project_id =
                    "katana.artifact-only-game-entry-handoff";
                definition.project_version = "1";
                definition.identity = {
                    binding.executable.content_identity,
                    binding.executable.boot_file_name,
                    binding.executable.boot_byte_identity};
            }
            if (!definition.boot_config.has_value())
                definition.boot_config =
                    katana::runtime::DreamcastRuntimeBootConfig{};
            definition.game_entry_handoff = binding;
            katana::runtime::validate_game_project_definition(definition);
        }
    } else {
        const auto disc_load_started = std::chrono::steady_clock::now();
        phase_timings.transition("disc-load");
        std::cout << "KATANA_PORT_SUBPHASE disc-load\n" << std::flush;
        verified_native_disc =
            katana::platform::load_dreamcast_gdi_boot(
                source_path,
                port_progress);
        const auto boot_sha256 = katana::io::sha256_bytes(
            std::string_view(
                reinterpret_cast<const char*>(
                    verified_native_disc->boot_file.data()),
                verified_native_disc->boot_file.size()));
        const auto project_identity =
            katana::platform::dreamcast_disc_project_identity(
                *verified_native_disc);
        verified_install_recipe =
            katana::runtime::make_disc_install_recipe(
                *verified_native_disc->source,
                project_identity,
                boot_sha256,
                port_progress);
        std::cout
            << "KATANA_PORT_SUBPHASE disc-load-complete-ms"
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - disc_load_started)
                   .count()
            << '\n'
            << std::flush;
        phase_timings.transition("analysis-codegen");
        whole_export_source_kind = "native-disc";
        whole_export_source_contract_version = 1u;
        whole_export_boot_file_name =
            verified_native_disc->metadata.boot_file_name;
        whole_export_entry_address =
            katana::platform::dreamcast_system_bootstrap_entry_address;
    }
    if (!verified_install_recipe)
        throw std::logic_error(
            "Portexport besitzt keine validierte Installationsrecipe.");
    if (verified_game_project) {
        const auto& definition = verified_game_project->definition();
        if (definition.identity.content_identity !=
                verified_install_recipe->content_identity ||
            definition.identity.boot_file_name !=
                whole_export_boot_file_name ||
            definition.identity.boot_byte_identity !=
                "sha256:" + verified_install_recipe->boot_sha256)
            throw std::invalid_argument(
                "Game-project artifact passt nicht exakt zum Produktport.");
    }
    runtime_image_payload_storage.reserve(
        runtime_image_payload_arguments.size());
    for (std::size_t index = 0u;
         index < runtime_image_payload_arguments.size();
         ++index) {
        const auto& [image_id, payload_path] =
            runtime_image_payload_arguments[index];
        if (image_id.empty())
            throw std::invalid_argument(
                "--runtime-image-payload besitzt keine Image-ID.");
        if (std::any_of(
                runtime_image_payload_arguments.begin(),
                runtime_image_payload_arguments.begin() +
                    static_cast<std::ptrdiff_t>(index),
                [&](const auto& previous) {
                    return previous.first == image_id;
                }))
            throw std::invalid_argument(
                "--runtime-image-payload wurde fuer dieselbe Image-ID "
                "mehrfach angegeben.");
        if (!resolved_game_project.has_value())
            throw std::invalid_argument(
                "--runtime-image-payload besitzt kein Game-Project.");
        const auto descriptor = std::find_if(
            resolved_game_project->runtime_images.begin(),
            resolved_game_project->runtime_images.end(),
            [&](const auto& candidate) {
                return candidate.image_id == image_id;
            });
        if (descriptor ==
            resolved_game_project->runtime_images.end())
            throw std::invalid_argument(
                "--runtime-image-payload verweist auf keine deklarierte "
                "Runtime-Image-ID.");
        runtime_image_payload_storage.push_back(
            load_runtime_image_payload(
                payload_path,
                descriptor->byte_size,
                source_root));
    }
    runtime_image_payloads.reserve(
        runtime_image_payload_arguments.size());
    for (std::size_t index = 0u;
         index < runtime_image_payload_arguments.size();
         ++index)
        runtime_image_payloads.push_back(
            {runtime_image_payload_arguments[index].first,
             runtime_image_payload_storage[index]});
    katana::codegen::validate_game_project_runtime_image_payloads(
        resolved_game_project.has_value()
            ? &*resolved_game_project
            : nullptr,
        runtime_image_payloads,
        verified_native_port
            ? &verified_native_port->definition()
            : nullptr);
    bootstrap_write_payload_storage.reserve(
        bootstrap_write_payload_arguments.size());
    for (std::size_t index = 0u;
         index < bootstrap_write_payload_arguments.size();
         ++index) {
        const auto& [guest_address, payload_path] =
            bootstrap_write_payload_arguments[index];
        if (std::any_of(
                bootstrap_write_payload_arguments.begin(),
                bootstrap_write_payload_arguments.begin() +
                    static_cast<std::ptrdiff_t>(index),
                [&](const auto& previous) {
                    return previous.first == guest_address;
                }))
            throw std::invalid_argument(
                "--native-bootstrap-write-payload wurde fuer dieselbe "
                "Gastadresse mehrfach angegeben.");
        if (!verified_native_port)
            throw std::invalid_argument(
                "--native-bootstrap-write-payload besitzt keine "
                "Native-Port-Definition.");
        const auto& definition = verified_native_port->definition();
        const auto descriptor = std::find_if(
            definition.bootstrap.writes.begin(),
            definition.bootstrap.writes.end(),
            [&](const auto& candidate) {
                return candidate.guest_address == guest_address;
            });
        if (descriptor == definition.bootstrap.writes.end())
            throw std::invalid_argument(
                "--native-bootstrap-write-payload verweist auf keine "
                "deklarierte Bootstrap-Write-Range.");
        bootstrap_write_payload_storage.push_back(
            load_runtime_image_payload(
                payload_path, descriptor->byte_size, source_root));
    }
    bootstrap_write_payloads.reserve(
        bootstrap_write_payload_arguments.size());
    for (std::size_t index = 0u;
         index < bootstrap_write_payload_arguments.size();
         ++index)
        bootstrap_write_payloads.push_back(
            {bootstrap_write_payload_arguments[index].first,
             bootstrap_write_payload_storage[index]});
    katana::codegen::validate_native_port_bootstrap_write_payloads(
        verified_native_port
            ? &verified_native_port->definition()
            : nullptr,
        bootstrap_write_payloads);
    const auto game_project_definition_identity =
        resolved_game_project.has_value()
            ? katana::runtime::game_project_definition_identity(
                  *resolved_game_project)
            : std::string{};
    const auto game_project_identity =
        verified_game_project
            ? game_project_definition_identity + ':' +
                  verified_game_project->artifact_identity()
            : game_project_definition_identity;
    const auto handoff_artifact_identity =
        verified_game_entry_handoff
            ? verified_game_entry_handoff->artifact_identity()
            : std::string{};
    const auto latent_aot_hint_identity =
        latent_aot_entry_hint_identity(normalized_latent_aot_entry_hints,
                                       latent_aot_discovery_mode);
    whole_export_cache_key = port_export_cache_key(
        whole_export_source_kind,
        whole_export_source_contract_version,
        *verified_install_recipe,
        whole_export_boot_file_name,
        whole_export_entry_address,
        target_name,
        diagnostic_partial,
        console_profile,
        game_project_identity,
        handoff_artifact_identity,
        native_port_artifact_identity,
        native_port_artifact_format_version_for_cache,
        latent_aot_hint_identity,
        analysis_mode_identity,
        implementation_identities.whole_export);
    const auto workspace_key = port_export_workspace_key(
        whole_export_source_kind,
        *verified_install_recipe,
        target_name);
    const auto workspace_root =
        port_export_workspace_root(
            absolute_output.parent_path());
    if (verified_native_port &&
        path_is_within(
            verified_native_port->canonical_path(),
            std::filesystem::absolute(workspace_root).lexically_normal()))
        throw std::invalid_argument(
            "Native-Port-Definition muss ausserhalb des globalen "
            "Port-Arbeitscaches liegen.");
    if (workspace_root == workspace_root.root_path() ||
        workspace_root.filename().empty())
        throw std::invalid_argument(
            "Globaler Port-Arbeitscache darf kein Dateisystemstamm sein.");
    if (!source_root.empty() &&
        path_is_within(workspace_root, source_root))
        throw std::invalid_argument(
            "Globaler Port-Arbeitscache muss ausserhalb des "
            "KatanaRecomp-Quellbaums liegen.");
    ensure_safe_absolute_directory_chain(
        workspace_root, "Globaler Port-Arbeitscache");
    const auto component_cache_root =
        workspace_root / "component-cache";
    ensure_safe_absolute_directory_chain(
        component_cache_root,
        "Globaler Port-Komponentencache");
    const auto make_export_options =
        [&](const std::filesystem::path& codegen_cache_root) {
            katana::codegen::PortExportOptions export_options{
                target_name,
                KATANA_RECOMP_VERSION,
                {},
                source_root,
                diagnostic_partial,
                console_profile,
                [&phase_timings](const std::string_view phase) {
                    observe_port_export_progress(
                        phase_timings, phase);
                }};
            export_options.progress = port_progress;
            export_options.detailed_analysis_telemetry =
                detailed_analysis_telemetry;
            export_options.analysis_cache_root =
                component_cache_root;
            export_options.codegen_cache_root = codegen_cache_root;
            export_options.analysis_implementation_identity =
                implementation_identities.analysis;
            export_options.analysis_cache_implementation_identity =
                implementation_identities.analysis_cache;
            export_options.codegen_implementation_identity =
                implementation_identities.codegen;
            export_options.game_project =
                resolved_game_project.has_value()
                    ? &*resolved_game_project
                    : nullptr;
            export_options.native_port_definition =
                verified_native_port
                    ? &verified_native_port->definition()
                    : nullptr;
            export_options.native_port_artifact_identity =
                native_port_artifact_identity;
            export_options.game_project_runtime_image_payloads =
                runtime_image_payloads;
            export_options.native_port_bootstrap_write_payloads =
                bootstrap_write_payloads;
            export_options.native_aot_resume_entries =
                native_aot_resume_entries;
            export_options.latent_aot_entry_hints =
                normalized_latent_aot_entry_hints;
            export_options.latent_aot_discovery_mode =
                latent_aot_discovery_mode;
            export_options.analysis_artifact_archive_requested =
                analysis_only;
            export_options.agent_analysis_artifacts_requested =
                analysis_only;
            export_options.analysis_artifact_refresh_requested =
                analysis_only && refresh_analysis;
            return export_options;
        };
    if (analysis_only) {
        if (!verified_native_disc)
            throw std::logic_error(
                "analyze-port besitzt keine validierte NativeDisc-Quelle.");
        // The final session-ledger record is the commit marker. Holding one
        // sibling lock across analysis plus every artifact replacement keeps
        // concurrent analyze-port runs from interleaving their world, JSON,
        // report and session ledger generations.
        const ExclusivePortExportLock analysis_lock(
            std::filesystem::path(
                absolute_output.string() + ".katana-analysis"));
        std::optional<katana::agent::ExecutableMaterializationWorld>
            previous_world;
        std::optional<std::string> previous_analysis_artifact_id;
        std::string previous_analysis_archive;
        std::string previous_producer_identity;
        std::string previous_analysis_session_contract_identity;
        if (resume_analysis) {
            if (!safe_regular_port_directory_exists(
                    absolute_output,
                    "Vorheriger Analyseartefaktordner"))
                throw std::invalid_argument(
                    "analyze-port --resume braucht einen vorhandenen "
                    "sicheren Analyseartefaktordner.");
            auto committed = load_committed_agent_generation(
                absolute_output);
            previous_world = std::move(committed.world);
            previous_analysis_artifact_id =
                std::move(committed.analysis_artifact_id);
            previous_analysis_archive =
                std::move(committed.analysis_archive);
            previous_producer_identity =
                std::move(committed.producer_identity);
            previous_analysis_session_contract_identity =
                std::move(
                    committed.analysis_session_contract_identity);
            if (previous_analysis_session_contract_identity.empty())
                throw std::invalid_argument(
                    "analyze-port --resume verweigert eine Legacy-Session "
                    "ohne gebundenen Analysevertrag; eine neue "
                    "Analyse-Session ist erforderlich.");
        }
        std::optional<RuntimeFrontierImport> runtime_import;
        if (runtime_frontier_import_path.has_value())
            runtime_import = load_runtime_frontier_import(
                *runtime_frontier_import_path);
        auto analysis_options = make_export_options({});
        if (!refresh_analysis && !previous_analysis_archive.empty()) {
            analysis_options.resume_analysis_artifact =
                std::span(
                    reinterpret_cast<const std::uint8_t*>(
                        previous_analysis_archive.data()),
                    previous_analysis_archive.size());
            analysis_options.resume_analysis_artifact_key =
                *previous_analysis_artifact_id;
        }
        const auto current_producer_identity =
            agent_session_producer_identity(implementation_identities);
        std::optional<AgentAnalysisAuthorityBaseline>
            previous_analysis_authority;
        std::string analysis_session_contract_identity;
        if (resume_analysis) {
            if (previous_analysis_archive.empty() ||
                !previous_analysis_artifact_id.has_value())
                throw std::invalid_argument(
                    "analyze-port --resume braucht ein autoritatives "
                    "Analysearchiv.");
            auto parsed_manifest =
                katana::codegen::parse_native_disc_analysis_artifact(
                    *previous_analysis_artifact_id,
                    std::span(
                        reinterpret_cast<const std::uint8_t*>(
                            previous_analysis_archive.data()),
                        previous_analysis_archive.size()));
            if (parsed_manifest.state !=
                katana::codegen::NativeDiscAnalysisArtifactState::Hit)
                throw std::invalid_argument(
                    "analyze-port --resume verweigert ein nicht "
                    "revalidierbares Analysearchiv.");
            const auto current_manifest =
                katana::codegen::
                    native_disc_analysis_resume_manifest_identity(
                        *verified_native_disc,
                        analysis_options,
                        parsed_manifest.artifact.external_primary_roots,
                        analysis_mode);
            analysis_session_contract_identity =
                agent_analysis_session_contract_identity(
                    current_manifest, analysis_options);
            if (previous_analysis_session_contract_identity !=
                analysis_session_contract_identity)
                throw std::invalid_argument(
                    "analyze-port --resume verweigert einen veraenderten "
                    "semantischen Analysevertrag; eine neue "
                    "Analyse-Session ist erforderlich.");
            previous_analysis_authority =
                capture_agent_analysis_authority(
                    parsed_manifest.artifact);
            if (!refresh_analysis &&
                katana::build_contract::source_identity_trusted &&
                !runtime_import.has_value() &&
                previous_producer_identity == current_producer_identity &&
                same_noop_analysis_manifest_identity(
                    parsed_manifest.artifact.identity,
                    current_manifest) &&
                previous_world.has_value() &&
                materialization_world_matches_analysis_identity(
                    *previous_world,
                    parsed_manifest.artifact.identity)) {
                const auto report_path =
                    absolute_output / "native-disc-analysis.json";
                std::cout
                    << "KATANA_ANALYZE_PORT_NOOP_RESUME "
                    << report_path.string() << '\n'
                    << "KATANA_ANALYZE_PORT_COMPLETE "
                    << report_path.string() << '\n'
                    << std::flush;
                telemetry_run.complete();
                return 0;
            }
        }
        const auto analysis_started = std::chrono::steady_clock::now();
        auto analyzed =
            katana::codegen::analyze_native_disc_port(
                *verified_native_disc,
                analysis_options,
                analysis_mode);
        if (analysis_session_contract_identity.empty())
            analysis_session_contract_identity =
                agent_analysis_session_contract_identity(
                    analyzed.analysis_artifact_identity,
                    analysis_options);
        const auto analysis_wall_time_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - analysis_started)
                .count());
        std::error_code output_error;
        auto output_status = std::filesystem::symlink_status(
            absolute_output, output_error);
        const bool output_missing =
            output_error == std::errc::no_such_file_or_directory ||
            (!output_error &&
             output_status.type() ==
                 std::filesystem::file_type::not_found);
        if (output_missing) {
            output_error.clear();
            if (!std::filesystem::create_directory(
                    absolute_output, output_error) &&
                output_error)
                throw std::filesystem::filesystem_error(
                    "Analyseartefaktordner konnte nicht erstellt werden.",
                    absolute_output,
                    output_error);
            output_status = std::filesystem::symlink_status(
                absolute_output, output_error);
        }
        if (output_error ||
            !std::filesystem::is_directory(output_status) ||
            unsafe_port_filesystem_link(
                absolute_output, output_status))
            throw std::runtime_error(
                "Analyseartefaktziel ist kein sicherer lokaler Ordner.");
        if (analyzed.analysis_artifact_bytes.empty())
            throw std::runtime_error(
                "analyze-port erhielt kein autoritatives Analysearchiv.");
        // Capture the direct analysis output before resume/runtime evidence
        // projection can reject or mutate the World.  A later successful
        // refresh publishes another complete generation in the alternate
        // bounded slot.
        publish_pending_agent_analysis_candidate(
            absolute_output,
            analyzed,
            analysis_session_contract_identity);
        if (analyzed.materialization_world_artifact_bytes.empty() ||
            analyzed.materialization_world_json.empty()) {
            write_pending_agent_analysis_candidate_manifest(
                absolute_output,
                analyzed,
                "rejected",
                "materialization-world-missing",
                analysis_session_contract_identity);
            throw std::runtime_error(
                "analyze-port erhielt keinen Materialization-World vom "
                "autoritativen Analysepfad; das Analysearchiv wurde bounded "
                "separat gesichert.");
        }
        bool previous_world_evidence_reusable = false;
        if (previous_world.has_value()) {
            const bool exact_generation =
                previous_analysis_artifact_id.has_value() &&
                *previous_analysis_artifact_id ==
                    analyzed.analysis_artifact_identity.key &&
                materialization_world_matches_analysis_identity(
                    *previous_world,
                    analyzed.analysis_artifact_identity);
            const bool revalidated_admission_generation =
                previous_analysis_artifact_id.has_value() &&
                analyzed.resumed_from_analysis_artifact_identity.has_value() &&
                *previous_analysis_artifact_id ==
                    analyzed.resumed_from_analysis_artifact_identity->key &&
                materialization_world_matches_analysis_identity(
                    *previous_world,
                    *analyzed.resumed_from_analysis_artifact_identity);
            previous_world_evidence_reusable =
                exact_generation || revalidated_admission_generation;
            if (!previous_world_evidence_reusable && !refresh_analysis) {
                write_pending_agent_analysis_candidate_manifest(
                    absolute_output,
                    analyzed,
                    "rejected",
                    "resume-world-binding",
                    analysis_session_contract_identity);
                throw std::invalid_argument(
                    "--resume verweigert: der vorhandene "
                    "Materialization-World ist weder an die aktuelle noch "
                    "an die revalidierte Eingabe-Analysegeneration gebunden.");
            }
            if (!previous_world_evidence_reusable)
                std::cout
                    << "KATANA_ANALYZE_PORT_REFRESH_BASELINE_REBUILT\n"
                    << std::flush;
        }
        if (runtime_import.has_value()) {
            try {
                validate_runtime_frontier_import_binding(
                    runtime_import.value(), analyzed);
            } catch (...) {
                write_pending_agent_analysis_candidate_manifest(
                    absolute_output,
                    analyzed,
                    "rejected",
                    "runtime-frontier-binding",
                    analysis_session_contract_identity);
                throw;
            }
        }
        const auto runtime_observations = runtime_import.has_value()
            ? std::span<const RuntimeFrontierObservation>(
                  runtime_import->observations.data(),
                  runtime_import->observations.size())
            : std::span<const RuntimeFrontierObservation>{};
        std::size_t accepted_runtime_observations = 0u;
        const std::optional<katana::agent::ExecutableMaterializationWorld>
            no_previous_world;
        const auto& previous_world_for_evidence =
            previous_world_evidence_reusable
                ? previous_world
                : no_previous_world;
        if (previous_world_evidence_reusable ||
            !runtime_observations.empty()) {
            write_pending_agent_analysis_candidate_manifest(
                absolute_output,
                analyzed,
                "unvalidated",
                "materialization-world-refresh-pending",
                analysis_session_contract_identity);
            accepted_runtime_observations = refresh_agent_artifacts(
                analyzed,
                previous_world_for_evidence,
                runtime_observations);
        }
        // A successful refresh becomes a second complete two-slot generation;
        // no active candidate is ever edited into a mixed archive/World set.
        publish_pending_agent_analysis_candidate(
            absolute_output,
            analyzed,
            analysis_session_contract_identity);
        katana::agent::ExecutableMaterializationWorld current_world;
        if (!katana::agent::parse_agent_world_binary(
                analyzed.materialization_world_artifact_bytes,
                current_world)) {
            write_pending_agent_analysis_candidate_manifest(
                absolute_output,
                analyzed,
                "rejected",
                "materialization-world-codec",
                analysis_session_contract_identity);
            throw std::runtime_error(
                "Kandidaten-Materialization-World konnte nicht fuer das "
                "Authority-Gate validiert werden.");
        }
        const auto candidate_artifact =
            katana::codegen::parse_native_disc_analysis_artifact(
                analyzed.analysis_artifact_identity.key,
                analyzed.analysis_artifact_bytes);
        if (candidate_artifact.state !=
            katana::codegen::NativeDiscAnalysisArtifactState::Hit) {
            const auto rejection =
                "analysis-archive-" + candidate_artifact.reason;
            write_pending_agent_analysis_candidate_manifest(
                absolute_output,
                analyzed,
                "rejected",
                rejection,
                analysis_session_contract_identity);
            throw std::runtime_error(
                "Kandidaten-Analysearchiv konnte nicht fuer das "
                "Authority-Gate validiert werden: " +
                candidate_artifact.reason + ".");
        }
        AgentIterationDelta authority_world_delta;
        if (previous_world.has_value())
            authority_world_delta =
                measure_agent_iteration(*previous_world, current_world);
        if (previous_analysis_authority.has_value()) {
            if (const auto rejection =
                    agent_analysis_authority_rejection(
                        *previous_analysis_authority,
                        candidate_artifact.artifact,
                        authority_world_delta)) {
                write_pending_agent_analysis_candidate_manifest(
                    absolute_output,
                    analyzed,
                    "rejected",
                    *rejection,
                    analysis_session_contract_identity);
                std::cout
                    << "KATANA_ANALYZE_PORT_CANDIDATE_REJECTED reason="
                    << *rejection << '\n'
                    << std::flush;
                throw std::runtime_error(
                    "Analyse-Refresh verletzt den autoritativen "
                    "Closure-Vertrag; die letzte Generation bleibt aktiv "
                    "und der Kandidat wurde bounded separat gesichert.");
            }
        }
        write_pending_agent_analysis_candidate_manifest(
            absolute_output,
            analyzed,
            "validated",
            "authority-gate-passed",
            analysis_session_contract_identity);
        const auto report_path =
            absolute_output / "native-disc-analysis.json";
        const auto archive_target_path =
            absolute_output / "native-disc-analysis.katana-analysis";
        const auto world_artifact_path =
            absolute_output / "materialization-world.katana-world";
        const auto world_json_path =
            absolute_output / "materialization-world.json";
        AnalysisArtifactRollback publication(absolute_output);
        publication.prepare(report_path, "NativeDisc-Analysebericht");
        publication.prepare(
            archive_target_path,
            "Identitaetsgebundenes NativeDisc-Analysearchiv");
        publication.prepare(
            world_artifact_path,
            "Binaeres Materialization-World-Artefakt");
        publication.prepare(
            world_json_path,
            "Materialization-World-JSON");
        std::optional<std::filesystem::path> analysis_archive_path;
        if (!analyzed.analysis_artifact_bytes.empty()) {
            analysis_archive_path = archive_target_path;
            write_atomic_analysis_file(
                absolute_output,
                *analysis_archive_path,
                analyzed.analysis_artifact_bytes,
                "Identitaetsgebundenes NativeDisc-Analysearchiv");
        }
        write_atomic_analysis_file(
            absolute_output,
            world_artifact_path,
            analyzed.materialization_world_artifact_bytes,
            "Binaeres Materialization-World-Artefakt");
        auto world_json = analyzed.materialization_world_json;
        world_json.push_back('\n');
        write_atomic_analysis_file(
            absolute_output,
            world_json_path,
            world_json,
            "Materialization-World-JSON");
        std::ostringstream report;
        report << "{\"schema\":2,\"kind\":\"katana-native-disc-analysis\""
               << ",\"project_identity\":"
               << katana::io::quote_json(analyzed.project_identity)
               << ",\"content_identity\":"
               << katana::io::quote_json(
                      verified_install_recipe->content_identity)
               << ",\"boot_sha256\":"
               << katana::io::quote_json(
                      verified_install_recipe->boot_sha256)
               << ",\"analysis_implementation\":"
               << katana::io::quote_json(
                      implementation_identities.analysis)
               << ",\"primary_functions\":"
               << analyzed.summary.primary_functions
               << ",\"combined_functions\":"
               << analyzed.summary.combined_functions
               << ",\"latent_modules\":"
               << analyzed.summary.latent_modules
               << ",\"external_primary_roots\":"
               << analyzed.summary.external_primary_roots
               << ",\"native_resume_entries\":"
               << analyzed.summary.native_resume_entries
               << ",\"known_hardware_sites\":"
               << analyzed.summary.known_hardware_sites
               << ",\"native_hardware_gaps\":"
               << analyzed.summary.native_hardware_gaps
               << ",\"sdk_provider_candidates\":"
               << analyzed.summary.sdk_provider_candidates
               << ",\"guarded_inventory_complete\":"
               << (analyzed.summary.guarded_inventory_complete
                       ? "true" : "false")
               << ",\"native_hardware_closure_complete\":"
               << (analyzed.summary.native_hardware_closure_complete
                       ? "true" : "false")
               << ",\"backend_admitted\":"
               << (analyzed.summary.backend_admitted
                       ? "true" : "false")
               << ",\"analysis_artifact_key\":"
               << katana::io::quote_json(
                      analyzed.analysis_artifact_identity.key)
               << ",\"analysis_artifact_cache_hit\":"
               << (analyzed.analysis_artifact_cache_hit
                       ? "true" : "false")
               << ",\"analysis_artifact_cache_published\":"
               << (analyzed.analysis_artifact_cache_published
                       ? "true" : "false")
               << ",\"analysis_artifact_cache_publish_missed\":"
               << (analyzed.analysis_artifact_cache_publish_missed
                       ? "true" : "false")
               << ",\"boot_analysis_cache_hit\":"
               << (analyzed.boot_analysis_cache_hit ? "true" : "false")
               << ",\"boot_analysis_pipeline_runs\":"
               << analyzed.boot_analysis_pipeline_runs
               << ",\"latent_root_seed_cache_hit\":"
               << (analyzed.latent_primary_root_seed_cache_hit
                       ? "true" : "false")
               << ",\"latent_root_seed_cache_published\":"
               << (analyzed.latent_primary_root_seed_cache_published
                       ? "true" : "false")
               << ",\"latent_root_seed_cache_publish_missed\":"
               << (analyzed.latent_primary_root_seed_cache_publish_missed
                       ? "true" : "false")
               << ",\"resume_requested\":"
               << (resume_analysis ? "true" : "false")
               << ",\"runtime_frontiers_received\":"
               << runtime_observations.size()
               << ",\"runtime_frontiers_imported\":"
               << accepted_runtime_observations
               << ",\"runtime_frontiers_rejected\":"
               << (runtime_observations.size() -
                   accepted_runtime_observations)
               << ",\"timing_kind\":\"nondeterministic-telemetry\""
               << ",\"telemetry_analysis_wall_time_ms\":"
               << analysis_wall_time_ms
               << ",\"agent_decision\":"
               << katana::io::quote_json(analyzed.agent_decision)
               << ",\"agent_decision_reason\":"
               << katana::io::quote_json(
                      analyzed.agent_decision_reason)
               << ",\"agent_decision_focus\":"
               << analyzed.agent_decision_focus
               << ",\"agent_actionable_frontier\":"
               << analyzed.agent_actionable_frontier
               << ",\"materialization_world_file\":"
               << katana::io::quote_json(
                      world_artifact_path.filename().string())
               << ",\"materialization_world_json\":"
               << katana::io::quote_json(
                      world_json_path.filename().string())
               << ",\"analysis_artifact_file\":";
        if (analysis_archive_path.has_value())
            report << katana::io::quote_json(
                analysis_archive_path->filename().string());
        else
            report << "null";
        report
               << "}\n";
        const auto serialized = report.str();
        const auto world_artifact_sha256 = katana::io::sha256_bytes(
            std::string_view(
                reinterpret_cast<const char*>(
                    analyzed.materialization_world_artifact_bytes.data()),
                analyzed.materialization_world_artifact_bytes.size()));
        const auto world_json_sha256 =
            katana::io::sha256_bytes(world_json);
        const auto report_sha256 =
            katana::io::sha256_bytes(serialized);
        std::optional<std::string> analysis_archive_sha256;
        if (!analyzed.analysis_artifact_bytes.empty())
            analysis_archive_sha256 = katana::io::sha256_bytes(
                std::string_view(
                    reinterpret_cast<const char*>(
                        analyzed.analysis_artifact_bytes.data()),
                    analyzed.analysis_artifact_bytes.size()));
        write_atomic_analysis_file(
            absolute_output,
            report_path,
            serialized,
            "NativeDisc-Analysebericht");
        // The report is the generation payload; only after it is atomically
        // visible may the final ledger line act as the commit marker.  A
        // failed ledger append therefore leaves a retryable report rather
        // than a ledger entry pointing at an unpublished generation.
        append_agent_session_ledger(
            absolute_output,
            analyzed,
            previous_world,
            current_world,
            analysis_wall_time_ms,
            accepted_runtime_observations,
            world_artifact_sha256,
            world_json_sha256,
            report_sha256,
            analysis_archive_sha256.has_value()
                ? std::optional<std::string_view>(*analysis_archive_sha256)
                : std::nullopt,
            current_producer_identity,
            analysis_session_contract_identity);
        publication.commit();
        std::cout << "KATANA_ANALYZE_PORT_COMPLETE "
                  << report_path.string() << '\n'
                  << std::flush;
        telemetry_run.complete();
        return 0;
    }
    const auto workspace =
        workspace_root /
        (".katana-port-work-" + workspace_key.substr(0u, 12u));
    ensure_safe_absolute_directory_chain(
        workspace.parent_path(), "Port-Arbeitsverzeichnis");
    static_cast<void>(
        safe_regular_port_directory_exists(
            workspace, "Port-Arbeitsverzeichnis"));
    const ExclusivePortExportLock workspace_lock(workspace);
    try {
        if (!safe_regular_port_directory_exists(
                workspace,
                "Port-Arbeitsverzeichnis") &&
            safe_regular_port_directory_exists(
                absolute_output,
                "Bestehende Portdistribution")) {
            const auto existing_target =
                declared_port_distribution_target_name(
                    absolute_output);
            if (existing_target == target_name) {
                copy_validated_port_distribution(
                    absolute_output,
                    workspace,
                    target_name,
                    *verified_install_recipe,
                    port_runtime_profile);
            } else {
                // Der Workspace-Schluessel ist targetgebunden. Eine gueltige
                // Distribution fuer ein anderes Target darf deshalb weder als
                // Seed dienen noch unter dem neuen Executable-Namen validiert
                // werden. Ihre Disc-/Publish-Bindung bleibt trotzdem
                // fail-closed geprueft, bevor ein leerer Workspace entsteht.
                copy_validated_port_distribution(
                    absolute_output,
                    {},
                    existing_target,
                    *verified_install_recipe,
                    port_runtime_profile,
                    false);
            }
        }
        if (!safe_regular_port_directory_exists(
                workspace, "Port-Arbeitsverzeichnis")) {
            std::error_code create_error;
            if (!std::filesystem::create_directory(workspace, create_error) &&
                create_error)
                throw std::filesystem::filesystem_error(
                    "Port-Arbeitsverzeichnis konnte nicht sicher erstellt werden.",
                    workspace,
                    create_error);
        }
        if (!safe_regular_port_directory_exists(
                workspace, "Port-Arbeitsverzeichnis"))
            throw std::runtime_error(
                "Port-Arbeitsverzeichnis wurde nicht als sicherer Ordner erstellt.");
        ensure_safe_port_directory_chain(
            workspace,
            workspace / ".katana-codegen-cache",
            "Privater Port-Codegen-Cache");
        auto export_options = make_export_options(
            workspace / ".katana-codegen-cache");
        katana::codegen::PortExportResult report;
        bool whole_export_cache_hit = false;
        if (whole_export_cache_key) {
            if (const auto cached =
                    load_cached_port_export(
                        workspace,
                        whole_export_source_kind,
                        *whole_export_cache_key,
                        *verified_install_recipe,
                        port_progress);
                cached) {
                report.output_root = workspace;
                report.functions = cached->functions;
                report.partitions = cached->partitions;
                report.codegen_cache_hits = cached->partitions;
                report.metadata_cache_hit = true;
                report.disc_install_recipe =
                    workspace / "content" / "game.katana-install";
                report.job_generation =
                    verified_install_recipe->job_generation;
                report.content_identity =
                    verified_install_recipe->content_identity;
                report.disc_tracks =
                    verified_install_recipe->tracks.size();
                report.checkpoints = {
                    "whole-program-analysis-ir-cache-hit",
                    "partition-codegen-cache-hit",
                    "metadata-cache-hit"};
                whole_export_cache_hit = true;
                const auto cached_phase =
                    [&port_progress](
                        const katana::ProgressOperation operation,
                        const katana::ProgressUnit unit,
                        const std::string_view label) {
                        auto scope = port_progress.begin(
                            operation,
                            unit,
                            std::nullopt,
                            std::string(label));
                        scope.cached();
                    };
                cached_phase(
                    katana::ProgressOperation::ProgramValidation,
                    katana::ProgressUnit::Steps,
                    "whole-export-cache");
                cached_phase(
                    katana::ProgressOperation::ControlFlowAnalysis,
                    katana::ProgressUnit::Steps,
                    "whole-export-cache");
                cached_phase(
                    katana::ProgressOperation::CandidateContractIteration,
                    katana::ProgressUnit::Steps,
                    "whole-export-cache");
                cached_phase(
                    katana::ProgressOperation::FunctionValueAnalysis,
                    katana::ProgressUnit::Functions,
                    "whole-export-cache");
                cached_phase(
                    katana::ProgressOperation::CandidateResolution,
                    katana::ProgressUnit::Functions,
                    "whole-export-cache");
                cached_phase(
                    katana::ProgressOperation::LatentAotAnalysis,
                    katana::ProgressUnit::Modules,
                    "whole-export-cache");
                cached_phase(
                    katana::ProgressOperation::IrGeneration,
                    katana::ProgressUnit::Functions,
                    "whole-export-cache");
                cached_phase(
                    katana::ProgressOperation::IrOptimization,
                    katana::ProgressUnit::Steps,
                    "whole-export-cache");
                cached_phase(
                    katana::ProgressOperation::SourceGeneration,
                    katana::ProgressUnit::Partitions,
                    "whole-export-cache");
                cached_phase(
                    katana::ProgressOperation::MetadataGeneration,
                    katana::ProgressUnit::Files,
                    "whole-export-cache");
                cached_phase(
                    katana::ProgressOperation::ArtifactWrite,
                    katana::ProgressUnit::Files,
                    "whole-export-cache");
                std::cout
                    << "KATANA_PORT_SUBPHASE whole-program-analysis-ir-cache-hit\n"
                    << std::flush;
            }
        }
        if (!whole_export_cache_hit) {
            reconcile_generated_artifacts_after_cache_miss(workspace);
            reconcile_generated_sources_after_cache_miss(workspace);
            report =
                boot_executable_artifact
                    ? katana::codegen::
                          export_dreamcast_port_project_from_boot_artifact(
                              source_path, workspace, export_options,
                              analysis_mode)
                    : katana::codegen::export_dreamcast_port_project(
                          *verified_native_disc,
                          workspace,
                          export_options,
                          analysis_mode);
            if (whole_export_cache_key)
                store_cached_port_export(
                    workspace,
                    whole_export_source_kind,
                    *whole_export_cache_key,
                    *verified_install_recipe,
                    report,
                    port_progress);
        }
        const auto build_profile =
            configured_environment_value("KATANA_PORT_BUILD_PROFILE")
                .value_or("bringup");
        if (build_profile != "bringup" && build_profile != "gate")
            throw std::invalid_argument(
                "KATANA_PORT_BUILD_PROFILE muss bringup oder gate sein.");
        const auto host_build_configuration =
            configured_environment_value(
                "KATANA_PORT_HOST_BUILD_CONFIGURATION")
                .value_or(build_profile == "bringup"
                              ? "Release"
                              : "RelWithDebInfo");
        if (host_build_configuration != "RelWithDebInfo" &&
            host_build_configuration != "Release")
            throw std::invalid_argument(
                "KATANA_PORT_HOST_BUILD_CONFIGURATION muss RelWithDebInfo "
                "oder Release sein.");
        const auto compiler_cache_binding =
            configured_compiler_cache_binding();
        const auto compiler_launcher =
            compiler_cache_binding
                ? std::optional(compiler_cache_binding->launcher)
                : std::nullopt;
#ifdef _WIN32
        const auto host_compiler =
            configured_environment_value("KATANA_PORT_CXX_COMPILER")
                .value_or("msvc");
        if (host_compiler != "msvc" && host_compiler != "clang-cl")
            throw std::invalid_argument(
                "KATANA_PORT_CXX_COMPILER muss msvc oder clang-cl sein.");
        const auto host_linker =
            configured_environment_value("KATANA_PORT_LINKER").value_or("default");
        if (host_linker != "default" && host_linker != "msvc" &&
            host_linker != "lld")
            throw std::invalid_argument(
                "KATANA_PORT_LINKER muss default, msvc oder lld sein.");
        const auto requested_generator =
            configured_environment_value("KATANA_HOST_BUILD_GENERATOR");
        if (requested_generator &&
            *requested_generator != "Ninja" &&
            *requested_generator != "Visual Studio")
            throw std::invalid_argument(
                "KATANA_HOST_BUILD_GENERATOR muss Ninja oder Visual Studio sein.");
        const bool use_ninja =
            requested_generator
                ? *requested_generator == "Ninja"
                : compiler_launcher.has_value();
        if (compiler_launcher && !use_ninja)
            throw std::invalid_argument(
                "Ein Compiler-Launcher braucht den Ninja-Hostbuild; "
                "Visual Studio ignoriert CXX_COMPILER_LAUNCHER.");
#else
        const auto host_compiler = std::string("native");
        const auto host_linker = std::string("default");
        const bool use_ninja = true;
        if (configured_environment_value("KATANA_PORT_CXX_COMPILER") ||
            configured_environment_value("KATANA_PORT_LINKER"))
            throw std::invalid_argument(
                "KATANA_PORT_CXX_COMPILER und KATANA_PORT_LINKER sind nur fuer "
                "Windows-Portbuilds verfuegbar.");
#endif
        const auto generator_identity = use_ninja ? "ninja" : "vs";
        const auto configuration_identity =
            host_build_configuration == "Release"
                ? "release"
                : "relwithdebinfo";
        const auto build_path =
            report.output_root /
            ("build-" + host_compiler + '-' + host_linker + '-' +
             build_profile + '-' + configuration_identity + '-' +
             port_runtime_profile + '-' + generator_identity);
        const auto host_build_directory_existed_before_configure =
            safe_regular_port_directory_exists(
                build_path, "Inkrementeller Hostbuild-Cache");
        const auto reusable_host_build_state_existed_before_configure =
            host_build_directory_existed_before_configure &&
            safe_regular_port_file_exists(
                build_path / "CMakeCache.txt",
                "Inkrementeller Hostbuild-CMakeCache");
        if (!runtime_binding.build_targets_file.empty()) {
            const auto runtime_build_root =
                runtime_binding.build_targets_file.parent_path();
            const auto runtime_target =
                port_runtime_profile == "diagnostic-interpreter"
                    ? std::string_view("katana_runtime")
                    : std::string_view("katana_native_port_runtime");
            auto runtime_build =
                std::string("cmake --build ") +
                shell_quote(runtime_build_root) +
                " --target " + std::string(runtime_target);
            if (runtime_binding.multi_config)
                runtime_build +=
                    " --config " +
                    runtime_binding.build_configuration;
            runtime_build +=
                " --parallel " +
                std::to_string(host_compile_budget.effective);
#ifdef _WIN32
            if (runtime_binding.msbuild_generator) {
                runtime_build +=
                    " -- /nodeReuse:false /p:UseMultiToolTask=true "
                    "/p:EnforceProcessCountAcrossBuilds=true "
                    "/p:MultiProcMaxCount=" +
                    std::to_string(host_compile_budget.effective) +
                    " /p:CL_MPCount=" +
                    std::to_string(host_compile_budget.effective);
            }
#endif
            phase_timings.transition("runtime-sdk-build");
            std::cout
                << "KATANA_PORT_PHASE runtime-sdk-build"
                << " config="
                << runtime_binding.build_configuration
                << " generator="
                << (runtime_binding.multi_config ? "multi" : "single")
                << " compile_jobs_requested="
                << host_compile_budget.requested
                << " compile_jobs_effective="
                << host_compile_budget.effective
                << '\n'
                << std::flush;
            auto runtime_build_progress =
                port_progress.begin(
                    katana::ProgressOperation::HostRuntimeBuild,
                    katana::ProgressUnit::Steps,
                    std::nullopt,
                    "build-tree-runtime");
            katana::ProgressCounterSnapshot runtime_build_counters;
            runtime_build_counters.configured_workers =
                host_compile_budget.effective;
            runtime_build_progress.heartbeat(
                std::move(runtime_build_counters));
            const auto runtime_build_result =
                run_supervised_host_command(
#ifdef _WIN32
                    prepare_windows_host_command(
                        runtime_build,
                        true),
#else
                    normalized_host_command(runtime_build),
#endif
                    configured_port_host_command_runtime(
                        "runtime-sdk-build"),
                    &build_telemetry,
                    "runtime-sdk-build"
#ifdef _WIN32
                    , runtime_binding.msbuild_generator
                          ? std::optional<std::size_t>(
                                host_compile_budget.effective)
                          : std::optional<std::size_t>(0u)
#else
                    , std::nullopt
#endif
                    , [&runtime_build_progress,
                       &host_compile_budget] {
                          katana::ProgressCounterSnapshot counters;
                          counters.configured_workers =
                              host_compile_budget.effective;
                          runtime_build_progress.heartbeat(
                              std::move(counters));
                      }
                    );
            if (!runtime_build_result.process_tree_quiescent)
                throw katana::cli::Error(
                    katana::cli::ExitCode::BuildFailure,
                    "Runtime-SDK-Hostprozessbaum ist nicht "
                    "nachweislich quieszent.");
            if (runtime_build_result.timed_out) {
                throw katana::cli::Error(
                    katana::cli::ExitCode::BuildFailure,
                    "KATANA_PORT_HOST_COMMAND_TIMEOUT "
                    "stage=runtime-sdk-build process_tree=terminated");
            }
            if (runtime_build_result.exit_code != 0)
                throw katana::cli::Error(
                    katana::cli::ExitCode::BuildFailure,
                    "Buildtree-Runtime konnte vor dem Portlink nicht "
                    "aktualisiert werden.");
            runtime_build_progress.complete();
        }
        const auto host_build_event_root =
            build_path / ".katana-host-build-events";
        const auto host_build_tool_root =
            build_path / ".katana-host-build-tools";
        ensure_safe_port_directory_chain(
            report.output_root,
            build_path,
            "Inkrementeller Hostbuild-Cache");
        reset_host_build_event_root(
            report.output_root, host_build_event_root);
        const auto katana_cli_path =
            current_process_executable_path();
        const auto require_launcher_component = [](
            const std::string_view value,
            const std::string_view description) {
            if (value.empty() ||
                value.find_first_of(";\r\n\"") !=
                    std::string_view::npos)
                throw std::invalid_argument(
                    std::string(description) +
                    " ist nicht sicher als CMake-Launcher bindbar.");
        };
        const auto cli_component = katana_cli_path.generic_string();
        const auto event_component =
            host_build_event_root.generic_string();
        require_launcher_component(
            cli_component, "Katana-Hostbuild-Launcher");
        require_launcher_component(
            event_component, "Hostbuild-Ereignisverzeichnis");
        if (compiler_launcher)
            require_launcher_component(
                *compiler_launcher, "Compiler-Cache-Launcher");
        // Keep the Ninja command graph invariant when a compiler cache is
        // enabled, disabled or moved. The launcher resolves
        // KATANA_COMPILER_CACHE from the build process environment at
        // execution time; embedding the cache executable in every compile
        // edge would itself force a full rebuild when cache policy changes.
        const auto instrumented_compiler_launcher =
            cli_component + ";__host-build-tool;" +
            event_component + ";compile;--direct";
        const auto instrumented_linker_launcher =
            cli_component + ";__host-build-tool;" +
            event_component + ";link;--direct";
#ifdef _WIN32
        const auto windows_host_tool_wrappers =
            prepare_windows_host_tool_wrappers(
                report.output_root, host_build_tool_root);
        const auto archive_launcher_path =
            windows_host_tool_wrappers.archiver;
#else
        const auto archive_launcher_path =
            prepare_posix_host_archive_wrapper(
                report.output_root, host_build_tool_root);
#endif
        auto configure = std::string("cmake -S ") + shell_quote(report.output_root) + " -B " +
                         shell_quote(build_path);
#ifdef _WIN32
        if (use_ninja) {
            configure += " -G Ninja";
            configure += " -DCMAKE_CXX_COMPILER=" +
                         std::string(host_compiler == "clang-cl" ? "clang-cl" : "cl");
            if (const auto make_program =
                    configured_environment_value("KATANA_HOST_BUILD_MAKE_PROGRAM"))
                configure +=
                    " -DCMAKE_MAKE_PROGRAM=" + shell_quote(std::filesystem::path(*make_program));
        } else {
            configure += " -G \"Visual Studio 17 2022\" -A x64";
            if (host_compiler == "clang-cl") configure += " -T ClangCL";
            configure +=
                " -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_" +
                std::string(host_build_configuration == "Release"
                                ? "RELEASE"
                                : "RELWITHDEBINFO") +
                '=' + shell_quote(build_path);
        }
#else
        configure += " -G Ninja";
#endif
        configure +=
            " -DCMAKE_BUILD_TYPE=" + host_build_configuration +
            " -DKATANA_PORT_BUILD_PROFILE=" + build_profile;
#ifdef _WIN32
        if (host_compiler == "msvc" || host_compiler == "clang-cl") {
            configure += " -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=";
            if (host_build_configuration == "RelWithDebInfo")
                configure += "Embedded";
        }
#endif
        configure +=
            " -DKATANA_PORT_RUNTIME_PROFILE=" + port_runtime_profile;
        configure +=
            std::string(" -DKATANA_PERSISTENT_COMPILER_CACHE_ACTIVE=") +
            (compiler_launcher ? "ON" : "OFF");
        // The generated CMake option is cached across exports. Force the
        // cache-compatible choice whenever an MSVC object cache is active so
        // a prior PCH build cannot keep injecting /Fp and silently make every
        // generated translation unit non-cacheable.
        if ((host_compiler == "msvc" || host_compiler == "clang-cl") &&
            compiler_launcher)
            configure +=
                " -DKATANA_PERSISTENT_COMPILER_CACHE_USE_PCH=OFF";
        if (native_adapter_source_dir.has_value())
            configure +=
                " -DKATANA_NATIVE_ADAPTER_SOURCE_DIR=" +
                shell_quote(*native_adapter_source_dir);
        configure +=
            " -DKATANA_HOST_COMPILE_JOBS_REQUESTED=" +
            std::to_string(host_compile_budget.requested) +
            " -DKATANA_HOST_COMPILE_JOBS=" +
            std::to_string(host_compile_budget.effective);
        if (!runtime_binding.package_prefix.empty()) {
            configure +=
                " -DKATANA_RUNTIME_BUILD_TARGETS= "
                "-DKATANA_RUNTIME_ROOT= "
                "-DKATANA_RUNTIME_PREFIX=" +
                shell_quote(runtime_binding.package_prefix);
        } else if (!runtime_binding.build_targets_file.empty()) {
            configure +=
                " -DKATANA_RUNTIME_PREFIX= "
                "-DKATANA_RUNTIME_ROOT= "
                "-DKATANA_RUNTIME_BUILD_TARGETS=" +
                shell_quote(
                    runtime_binding.build_targets_file);
        } else {
            configure +=
                " -DKATANA_RUNTIME_PREFIX= "
                "-DKATANA_RUNTIME_BUILD_TARGETS= "
                "-DKATANA_RUNTIME_ROOT=" +
                shell_quote(runtime_binding.source_root);
        }
        if (host_linker != "default")
            configure += " -DCMAKE_LINKER_TYPE=" +
                         std::string(host_linker == "msvc" ? "MSVC" : "LLD");
        if (use_ninja) {
            configure +=
                " -DCMAKE_CXX_COMPILER_LAUNCHER=" +
                shell_quote(std::filesystem::path(
                    instrumented_compiler_launcher));
            configure +=
                " -DCMAKE_CXX_LINKER_LAUNCHER=" +
                shell_quote(std::filesystem::path(
                    instrumented_linker_launcher));
            configure +=
                " -DKATANA_HOST_ARCHIVE_LAUNCHER=" +
                shell_quote(archive_launcher_path);
        } else {
            // The incremental build directory survives exports. Clear a
            // previously configured launcher when this invocation requests
            // none instead of silently retaining stale host-build state.
            configure += " -DCMAKE_CXX_COMPILER_LAUNCHER=";
            configure += " -DCMAKE_CXX_LINKER_LAUNCHER=";
            configure += " -DKATANA_HOST_ARCHIVE_LAUNCHER=";
        }
        // Refresh or clear the cached source assertion on every configure.
        // Dirty/local exporters use the sentinel identity and must not retain
        // a previous clean-HEAD assertion in the reusable build directory.
        configure += " -DKATANA_GIT_COMMIT=";
        if (katana::build_contract::katana_git_commit !=
            "0000000000000000000000000000000000000000")
            configure +=
                std::string(katana::build_contract::katana_git_commit);
        phase_timings.transition("configure");
        std::cout
            << "KATANA_PORT_PHASE configure compile_jobs_requested="
            << host_compile_budget.requested
            << " compile_jobs_effective="
            << host_compile_budget.effective << '\n'
            << std::flush;
        const auto configure_command =
#ifdef _WIN32
            prepare_windows_host_command(configure, true);
#else
            normalized_host_command(configure);
#endif
        const auto configure_runtime =
            configured_port_host_command_runtime("configure");
        auto configure_progress =
            port_progress.begin(
                katana::ProgressOperation::Configure,
                katana::ProgressUnit::Steps,
                1u,
                "port-host-configure");
        SupervisedHostCommandResult configure_result;
        try {
            configure_result =
                run_supervised_host_command(
                    configure_command,
                    configure_runtime,
                    &build_telemetry,
                    "configure"
#ifdef _WIN32
                    , std::optional<std::size_t>(0u)
#endif
                    );
        } catch (const std::exception& process_error) {
            try {
                const auto removed = remove_new_failed_port_host_build_state(
                    report.output_root,
                    build_path,
                    reusable_host_build_state_existed_before_configure);
                if (!removed &&
                    reusable_host_build_state_existed_before_configure)
                    std::cerr
                        << "KATANA_PORT_HOST_CACHE_PRESERVED stage=configure "
                           "reason=process-error\n";
            } catch (const std::exception& cleanup_error) {
                throw katana::cli::Error(
                    katana::cli::ExitCode::BuildFailure,
                    std::string(
                        "Port-Hostbuild-Prozess konnte nicht sicher ausgefuehrt "
                        "und sein neuer unvollstaendiger CMake-Zustand nicht "
                        "bereinigt werden: ") +
                        process_error.what() + ' ' +
                        cleanup_error.what());
            }
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                std::string(
                    "Port-Hostbuild-Prozess konnte nicht sicher ausgefuehrt "
                    "werden; ") +
                    (reusable_host_build_state_existed_before_configure
                         ? "der vorhandene inkrementelle Hostcache blieb erhalten: "
                         : "der neue unvollstaendige Zustand wurde entfernt: ") +
                    process_error.what());
        }
        if (!configure_result.process_tree_quiescent)
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                "Configure-Hostprozessbaum ist nicht nachweislich "
                "quieszent.");
        if (configure_result.exit_code != 0) {
            const auto failure =
                configure_result.timed_out
                    ? std::string(
                          "KATANA_PORT_HOST_COMMAND_TIMEOUT "
                          "stage=configure limit_ms=") +
                          std::to_string(
                              configure_runtime->count()) +
                          " process_tree=terminated"
                    : std::string(
                          "Port-Hostbuild konnte nicht konfiguriert werden");
            try {
                const auto removed = remove_new_failed_port_host_build_state(
                    report.output_root,
                    build_path,
                    reusable_host_build_state_existed_before_configure);
                if (!removed &&
                    reusable_host_build_state_existed_before_configure)
                    std::cerr
                        << "KATANA_PORT_HOST_CACHE_PRESERVED stage=configure "
                           "reason=configure-failure\n";
            } catch (const std::exception& cleanup_error) {
                throw katana::cli::Error(
                    katana::cli::ExitCode::BuildFailure,
                    failure +
                        "; neuer unvollstaendiger CMake-Zustand konnte nicht "
                        "bereinigt werden: " +
                        cleanup_error.what());
            }
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                failure +
                    (reusable_host_build_state_existed_before_configure
                         ? "; vorhandener inkrementeller Hostcache blieb erhalten."
                         : "; neuer unvollstaendiger CMake-Zustand wurde entfernt."));
        }
#ifdef _WIN32
        try {
            require_optimized_msvc_configuration(
                build_path, host_build_configuration);
        } catch (const std::exception& configuration_error) {
            try {
                const auto removed = remove_new_failed_port_host_build_state(
                    report.output_root,
                    build_path,
                    reusable_host_build_state_existed_before_configure);
                if (!removed &&
                    reusable_host_build_state_existed_before_configure)
                    std::cerr
                        << "KATANA_PORT_HOST_CACHE_PRESERVED stage=configure "
                           "reason=configuration-contract\n";
            } catch (const std::exception& cleanup_error) {
                throw katana::cli::Error(
                    katana::cli::ExitCode::BuildFailure,
                    std::string("Unsicherer Port-Hostbuild-Configure und fehlgeschlagene "
                                "Bereinigung: ") +
                        configuration_error.what() + ' ' + cleanup_error.what());
            }
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                std::string("Unsicherer Port-Hostbuild-Configure wurde abgelehnt; ") +
                    (reusable_host_build_state_existed_before_configure
                         ? "der vorhandene inkrementelle Hostcache blieb erhalten: "
                         : "der neue unvollstaendige Zustand wurde entfernt: ") +
                    configuration_error.what());
        }
        const auto windows_configured_host_tools =
            use_ninja
                ? std::optional<WindowsConfiguredHostTools>{}
                : std::optional<WindowsConfiguredHostTools>{
                      configured_windows_host_tools(
                          build_path / "CMakeCache.txt")};
#endif
        configure_progress.complete();
        if (normalized_telemetry_jsonl_path.has_value() &&
            !build_telemetry.record_resolved_environment(
                katana::cli::resolve_port_build_cmake_environment(
                    build_path,
                    katana::analysis::
                        configured_analysis_parallel_jobs(),
                    katana::codegen::configured_port_codegen_jobs(
                        report.partitions),
                    host_compile_budget.requested,
                    host_compile_budget.effective,
                    resolved_runtime_jobs())))
            throw katana::cli::Error(
                katana::cli::ExitCode::InputOutput,
                "Der kritische post-Configure-Umgebungsrecord "
                "konnte nicht vollstaendig gebunden werden.");
        reset_host_build_event_root(
            report.output_root, host_build_event_root);
        const auto generated_artifacts =
            validated_generated_artifact_files(report.output_root);
        const auto generated_sources =
            validated_generated_source_files(report.output_root);
        if (!generated_artifacts || !generated_sources)
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                "Hostbuild-TU-Plan besitzt kein exaktes "
                "generiertes Quellinventar.");
        const auto is_cpp_source = [](const std::filesystem::path& path) {
            auto extension = path.extension().string();
            std::transform(
                extension.begin(), extension.end(), extension.begin(),
                [](const unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            return extension == ".c" || extension == ".cc" ||
                   extension == ".cpp" || extension == ".cxx" ||
                   extension == ".c++";
        };
        const auto generated_translation_units =
            static_cast<std::uint64_t>(std::count_if(
                generated_artifacts->begin(),
                generated_artifacts->end(),
                is_cpp_source));
        const auto support_translation_units =
            static_cast<std::uint64_t>(std::count_if(
                generated_sources->begin(),
                generated_sources->end(),
                is_cpp_source));
        if (generated_translation_units == 0u ||
            support_translation_units == 0u)
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                "Hostbuild-TU-Plan ist unerwartet leer.");
        // katana_generated normally enables one CMake-generated PCH compile
        // edge. The MSVC-compatible cache path deliberately disables that
        // edge because /Fp would make every object non-cacheable.
        const std::uint64_t generated_pch_translation_units =
#ifdef _WIN32
            ((host_compiler == "msvc" || host_compiler == "clang-cl") &&
             compiler_launcher)
                ? 0u
                : 1u;
#else
            1u;
#endif
        std::uint64_t native_translation_unit_supplement = 0u;
        if (verified_native_port) {
            const auto supplement =
                validated_native_host_translation_unit_supplement(
                    build_path);
            if (!supplement)
                throw katana::cli::Error(
                    katana::cli::ExitCode::BuildFailure,
                    "Nativer Hostbuild besitzt keinen exakten "
                    "Runtime-/Adapter-TU-Plan.");
            native_translation_unit_supplement = *supplement;
        }
        if (generated_translation_units >
                std::numeric_limits<std::uint64_t>::max() -
                    support_translation_units -
                    generated_pch_translation_units ||
            generated_translation_units + support_translation_units +
                    generated_pch_translation_units >
                std::numeric_limits<std::uint64_t>::max() -
                    native_translation_unit_supplement)
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                "Hostbuild-TU-Plan ist numerisch ungueltig.");
        const auto planned_translation_units =
            generated_translation_units + support_translation_units +
            generated_pch_translation_units +
            native_translation_unit_supplement;
        auto built_executable = build_path / target_name;
#ifdef _WIN32
        built_executable += ".exe";
#endif
        std::optional<katana::io::InputProvenance>
            previous_executable_identity;
        std::error_code previous_status_error;
        const auto previous_status = std::filesystem::symlink_status(
            built_executable, previous_status_error);
        if (!previous_status_error &&
            std::filesystem::is_regular_file(previous_status) &&
            !unsafe_port_filesystem_link(
                built_executable, previous_status))
            previous_executable_identity =
                katana::io::capture_input_provenance(
                    "previous-host-executable", built_executable);
        else if (previous_status_error !=
                     std::errc::no_such_file_or_directory &&
                 (previous_status_error ||
                  previous_status.type() !=
                      std::filesystem::file_type::not_found))
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                "Vorheriges Hostartefakt konnte nicht sicher geprueft "
                "werden.");
        // CMake's Ninja/MSVC launcher runs vs_link_exe for a required link
        // pass and may run FINAL-LINK once more while updating a manifest.
        // A native product also builds the small streaming link-audit tool,
        // so its graph contains two executable links. Zero or omitted passes
        // remain up-to-date hits; an excess still fails closed.
        const auto planned_executables =
            verified_native_port ? 2u : 1u;
#ifdef _WIN32
        const auto planned_link_steps =
            planned_executables * (use_ninja ? 2u : 1u);
#else
        const auto planned_link_steps = planned_executables;
#endif
        katana::cli::HostBuildProgressObserver host_build_progress(
            host_build_event_root,
            katana::cli::HostBuildProgressPlan{
                planned_translation_units,
                1u,
                planned_link_steps,
                host_compile_budget.effective},
            port_progress);
        auto build =
            std::string("cmake --build ") + shell_quote(build_path) + " --target " + target_name;
        build += " --parallel " +
                 std::to_string(host_compile_budget.effective);
#ifdef _WIN32
        if (!use_ninja) {
            auto wrapper_directory =
                windows_host_tool_wrappers.directory.string();
            if (wrapper_directory.find('"') != std::string::npos)
                throw std::invalid_argument(
                    "Windows-Hosttool-Wrapperpfad ist nicht sicher "
                    "quotierbar.");
            if (!wrapper_directory.ends_with('\\'))
                wrapper_directory += '\\';
            build +=
                " --config " + host_build_configuration +
                " -- /nodeReuse:false "
                "/p:UseMultiToolTask=true "
                "/p:EnforceProcessCountAcrossBuilds=true "
                "/p:MultiProcMaxCount=" +
                std::to_string(host_compile_budget.effective) +
                " /p:CL_MPCount=" +
                std::to_string(host_compile_budget.effective) +
                " /p:CLToolPath=\"" + wrapper_directory +
                "\" /p:CLToolExe=katana-host-cl-wrapper.exe"
                " /p:LibToolPath=\"" + wrapper_directory +
                "\" /p:LibToolExe=katana-host-archive-wrapper.exe"
                " /p:LinkToolPath=\"" + wrapper_directory +
                "\" /p:LinkToolExe=katana-host-link-wrapper.exe";
        }
#endif
        phase_timings.transition("host-build");
        std::cout
            << "KATANA_PORT_PHASE host-build compile_jobs_requested="
            << host_compile_budget.requested
            << " compile_jobs_effective="
            << host_compile_budget.effective << '\n'
            << std::flush;
#ifdef _WIN32
        auto windows_host_build_environment =
            "set \"KATANA_HOST_BUILD_EVENT_ROOT=" +
            host_build_event_root.string() + "\" && ";
        if (compiler_cache_binding &&
            compiler_cache_binding->managed_storage &&
            !configured_environment_value("SCCACHE_DIR")) {
            const auto storage =
                compiler_cache_binding->managed_storage->string();
            if (storage.find_first_of("\"\r\n") != std::string::npos ||
                !prepare_managed_compiler_cache_storage(
                    *compiler_cache_binding->managed_storage))
                throw std::runtime_error(
                    "Verwalteter Compiler-Cache besitzt keinen sicheren "
                    "persistenten Speicherpfad.");
            windows_host_build_environment +=
                "set \"SCCACHE_DIR=" + storage + "\" && ";
        }
        if (!use_ninja) {
            if (!windows_configured_host_tools)
                throw std::runtime_error(
                    "Visual-Studio-Hosttools wurden nicht gebunden.");
            windows_host_build_environment +=
                "set \"KATANA_HOST_BUILD_REAL_COMPILER=" +
                windows_configured_host_tools->compiler +
                "\" && set \"KATANA_HOST_BUILD_REAL_ARCHIVER=" +
                windows_configured_host_tools->archiver +
                "\" && set \"KATANA_HOST_BUILD_REAL_LINKER=" +
                windows_configured_host_tools->linker +
                "\" && ";
        }
#endif
        const auto build_command =
#ifdef _WIN32
            prepare_windows_host_command(
                windows_host_build_environment + build,
                true);
#else
            normalized_host_command(
                "KATANA_HOST_BUILD_EVENT_ROOT=" +
                shell_quote(host_build_event_root) + ' ' + build);
#endif
        const auto build_runtime =
            configured_port_host_command_runtime("host-build");
        SupervisedHostCommandResult build_result;
        try {
            build_result = run_supervised_host_command(
                    build_command,
                    build_runtime,
                    &build_telemetry,
                    "host-build"
#ifdef _WIN32
                    , std::optional<std::size_t>(
                          use_ninja
                              ? 0u
                              : host_compile_budget.effective)
#else
                    , std::nullopt
#endif
                    , [&host_build_progress] {
                          host_build_progress.poll();
                      });
        } catch (...) {
            host_build_progress.fail();
            throw;
        }
        if (!build_result.process_tree_quiescent) {
            host_build_progress.fail();
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                "Hostbuild-Prozessbaum ist nicht nachweislich "
                "quieszent.");
        }
        if (build_result.timed_out) {
            host_build_progress.fail();
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                std::string(
                    "KATANA_PORT_HOST_COMMAND_TIMEOUT "
                    "stage=host-build limit_ms=") +
                    std::to_string(build_runtime->count()) +
                    " process_tree=terminated; inkrementeller Ninja-/CMake-"
                    "Buildzustand bleibt fuer den naechsten Lauf erhalten.");
        }
        if (build_result.exit_code != 0) {
            host_build_progress.fail();
            throw katana::cli::Error(katana::cli::ExitCode::BuildFailure,
                                     "Port-Hosttarget konnte nicht gebaut werden.");
        }
        std::error_code built_status_error;
        const auto built_status = std::filesystem::symlink_status(
            built_executable, built_status_error);
        if (built_status_error ||
            !std::filesystem::is_regular_file(built_status) ||
            unsafe_port_filesystem_link(
                built_executable, built_status)) {
            host_build_progress.fail();
            throw katana::cli::Error(katana::cli::ExitCode::BuildFailure,
                                     "Port-Hostbuild besitzt kein ausfuehrbares Artefakt.");
        }
        const auto built_executable_identity =
            katana::io::capture_input_provenance(
                "built-host-executable", built_executable);
        if (verified_native_port) {
            const auto expected_marker =
                std::string("KATANA_NATIVE_PORT_BUILD_IDENTITY_V1:") +
                native_port_artifact_identity;
            if (!regular_file_contains_token(
                    built_executable, expected_marker)) {
                host_build_progress.fail();
                throw katana::cli::Error(
                    katana::cli::ExitCode::BuildFailure,
                    "Hostprogramm und ausgewaehlte Native-Port-Definition "
                    "besitzen abweichende Identitaeten; Publikation wurde "
                    "verweigert.");
            }
        }
        const auto unchanged_existing_artifact =
            previous_executable_identity &&
            previous_executable_identity->size ==
                built_executable_identity.size &&
            previous_executable_identity->sha256 ==
                built_executable_identity.sha256;
        // The successful, instrumented target build is authoritative for
        // graph edges which did not admit a real tool invocation. The
        // observer derives their exact count atomically from its strict final
        // scan, so a late terminal event cannot race a caller-side snapshot.
        const auto host_build_completion_proof =
            katana::cli::HostBuildCompletionProof{
                true,
                build_result.process_tree_quiescent,
                true,
                true,
                unchanged_existing_artifact};
        if (!host_build_progress.finish_success(
                host_build_completion_proof))
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                "Hostbuild-Werkzeugfortschritt ist unvollstaendig oder "
                "widerspruechlich.");
        auto packaging_progress =
            port_progress.begin(
                katana::ProgressOperation::Packaging,
                katana::ProgressUnit::Steps,
                7u,
                "port-distribution");
        const auto published_executable = report.output_root / built_executable.filename();
        require_safe_replaceable_port_file(
            report.output_root,
            published_executable,
            "Publiziertes Hostprogramm");
        std::filesystem::copy_file(built_executable,
                                   published_executable,
                                   std::filesystem::copy_options::overwrite_existing);
        packaging_progress.advance(1u);
        const auto runtime_dependencies =
            publish_port_runtime_dependencies(
                built_executable.parent_path(), report.output_root);
        packaging_progress.advance(1u);
        const auto recipe = katana::runtime::parse_disc_install_recipe(report.disc_install_recipe);
        if (recipe.job_generation != report.job_generation ||
            recipe.content_identity != report.content_identity)
            throw std::runtime_error("Disc-Installations-Recipe besitzt eine falsche Bindung.");
        const auto recipe_sha256 =
            katana::io::capture_input_provenance("disc-install-recipe", report.disc_install_recipe)
                .sha256;
        const auto executable_sha256 =
            katana::io::capture_input_provenance("host-executable", published_executable).sha256;
        packaging_progress.advance(1u);
        const auto install_manifest = report.output_root / "content" / "game.katana-install.json";
        ensure_safe_port_directory_chain(
            report.output_root,
            install_manifest.parent_path(),
            "Port-Installationsmanifest");
        require_safe_replaceable_port_file(
            report.output_root,
            install_manifest,
            "Port-Installationsmanifest");
        std::ofstream manifest(install_manifest, std::ios::binary | std::ios::trunc);
        manifest << disc_install_manifest_document(
            recipe,
            recipe_sha256,
            published_executable.filename().generic_string(),
            executable_sha256);
        if (!manifest)
            throw std::runtime_error("Disc-Installationsmanifest konnte nicht finalisiert werden.");
        manifest.close();
        packaging_progress.advance(1u);
        const auto runtime_manifest_path =
            report.output_root / "runtime" / "runtime-dependencies.json";
        ensure_safe_port_directory_chain(
            report.output_root,
            runtime_manifest_path.parent_path(),
            "Port-Runtimemanifest");
        require_safe_replaceable_port_file(
            report.output_root,
            runtime_manifest_path,
            "Port-Runtimemanifest");
        std::ofstream runtime_manifest(runtime_manifest_path,
                                       std::ios::binary | std::ios::trunc);
        runtime_manifest << runtime_dependency_manifest_document(
            recipe,
            port_runtime_profile,
            runtime_dependencies);
        if (!runtime_manifest)
            throw std::runtime_error(
                "Runtime-Abhaengigkeitsmanifest konnte nicht geschrieben werden.");
        runtime_manifest.close();
        ensure_safe_port_directory_chain(
            report.output_root,
            report.output_root / "user-data",
            "Lokaler Portdatenordner");
        packaging_progress.advance(1u);
        phase_timings.transition("package");
        std::cout << "KATANA_PORT_PHASE package\n" << std::flush;
        const auto publish_transaction =
            begin_port_publish_transaction(publish_paths);
        copy_validated_port_distribution(
            report.output_root,
            publish_transaction.stage,
            target_name,
            recipe,
            port_runtime_profile);
        packaging_progress.advance(1u);
        ensure_safe_port_directory_chain(
            publish_transaction.stage,
            publish_transaction.stage / "user-data",
            "Publizierter lokaler Portdatenordner");
        write_exclusive_safe_port_file(
            publish_transaction.stage / ".katana-publish-owner",
            publish_transaction.owner_document,
            "Neuer Port-Publish-Eigentuemermarker");
        const auto existing_output =
            safe_regular_port_directory_exists(
                publish_paths.output, "Bestehendes Portpaket");
        if (existing_output) {
            std::filesystem::rename(
                publish_paths.output,
                publish_transaction.backup);
            write_port_publish_state(
                publish_transaction,
                "old-moved");
            maybe_crash_port_publish_for_test("after-old-move");
        }
        std::filesystem::rename(
            publish_transaction.stage,
            publish_paths.output);
        write_port_publish_state(
            publish_transaction,
            "new-published");
        maybe_crash_port_publish_for_test("after-new-publish");
        if (existing_output)
            katana::codegen::preserve_local_port_user_data(
                publish_transaction.backup,
                publish_paths.output);
        write_port_publish_state(
            publish_transaction,
            "user-data-preserved");
        if (existing_output)
            remove_safe_port_tree(
                publish_transaction.backup,
                "Publiziertes altes Port-Backup");
        write_port_publish_state(
            publish_transaction,
            "committed");
        remove_owned_output_publish_marker(
            publish_transaction);
        cleanup_owned_port_publish_transaction(
            publish_transaction);
        packaging_progress.complete();
        std::cout << "Portpaket erzeugt: " << absolute_output.string() << '\n'
                  << "Funktionen: " << report.functions << '\n'
                  << "Partitionen: " << report.partitions << '\n'
                  << "Codegen-Cache-Hits: " << report.codegen_cache_hits << '\n'
                  << "Codegen-Cache-Misses: " << report.codegen_cache_misses << '\n'
                  << "Metadaten-Cache-Hit: "
                  << (report.metadata_cache_hit ? "ja" : "nein") << '\n'
                  << "Analyse-/IR-Cache-Hit: "
                  << (whole_export_cache_hit ? "ja" : "nein") << '\n'
                  << "Installations-Recipe-Tracks: " << report.disc_tracks << '\n'
                  << "Retail-Sektoren im Portpaket: 0\n"
                  << "Hostcompiler: " << host_compiler << '\n'
                  << "Hostlinker: " << host_linker << '\n'
                  << "Hostkonfiguration: " << host_build_configuration << '\n'
                  << "Buildprofil: " << build_profile << '\n'
                  << "Runtimeprofil: " << port_runtime_profile << '\n'
                   << "Inkrementeller Hostbuild-Cache: " << build_path.string() << '\n'
                   << "Optimierter Hostbuild erfolgreich: " << target_name << '\n';
        phase_timings.transition("complete");
        telemetry_run.complete();
        return 0;
    } catch (...) {
        const auto original_error = std::current_exception();
        try {
            recover_port_publish_transaction(publish_paths);
        } catch (const std::exception& recovery_error) {
            throw std::runtime_error(
                std::string(
                    "Port-Publishing scheiterte; die transaktionseigene "
                    "Recovery liess alle nicht sicher zuordenbaren Daten "
                    "unangetastet: ") +
                recovery_error.what());
        }
        std::rethrow_exception(original_error);
    }
    } catch (const katana::cli::Error& error) {
        require_requested_failure_telemetry(
            telemetry_run.fail(
                katana::cli::exit_status(error.code())));
        throw;
    } catch (const std::invalid_argument&) {
        require_requested_failure_telemetry(
            telemetry_run.fail(
                katana::cli::exit_status(
                    katana::cli::ExitCode::InvalidInput)));
        throw;
    } catch (const std::filesystem::filesystem_error&) {
        require_requested_failure_telemetry(
            telemetry_run.fail(
                katana::cli::exit_status(
                    katana::cli::ExitCode::InputOutput)));
        throw;
    } catch (const katana::io::InputOutputError&) {
        require_requested_failure_telemetry(
            telemetry_run.fail(
                katana::cli::exit_status(
                    katana::cli::ExitCode::InputOutput)));
        throw;
    } catch (const std::runtime_error&) {
        require_requested_failure_telemetry(
            telemetry_run.fail(
                katana::cli::exit_status(
                    katana::cli::ExitCode::ProcessingFailure)));
        throw;
    } catch (...) {
        require_requested_failure_telemetry(
            telemetry_run.fail(
                katana::cli::exit_status(
                    katana::cli::ExitCode::InternalError)));
        throw;
    }
}

katana::agent::ExecutableMaterializationWorld load_agent_world(
    const std::filesystem::path& path,
    std::string* artifact_sha256) {
    const auto document = read_safe_small_port_file(
        path,
        katana::agent::materialization_world_max_binary_artifact_bytes,
        "Materialization-World-Artefakt");
    if (artifact_sha256 != nullptr)
        *artifact_sha256 = katana::io::sha256_bytes(document);
    katana::agent::ExecutableMaterializationWorld world;
    if (!katana::agent::parse_agent_world_binary(
            std::span(
                reinterpret_cast<const std::uint8_t*>(document.data()),
                document.size()),
            world))
        throw std::invalid_argument(
            "Materialization-World-Artefakt ist ungueltig, beschaedigt oder "
            "nicht schema-kompatibel.");
    return world;
}

std::uint64_t parse_agent_stable_id(const std::string_view text) {
    auto digits = text;
    int base = 10;
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        digits.remove_prefix(2u);
        base = 16;
    }
    if (digits.empty())
        throw std::invalid_argument("Agenten-ID ist leer.");
    std::uint64_t value = 0u;
    const auto parsed = std::from_chars(
        digits.data(), digits.data() + digits.size(), value, base);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != digits.data() + digits.size() || value == 0u)
        throw std::invalid_argument(
            "Agenten-ID muss eine positive dezimale oder 0x-Hex-ID sein.");
    return value;
}

void write_agent_string_vector_json(
    std::ostream& output,
    const std::vector<std::string>& values) {
    output << '[';
    for (std::size_t index = 0u; index < values.size(); ++index) {
        if (index != 0u) output << ',';
        output << katana::io::quote_json(values[index]);
    }
    output << ']';
}

void write_agent_frontier_json(
    std::ostream& output,
    const katana::agent::FrontierEntry& entry) {
    const bool projected_replacement_owner =
        entry.family == "replacement-reachability" &&
        entry.blocked_functions.size() == 1u;
    const std::string_view agent_owner = projected_replacement_owner
        ? entry.blocked_functions.front()
        : entry.owner;
    output << "{\"id\":" << entry.id.value
           << ",\"family\":" << katana::io::quote_json(entry.family)
           << ",\"owner\":" << katana::io::quote_json(agent_owner);
    if (projected_replacement_owner)
        output << ",\"world_owner\":"
               << katana::io::quote_json(entry.owner);
    output << ",\"site\":" << katana::io::quote_json(entry.site)
           << ",\"state\":"
           << katana::io::quote_json(
                  katana::agent::frontier_state_name(entry.state))
           << ",\"proof\":"
           << katana::io::quote_json(
                  katana::agent::frontier_proof_name(entry.proof))
           << ",\"severity\":"
           << katana::io::quote_json(
                  katana::agent::frontier_severity_name(entry.severity))
           << ",\"missing_proof\":"
           << katana::io::quote_json(entry.missing_proof)
           << ",\"fanout\":" << entry.fanout
           << ",\"runtime_evidence_required\":"
           << (entry.runtime_evidence_required ? "true" : "false")
           << ",\"static_complete\":"
           << (entry.static_complete ? "true" : "false")
           << ",\"blocked_kind\":"
           << katana::io::quote_json(
                  katana::agent::frontier_block_kind_name(
                      entry.blocked_kind))
           << ",\"dependency_generation\":"
           << entry.dependency_generation
           << ",\"evidence_digest\":" << entry.evidence_digest
           << ",\"evidence\": [";
    for (std::size_t index = 0u; index < entry.evidence.size(); ++index) {
        if (index != 0u) output << ',';
        output << entry.evidence[index].value;
    }
    output << "],\"contracts\":";
    write_agent_string_vector_json(output, entry.contracts);
    output << ",\"blocked_sites\":";
    write_agent_string_vector_json(output, entry.blocked_sites);
    output << ",\"blocked_functions\":";
    write_agent_string_vector_json(output, entry.blocked_functions);
    output << ",\"blocked_roots\":";
    write_agent_string_vector_json(output, entry.blocked_roots);
    output << ",\"blocked_modules\":";
    write_agent_string_vector_json(output, entry.blocked_modules);
    output << ",\"blocked_materializations\":";
    write_agent_string_vector_json(output, entry.blocked_materializations);
    output << ",\"blocked_hardware\":";
    write_agent_string_vector_json(output, entry.blocked_hardware);
    output << ",\"causal_chain\":";
    write_agent_string_vector_json(output, entry.causal_chain);
    output << ",\"source_paths\":";
    write_agent_string_vector_json(output, entry.source_paths);
    output << ",\"source_symbols\":";
    write_agent_string_vector_json(output, entry.source_symbols);
    output << ",\"invariants\":";
    write_agent_string_vector_json(output, entry.invariants);
    output << ",\"acceptance_criteria\":";
    write_agent_string_vector_json(output, entry.acceptance_criteria);
    output << '}';
}

int next_analysis_task_cli(const std::filesystem::path& artifact) {
    const auto world = load_agent_world(artifact);
    // Expose two deterministic conflict-checked implementation waves. The
    // orchestrator still admits at most three concurrent writers and assigns
    // the second wave deliberately; emitting six here only avoids another
    // artifact read and leaves scheduling/authority unchanged.
    std::array<katana::agent::AgentTaskView, 6u> tasks{};
    std::size_t task_count = 0u;
    const bool has_frontier = katana::agent::next_agent_tasks(
        world, tasks, task_count);
    const auto& task = tasks.front();
    const auto decision = has_frontier
        ? task.decision
        : katana::agent::evaluate_agent_decision(world);
    std::cout << "{\"schema\":2,\"kind\":\"katana-agent-task\""
              << ",\"task_id\":" << decision.focus.value
              << ",\"decision\":"
              << katana::io::quote_json(
                     katana::agent::agent_decision_kind_name(
                         decision.kind))
              << ",\"reason\":"
              << katana::io::quote_json(decision.reason)
              << ",\"focus\":" << decision.focus.value
              << ",\"actionable_frontier\":"
              << decision.actionable_frontier
              << ",\"priority\":"
              << (has_frontier && task.frontier != nullptr
                      ? static_cast<unsigned>(task.frontier->severity) + 1u
                      : 0u)
              << ",\"category\":";
    if (has_frontier && task.frontier != nullptr)
        std::cout << katana::io::quote_json(task.frontier->family);
    else
        std::cout << "null";
    std::cout << ",\"title\":";
    if (has_frontier && task.frontier != nullptr)
        std::cout << katana::io::quote_json(
            task.frontier->missing_proof.empty()
                ? task.frontier->family
                : task.frontier->missing_proof);
    else
        std::cout << "null";
    std::cout << ",\"must_not_do\":["
              << "\"do-not-promote-runtime-hints-to-static-proof\","
              << "\"do-not-add-runtime-sh4-decoding\","
              << "\"do-not-add-title-specific-address-seeds\"]"
              << ",\"acceptance_predicate\":{"
              << "\"frontier_id\":" << decision.focus.value
              << ",\"must_be_resolved\":true,"
              << "\"proof_downgrades\":0,"
              << "\"new_incomplete_roots\":0}"
              << ",\"frontier\":";
    if (has_frontier && task.frontier != nullptr)
        write_agent_frontier_json(std::cout, *task.frontier);
    else
        std::cout << "null";
    std::cout << ",\"task_count\":" << task_count
              << ",\"tasks\":[";
    for (std::size_t index = 0u; index < task_count; ++index) {
        if (index != 0u) std::cout << ',';
        const auto& batched = tasks[index];
        const auto* const frontier = batched.frontier;
        std::cout << "{\"task_id\":" << batched.decision.focus.value
                  << ",\"priority\":"
                  << (frontier != nullptr
                          ? static_cast<unsigned>(frontier->severity) + 1u
                          : 0u)
                  << ",\"category\":";
        if (frontier != nullptr)
            std::cout << katana::io::quote_json(frontier->family);
        else
            std::cout << "null";
        std::cout << ",\"title\":";
        if (frontier != nullptr)
            std::cout << katana::io::quote_json(
                frontier->missing_proof.empty()
                    ? frontier->family
                    : frontier->missing_proof);
        else
            std::cout << "null";
        std::cout << ",\"acceptance_predicate\":{"
                  << "\"frontier_id\":"
                  << batched.decision.focus.value
                  << ",\"must_be_resolved\":true,"
                  << "\"proof_downgrades\":0,"
                  << "\"new_incomplete_roots\":0}"
                  << ",\"frontier\":";
        if (frontier != nullptr)
            write_agent_frontier_json(std::cout, *frontier);
        else
            std::cout << "null";
        std::cout << '}';
    }
    std::cout << ']';
    std::cout << "}\n" << std::flush;
    if (!std::cout)
        throw std::runtime_error(
            "Agenten-Task konnte nicht vollstaendig ausgegeben werden.");
    return 0;
}

int explain_analysis_frontier_cli(
    const std::filesystem::path& artifact,
    const std::uint64_t frontier_id) {
    const auto world = load_agent_world(artifact);
    katana::agent::FrontierExplanationView explanation;
    if (!katana::agent::explain_frontier(
            world,
            katana::agent::StableId{frontier_id},
            explanation) ||
        explanation.frontier == nullptr)
        throw std::invalid_argument(
            "Frontier-ID existiert nicht im Materialization-World-Artefakt.");
    std::cout << "{\"schema\":1,\"kind\":\"katana-agent-explanation\""
              << ",\"related_nodes\":" << explanation.related_nodes
              << ",\"related_evidence\":"
              << explanation.related_evidence
              << ",\"frontier\":";
    write_agent_frontier_json(std::cout, *explanation.frontier);
    std::cout << "}\n" << std::flush;
    if (!std::cout)
        throw std::runtime_error(
            "Agenten-Erklaerung konnte nicht vollstaendig ausgegeben werden.");
    return 0;
}

const char* agent_diff_entity_name(
    const katana::agent::AgentDiffEntityKind kind) noexcept {
    switch (kind) {
    case katana::agent::AgentDiffEntityKind::WorldMetadata:
        return "world-metadata";
    case katana::agent::AgentDiffEntityKind::Evidence:
        return "evidence";
    case katana::agent::AgentDiffEntityKind::Node:
        return "node";
    case katana::agent::AgentDiffEntityKind::Dependency:
        return "dependency";
    case katana::agent::AgentDiffEntityKind::Frontier:
        return "frontier";
    }
    return "unknown";
}

const char* agent_diff_change_name(
    const katana::agent::AgentDiffChange change) noexcept {
    switch (change) {
    case katana::agent::AgentDiffChange::Added:
        return "added";
    case katana::agent::AgentDiffChange::Removed:
        return "removed";
    case katana::agent::AgentDiffChange::Changed:
        return "changed";
    }
    return "unknown";
}

int diff_analysis_cli(const std::filesystem::path& before_path,
                      const std::filesystem::path& after_path) {
    const auto before = load_agent_world(before_path);
    const auto after = load_agent_world(after_path);
    const auto delta = measure_agent_iteration(before, after);
    const auto measured = katana::agent::diff_agent_worlds(before, after, {});
    constexpr std::size_t maximum_diff_entries =
        1u + katana::agent::materialization_world_max_evidence +
        katana::agent::materialization_world_max_nodes +
        katana::agent::materialization_world_max_edges +
        katana::agent::materialization_world_max_frontier;
    if (!measured.before_valid || !measured.after_valid ||
        measured.total > maximum_diff_entries)
        throw std::runtime_error(
            "Materialization-World-Diff ueberschreitet sein festes Budget.");
    std::vector<katana::agent::AgentDiffEntry> entries(measured.total);
    const auto result = katana::agent::diff_agent_worlds(
        before, after, entries);
    if (!result.before_valid || !result.after_valid || !result.complete ||
        result.truncated || result.written != result.total)
        throw std::runtime_error(
            "Materialization-World-Diff konnte nicht vollstaendig erzeugt "
            "werden.");
    std::cout << "{\"schema\":2,\"kind\":\"katana-agent-diff\""
              << ",\"change_count\":" << result.total
              << ",\"agent_result\":"
              << katana::io::quote_json(
                     delta.proof_downgrades != 0u ||
                             delta.new_frontiers != 0u ||
                             delta.new_incomplete_roots != 0u ||
                             delta.routed_to_static_frontiers != 0u
                         ? "regression"
                         : delta.resolved_frontiers != 0u ||
                                   delta.proof_upgrades != 0u ||
                                   delta.routed_to_runtime_frontiers != 0u
                               ? "improved"
                               : "no_progress")
              << ",\"resolved_frontiers\":"
              << delta.resolved_frontiers
              << ",\"new_frontiers\":" << delta.new_frontiers
              << ",\"new_runtime_hints\":"
              << delta.new_runtime_hints
              << ",\"routed_to_runtime_frontiers\":"
              << delta.routed_to_runtime_frontiers
              << ",\"routed_to_static_frontiers\":"
              << delta.routed_to_static_frontiers
              << ",\"static_actionable_before\":"
              << delta.static_actionable_before
              << ",\"static_actionable_after\":"
              << delta.static_actionable_after
              << ",\"proof_upgrades\":" << delta.proof_upgrades
              << ",\"proof_downgrades\":"
              << delta.proof_downgrades
              << ",\"new_incomplete_roots\":"
              << delta.new_incomplete_roots
              << ",\"resolved_incomplete_roots\":"
              << delta.resolved_incomplete_roots
              << ",\"analysis_wall_time_delta_ms\":null"
              << ",\"reused_shards\":null"
              << ",\"recomputed_shards\":null"
              << ",\"before_decision\":"
              << katana::io::quote_json(
                     katana::agent::agent_decision_kind_name(
                         katana::agent::evaluate_agent_decision(before).kind))
              << ",\"after_decision\":"
              << katana::io::quote_json(
                     katana::agent::agent_decision_kind_name(
                         katana::agent::evaluate_agent_decision(after).kind))
              << ",\"changes\":[";
    for (std::size_t index = 0u; index < entries.size(); ++index) {
        if (index != 0u) std::cout << ',';
        const auto& entry = entries[index];
        std::cout << "{\"entity\":"
                  << katana::io::quote_json(
                         agent_diff_entity_name(entry.entity))
                  << ",\"change\":"
                  << katana::io::quote_json(
                         agent_diff_change_name(entry.change))
                  << ",\"id\":" << entry.id.value
                  << ",\"related\":" << entry.related.value
                  << ",\"variant\":"
                  << static_cast<unsigned>(entry.variant) << '}';
    }
    std::cout << "]}\n" << std::flush;
    if (!std::cout)
        throw std::runtime_error(
            "Agenten-Diff konnte nicht vollstaendig ausgegeben werden.");
    return 0;
}

void print_usage(std::ostream& output) {
    output << "Verwendung:\n"
           << "  katana-recomp <Opcode>\n"
           << "  katana-recomp opcode <Opcode>\n"
           << "  katana-recomp isa-report [--json] "
              "[--external-evidence <katana-sh4-sst-conformance.json>]\n"
           << "  katana-recomp analyze <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp analyze-json <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp callback-contract-audit <Projektmanifest> [--json]\n"
           << "  katana-recomp cfg-json <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp cfg-dot <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp callgraph-json <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp callgraph-dot <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp disc-audit <Quelle.gdi> [--json] [--include-accesses] "
              "[--fail-on-gap|--strict]\n"
           << "  katana-recomp disc-audit-set <Verzeichnis> [--json] [--jobs N] "
              "[--fail-on-gap|--strict]\n"
           << "  katana-recomp latent-aot-module-audit <Modul.bin> "
              "--source-address <0xAdresse> --entry <0xOffset>... "
              "[--external-callback-sink <0xAdresse>:<0xMaske>]... "
              "[--runtime-base <0xAdresse>] [--sega-prs] [--json]\n"
           << "  katana-recomp firmware-diagnose <bios|flash> <Datei> [--sha256 <Hash>] "
              "[--include-sensitive]\n"
           << "  katana-recomp disasm <Datei> [Basisadresse [Dateioffset [Byteanzahl]]]\n"
           << "  katana-recomp blocks <Datei> [Basisadresse]\n"
           << "  katana-recomp functions <Datei> <Einstieg> [Basisadresse]\n"
           << "  katana-recomp ir <Raw|ELF|Manifest> <Einstieg> [Basisadresse] [--directives "
              "<Datei>]\n"
           << "  katana-recomp ir-json <Raw|ELF|Manifest> <Einstieg> [Basisadresse] [--directives "
              "<Datei>]\n"
           << "  katana-recomp emit-cpp <Raw|ELF|Manifest> <Einstieg> <Ausgabe.cpp> [Basisadresse] "
              "[--no-opt] [--dump-ir <Praefix>] [--directives <Datei>]\n\n"
           << "  katana-recomp phase6-probe-source <GDI> <Ausgabe.cpp>\n\n"
           << "  katana-recomp extract-boot-executable <eigene.gdi> --output "
              "<privater-Ordner>\n"
           << "  katana-recomp port <Quelle.gdi> --output <Ordner> --target-name <Name> "
              "[--console-profile <japan-ntsc|north-america-ntsc|europe-pal|vga>] "
              "[--game-project <Descriptor-Artefakt>] "
              "[--native-port-definition <private .katana-native-port>] "
               "[--analysis-mode <platform|runtime-only>] "
               "[--native-aot-resume-entry <0xAdresse>]... "
               "[--runtime-image-payload <Image-ID>=<private-Datei>] "
              "[--native-bootstrap-write-payload "
              "<0xGastadresse>=<private-Datei>] "
               "[--telemetry-jsonl <Datei>] "
               "[--detailed-analysis-telemetry] "
              "[--latent-aot-mode <heuristic|exact-only>] "
              "[--latent-aot-entry "
              "<sha256:<64-lowerhex>@<disc-byte-offset>:<encoded-byte-size>:"
              "<module-relative-offset>>]... "
              "[--latent-aot-entry-file <Datei>]...\n"
           << "  katana-recomp analyze-port <Quelle.gdi> --output <privater-Analyseordner> "
              "--target-name <Name> --game-project <Descriptor-Artefakt> "
              "--native-port-definition <private .katana-native-port> "
              "[--resume] [--refresh-analysis] "
              "[--import-runtime-frontier <Produktlog>] "
              "[dieselben Analyse-/Payload-/Latent-Optionen wie port]\n"
           << "  katana-recomp next-analysis-task --analysis-artifact "
              "<materialization-world.katana-world> --format agent-json\n"
           << "  katana-recomp explain --analysis-artifact "
              "<materialization-world.katana-world> --frontier <ID> "
              "--format agent-json\n"
           << "  katana-recomp diff-analysis --before <world> --after <world> "
              "--format agent-json\n"
           << "  katana-recomp probe-port <Quelle.gdi> --output <Ordner> --target-name <Name> "
              "[--console-profile <...>] [--telemetry-jsonl <Datei>]\n"
           << "  katana-recomp port-executable <boot.katana-executable> --output <Ordner> "
              "--target-name <Name> [--console-profile <...>] "
               "[--game-project <Descriptor-Artefakt>] "
               "[--native-port-definition <private .katana-native-port>] "
               "[--analysis-mode <platform|runtime-only>] "
               "[--runtime-image-payload <Image-ID>=<private-Datei>]... "
              "[--native-bootstrap-write-payload "
              "<0xGastadresse>=<private-Datei>]... "
              "[--game-entry-handoff <privates-Artefakt>] "
              "[--telemetry-jsonl <Datei>]\n"
           << "  katana-recomp probe-port-executable <boot.katana-executable> --output "
              "<Ordner> --target-name <Name> [--console-profile <...>] "
              "[--telemetry-jsonl <Datei>]\n\n"
           << "  katana-recomp workflow <validate|analyze|codegen|build|run-preflight> "
              "<Projektmanifest> --output <Ordner>\n\n"
           << "Beispiel:\n"
           << "  katana-recomp emit-cpp programm.bin 8C010000 generated.cpp 8C010000\n";
}

} // namespace

int main(const int argc, char* argv[]) {
    using katana::cli::exit_status;
    using katana::cli::ExitCode;
    try {
        {
            auto invoked_name =
                std::filesystem::path(argv[0]).filename().string();
            std::transform(
                invoked_name.begin(),
                invoked_name.end(),
                invoked_name.begin(),
                [](const unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            if (invoked_name == "katana-host-archive-wrapper" ||
                invoked_name ==
                    "katana-host-archive-wrapper.exe") {
                const auto event_root = configured_environment_value(
                    "KATANA_HOST_BUILD_EVENT_ROOT");
                const auto real_tool = configured_environment_value(
                    "KATANA_HOST_BUILD_REAL_ARCHIVER");
                std::optional<std::filesystem::path> derived_event_root;
                if (!event_root) {
                    const auto wrapper_directory =
                        current_process_executable_path().parent_path();
                    if (wrapper_directory.filename() ==
                        ".katana-host-build-tools") {
                        derived_event_root =
                            wrapper_directory.parent_path() /
                            ".katana-host-build-events";
                    }
                }
                if ((!event_root && !derived_event_root) || argc < 2)
                    return 125;
                const auto tool = real_tool
                                      ? *real_tool
                                      : std::string(argv[1]);
                const auto first_argument = real_tool ? 1 : 2;
                std::vector<const char*> arguments;
                arguments.reserve(static_cast<std::size_t>(
                    std::max(0, argc - first_argument)));
                for (int index = first_argument; index < argc; ++index)
                    arguments.push_back(argv[index]);
                return katana::cli::run_host_build_tool_launcher(
                    katana::cli::HostBuildToolKind::Archive,
                    event_root
                        ? std::filesystem::path(*event_root)
                        : *derived_event_root,
                    tool,
                    arguments);
            }
        }
#ifdef _WIN32
        {
            auto invoked_name =
                std::filesystem::path(argv[0]).filename().string();
            std::transform(
                invoked_name.begin(),
                invoked_name.end(),
                invoked_name.begin(),
                [](const unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            const auto wrapper_kind =
                invoked_name == "katana-host-cl-wrapper.exe"
                    ? std::optional(
                          katana::cli::HostBuildToolKind::Compile)
                    : invoked_name ==
                              "katana-host-link-wrapper.exe"
                          ? std::optional(
                                katana::cli::HostBuildToolKind::Link)
                          : std::nullopt;
            if (wrapper_kind) {
                const auto event_root = configured_environment_value(
                    "KATANA_HOST_BUILD_EVENT_ROOT");
                const auto real_tool = configured_environment_value(
                    *wrapper_kind ==
                            katana::cli::HostBuildToolKind::Compile
                        ? "KATANA_HOST_BUILD_REAL_COMPILER"
                        : "KATANA_HOST_BUILD_REAL_LINKER");
                if (!event_root || !real_tool || argc < 2) return 125;
                std::vector<const char*> arguments;
                arguments.reserve(static_cast<std::size_t>(argc - 1));
                for (int index = 1; index < argc; ++index)
                    arguments.push_back(argv[index]);
                return katana::cli::run_host_build_tool_launcher(
                    *wrapper_kind,
                    *event_root,
                    *real_tool,
                    arguments);
            }
        }
#endif
        if (argc >= 6 &&
            std::string_view(argv[1]) == "__host-build-tool") {
            const auto kind_text = std::string_view(argv[3]);
            const auto kind = kind_text == "compile"
                                  ? std::optional(
                                        katana::cli::HostBuildToolKind::
                                            Compile)
                                  : kind_text == "link"
                                        ? std::optional(
                                              katana::cli::
                                                  HostBuildToolKind::Link)
                                        : kind_text == "archive"
                                              ? std::optional(
                                                    katana::cli::
                                                        HostBuildToolKind::
                                                            Archive)
                                              : std::nullopt;
            const auto mode = std::string_view(argv[4]);
            if (!kind ||
                (mode != "--direct" && mode != "--chain"))
                return 125;
            std::vector<const char*> arguments;
            arguments.reserve(static_cast<std::size_t>(argc - 5));
            auto real_tool = std::string(argv[5]);
            if (mode == "--direct" &&
                *kind == katana::cli::HostBuildToolKind::Compile) {
                const auto transparent_cache =
                    configured_compiler_cache_binding();
                if (transparent_cache &&
                    transparent_cache->launcher != real_tool) {
                    if (transparent_cache->launcher.find_first_of(
                            ";\r\n\"") !=
                        std::string::npos)
                        return 125;
                    // The instrumented Katana launcher is the outer layer;
                    // binding it (or one of the copied host wrappers) as the
                    // cache would re-enter this dispatch path indefinitely.
                    // Fail closed for explicit self-references instead of
                    // silently compiling without the requested cache.
                    if (compiler_cache_would_recurse(
                            transparent_cache->launcher))
                        return 125;
#ifdef _WIN32
                    if (transparent_cache->managed_storage) {
                        if (!configured_environment_value("SCCACHE_DIR")) {
                            if (!prepare_managed_compiler_cache_storage(
                                    *transparent_cache->managed_storage))
                                return 125;
                            const auto storage =
                                transparent_cache->managed_storage->string();
                            if (_putenv_s("SCCACHE_DIR", storage.c_str()) !=
                                0)
                                return 125;
                        }
                    }
#endif
                    arguments.push_back(argv[5]);
                    real_tool = transparent_cache->launcher;
                }
            }
            for (int index = 6; index < argc; ++index)
                arguments.push_back(argv[index]);
            return katana::cli::run_host_build_tool_launcher(
                *kind,
                std::filesystem::path(argv[2]),
                real_tool,
                arguments);
        }
        // Test-only product entry which exercises the exact process-tree
        // supervisor used by configure/runtime/host-build. It deliberately
        // accepts one already-formed host command so regression tests can
        // prove descendant quiescence without exposing this in --help.
        if ((argc == 4 || argc == 5 || argc == 6) &&
            std::string_view(argv[1]) ==
                "__host-supervision-probe") {
            std::uint64_t timeout_milliseconds = 0u;
            const auto timeout_text = std::string_view(argv[2]);
            const auto conversion = std::from_chars(
                timeout_text.data(),
                timeout_text.data() + timeout_text.size(),
                timeout_milliseconds,
                10);
            if (conversion.ec != std::errc{} ||
                conversion.ptr !=
                    timeout_text.data() + timeout_text.size() ||
                timeout_milliseconds >
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()))
                throw std::invalid_argument(
                    "Host-Supervision-Probe besitzt ein ungueltiges "
                    "Zeitlimit.");
            std::optional<std::size_t> windows_cl_jobs;
            std::optional<std::filesystem::path>
                probe_telemetry_path;
            if (argc == 5) {
                std::uint64_t parsed_jobs = 0u;
                const auto jobs_text = std::string_view(argv[4]);
                const auto jobs_conversion = std::from_chars(
                    jobs_text.data(),
                    jobs_text.data() + jobs_text.size(),
                    parsed_jobs,
                    10);
                if (jobs_conversion.ec != std::errc{} ||
                    jobs_conversion.ptr !=
                        jobs_text.data() + jobs_text.size() ||
                    parsed_jobs > 256u)
                    throw std::invalid_argument(
                        "Host-Supervision-Probe besitzt ein "
                        "ungueltiges MSVC-Budget.");
                windows_cl_jobs =
                    static_cast<std::size_t>(parsed_jobs);
            } else if (argc == 6) {
                if (std::string_view(argv[4]) !=
                    "--telemetry-jsonl")
                    throw std::invalid_argument(
                        "Host-Supervision-Probe besitzt eine "
                        "ungueltige Telemetrieoption.");
                probe_telemetry_path =
                    std::filesystem::absolute(argv[5]).
                        lexically_normal();
            }

            std::unique_ptr<
                katana::cli::PortBuildTelemetryRecorder>
                probe_telemetry;
            if (probe_telemetry_path) {
                katana::cli::PortBuildTelemetryOptions options;
                options.jsonl_path = *probe_telemetry_path;
                options.job_kind = "host-supervision-probe";
                options.require_resolved_environment = false;
                options.resource_sample_interval =
                    std::chrono::milliseconds(100);
                probe_telemetry = std::make_unique<
                    katana::cli::PortBuildTelemetryRecorder>(
                    std::move(options));
                if (!probe_telemetry->enabled())
                    throw katana::cli::Error(
                        katana::cli::ExitCode::InputOutput,
                        "Host-Supervision-Probe konnte die "
                        "Telemetrie nicht initialisieren.");
            }
            auto supervised_command = std::string(argv[3]);
#ifdef _WIN32
            // Exercise the same cmd normalization as configure/runtime/build,
            // including literal ! and % path/argument handling.
            supervised_command =
                normalized_host_command(supervised_command);
#endif
            auto next_probe_resource_sample =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(100);
            const auto result = run_supervised_host_command(
                supervised_command,
                std::chrono::milliseconds(timeout_milliseconds),
                probe_telemetry.get(),
                "host-supervision-probe",
                windows_cl_jobs,
                [&] {
                    if (probe_telemetry == nullptr) return;
                    const auto now =
                        std::chrono::steady_clock::now();
                    if (now < next_probe_resource_sample) return;
                    next_probe_resource_sample =
                        now + std::chrono::milliseconds(100);
                    probe_telemetry->sample_resources(
                        "host-supervision-probe");
                });
            if (probe_telemetry != nullptr) {
                probe_telemetry->finish(
                    result.exit_code == 0
                        ? katana::cli::
                              PortBuildTerminalOutcome::Completed
                        : katana::cli::
                              PortBuildTerminalOutcome::Failed,
                    result.exit_code,
                    "host-supervision-probe");
                const auto status = probe_telemetry->status();
                if (!status.terminal_emitted ||
                    !status.telemetry_complete ||
                    status.io_failed)
                    throw katana::cli::Error(
                        katana::cli::ExitCode::InputOutput,
                        "Host-Supervision-Probe konnte keine "
                        "vollstaendige Telemetrie publizieren.");
            }
            std::cout
                << "host_exit=" << result.exit_code
                << " timed_out=" << (result.timed_out ? 1 : 0)
                << " process_tree_quiescent="
                << (result.process_tree_quiescent ? 1 : 0)
                << '\n';
            return result.exit_code;
        }
#ifdef _WIN32
        if (argc == 5 &&
            std::string_view(argv[1]) ==
                "__host-msvc-environment-probe") {
            const auto source =
                std::filesystem::absolute(argv[2]).lexically_normal();
            const auto executable =
                std::filesystem::absolute(argv[3]).lexically_normal();
            if (!std::filesystem::is_regular_file(source) ||
                !executable.has_parent_path())
                throw std::invalid_argument(
                    "MSVC-Umgebungsprobe besitzt ungueltige Pfade.");
            std::uint64_t parsed_jobs = 0u;
            const auto jobs_text = std::string_view(argv[4]);
            const auto jobs_conversion = std::from_chars(
                jobs_text.data(),
                jobs_text.data() + jobs_text.size(),
                parsed_jobs,
                10);
            if (jobs_conversion.ec != std::errc{} ||
                jobs_conversion.ptr !=
                    jobs_text.data() + jobs_text.size() ||
                parsed_jobs == 0u || parsed_jobs > 256u)
                throw std::invalid_argument(
                    "MSVC-Umgebungsprobe besitzt ein ungueltiges "
                    "Compilerbudget.");
            const auto quote = [](const std::filesystem::path& path) {
                const auto text = path.string();
                if (text.find('"') != std::string::npos)
                    throw std::invalid_argument(
                        "MSVC-Umgebungsprobepfad ist nicht sicher "
                        "quotierbar.");
                return '"' + text + '"';
            };
            auto object = executable;
            object.replace_extension(".obj");
            auto pdb = executable;
            pdb.replace_extension(".pdb");
            const auto command =
                std::string("cl.exe /nologo /std:c++20 /EHsc ") +
                quote(source) + " /Fo:" + quote(object) +
                " /Fe:" + quote(executable) +
                " /link /PDB:" + quote(pdb);
            const auto result = run_supervised_host_command(
                prepare_windows_host_command(command, true),
                std::chrono::minutes(2),
                nullptr,
                {},
                static_cast<std::size_t>(parsed_jobs));
            if (!result.process_tree_quiescent)
                throw std::runtime_error(
                    "MSVC-Umgebungsprobe endete ohne leeren "
                    "Prozessbaum.");
            if (result.exit_code != 0) return result.exit_code;
            if (!std::filesystem::is_regular_file(executable))
                throw std::runtime_error(
                    "MSVC-Umgebungsprobe erzeugte kein Linkartefakt.");
            return 0;
        }
#endif
        if (argc == 2 &&
            (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
            print_usage(std::cout);
            return exit_status(ExitCode::Success);
        }
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cout << "KatanaRecomp " << KATANA_RECOMP_VERSION << '\n';
            return exit_status(ExitCode::Success);
        }
        if (argc >= 7 &&
            std::string_view(argv[1]) ==
                "latent-aot-module-audit") {
            std::optional<std::uint32_t> source_address;
            std::optional<std::uint32_t> runtime_base;
            std::vector<std::uint32_t> entry_offsets;
            std::vector<katana::codegen::LatentAotExternalCallbackSink>
                external_callback_sinks;
            bool sega_prs = false;
            bool json = false;
            for (int index = 3; index < argc; ++index) {
                const auto option = std::string_view(argv[index]);
                if (option == "--json") {
                    if (json)
                        throw std::invalid_argument(
                            "latent-aot-module-audit erhielt --json doppelt.");
                    json = true;
                } else if (option == "--sega-prs") {
                    if (sega_prs)
                        throw std::invalid_argument(
                            "latent-aot-module-audit erhielt --sega-prs doppelt.");
                    sega_prs = true;
                } else if (option == "--source-address") {
                    if (source_address.has_value() || ++index >= argc)
                        throw std::invalid_argument(
                            "latent-aot-module-audit erwartet genau eine "
                            "--source-address.");
                    source_address = parse_hex_value(
                        argv[index],
                        std::numeric_limits<std::uint32_t>::max(),
                        "Die Modulaudit-Quelladresse");
                } else if (option == "--runtime-base") {
                    if (runtime_base.has_value() || ++index >= argc)
                        throw std::invalid_argument(
                            "latent-aot-module-audit erwartet hoechstens eine "
                            "--runtime-base.");
                    runtime_base = parse_hex_value(
                        argv[index],
                        std::numeric_limits<std::uint32_t>::max(),
                        "Die Modulaudit-Runtime-Basis");
                } else if (option == "--entry") {
                    if (++index >= argc)
                        throw std::invalid_argument(
                            "latent-aot-module-audit erwartet nach --entry "
                            "einen Offset.");
                    entry_offsets.push_back(parse_hex_value(
                        argv[index],
                        std::numeric_limits<std::uint32_t>::max(),
                        "Der Modulaudit-Entry"));
                } else if (option == "--external-callback-sink") {
                    if (++index >= argc)
                        throw std::invalid_argument(
                            "latent-aot-module-audit erwartet nach "
                            "--external-callback-sink Adresse:Maske.");
                    const std::string value(argv[index]);
                    const auto separator = value.find(':');
                    if (separator == std::string::npos || separator == 0u ||
                        separator + 1u == value.size())
                        throw std::invalid_argument(
                            "--external-callback-sink erwartet "
                            "Adresse:Maske.");
                    const auto function_address = parse_hex_value(
                        value.substr(0u, separator),
                        std::numeric_limits<std::uint32_t>::max(),
                        "Die Callback-Sink-Adresse");
                    const auto argument_mask = parse_hex_value(
                        value.substr(separator + 1u), 0x0fu,
                        "Die Callback-Sink-Argumentmaske");
                    if (argument_mask == 0u)
                        throw std::invalid_argument(
                            "Die Callback-Sink-Argumentmaske darf nicht null "
                            "sein.");
                    external_callback_sinks.push_back({
                        function_address,
                        static_cast<std::uint8_t>(argument_mask)});
                } else {
                    throw std::invalid_argument(
                        "Unbekannte latent-aot-module-audit-Option: " +
                        std::string(option));
                }
            }
            if (!source_address.has_value())
                throw std::invalid_argument(
                    "latent-aot-module-audit benoetigt --source-address.");
            return audit_latent_aot_module_cli(
                std::filesystem::path(argv[2]),
                *source_address,
                std::move(entry_offsets),
                std::move(external_callback_sinks),
                runtime_base,
                sega_prs,
                json);
        }
        if ((argc == 3 || argc == 4) &&
            std::string_view(argv[1]) == "callback-contract-audit") {
            if (argc == 4 && std::string_view(argv[3]) != "--json")
                throw std::invalid_argument(
                    "callback-contract-audit kennt nur --json.");
            return audit_callback_contracts_manifest(
                std::filesystem::path(argv[2]), argc == 4);
        }
        if (argc >= 2 && std::string_view(argv[1]) == "isa-report") {
            bool json = false;
            std::optional<std::filesystem::path> external_evidence_path;
            for (int index = 2; index < argc; ++index) {
                const auto option = std::string_view(argv[index]);
                if (option == "--json") {
                    if (json) throw std::invalid_argument("--json wurde doppelt angegeben.");
                    json = true;
                } else if (option == "--external-evidence") {
                    if (external_evidence_path || ++index >= argc)
                        throw std::invalid_argument(
                            "--external-evidence erwartet genau einen Bericht.");
                    external_evidence_path = std::filesystem::path(argv[index]);
                } else {
                    throw std::invalid_argument("Unbekannte isa-report-Option: " +
                                                std::string(option));
                }
            }
            if (external_evidence_path && !json)
                throw std::invalid_argument("--external-evidence erfordert --json.");

            const auto report = katana::sh4::build_isa_coverage_report();
            if (!json) {
                std::cout << katana::sh4::format_isa_coverage_report(report);
                return 0;
            }

            std::optional<katana::sh4::ExternalIsaEvidence> external_evidence;
            if (external_evidence_path)
                external_evidence = load_external_isa_evidence(*external_evidence_path);
            std::cout << katana::sh4::format_alpha_isa_json(report, external_evidence) << '\n';
            return 0;
        }
        if (argc >= 3 && argc <= 6 && std::string_view(argv[1]) == "disc-audit") {
            bool json = false;
            bool include_accesses = false;
            bool fail_on_gap = false;
            bool strict = false;
            for (int index = 3; index < argc; ++index) {
                const auto option = std::string_view(argv[index]);
                if (option == "--json")
                    json = true;
                else if (option == "--include-accesses")
                    include_accesses = true;
                else if (option == "--fail-on-gap")
                    fail_on_gap = true;
                else if (option == "--strict")
                    strict = true;
                else
                    throw std::invalid_argument("Unbekannte disc-audit-Option: " +
                                                std::string(option));
            }
            if (fail_on_gap && strict)
                throw std::invalid_argument("disc-audit akzeptiert nur einen Fehlermodus.");
            if (include_accesses && !json)
                throw std::invalid_argument("--include-accesses erfordert --json.");
            return audit_disc_hardware(
                std::filesystem::path(argv[2]), json, include_accesses, fail_on_gap, strict);
        }
        if (argc >= 3 && std::string_view(argv[1]) == "disc-audit-set") {
            bool json = false;
            bool fail_on_gap = false;
            bool strict = false;
            std::size_t jobs = std::max(1u, std::thread::hardware_concurrency());
            for (int index = 3; index < argc; ++index) {
                const auto option = std::string_view(argv[index]);
                if (option == "--json")
                    json = true;
                else if (option == "--fail-on-gap")
                    fail_on_gap = true;
                else if (option == "--strict")
                    strict = true;
                else if (option == "--jobs") {
                    if (++index >= argc)
                        throw std::invalid_argument("--jobs braucht eine positive Anzahl.");
                    const auto parsed = std::stoull(argv[index]);
                    if (parsed == 0u || parsed > 256u)
                        throw std::invalid_argument("--jobs muss zwischen 1 und 256 liegen.");
                    jobs = static_cast<std::size_t>(parsed);
                } else {
                    throw std::invalid_argument("Unbekannte disc-audit-set-Option: " +
                                                std::string(option));
                }
            }
            if (fail_on_gap && strict)
                throw std::invalid_argument("disc-audit-set akzeptiert nur einen Fehlermodus.");
            return audit_disc_hardware_set(
                std::filesystem::path(argv[2]), json, jobs, fail_on_gap, strict);
        }

        if (argc == 6 && std::string_view(argv[1]) == "workflow" &&
            std::string_view(argv[4]) == "--output") {
            const std::string_view kind_name = argv[2];
            katana::app::JobKind kind;
            if (kind_name == "validate")
                kind = katana::app::JobKind::Validate;
            else if (kind_name == "analyze")
                kind = katana::app::JobKind::Analyze;
            else if (kind_name == "codegen")
                kind = katana::app::JobKind::Codegen;
            else if (kind_name == "build")
                kind = katana::app::JobKind::Build;
            else if (kind_name == "run-preflight")
                kind = katana::app::JobKind::RunPreflight;
            else
                throw std::invalid_argument("workflow erhielt einen unbekannten Jobtyp.");
            katana::app::ApplicationService service;
            const auto result =
                service.execute({"cli-workflow", kind, argv[3], argv[5], KATANA_RECOMP_VERSION},
                                {},
                                [](const katana::app::JobEvent& event) {
                                    std::cerr << katana::app::format_job_event_json(event);
                                    std::cerr.flush();
                                });
            std::cout << katana::app::format_job_result_json(result);
            if (result.state == katana::app::JobState::Completed)
                return exit_status(ExitCode::Success);
            switch (result.failure_category) {
            case katana::app::JobFailureCategory::InputOutput:
                return exit_status(ExitCode::InputOutput);
            case katana::app::JobFailureCategory::CodeGeneration:
                return exit_status(ExitCode::CodeGenerationFailure);
            case katana::app::JobFailureCategory::Build:
                return exit_status(ExitCode::BuildFailure);
            case katana::app::JobFailureCategory::Internal:
                return exit_status(ExitCode::InternalError);
            case katana::app::JobFailureCategory::None:
            case katana::app::JobFailureCategory::Processing:
                return exit_status(ExitCode::ProcessingFailure);
            }
            return exit_status(ExitCode::InternalError);
        }

        if (argc == 5 &&
            std::string_view(argv[1]) == "extract-boot-executable") {
            if (std::string_view(argv[3]) != "--output")
                throw std::invalid_argument(
                    "extract-boot-executable erwartet --output genau einmal.");
            return extract_boot_executable_artifact(
                std::filesystem::path(argv[2]),
                std::filesystem::path(argv[4]));
        }

        if (argc >= 2 &&
            std::string_view(argv[1]) == "next-analysis-task") {
            std::optional<std::filesystem::path> artifact;
            std::optional<std::string_view> format;
            if (((argc - 2) & 1) != 0)
                throw std::invalid_argument(
                    "next-analysis-task erwartet Optionspaare.");
            for (int index = 2; index < argc; index += 2) {
                const auto option = std::string_view(argv[index]);
                if (option == "--analysis-artifact" && !artifact)
                    artifact = std::filesystem::path(argv[index + 1]);
                else if (option == "--format" && !format)
                    format = std::string_view(argv[index + 1]);
                else
                    throw std::invalid_argument(
                        "next-analysis-task erhielt eine unbekannte oder "
                        "doppelte Option.");
            }
            if (!artifact || format != std::optional<std::string_view>{
                                           "agent-json"})
                throw std::invalid_argument(
                    "next-analysis-task braucht --analysis-artifact und "
                    "--format agent-json.");
            return next_analysis_task_cli(*artifact);
        }

        if (argc >= 2 && std::string_view(argv[1]) == "explain") {
            std::optional<std::filesystem::path> artifact;
            std::optional<std::uint64_t> frontier;
            std::optional<std::string_view> format;
            if (((argc - 2) & 1) != 0)
                throw std::invalid_argument(
                    "explain erwartet Optionspaare.");
            for (int index = 2; index < argc; index += 2) {
                const auto option = std::string_view(argv[index]);
                if (option == "--analysis-artifact" && !artifact)
                    artifact = std::filesystem::path(argv[index + 1]);
                else if (option == "--frontier" && !frontier)
                    frontier = parse_agent_stable_id(argv[index + 1]);
                else if (option == "--format" && !format)
                    format = std::string_view(argv[index + 1]);
                else
                    throw std::invalid_argument(
                        "explain erhielt eine unbekannte oder doppelte "
                        "Option.");
            }
            if (!artifact || !frontier ||
                format != std::optional<std::string_view>{"agent-json"})
                throw std::invalid_argument(
                    "explain braucht --analysis-artifact, --frontier und "
                    "--format agent-json.");
            return explain_analysis_frontier_cli(*artifact, *frontier);
        }

        if (argc >= 2 &&
            std::string_view(argv[1]) == "diff-analysis") {
            std::optional<std::filesystem::path> before;
            std::optional<std::filesystem::path> after;
            std::optional<std::string_view> format;
            if (((argc - 2) & 1) != 0)
                throw std::invalid_argument(
                    "diff-analysis erwartet Optionspaare.");
            for (int index = 2; index < argc; index += 2) {
                const auto option = std::string_view(argv[index]);
                if (option == "--before" && !before)
                    before = std::filesystem::path(argv[index + 1]);
                else if (option == "--after" && !after)
                    after = std::filesystem::path(argv[index + 1]);
                else if (option == "--format" && !format)
                    format = std::string_view(argv[index + 1]);
                else
                    throw std::invalid_argument(
                        "diff-analysis erhielt eine unbekannte oder "
                        "doppelte Option.");
            }
            if (!before || !after ||
                format != std::optional<std::string_view>{"agent-json"})
                throw std::invalid_argument(
                    "diff-analysis braucht --before, --after und --format "
                    "agent-json.");
            return diff_analysis_cli(*before, *after);
        }

        const auto port_command =
            argc >= 2 ? std::string_view(argv[1]) : std::string_view{};
        if (argc >= 7 &&
            (port_command == "port" || port_command == "analyze-port" ||
             port_command == "probe-port" ||
             port_command == "port-executable" ||
             port_command == "probe-port-executable")) {
            const bool analysis_only =
                port_command == "analyze-port";
            const bool diagnostic_partial =
                port_command == "probe-port" ||
                port_command == "probe-port-executable";
            const bool boot_executable_artifact =
                port_command == "port-executable" ||
                port_command == "probe-port-executable";
            std::optional<std::filesystem::path> output_path;
            std::optional<std::string> target_name;
            std::optional<std::filesystem::path>
                game_entry_handoff_path;
            std::optional<std::filesystem::path> game_project_path;
            std::optional<std::filesystem::path>
                native_port_definition_path;
            std::optional<std::filesystem::path>
                telemetry_jsonl_path;
            std::vector<RuntimeImagePayloadArgument>
                runtime_image_payload_arguments;
            std::vector<NativeBootstrapWritePayloadArgument>
                bootstrap_write_payload_arguments;
            std::vector<LatentAotEntryHintArgument>
                latent_aot_entry_hints;
            std::vector<std::uint32_t> native_aot_resume_entries;
            auto latent_aot_discovery_mode =
                LatentAotDiscoveryModeArgument::HintsAndHeuristics;
            bool latent_aot_discovery_mode_seen = false;
            auto analysis_mode = PortAnalysisMode::PlatformAbi;
            bool analysis_mode_seen = false;
            std::string console_profile = "japan-ntsc";
            bool console_profile_seen = false;
            bool resume_analysis = false;
            bool refresh_analysis = false;
            bool detailed_analysis_telemetry = false;
            std::optional<std::filesystem::path>
                runtime_frontier_import_path;
            std::vector<std::string_view> paired_arguments;
            paired_arguments.reserve(static_cast<std::size_t>(argc) - 3u);
            for (std::size_t argument = 3u;
                 argument < static_cast<std::size_t>(argc);) {
                const std::string_view option = argv[argument++];
                if (option == "--resume") {
                    if (!analysis_only || resume_analysis)
                        throw std::invalid_argument(
                            "--resume ist nur einmal fuer analyze-port erlaubt.");
                    resume_analysis = true;
                    continue;
                }
                if (option == "--refresh-analysis") {
                    if (!analysis_only || refresh_analysis)
                        throw std::invalid_argument(
                            "--refresh-analysis ist nur einmal fuer "
                            "analyze-port erlaubt.");
                    refresh_analysis = true;
                    continue;
                }
                if (option == "--detailed-analysis-telemetry") {
                    if (detailed_analysis_telemetry)
                        throw std::invalid_argument(
                            "--detailed-analysis-telemetry ist nur einmal "
                            "erlaubt.");
                    detailed_analysis_telemetry = true;
                    continue;
                }
                if (argument >= static_cast<std::size_t>(argc))
                    throw std::invalid_argument(
                        "port erhielt eine Option ohne Wert.");
                paired_arguments.push_back(option);
                paired_arguments.push_back(argv[argument++]);
            }
            for (std::size_t argument = 0u;
                 argument < paired_arguments.size();
                 argument += 2u) {
                const auto option = paired_arguments[argument];
                const auto value = paired_arguments[argument + 1u];
                if (option == "--output" && !output_path.has_value()) {
                    output_path = std::filesystem::path(value);
                } else if (option == "--target-name" && !target_name.has_value()) {
                    target_name = value;
                } else if (option == "--console-profile" && !console_profile_seen) {
                    console_profile = value;
                    console_profile_seen = true;
                } else if (option == "--telemetry-jsonl" &&
                           !telemetry_jsonl_path.has_value()) {
                    telemetry_jsonl_path =
                        std::filesystem::path(value);
                } else if (option == "--game-entry-handoff" &&
                           port_command == "port-executable" &&
                           !game_entry_handoff_path.has_value()) {
                    game_entry_handoff_path =
                        std::filesystem::path(value);
                } else if (option == "--game-project" &&
                           (port_command == "port" ||
                            port_command == "analyze-port" ||
                            port_command == "port-executable") &&
                           !game_project_path.has_value()) {
                    game_project_path =
                        std::filesystem::path(value);
                } else if (
                    option == "--native-port-definition" &&
                    (port_command == "port" ||
                     port_command == "analyze-port" ||
                     port_command == "port-executable") &&
                    !native_port_definition_path.has_value()) {
                    native_port_definition_path =
                        std::filesystem::path(value);
                } else if (
                    option == "--import-runtime-frontier" &&
                    analysis_only &&
                    !runtime_frontier_import_path.has_value()) {
                    runtime_frontier_import_path =
                        std::filesystem::path(value);
                } else if (
                    option == "--runtime-image-payload" &&
                    (port_command == "port" ||
                     port_command == "analyze-port" ||
                     port_command == "port-executable")) {
                    const std::string_view binding = value;
                    const auto separator = binding.find('=');
                    if (separator == 0u ||
                        separator == std::string_view::npos ||
                        separator + 1u >= binding.size())
                        throw std::invalid_argument(
                            "--runtime-image-payload erwartet "
                            "<Image-ID>=<private-Datei>.");
                    runtime_image_payload_arguments.emplace_back(
                        std::string(binding.substr(0u, separator)),
                        std::filesystem::path(
                            std::string(binding.substr(separator + 1u))));
                } else if (
                    option == "--native-bootstrap-write-payload" &&
                    (port_command == "port" ||
                     port_command == "analyze-port" ||
                     port_command == "port-executable")) {
                    const std::string_view binding = value;
                    const auto separator = binding.find('=');
                    if (separator == 0u ||
                        separator == std::string_view::npos ||
                        separator + 1u >= binding.size())
                        throw std::invalid_argument(
                            "--native-bootstrap-write-payload erwartet "
                            "<0xGastadresse>=<private-Datei>.");
                    bootstrap_write_payload_arguments.emplace_back(
                        parse_native_bootstrap_write_address(
                            binding.substr(0u, separator)),
                        std::filesystem::path(std::string(
                            binding.substr(separator + 1u))));
                } else if (option == "--latent-aot-entry" &&
                           (port_command == "port" ||
                            port_command == "analyze-port")) {
                    latent_aot_entry_hints.push_back(
                        parse_latent_aot_entry_hint(value));
                } else if (option == "--latent-aot-entry-file" &&
                           (port_command == "port" ||
                            port_command == "analyze-port")) {
                    auto file_hints = load_latent_aot_entry_hint_file(
                        std::filesystem::path(value));
                    if (file_hints.size() >
                        maximum_latent_aot_entry_hint_arguments -
                            std::min(latent_aot_entry_hints.size(),
                                     maximum_latent_aot_entry_hint_arguments))
                        throw std::invalid_argument(
                            "--latent-aot-entry-file ueberschreitet zusammen mit "
                            "direkten Hints das Hintbudget.");
                    latent_aot_entry_hints.insert(
                        latent_aot_entry_hints.end(),
                        std::make_move_iterator(file_hints.begin()),
                        std::make_move_iterator(file_hints.end()));
                } else if (option == "--latent-aot-mode" &&
                           (port_command == "port" ||
                            port_command == "analyze-port") &&
                           !latent_aot_discovery_mode_seen) {
                    latent_aot_discovery_mode =
                        parse_latent_aot_discovery_mode(value);
                    latent_aot_discovery_mode_seen = true;
                } else if (option == "--native-aot-resume-entry" &&
                           (port_command == "port" ||
                            port_command == "analyze-port")) {
                    if (native_aot_resume_entries.size() >=
                        maximum_native_aot_resume_entry_arguments)
                        throw std::invalid_argument(
                            "--native-aot-resume-entry ueberschreitet das Argumentbudget.");
                    native_aot_resume_entries.push_back(
                        parse_native_aot_resume_entry(value));
                } else if (option == "--analysis-mode" &&
                           (port_command == "port" ||
                            port_command == "analyze-port" ||
                            port_command == "port-executable") &&
                           !analysis_mode_seen) {
                    analysis_mode = parse_port_analysis_mode(
                        value);
                    analysis_mode_seen = true;
                } else {
                    throw std::invalid_argument(
                        "port erwartet eindeutige Ausgabe-, Ziel- und Konsolenprofiloptionen.");
                }
            }
            latent_aot_entry_hints =
                normalize_latent_aot_entry_hints(std::move(latent_aot_entry_hints));
            std::sort(native_aot_resume_entries.begin(),
                      native_aot_resume_entries.end());
            if (std::adjacent_find(native_aot_resume_entries.begin(),
                                   native_aot_resume_entries.end()) !=
                native_aot_resume_entries.end())
                throw std::invalid_argument(
                    "--native-aot-resume-entry darf nicht doppelt angegeben werden.");
            if (!latent_aot_discovery_mode_seen &&
                !latent_aot_entry_hints.empty())
                latent_aot_discovery_mode =
                    LatentAotDiscoveryModeArgument::ExactOnly;
            if (!output_path.has_value() || !target_name.has_value()) {
                throw std::invalid_argument(
                    "port erwartet --output und --target-name jeweils genau einmal.");
            }
            if (analysis_mode == PortAnalysisMode::ConservativeRuntimeOnly &&
                !game_project_path.has_value())
                throw std::invalid_argument(
                    "--analysis-mode runtime-only braucht --game-project.");
            return export_port_project(std::filesystem::path(argv[2]),
                                       *output_path,
                                       *target_name,
                                       diagnostic_partial,
                                       console_profile,
                                       boot_executable_artifact,
                                       game_project_path,
                                       native_port_definition_path,
                                       game_entry_handoff_path,
                                       runtime_image_payload_arguments,
                                       bootstrap_write_payload_arguments,
                                       latent_aot_entry_hints,
                                       latent_aot_discovery_mode,
                                        native_aot_resume_entries,
                                        telemetry_jsonl_path,
                                        detailed_analysis_telemetry,
                                        analysis_mode,
                                       analysis_only,
                                       resume_analysis,
                                       refresh_analysis,
                                       runtime_frontier_import_path);
        }

        if ((argc == 3 || argc == 4) &&
            (std::string(argv[1]) == "analyze" || std::string(argv[1]) == "analyze-json")) {
            return analyze_manifest(
                std::filesystem::path(argv[2]),
                argc == 4 ? std::optional<std::filesystem::path>{std::filesystem::path(argv[3])}
                          : std::nullopt,
                std::string(argv[1]) == "analyze-json");
        }

        if (argc == 3 || argc == 4) {
            const std::string_view command = argv[1];
            if (command == "cfg-json" || command == "cfg-dot" || command == "callgraph-json" ||
                command == "callgraph-dot") {
                return export_analysis_graph(
                    std::filesystem::path(argv[2]),
                    argc == 4 ? std::optional<std::filesystem::path>{std::filesystem::path(argv[3])}
                              : std::nullopt,
                    command);
            }
        }

        if (argc >= 4 && argc <= 7 && std::string_view(argv[1]) == "firmware-diagnose") {
            const std::string_view kind_name = argv[2];
            katana::platform::FirmwareImageKind kind;
            if (kind_name == "bios") {
                kind = katana::platform::FirmwareImageKind::Bios;
            } else if (kind_name == "flash") {
                kind = katana::platform::FirmwareImageKind::Flash;
            } else {
                throw std::invalid_argument("firmware-diagnose erwartet bios oder flash.");
            }
            katana::platform::FirmwareDiagnosticOptions options;
            std::size_t argument = 4u;
            while (argument < static_cast<std::size_t>(argc)) {
                const std::string_view option = argv[argument++];
                if (option == "--sha256" && !options.expected_sha256.has_value() &&
                    argument < static_cast<std::size_t>(argc)) {
                    options.expected_sha256 = argv[argument++];
                } else if (option == "--include-sensitive" && !options.include_sensitive) {
                    options.include_sensitive = true;
                } else {
                    throw std::invalid_argument("Ungueltige firmware-diagnose-Option.");
                }
            }
            return diagnose_firmware(std::filesystem::path(argv[3]), kind, options);
        }

        if (argc == 2) {
            return decode_single_opcode(argv[1]);
        }

        if (argc == 3 && std::string(argv[1]) == "opcode") {
            return decode_single_opcode(argv[2]);
        }

        if (argc >= 3 && argc <= 6 && std::string(argv[1]) == "disasm") {
            const auto base_address =
                argc >= 4 ? parse_hex_value(argv[3],
                                            std::numeric_limits<std::uint32_t>::max(),
                                            "Die Basisadresse")
                          : 0u;
            const auto file_offset =
                argc >= 5 ? parse_hex_value(argv[4],
                                            std::numeric_limits<std::uint32_t>::max(),
                                            "Der Dateioffset")
                          : 0u;
            const auto byte_count =
                argc == 6
                    ? std::optional<std::uint32_t>{parse_hex_value(
                          argv[5], std::numeric_limits<std::uint32_t>::max(), "Die Byteanzahl")}
                    : std::nullopt;

            return disassemble_file(
                std::filesystem::path(argv[2]), base_address, file_offset, byte_count);
        }

        if ((argc == 3 || argc == 4) && std::string(argv[1]) == "blocks") {
            const auto base_address =
                argc == 4 ? parse_hex_value(argv[3],
                                            std::numeric_limits<std::uint32_t>::max(),
                                            "Die Basisadresse")
                          : 0u;

            return analyze_blocks(std::filesystem::path(argv[2]), base_address);
        }

        if ((argc == 4 || argc == 5) && std::string(argv[1]) == "functions") {
            const auto entry_address = parse_hex_value(
                argv[3], std::numeric_limits<std::uint32_t>::max(), "Die Einstiegsadresse");

            const auto base_address =
                argc == 5 ? parse_hex_value(argv[4],
                                            std::numeric_limits<std::uint32_t>::max(),
                                            "Die Basisadresse")
                          : 0u;

            return analyze_functions(std::filesystem::path(argv[2]), entry_address, base_address);
        }

        if ((argc >= 4 && argc <= 7) &&
            (std::string(argv[1]) == "ir" || std::string(argv[1]) == "ir-json")) {
            const auto entry_address = parse_hex_value(
                argv[3], std::numeric_limits<std::uint32_t>::max(), "Die Einstiegsadresse");

            std::size_t argument = 4u;
            std::uint32_t base_address = 0u;
            if (argument < static_cast<std::size_t>(argc) &&
                !std::string_view(argv[argument]).starts_with("--")) {
                base_address = parse_hex_value(argv[argument++],
                                               std::numeric_limits<std::uint32_t>::max(),
                                               "Die Basisadresse");
            }
            std::optional<std::filesystem::path> override_path;
            while (argument < static_cast<std::size_t>(argc)) {
                const std::string_view option = argv[argument++];
                if ((option != "--overrides" && option != "--directives") || override_path ||
                    argument >= static_cast<std::size_t>(argc)) {
                    throw std::invalid_argument(
                        "Ungueltige IR-Option; erwartet wird --directives <Datei>.");
                }
                override_path = std::filesystem::path(argv[argument++]);
            }

            return analyze_ir(std::filesystem::path(argv[2]),
                              entry_address,
                              base_address,
                              std::string(argv[1]) == "ir-json",
                              override_path);
        }

        if (argc == 4 && std::string(argv[1]) == "phase6-probe-source") {
            return emit_phase6_probe_source(std::filesystem::path(argv[2]),
                                            std::filesystem::path(argv[3]));
        }

        if ((argc >= 5 && argc <= 11) && std::string(argv[1]) == "emit-cpp") {
            const auto entry_address = parse_hex_value(
                argv[3], std::numeric_limits<std::uint32_t>::max(), "Die Einstiegsadresse");

            std::size_t argument = 5u;
            std::uint32_t base_address = 0u;
            if (argument < static_cast<std::size_t>(argc) &&
                !std::string_view(argv[argument]).starts_with("--")) {
                base_address = parse_hex_value(argv[argument++],
                                               std::numeric_limits<std::uint32_t>::max(),
                                               "Die Basisadresse");
            }

            katana::ir::OptimizationOptions optimization_options;
            std::optional<std::filesystem::path> dump_prefix;
            std::optional<std::filesystem::path> override_path;
            while (argument < static_cast<std::size_t>(argc)) {
                const std::string_view option = argv[argument++];
                if (option == "--no-opt") {
                    optimization_options.enabled = false;
                } else if (option == "--dump-ir") {
                    if (dump_prefix || argument >= static_cast<std::size_t>(argc)) {
                        throw std::invalid_argument("--dump-ir erwartet genau ein Praefix.");
                    }
                    dump_prefix = std::filesystem::path(argv[argument++]);
                } else if (option == "--overrides" || option == "--directives") {
                    if (override_path || argument >= static_cast<std::size_t>(argc)) {
                        throw std::invalid_argument("--directives erwartet genau eine Datei.");
                    }
                    override_path = std::filesystem::path(argv[argument++]);
                } else {
                    throw std::invalid_argument("Unbekannte emit-cpp-Option: " +
                                                std::string(option));
                }
            }

            return emit_cpp(std::filesystem::path(argv[2]),
                            entry_address,
                            std::filesystem::path(argv[4]),
                            base_address,
                            optimization_options,
                            dump_prefix,
                            override_path);
        }

        print_usage(std::cerr);
        return exit_status(ExitCode::Usage);
    } catch (const katana::cli::Error& error) {
        std::cerr << "Fehler [" << katana::cli::exit_code_name(error.code())
                  << "]: " << error.what() << '\n';
        return exit_status(error.code());
    } catch (const std::invalid_argument& error) {
        std::cerr << "Fehler [invalid-input]: " << error.what() << '\n';
        return exit_status(ExitCode::InvalidInput);
    } catch (const std::filesystem::filesystem_error& error) {
        std::cerr << "Fehler [input-output]: " << error.what() << '\n';
        return exit_status(ExitCode::InputOutput);
    } catch (const katana::io::InputOutputError& error) {
        std::cerr << "Fehler [input-output]: " << error.what() << '\n';
        return exit_status(ExitCode::InputOutput);
    } catch (const std::runtime_error& error) {
        std::cerr << "Fehler [processing-failure]: " << error.what() << '\n';
        return exit_status(ExitCode::ProcessingFailure);
    } catch (const std::exception& error) {
        std::cerr << "Fehler [internal-error]: " << error.what() << '\n';
        return exit_status(ExitCode::InternalError);
    }
}
