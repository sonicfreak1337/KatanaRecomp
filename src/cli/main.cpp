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
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"
#include "katana/sh4/isa_coverage.hpp"

#include "port_export_orchestration.hpp"
#include "port_build_telemetry.hpp"
#include "host_build_progress.hpp"

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
            phase) != timing_boundaries.end();
    if (!recorded_module_timing && timing_boundary)
        phase_timings.transition(
            std::string("export:") + std::string(phase));
    const bool terminal_dynamic =
        phase.find("budget-exhausted") !=
            std::string_view::npos ||
        phase.find("cycle-exhausted") !=
            std::string_view::npos ||
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
        std::cout << "{\"schema\":\"katana.hardware-audit-set.v1\",\"status\":"
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

int disassemble_file(const std::filesystem::path& path, const std::uint32_t base_address) {
    katana::io::RawBinaryLoadOptions options;
    options.base_address = base_address;
    const auto image = katana::io::load_raw_binary(path, options);
    const auto lines = katana::sh4::disassemble(image);

    std::size_t unknown_count = 0;
    std::size_t control_flow_count = 0;
    std::size_t delay_slot_count = 0;

    std::cout << "Datei:         " << path.string() << '\n'
              << "Dateigroesse:  " << std::dec << image.segments()[0].bytes.size() << " Bytes\n"
              << "Basisadresse:  0x" << std::hex << std::uppercase << std::setw(8)
              << std::setfill('0') << base_address << "\n\n";

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

        std::cout << "  " << std::setw(4) << line.opcode << "  " << format_disassembly_text(line)
                  << '\n';
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
        observation_.process_tree_scope = "job-object-tree";
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
    std::chrono::minutes(15);

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
        std::uint64_t milliseconds = 0u;
        const auto conversion = std::from_chars(
            value->data(),
            value->data() + value->size(),
            milliseconds,
            10);
        if (conversion.ec != std::errc{} ||
            conversion.ptr != value->data() + value->size())
            throw std::invalid_argument(
                "KATANA_PORT_HOST_COMMAND_TIMEOUT_MS ist ungueltig.");
        const auto maximum_wait_milliseconds =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    maximum_port_host_command_runtime)
                    .count());
        if (milliseconds == 0u ||
            milliseconds > maximum_wait_milliseconds)
            throw std::invalid_argument(
                "KATANA_PORT_HOST_COMMAND_TIMEOUT_MS muss zwischen 1 und "
                "900000 liegen.");
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
    if (!timeout || timeout->count() <= 0 ||
        *timeout > maximum_port_host_command_runtime)
        throw std::invalid_argument(
            "Port-Hostprozess braucht ein Zeitlimit von hoechstens "
            "15 Minuten.");
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
        std::chrono::steady_clock::now() + *timeout;
    bool root_terminal = false;
    DWORD root_exit_code = 1u;
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

        if (std::chrono::steady_clock::now() >= deadline) {
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
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES ||
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u;
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
                "Port-Exportpfad-Sperre konnte nicht geoeffnet werden.");
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

