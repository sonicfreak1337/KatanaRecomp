#include "katana/phase9/homebrew.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>

int main(const int argc, char* argv[]) {
    try {
        const auto corpus = katana::phase9::build_homebrew_corpus();
        if (argc == 2 && std::string_view(argv[1]) == "--corpus") {
            std::cout << katana::phase9::format_homebrew_corpus_json(corpus);
            return EXIT_SUCCESS;
        }
        if (argc != 1) return EXIT_FAILURE;
        const auto frame = katana::phase9::run_homebrew_host_frame();
        if (frame.silent_failures != 0u) return EXIT_FAILURE;
        std::cout << katana::phase9::format_homebrew_host_frame_json(frame);
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
