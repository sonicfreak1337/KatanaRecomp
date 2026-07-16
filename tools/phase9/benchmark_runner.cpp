#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/ir/lower.hpp"
#include "katana/phase9/homebrew.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t microseconds(const Clock::time_point begin, const Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
}

} // namespace

int main() {
    try {
        katana::io::ExecutableImage image("phase9-benchmark");
        image.add_segment({".text",
                           0x8C010000u,
                           0u,
                           8u,
                           katana::io::SegmentKind::Code,
                           {true, false, true},
                           {0x01u, 0xE0u, 0x01u, 0x70u, 0x0Bu, 0x00u, 0x09u, 0x00u}});
        image.add_entry_point(0x8C010000u);

        const auto analysis_begin = Clock::now();
        const auto analysis = katana::analysis::analyze_control_flow(image);
        const auto analysis_end = Clock::now();
        const auto codegen_begin = Clock::now();
        const auto program = katana::ir::lower_program(analysis);
        const auto source = katana::codegen::emit_cpp_program(program, 0x8C010000u);
        const auto codegen_end = Clock::now();
        const auto runtime_begin = Clock::now();
        const auto report = katana::phase9::run_homebrew_host_frame();
        const auto runtime_end = Clock::now();

        std::cout << "{\"schema\":\"katana-phase9-benchmark\",\"version\":1"
                  << ",\"analysis_us\":" << microseconds(analysis_begin, analysis_end)
                  << ",\"codegen_us\":" << microseconds(codegen_begin, codegen_end)
                  << ",\"runtime_us\":" << microseconds(runtime_begin, runtime_end)
                  << ",\"generated_cpp_bytes\":" << source.size()
                  << ",\"state_hash\":" << report.state_hash
                  << ",\"silent_failures\":" << report.silent_failures << "}\n";
        return report.silent_failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