std::string read_safe_small_port_file(
    const std::filesystem::path& path,
    const std::size_t maximum_size,
    const std::string_view description) {
    if (!safe_regular_port_file_exists(path, description))
        throw std::runtime_error(
            std::string(description) + " fehlt.");
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (size_error || size > maximum_size)
        throw std::runtime_error(
            std::string(description) +
            " besitzt keine sichere begrenzte Groesse.");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error(
            std::string(description) +
            " konnte nicht sicher geoeffnet werden.");
    std::string document{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (input.bad() || document.size() != size)
        throw std::runtime_error(
            std::string(description) +
            " konnte nicht vollstaendig gelesen werden.");
    return document;
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

void remove_failed_port_host_build_state(const std::filesystem::path& port_root,
                                         const std::filesystem::path& build_root) {
    const auto normalized_port = std::filesystem::absolute(port_root).lexically_normal();
    const auto normalized_build = std::filesystem::absolute(build_root).lexically_normal();
    const auto build_name = normalized_build.filename().generic_string();
    if (normalized_build.parent_path() != normalized_port || build_name.size() <= 6u ||
        !build_name.starts_with("build-"))
        throw std::runtime_error(
            "Fehlgeschlagener Hostbuild besitzt keinen sicher abgeleiteten Buildpfad.");
    if (!safe_regular_port_directory_exists(normalized_build,
                                            "Fehlgeschlagener CMake-Configure-Zustand"))
        return;
    std::error_code canonical_error;
    const auto resolved_port = std::filesystem::canonical(normalized_port, canonical_error);
    if (canonical_error)
        throw std::runtime_error(
            "Portwurzel fuer Configure-Bereinigung konnte nicht aufgeloest werden.");
    const auto resolved_build = std::filesystem::canonical(normalized_build, canonical_error);
    if (canonical_error || resolved_build.parent_path() != resolved_port)
        throw std::runtime_error(
            "Configure-Bereinigung wuerde den sicheren Portbuildpfad verlassen.");
    remove_safe_port_tree(
        normalized_build,
        "Fehlgeschlagener CMake-Configure-Zustand");
}

#ifdef _WIN32
void require_optimized_msvc_relwithdebinfo(const std::filesystem::path& build_root) {
    std::ifstream cache(build_root / "CMakeCache.txt", std::ios::binary);
    if (!cache)
        throw std::runtime_error(
            "CMakeCache fehlt nach erfolgreicher Hostbuild-Konfiguration.");
    constexpr std::string_view entry = "CMAKE_CXX_FLAGS_RELWITHDEBINFO:";
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
            "MSVC-RelWithDebInfo-Configure besitzt keine wirksame Optimierung.");
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

std::string runtime_dependency_manifest_document(
    const katana::runtime::DiscInstallRecipe& recipe) {
    return "{\"schema\":\"katana-runtime-dependencies\",\"version\":1,"
           "\"linkage\":\"static\",\"job_generation\":\"" +
           recipe.job_generation + "\",\"files\":[]}\n";
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
        runtime_dependency_manifest_document(expected_recipe))
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
    const auto state_path = port_export_cache_path(workspace, source_kind, key);
    ensure_safe_port_directory_chain(
        workspace,
        state_path.parent_path(),
        "Content-addressed Portexport-Cache");
    const auto temporary = std::filesystem::path(state_path.string() + ".tmp");
    std::ostringstream state_content;
    state_content << "KATANA_PORT_EXPORT_STATE "
                  << port_export_cache_version << '\n'
                  << "key " << key << '\n'
                  << "source " << source_kind << '\n'
                  << "tree " << *tree_identity << '\n'
                  << "recipe " << port_export_recipe_identity(expected_recipe)
                  << '\n'
                  << "functions " << report.functions << '\n'
                  << "partitions " << report.partitions << '\n';
    const auto serialized_state = state_content.str();
    if (serialized_state.size() > maximum_port_export_cache_state_bytes)
        throw std::runtime_error(
            "Content-addressed Portexport-Status ueberschreitet sein Bytebudget.");
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
            "Temporaerer Portexport-Status ist kein sicher ersetzbares Artefakt.");
    }
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(
        serialized_state.data(),
        static_cast<std::streamsize>(serialized_state.size()));
    output.flush();
    if (!output)
        throw std::runtime_error(
            "Content-addressed Portexport-Status konnte nicht geschrieben werden.");
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
            "Alter content-addressed Portexport-Status konnte nicht sicher ersetzt werden.");
    }
    ensure_safe_port_directory_chain(
        workspace,
        state_path.parent_path(),
        "Content-addressed Portexport-Cache");
    std::filesystem::rename(temporary, state_path, replace_error);
    if (replace_error)
        throw std::runtime_error(
            "Content-addressed Portexport-Status konnte nicht publiziert werden.");
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

using LatentAotEntryHintArgument = katana::codegen::LatentAotEntryHint;
using LatentAotDiscoveryModeArgument =
    katana::codegen::LatentAotDiscoveryMode;

constexpr std::uint64_t latent_aot_entry_disc_sector_size = 2048u;
constexpr std::size_t maximum_latent_aot_entry_hint_arguments = 1024u;

