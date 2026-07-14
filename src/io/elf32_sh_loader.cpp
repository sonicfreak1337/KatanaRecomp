#include "katana/io/elf32_sh_loader.hpp"

#include "katana/io/binary_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace katana::io {
namespace {

constexpr std::size_t kElf32HeaderSize = 52u;
constexpr std::size_t kElf32ProgramHeaderSize = 32u;
constexpr std::uint16_t kExecutableType = 2u;
constexpr std::uint16_t kSuperHMachine = 42u;
constexpr std::uint32_t kLoadSegment = 1u;
constexpr std::uint32_t kFlagExecute = 1u;
constexpr std::uint32_t kFlagWrite = 2u;
constexpr std::uint32_t kFlagRead = 4u;

[[noreturn]] void fail(
    const std::filesystem::path& path,
    const std::size_t offset,
    const std::string& cause
) {
    throw std::runtime_error(
        "ELF32-SH-Loaderfehler in " + path.string() +
        " bei Offset " + std::to_string(offset) + ": " + cause
    );
}

void require_range(
    const std::span<const std::uint8_t> bytes,
    const std::size_t offset,
    const std::size_t width,
    const std::filesystem::path& path,
    const std::string& field
) {
    if (offset > bytes.size() || width > bytes.size() - offset) {
        fail(path, offset, field + " liegt ausserhalb der Datei.");
    }
}

std::uint16_t read_u16(
    const std::span<const std::uint8_t> bytes,
    const std::size_t offset,
    const std::filesystem::path& path,
    const std::string& field
) {
    require_range(bytes, offset, 2u, path, field);
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(bytes[offset + 1u]) << 8u)
    );
}

std::uint32_t read_u32(
    const std::span<const std::uint8_t> bytes,
    const std::size_t offset,
    const std::filesystem::path& path,
    const std::string& field
) {
    require_range(bytes, offset, 4u, path, field);
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

}

ExecutableImage load_elf32_sh(const std::filesystem::path& path) {
    const auto bytes = read_binary_file(path);
    require_range(bytes, 0u, kElf32HeaderSize, path, "ELF32-Header");

    if (bytes[0] != 0x7Fu || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        fail(path, 0u, "ungueltige ELF-Magie.");
    }
    if (bytes[4] != 1u) {
        fail(path, 4u, "nur ELFCLASS32 wird unterstuetzt.");
    }
    if (bytes[5] != 1u) {
        fail(path, 5u, "nur Little-Endian-ELF wird unterstuetzt.");
    }
    if (bytes[6] != 1u || read_u32(bytes, 20u, path, "e_version") != 1u) {
        fail(path, 6u, "ungueltige ELF-Version.");
    }
    if (read_u16(bytes, 16u, path, "e_type") != kExecutableType) {
        fail(path, 16u, "nur ausfuehrbare ELF-Dateien werden unterstuetzt.");
    }
    if (read_u16(bytes, 18u, path, "e_machine") != kSuperHMachine) {
        fail(path, 18u, "e_machine ist nicht EM_SH (42).");
    }
    if (read_u16(bytes, 40u, path, "e_ehsize") != kElf32HeaderSize) {
        fail(path, 40u, "unerwartete ELF32-Headergroesse.");
    }

    const auto entry = read_u32(bytes, 24u, path, "e_entry");
    const auto program_offset = read_u32(bytes, 28u, path, "e_phoff");
    const auto program_entry_size = read_u16(bytes, 42u, path, "e_phentsize");
    const auto program_count = read_u16(bytes, 44u, path, "e_phnum");
    if (program_entry_size != kElf32ProgramHeaderSize) {
        fail(path, 42u, "unerwartete ELF32-Program-Headergroesse.");
    }
    const auto table_size = static_cast<std::uint64_t>(program_entry_size) * program_count;
    if (table_size > bytes.size() || program_offset > bytes.size() - static_cast<std::size_t>(table_size)) {
        fail(path, program_offset, "Program-Header-Tabelle liegt ausserhalb der Datei.");
    }

    ExecutableImage image(path);
    std::size_t load_index = 0;
    for (std::size_t index = 0; index < program_count; ++index) {
        const auto header_offset = static_cast<std::size_t>(program_offset) + index * program_entry_size;
        if (read_u32(bytes, header_offset, path, "p_type") != kLoadSegment) {
            continue;
        }

        const auto file_offset = read_u32(bytes, header_offset + 4u, path, "p_offset");
        const auto virtual_address = read_u32(bytes, header_offset + 8u, path, "p_vaddr");
        const auto file_size = read_u32(bytes, header_offset + 16u, path, "p_filesz");
        const auto memory_size = read_u32(bytes, header_offset + 20u, path, "p_memsz");
        const auto flags = read_u32(bytes, header_offset + 24u, path, "p_flags");
        if (file_size > memory_size) {
            fail(path, header_offset + 16u, "p_filesz ist groesser als p_memsz.");
        }
        require_range(bytes, file_offset, file_size, path, "PT_LOAD-Dateidaten");
        if (memory_size == 0u) {
            continue;
        }

        std::vector<std::uint8_t> segment_bytes(
            bytes.begin() + static_cast<std::ptrdiff_t>(file_offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(file_offset + file_size)
        );
        try {
            image.add_segment({
                "LOAD" + std::to_string(load_index++),
                virtual_address,
                file_offset,
                memory_size,
                (flags & kFlagExecute) != 0u ? SegmentKind::Code : SegmentKind::Data,
                {
                    (flags & kFlagRead) != 0u,
                    (flags & kFlagWrite) != 0u,
                    (flags & kFlagExecute) != 0u
                },
                std::move(segment_bytes)
            });
        } catch (const std::exception& error) {
            fail(path, header_offset, error.what());
        }
    }
    if (image.segments().empty()) {
        fail(path, program_offset, "keine ladbaren PT_LOAD-Segmente gefunden.");
    }
    image.add_entry_point(entry);
    return image;
}

}
