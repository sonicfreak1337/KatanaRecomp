#include "katana/io/elf32_sh_loader.hpp"

#include "katana/io/binary_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace katana::io {
namespace {

constexpr std::size_t kElf32HeaderSize = 52u;
constexpr std::size_t kElf32ProgramHeaderSize = 32u;
constexpr std::size_t kElf32SectionHeaderSize = 40u;
constexpr std::size_t kElf32SymbolSize = 16u;
constexpr std::size_t kElf32RelocationSize = 8u;
constexpr std::uint16_t kExecutableType = 2u;
constexpr std::uint16_t kSuperHMachine = 42u;
constexpr std::uint32_t kLoadSegment = 1u;
constexpr std::uint32_t kFlagExecute = 1u;
constexpr std::uint32_t kFlagWrite = 2u;
constexpr std::uint32_t kFlagRead = 4u;
constexpr std::uint32_t kSymbolTableSection = 2u;
constexpr std::uint32_t kStringTableSection = 3u;
constexpr std::uint32_t kDynamicSymbolSection = 11u;
constexpr std::uint32_t kRelocationSection = 9u;
constexpr std::uint32_t kRelocationNone = 0u;
constexpr std::uint32_t kRelocationDirectory32 = 1u;
constexpr std::uint32_t kRelocationRelative32 = 2u;

struct ElfSymbolRecord {
    std::string name;
    std::uint32_t value = 0;
    std::uint16_t section_reference = 0;
};

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

std::string read_string(
    const std::span<const std::uint8_t> bytes,
    const std::size_t table_offset,
    const std::size_t table_size,
    const std::uint32_t string_offset,
    const std::filesystem::path& path
) {
    if (string_offset >= table_size) {
        fail(path, table_offset + string_offset, "Symbolname liegt ausserhalb der Stringtabelle.");
    }
    const auto begin = table_offset + string_offset;
    const auto limit = table_offset + table_size;
    auto end = begin;
    while (end < limit && bytes[end] != 0u) {
        ++end;
    }
    if (end == limit) {
        fail(path, begin, "Symbolname ist nicht nullterminiert.");
    }
    return std::string(
        reinterpret_cast<const char*>(bytes.data() + begin),
        end - begin
    );
}

SymbolKind elf_symbol_kind(const std::uint8_t info) noexcept {
    switch (info & 0x0Fu) {
        case 1u: return SymbolKind::Object;
        case 2u: return SymbolKind::Function;
        default: return SymbolKind::Unknown;
    }
}

SymbolBinding elf_symbol_binding(const std::uint8_t info) noexcept {
    switch (info >> 4u) {
        case 0u: return SymbolBinding::Local;
        case 1u: return SymbolBinding::Global;
        case 2u: return SymbolBinding::Weak;
        default: return SymbolBinding::Unknown;
    }
}

RelocationKind elf_relocation_kind(const std::uint32_t type) noexcept {
    switch (type) {
        case kRelocationNone: return RelocationKind::None;
        case kRelocationDirectory32: return RelocationKind::Absolute32;
        case kRelocationRelative32: return RelocationKind::PcRelative32;
        default: return RelocationKind::Unsupported;
    }
}

std::string format_address(const std::uint32_t address) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << address;
    return output.str();
}

void validate_entry_point(
    const ExecutableImage& image,
    const std::uint32_t entry,
    const std::filesystem::path& path
) {
    const auto address = format_address(entry);
    if ((entry & 1u) != 0u) {
        fail(path, 24u, "Einstiegspunkt " + address + " ist nicht auf zwei Byte ausgerichtet.");
    }

    const auto* segment = image.find_segment(entry);
    if (segment == nullptr) {
        fail(path, 24u, "Einstiegspunkt " + address + " liegt ausserhalb aller geladenen Segmente.");
    }
    if (!segment->permissions.executable) {
        fail(path, 24u, "Einstiegspunkt " + address + " liegt in einem nicht ausfuehrbaren Segment.");
    }
    if (segment->kind != SegmentKind::Code) {
        fail(path, 24u, "Einstiegspunkt " + address + " liegt nicht in einem Code-Segment.");
    }

    const auto byte_offset = segment->byte_offset(entry);
    if (!byte_offset.has_value() || segment->bytes.size() < 2u
        || *byte_offset > segment->bytes.size() - 2u) {
        fail(
            path,
            24u,
            "Einstiegspunkt " + address + " liegt ausserhalb der committed Segmentdaten."
        );
    }
}

}