LatentAotDiscoveryModeArgument parse_latent_aot_discovery_mode(
    const std::string_view text) {
    if (text == "heuristic")
        return LatentAotDiscoveryModeArgument::HintsAndHeuristics;
    if (text == "exact-only")
        return LatentAotDiscoveryModeArgument::ExactOnly;
    throw std::invalid_argument(
        "--latent-aot-mode erwartet heuristic oder exact-only.");
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
           hint.byte_size >= 2u && (hint.byte_size & 1u) == 0u &&
           hint.byte_size <=
               katana::runtime::maximum_native_aot_template_extent &&
           (hint.module_relative_offset & 1u) == 0u &&
           hint.module_relative_offset <= hint.byte_size - 2u &&
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
            "sha256:<64-lowerhex>@<disc-byte-offset>:<module-byte-size>:"
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
            "sha256:<64-lowerhex>@<disc-byte-offset>:<module-byte-size>:"
            "<module-relative-offset>.");
    const auto disc_byte_offset = parse_latent_aot_entry_integer(
        fields.substr(0u, first_separator), "Disc-Byteoffset");
    const auto byte_size = parse_latent_aot_entry_integer(
        fields.substr(first_separator + 1u, second_separator - first_separator - 1u),
        "Modulgroesse");
    const auto module_relative_offset = parse_latent_aot_entry_integer(
        fields.substr(second_separator + 1u), "Modulentryoffset");
    if ((disc_byte_offset % latent_aot_entry_disc_sector_size) != 0u ||
        byte_size < 2u || byte_size > std::numeric_limits<std::uint32_t>::max() ||
        (byte_size & 1u) != 0u ||
        module_relative_offset > std::numeric_limits<std::uint32_t>::max() ||
        (module_relative_offset & 1u) != 0u ||
        module_relative_offset + 2u > byte_size ||
        disc_byte_offset > std::numeric_limits<std::uint64_t>::max() - byte_size)
        throw std::invalid_argument(
            "--latent-aot-entry besitzt eine ungueltige Modulbindung.");
    return {std::string(identity),
            disc_byte_offset,
            static_cast<std::uint32_t>(byte_size),
            static_cast<std::uint32_t>(module_relative_offset)};
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
    if (status_error || !std::filesystem::is_regular_file(status) ||
        unsafe_port_filesystem_link(path, status))
        throw std::invalid_argument(
            "Runtime-Image-Payload muss eine regulaere Nicht-Symlink-Datei sein.");
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
    if (std::filesystem::file_size(canonical, status_error) !=
            expected_size ||
        status_error)
        throw std::invalid_argument(
            "Runtime-Image-Payloadgroesse passt nicht zum Deskriptor.");
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
                            game_entry_handoff_path = std::nullopt,
                        const std::vector<RuntimeImagePayloadArgument>&
                            runtime_image_payload_arguments = {},
                        const std::vector<LatentAotEntryHintArgument>&
                            latent_aot_entry_hints = {},
                        const LatentAotDiscoveryModeArgument
                            latent_aot_discovery_mode =
                                LatentAotDiscoveryModeArgument::
                                     HintsAndHeuristics,
                        const std::optional<std::filesystem::path>&
                            telemetry_jsonl_path = std::nullopt) {
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
    const auto source_root = discover_source_root_for_protection();
    const auto runtime_binding = discover_runtime_binding_for_build(source_root);
    const auto absolute_output = std::filesystem::absolute(output_path).lexically_normal();
    if (absolute_output == absolute_output.root_path() ||
        absolute_output.filename().empty())
        throw std::invalid_argument(
            "Port-Ausgabe darf kein Dateisystemstamm sein.");
    ensure_safe_absolute_directory_chain(
        absolute_output.parent_path(), "Port-Ausgabeelternpfad");
    const auto publish_paths =
        port_publish_output_paths(absolute_output);
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
    katana::cli::PortBuildTelemetryOptions telemetry_options;
    telemetry_options.jsonl_path =
        normalized_telemetry_jsonl_path;
    telemetry_options.require_phase_timings =
        normalized_telemetry_jsonl_path.has_value();
    telemetry_options.build_profile =
        configured_environment_value("KATANA_PORT_BUILD_PROFILE")
            .value_or("bringup");
    telemetry_options.host_compile_jobs_requested =
        host_compile_budget.requested;
    telemetry_options.host_compile_jobs_effective =
        host_compile_budget.effective;
    telemetry_options.job_kind =
        boot_executable_artifact
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
    const ExclusivePortExportLock publish_lock(
        publish_paths.lock_base);
    maybe_hold_port_publish_lock_for_test();
    recover_port_publish_transaction(publish_paths);
    if (exit_after_port_publish_recovery_for_test()) {
        telemetry_run.complete();
        return 0;
    }
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
    std::optional<std::string> whole_export_cache_key;
    std::string whole_export_source_kind;
    std::uint32_t whole_export_source_contract_version = 0u;
    std::string whole_export_boot_file_name;
    std::uint32_t whole_export_entry_address = 0u;
    const auto implementation_identities =
        port_export_implementation_identities();
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
    phase_timings.transition("analysis-codegen");
    std::cout << "KATANA_PORT_PHASE analysis-codegen\n";
    std::cout << std::flush;
    if (game_project_path.has_value()) {
        verified_game_project =
            katana::runtime::GameProjectArtifact::load(
                *game_project_path);
        resolved_game_project = verified_game_project->definition();
    }
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
        runtime_image_payloads);
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
        latent_aot_hint_identity,
        implementation_identities.whole_export);
    const auto workspace_key = port_export_workspace_key(
        whole_export_source_kind,
        *verified_install_recipe,
        target_name);
    const auto workspace_root =
        port_export_workspace_root(
            absolute_output.parent_path());
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
                    *verified_install_recipe);
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
        // Detailed miss-reason key comparisons are intentionally opt-in: a
        // normal human progress stream must stay on the zero-overhead key
        // path, while an explicitly requested JSONL performance trace must
        // contain the complete primary miss ledger.
        export_options.detailed_analysis_telemetry =
            normalized_telemetry_jsonl_path.has_value();
        export_options.analysis_cache_root =
            component_cache_root;
        export_options.codegen_cache_root = workspace / ".katana-codegen-cache";
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
        export_options.game_project_runtime_image_payloads =
            runtime_image_payloads;
        export_options.latent_aot_entry_hints =
            normalized_latent_aot_entry_hints;
        export_options.latent_aot_discovery_mode =
            latent_aot_discovery_mode;
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
                              source_path, workspace, export_options)
                    : katana::codegen::export_dreamcast_port_project(
                          *verified_native_disc, workspace, export_options);
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
        auto compiler_launcher =
            configured_environment_value("KATANA_COMPILER_CACHE");
        if (!compiler_launcher)
            compiler_launcher =
                configured_environment_value(
                    "CMAKE_CXX_COMPILER_LAUNCHER");
        if (!compiler_launcher &&
            !katana::build_contract::configured_compiler_launcher.empty())
            compiler_launcher = std::string(
                katana::build_contract::configured_compiler_launcher);
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
        const auto build_path =
            report.output_root /
            ("build-" + host_compiler + '-' + host_linker + '-' +
             build_profile + '-' + generator_identity);
        static_cast<void>(
            safe_regular_port_directory_exists(build_path, "Inkrementeller Hostbuild-Cache"));
        if (!runtime_binding.build_targets_file.empty()) {
            const auto runtime_build_root =
                runtime_binding.build_targets_file.parent_path();
            const auto runtime_target =
                diagnostic_partial
                    ? std::string_view("katana_runtime")
                    : std::string_view("katana_runtime_core");
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
        auto instrumented_compiler_launcher =
            cli_component + ";__host-build-tool;" +
            event_component + ";compile;";
        if (compiler_launcher)
            instrumented_compiler_launcher +=
                "--chain;" + *compiler_launcher;
        else
            instrumented_compiler_launcher += "--direct";
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
                " -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO=" + shell_quote(build_path);
        }
