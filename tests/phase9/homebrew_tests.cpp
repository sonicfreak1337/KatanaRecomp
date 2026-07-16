#include "katana/phase9/homebrew.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        const auto corpus = katana::phase9::build_homebrew_corpus();
        katana::phase9::require_valid_homebrew_corpus(corpus);
        const auto corpus_json = katana::phase9::format_homebrew_corpus_json(corpus);
        require(corpus_json.find("cpu-conformance") != std::string::npos &&
                    corpus_json.find("integrated-game") != std::string::npos &&
                    corpus_json.find("firmware-handoff") != std::string::npos &&
                    corpus_json.find("project-authored-synthetic") != std::string::npos &&
                    corpus_json.find("C:\\") == std::string::npos &&
                    corpus_json.find("/home/") == std::string::npos,
                "Homebrew-Korpus verliert Pflichtartefakt, Provenienz oder Portabilitaet.");

        const auto handoff = katana::phase9::run_synthetic_firmware_handoff();
        require(handoff.p2_entry == 0xAC010000u && handoff.p1_entry == 0x8C010000u &&
                    handoff.rom_physical != handoff.ram_physical && handoff.copied_bytes == 8u &&
                    handoff.dynamic_vectors == 5u && handoff.prefetches == 1u &&
                    handoff.store_queue_observed && handoff.invalidations == 1u &&
                    handoff.analysis_complete && handoff.generated_cpp_complete &&
                    handoff.dispatch_complete,
                "Synthetischer Firmware-Handoff erreicht Analyse, Codegen, Aliasdispatch oder "
                "Invalidierung nicht.");

        const auto first = katana::phase9::run_homebrew_host_frame();
        const auto second = katana::phase9::run_homebrew_host_frame();
        const auto first_json = katana::phase9::format_homebrew_host_frame_json(first);
        const auto second_json = katana::phase9::format_homebrew_host_frame_json(second);
        require(first.marker == "KR_PHASE9_HOMEBREW_HOST_FRAME" && first.pvr_frames >= 1u &&
                    first.frame_intervals >= 2u && first.frame_width == 320u &&
                    first.frame_height == 240u && first.frame_rgba_bytes == 320u * 240u * 4u &&
                    first.audio_buffers >= 1u && first.maple_transactions >= 1u &&
                    first.dma_units == 1u && first.interrupts == 1u && first.invalidations == 1u &&
                    first.replay_events == 7u && first.guest_cycles >= 4u &&
                    first.silent_failures == 0u && first.console_output == "KATANA-HOMEBREW-OK",
                "Zusammenhaengendes Homebrew-Testspiel erreicht keinen vollstaendigen Hostframe.");
        require(first_json == second_json && first.state_hash == second.state_hash &&
                    first.audio_hash == second.audio_hash && first.frame_hash == second.frame_hash,
                "Homebrew-Hostframe ist nicht deterministisch reproduzierbar.");
        require(first_json.find("firmware") == std::string::npos &&
                    first_json.find("private") == std::string::npos &&
                    first_json.find("C:\\") == std::string::npos,
                "Homebrew-Bericht enthaelt Firmware-, Privat- oder Hostpfadmarker.");

        std::cout << "KR-3801 bis KR-3808 Homebrew-Vertical-Slice erfolgreich.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
