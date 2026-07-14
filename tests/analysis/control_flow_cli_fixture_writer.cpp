#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

int main(const int argc, char* argv[]) {
    if (argc != 2) {
        return 2;
    }
    const auto directory = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(directory);
    constexpr std::array<std::uint8_t, 40> bytes{
        0x0Cu, 0xE1u, 0x0Bu, 0x41u, 0x09u, 0x00u, 0x0Bu, 0x00u,
        0x09u, 0x00u, 0x09u, 0x00u, 0x2Bu, 0x42u, 0x09u, 0x00u,
        0x09u, 0x00u, 0x2Bu, 0x43u, 0x09u, 0x00u, 0x09u, 0x00u,
        0x0Bu, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u,
        0x18u, 0x00u, 0x00u, 0x00u, 0x1Cu, 0x00u, 0x00u, 0x00u
    };
    {
        std::ofstream output(directory / "control-flow.bin", std::ios::binary);
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        );
        if (!output) {
            throw std::runtime_error("Synthetische Kontrollfluss-Fixture konnte nicht geschrieben werden.");
        }
    }
    {
        std::ofstream output(directory / "control-flow.katana");
        output
            << "version = 1\n"
            << "format = raw\n"
            << "input = control-flow.bin\n"
            << "base_address = 0\n"
            << "entry_point = 0\n"
            << "segment_name = .text\n"
            << "segment_kind = code\n"
            << "permissions = r-x\n";
    }
    {
        std::ofstream output(directory / "control-flow.overrides");
        output
            << "version = 1\n"
            << "jump = 0x0000000C 0x00000012\n"
            << "jump_table = 0x00000012 0x00000020 2\n";
    }
    return 0;
}