#else
        configure += " -G Ninja";
#endif
        configure +=
            " -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKATANA_PORT_BUILD_PROFILE=" +
            build_profile;
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
                remove_failed_port_host_build_state(report.output_root, build_path);
            } catch (const std::exception& cleanup_error) {
                throw katana::cli::Error(
                    katana::cli::ExitCode::BuildFailure,
                    std::string(
                        "Port-Hostbuild-Prozess konnte nicht sicher ausgefuehrt "
                        "und sein unvollstaendiger CMake-Zustand nicht "
                        "bereinigt werden: ") +
                        process_error.what() + ' ' +
                        cleanup_error.what());
            }
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                std::string(
                    "Port-Hostbuild-Prozess konnte nicht sicher ausgefuehrt "
                    "werden; der unvollstaendige CMake-Zustand wurde "
                    "entfernt: ") +
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
                remove_failed_port_host_build_state(
                    report.output_root,
                    build_path);
            } catch (const std::exception& cleanup_error) {
                throw katana::cli::Error(
                    katana::cli::ExitCode::BuildFailure,
                    failure +
                        "; unvollstaendiger CMake-Zustand konnte nicht "
                        "bereinigt werden: " +
                        cleanup_error.what());
            }
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                failure +
                    "; unvollstaendiger CMake-Zustand wurde entfernt.");
        }
