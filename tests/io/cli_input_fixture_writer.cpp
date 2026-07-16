#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void write_u16(std::vector<std::uint8_t>& bytes,
               const std::size_t offset,
               const std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

void write_u32(std::vector<std::uint8_t>& bytes,
               const std::size_t offset,
               const std::uint32_t value) {
    for (std::size_t index = 0u; index < 4u; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
    }
}

void save_binary(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc != 2) return 2;
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);

    const std::vector<std::uint8_t> code = {0x0Bu, 0x00u, 0x09u, 0x00u};
    save_binary(directory / "program.bin", code);

    std::vector<std::uint8_t> elf(88u, 0u);
    elf[0] = 0x7Fu;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 1u;
    elf[5] = 1u;
    elf[6] = 1u;
    write_u16(elf, 16u, 2u);
    write_u16(elf, 18u, 42u);
    write_u32(elf, 20u, 1u);
    write_u32(elf, 24u, 0x8C010000u);
    write_u32(elf, 28u, 52u);
    write_u16(elf, 40u, 52u);
    write_u16(elf, 42u, 32u);
    write_u16(elf, 44u, 1u);
    write_u32(elf, 52u, 1u);
    write_u32(elf, 56u, 84u);
    write_u32(elf, 60u, 0x8C010000u);
    write_u32(elf, 68u, 4u);
    write_u32(elf, 72u, 4u);
    write_u32(elf, 76u, 5u);
    std::copy(code.begin(), code.end(), elf.begin() + 84u);
    save_binary(directory / "program.elf", elf);

    std::ofstream manifest(directory / "program.katana");
    manifest << "version = 1\n"
             << "format = raw\n"
             << "input = program.bin\n"
             << "base_address = 0x8C010000\n"
             << "entry_point = 0x8C010000\n"
             << "segment_kind = code\n"
             << "permissions = r-x\n";
    return manifest ? 0 : 2;
}
