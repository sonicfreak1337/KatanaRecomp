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
    AicaSampleDecoder pcm16(AicaSampleFormat::Pcm16);
    require(pcm16.decode(std::vector<std::uint8_t>{0x34u, 0x12u, 0x00u, 0x80u}, 2u) ==
        std::vector<std::int16_t>({0x1234, -32768}), "PCM16 ist nicht signed little-endian.");
    AicaSampleDecoder pcm8(AicaSampleFormat::Pcm8);
    require(pcm8.decode(std::vector<std::uint8_t>{0x7Fu, 0x80u, 0xFFu}, 3u) ==
        std::vector<std::int16_t>({32512, -32768, -256}), "PCM8 wird nicht korrekt vorzeichenerweitert.");
    require(throws<std::out_of_range>([&] {
        static_cast<void>(pcm16.decode(std::vector<std::uint8_t>{0u}, 1u));
    }), "Abgeschnittenes PCM16 wird akzeptiert.");

    AicaSampleDecoder adpcm(AicaSampleFormat::Adpcm4);
    const auto first = adpcm.decode(std::vector<std::uint8_t>{0x70u}, 2u);
    require(first == std::vector<std::int16_t>({15, 253}) && adpcm.predictor() == 253 && adpcm.step() == 304,
        "AICA-ADPCM-Nibblefolge oder Step-Anpassung ist falsch.");
    const auto continued = adpcm.decode(std::vector<std::uint8_t>{0x08u}, 1u);
    require(continued == std::vector<std::int16_t>({215}) && adpcm.predictor() == 215,
        "AICA-ADPCM setzt den Zustand zwischen Streaming-Aufrufen zurueck.");
    adpcm.reset();
    require(adpcm.predictor() == 0 && adpcm.step() == 127 &&
        adpcm.decode(std::vector<std::uint8_t>{0x70u}, 2u) == first,
        "AICA-ADPCM-Reset ist nicht deterministisch.");
    AicaSampleDecoder clipping(AicaSampleFormat::Adpcm4);
    static_cast<void>(clipping.decode(std::vector<std::uint8_t>(32u, 0x77u), 64u));
    require(clipping.predictor() == 32767 && clipping.step() <= 24576,
        "AICA-ADPCM begrenzt Predictor oder Step nicht.");

    std::cout << "KR-2902 PCM und ADPCM erfolgreich.\n";
}