#ifdef _WIN32
        try {
            require_optimized_msvc_relwithdebinfo(build_path);
        } catch (const std::exception& configuration_error) {
            try {
                remove_failed_port_host_build_state(report.output_root, build_path);
            } catch (const std::exception& cleanup_error) {
                throw katana::cli::Error(
                    katana::cli::ExitCode::BuildFailure,
                    std::string("Unsicherer Port-Hostbuild-Configure und fehlgeschlagene "
                                "Bereinigung: ") +
                        configuration_error.what() + ' ' + cleanup_error.what());
            }
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                std::string("Unsicherer Port-Hostbuild-Configure wurde verworfen: ") +
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
        // katana_generated always enables one CMake-generated PCH compile
        // edge in addition to every listed generated/support source.
        const auto planned_translation_units =
            generated_translation_units + support_translation_units + 1u;
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
        katana::cli::HostBuildProgressObserver host_build_progress(
            host_build_event_root,
            katana::cli::HostBuildProgressPlan{
                planned_translation_units,
                1u,
                1u,
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
                " --config RelWithDebInfo -- /nodeReuse:false "
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
            const auto failure =
                std::string(
                    "KATANA_PORT_HOST_COMMAND_TIMEOUT "
                    "stage=host-build limit_ms=") +
                std::to_string(build_runtime->count()) +
                " process_tree=terminated";
            try {
                remove_failed_port_host_build_state(
                    report.output_root,
                    build_path);
            } catch (const std::exception& cleanup_error) {
                throw katana::cli::Error(
                    katana::cli::ExitCode::BuildFailure,
                    failure +
                        "; unvollstaendiger Buildzustand konnte nicht "
                        "bereinigt werden: " +
                        cleanup_error.what());
            }
            throw katana::cli::Error(
                katana::cli::ExitCode::BuildFailure,
                failure +
                    "; unvollstaendiger Buildzustand wurde entfernt.");
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
                6u,
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
        runtime_manifest << runtime_dependency_manifest_document(recipe);
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
            recipe);
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
                  << "Buildprofil: " << build_profile << '\n'
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

void print_usage(std::ostream& output) {
    output << "Verwendung:\n"
           << "  katana-recomp <Opcode>\n"
           << "  katana-recomp opcode <Opcode>\n"
           << "  katana-recomp isa-report [--json] "
              "[--external-evidence <katana-sh4-sst-conformance.json>]\n"
           << "  katana-recomp analyze <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp analyze-json <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp cfg-json <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp cfg-dot <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp callgraph-json <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp callgraph-dot <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp disc-audit <Quelle.gdi> [--json] [--include-accesses] "
              "[--fail-on-gap|--strict]\n"
           << "  katana-recomp disc-audit-set <Verzeichnis> [--json] [--jobs N] "
              "[--fail-on-gap|--strict]\n"
           << "  katana-recomp firmware-diagnose <bios|flash> <Datei> [--sha256 <Hash>] "
              "[--include-sensitive]\n"
           << "  katana-recomp disasm <Datei> [Basisadresse]\n"
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
              "[--runtime-image-payload <Image-ID>=<private-Datei>] "
              "[--telemetry-jsonl <Datei>] "
              "[--latent-aot-mode <heuristic|exact-only>] "
              "[--latent-aot-entry "
              "<sha256:<64-lowerhex>@<disc-byte-offset>:<module-byte-size>:"
              "<module-relative-offset>>]...\n"
           << "  katana-recomp probe-port <Quelle.gdi> --output <Ordner> --target-name <Name> "
              "[--console-profile <...>] [--telemetry-jsonl <Datei>]\n"
           << "  katana-recomp port-executable <boot.katana-executable> --output <Ordner> "
              "--target-name <Name> [--console-profile <...>] "
              "[--game-project <Descriptor-Artefakt>] "
              "[--runtime-image-payload <Image-ID>=<private-Datei>]... "
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
                if (!event_root || argc < 2) return 125;
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
                    *event_root,
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
            arguments.reserve(static_cast<std::size_t>(argc - 6));
            for (int index = 6; index < argc; ++index)
                arguments.push_back(argv[index]);
            return katana::cli::run_host_build_tool_launcher(
                *kind,
                std::filesystem::path(argv[2]),
                argv[5],
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

        const auto port_command =
            argc >= 2 ? std::string_view(argv[1]) : std::string_view{};
        if (argc >= 7 && (argc & 1) == 1 &&
            (port_command == "port" || port_command == "probe-port" ||
             port_command == "port-executable" ||
             port_command == "probe-port-executable")) {
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
                telemetry_jsonl_path;
            std::vector<RuntimeImagePayloadArgument>
                runtime_image_payload_arguments;
            std::vector<LatentAotEntryHintArgument>
                latent_aot_entry_hints;
            auto latent_aot_discovery_mode =
                LatentAotDiscoveryModeArgument::HintsAndHeuristics;
            bool latent_aot_discovery_mode_seen = false;
            std::string console_profile = "japan-ntsc";
            bool console_profile_seen = false;
            for (std::size_t argument = 3u; argument < static_cast<std::size_t>(argc);
                 argument += 2u) {
                const std::string_view option = argv[argument];
                if (option == "--output" && !output_path.has_value()) {
                    output_path = std::filesystem::path(argv[argument + 1u]);
                } else if (option == "--target-name" && !target_name.has_value()) {
                    target_name = argv[argument + 1u];
                } else if (option == "--console-profile" && !console_profile_seen) {
                    console_profile = argv[argument + 1u];
                    console_profile_seen = true;
                } else if (option == "--telemetry-jsonl" &&
                           !telemetry_jsonl_path.has_value()) {
                    telemetry_jsonl_path =
                        std::filesystem::path(
                            argv[argument + 1u]);
                } else if (option == "--game-entry-handoff" &&
                           port_command == "port-executable" &&
                           !game_entry_handoff_path.has_value()) {
                    game_entry_handoff_path =
                        std::filesystem::path(argv[argument + 1u]);
                } else if (option == "--game-project" &&
                           (port_command == "port" ||
                            port_command == "port-executable") &&
                           !game_project_path.has_value()) {
                    game_project_path =
                        std::filesystem::path(argv[argument + 1u]);
                } else if (
                    option == "--runtime-image-payload" &&
                    (port_command == "port" ||
                     port_command == "port-executable")) {
                    const std::string_view binding =
                        argv[argument + 1u];
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
                } else if (option == "--latent-aot-entry" &&
                           port_command == "port") {
                    latent_aot_entry_hints.push_back(
                        parse_latent_aot_entry_hint(argv[argument + 1u]));
                } else if (option == "--latent-aot-mode" &&
                           port_command == "port" &&
                           !latent_aot_discovery_mode_seen) {
                    latent_aot_discovery_mode =
                        parse_latent_aot_discovery_mode(
                            argv[argument + 1u]);
                    latent_aot_discovery_mode_seen = true;
                } else {
                    throw std::invalid_argument(
                        "port erwartet eindeutige Ausgabe-, Ziel- und Konsolenprofiloptionen.");
                }
            }
            latent_aot_entry_hints =
                normalize_latent_aot_entry_hints(std::move(latent_aot_entry_hints));
            if (!latent_aot_discovery_mode_seen &&
                !latent_aot_entry_hints.empty())
                latent_aot_discovery_mode =
                    LatentAotDiscoveryModeArgument::ExactOnly;
            if (!output_path.has_value() || !target_name.has_value()) {
                throw std::invalid_argument(
                    "port erwartet --output und --target-name jeweils genau einmal.");
            }
            return export_port_project(std::filesystem::path(argv[2]),
                                       *output_path,
                                       *target_name,
                                       diagnostic_partial,
                                       console_profile,
                                       boot_executable_artifact,
                                       game_project_path,
                                       game_entry_handoff_path,
                                       runtime_image_payload_arguments,
                                       latent_aot_entry_hints,
                                       latent_aot_discovery_mode,
                                       telemetry_jsonl_path);
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

        if ((argc == 3 || argc == 4) && std::string(argv[1]) == "disasm") {
            const auto base_address =
                argc == 4 ? parse_hex_value(argv[3],
                                            std::numeric_limits<std::uint32_t>::max(),
                                            "Die Basisadresse")
                          : 0u;

            return disassemble_file(std::filesystem::path(argv[2]), base_address);
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
