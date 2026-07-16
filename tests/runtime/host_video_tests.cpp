#include "katana/runtime/host_video.hpp"

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
} // namespace

int main() {
    using namespace katana::runtime;
    static_assert(native_video_contract_version == 1u);
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