ExecutableImage load_elf32_sh(const std::filesystem::path& path) {
    const auto bytes = read_binary_file(path);
    return load_elf32_sh(std::span<const std::uint8_t>(bytes), path);
}

ExecutableImage load_elf32_sh(
    const std::span<const std::uint8_t> bytes,
    const std::filesystem::path& path
) {
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

    const auto section_offset = read_u32(bytes, 32u, path, "e_shoff");
    const auto section_entry_size = read_u16(bytes, 46u, path, "e_shentsize");
    const auto section_count = read_u16(bytes, 48u, path, "e_shnum");
    if (section_count != 0u) {
        if (section_entry_size != kElf32SectionHeaderSize) {
            fail(path, 46u, "unerwartete ELF32-Section-Headergroesse.");
        }
        const auto section_table_size = static_cast<std::uint64_t>(section_entry_size) * section_count;
        if (section_table_size > bytes.size()
            || section_offset > bytes.size() - static_cast<std::size_t>(section_table_size)) {
            fail(path, section_offset, "Section-Header-Tabelle liegt ausserhalb der Datei.");
        }

        std::vector<std::vector<ElfSymbolRecord>> symbol_tables(section_count);
        for (std::size_t section_index = 0; section_index < section_count; ++section_index) {
            const auto header = static_cast<std::size_t>(section_offset) + section_index * section_entry_size;
            const auto type = read_u32(bytes, header + 4u, path, "sh_type");
            if (type != kSymbolTableSection && type != kDynamicSymbolSection) {
                continue;
            }
            const auto symbol_offset = read_u32(bytes, header + 16u, path, "sh_offset");
            const auto symbol_size = read_u32(bytes, header + 20u, path, "sh_size");
            const auto string_index = read_u32(bytes, header + 24u, path, "sh_link");
            const auto symbol_entry_size = read_u32(bytes, header + 36u, path, "sh_entsize");
            if (symbol_entry_size != kElf32SymbolSize || symbol_size % symbol_entry_size != 0u) {
                fail(path, header + 36u, "ungueltige ELF32-Symboltabellengroesse.");
            }
            require_range(bytes, symbol_offset, symbol_size, path, "Symboltabelle");
            if (string_index >= section_count) {
                fail(path, header + 24u, "sh_link verweist ausserhalb der Section-Tabelle.");
            }
            const auto string_header = static_cast<std::size_t>(section_offset) + string_index * section_entry_size;
            if (read_u32(bytes, string_header + 4u, path, "Stringtabellen-sh_type") != kStringTableSection) {
                fail(path, string_header + 4u, "Symboltabelle verweist nicht auf SHT_STRTAB.");
            }
            const auto string_offset = read_u32(bytes, string_header + 16u, path, "Stringtabellen-sh_offset");
            const auto string_size = read_u32(bytes, string_header + 20u, path, "Stringtabellen-sh_size");
            require_range(bytes, string_offset, string_size, path, "Stringtabelle");

            auto& symbol_records = symbol_tables[section_index];
            symbol_records.resize(symbol_size / symbol_entry_size);
            for (std::size_t symbol_index = 0; symbol_index < symbol_records.size(); ++symbol_index) {
                const auto symbol = static_cast<std::size_t>(symbol_offset) + symbol_index * symbol_entry_size;
                const auto name_offset = read_u32(bytes, symbol, path, "st_name");
                const auto section_reference = read_u16(bytes, symbol + 14u, path, "st_shndx");
                auto& record = symbol_records[symbol_index];
                record.value = read_u32(bytes, symbol + 4u, path, "st_value");
                record.section_reference = section_reference;
                if (name_offset != 0u) {
                    record.name = read_string(bytes, string_offset, string_size, name_offset, path);
                }
                if (record.name.empty() || section_reference == 0u
                    || image.find_symbol(record.name) != nullptr) {
                    continue;
                }
                const auto info = bytes[symbol + 12u];
                image.add_symbol({
                    record.name,
                    record.value,
                    read_u32(bytes, symbol + 8u, path, "st_size"),
                    elf_symbol_kind(info),
                    elf_symbol_binding(info)
                });
            }
        }

        for (std::size_t section_index = 0; section_index < section_count; ++section_index) {
            const auto header = static_cast<std::size_t>(section_offset) + section_index * section_entry_size;
            if (read_u32(bytes, header + 4u, path, "sh_type") != kRelocationSection) {
                continue;
            }
            const auto relocation_offset = read_u32(bytes, header + 16u, path, "sh_offset");
            const auto relocation_size = read_u32(bytes, header + 20u, path, "sh_size");
            const auto symbol_table_index = read_u32(bytes, header + 24u, path, "sh_link");
            const auto relocation_entry_size = read_u32(bytes, header + 36u, path, "sh_entsize");
            if (relocation_entry_size != kElf32RelocationSize
                || relocation_size % relocation_entry_size != 0u) {
                fail(path, header + 36u, "ungueltige ELF32-Relocationstabellengroesse.");
            }
            require_range(bytes, relocation_offset, relocation_size, path, "Relocationstabelle");
            if (symbol_table_index >= section_count || symbol_tables[symbol_table_index].empty()) {
                fail(path, header + 24u, "Relocationstabelle verweist nicht auf eine geladene Symboltabelle.");
            }

            const auto& symbol_records = symbol_tables[symbol_table_index];
            for (std::size_t relocation_index = 0;
                 relocation_index < relocation_size / relocation_entry_size;
                 ++relocation_index) {
                const auto relocation = static_cast<std::size_t>(relocation_offset)
                    + relocation_index * relocation_entry_size;
                const auto address = read_u32(bytes, relocation, path, "r_offset");
                const auto info = read_u32(bytes, relocation + 4u, path, "r_info");
                const auto symbol_index = info >> 8u;
                const auto raw_type = info & 0xFFu;
                if (symbol_index >= symbol_records.size()) {
                    fail(path, relocation + 4u, "Relocation verweist ausserhalb der Symboltabelle.");
                }

                const auto& symbol = symbol_records[symbol_index];
                const auto kind = elf_relocation_kind(raw_type);
                ImageRelocation image_relocation{
                    address,
                    raw_type,
                    kind,
                    symbol.name,
                    symbol.value
                };
                if (kind == RelocationKind::Absolute32 || kind == RelocationKind::PcRelative32) {
                    if (symbol.section_reference == 0u) {
                        fail(path, relocation + 4u, "unterstuetzte Relocation besitzt ein ungeloestes Symbol.");
                    }
                    try {
                        const auto raw_addend = image.read_u32_le(address);
                        image_relocation.addend = static_cast<std::int32_t>(raw_addend);
                        auto value = symbol.value + raw_addend;
                        if (kind == RelocationKind::PcRelative32) {
                            value -= address;
                        }
                        image.write_u32_le(address, value);
                        image_relocation.applied_value = value;
                    } catch (const std::exception& error) {
                        fail(path, relocation, error.what());
                    }
                }
                image.add_relocation(std::move(image_relocation));
            }
        }
    }
    validate_entry_point(image, entry, path);
    image.add_entry_point(entry);
    return image;
}

}
