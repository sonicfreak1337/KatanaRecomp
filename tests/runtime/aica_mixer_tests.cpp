#include "katana/runtime/aica.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) { std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
template<typename E, typename F> bool throws(F&& f) { try { f(); } catch (const E&) { return true; } return false; }
}

int main() {
    using namespace katana::runtime;
    const std::vector<std::int16_t> center_samples = {10000, -10000};
    const std::vector<std::int16_t> left_samples = {5000};
    const std::vector<AicaVoice> voices = {
        {center_samples, aica_unity_gain, aica_pan_center},
        {left_samples, aica_unity_gain, aica_pan_left}
    };
    AicaMixer mixer;
    const auto mixed = mixer.mix(voices, 2u);
    require(mixed == std::vector<std::int16_t>({15000, 10000, -10000, -10000}),
        "AICA-Mixer behandelt Center, Hard-Pan oder kurze Voices falsch.");
    const std::vector<std::int16_t> loud = {30000, -30000};
    const std::vector<AicaVoice> clipping = {
        {loud, aica_unity_gain, aica_pan_center},
        {loud, aica_unity_gain, aica_pan_center}
    };
    require(mixer.mix(clipping, 2u) == std::vector<std::int16_t>({32767, 32767, -32768, -32768}),
        "AICA-Mixer saettigt positive oder negative Uebersteuerung nicht.");
    require(throws<std::invalid_argument>([&] {
        static_cast<void>(mixer.mix(std::vector<AicaVoice>{{center_samples, aica_unity_gain + 1u, 0}}, 1u));
    }), "AICA-Mixer akzeptiert Gain oberhalb Eins.");

    RecordingAicaAudioBackend backend;
    backend.submit(mixed, 44100u);
    require(backend.submitted_buffers() == 1u && backend.submitted_frames() == 2u &&
        backend.sample_rate() == 44100u && backend.last_buffer() == mixed,
        "Host-Audio-Backend verliert Puffer, Takt oder Framezaehler.");
    require(throws<std::invalid_argument>([&] {
        backend.submit(std::vector<std::int16_t>{1}, 44100u);
    }), "Host-Audio-Backend akzeptiert unvollstaendige Stereo-Frames.");

    std::cout << "KR-2903 Mixer und Host-Audio erfolgreich.\n";
}
