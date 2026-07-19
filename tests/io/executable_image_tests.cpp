#include "katana/io/executable_image.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception, typename Function>
void require_throws(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    require(false, message);
}

katana::io::ImageSegment code_segment() {
    return {".text",
            0x8C010000u,
            0x40u,
            0x100u,
            katana::io::SegmentKind::Code,
            {true, false, true},
            {0x09u, 0x00u, 0x0Bu, 0x00u}};
}

} // namespace

int main() {
    using namespace katana::io;

    ExecutableImage image("fixture.elf");
    image.add_segment({".data",
                       0x8C020000u,
                       0x140u,
                       0x20u,
                       SegmentKind::Data,
                       {true, true, false},
                       {0x12u, 0x34u}});
    image.add_segment(code_segment());
    image.add_entry_point(0x8C010000u);
    image.add_entry_point(0x8C010000u);

    require(image.source_path() == "fixture.elf", "Die Quelldatei ging verloren.");
    require(image.segments().size() == 2u, "Segmente wurden nicht aufgenommen.");
    require(image.segments()[0].name == ".text", "Segmente sind nicht nach Adresse sortiert.");
    require(image.entry_points().size() == 1u,
            "Doppelte Einstiegspunkte wurden nicht normalisiert.");
    require(image.initial_snapshot_policy() == InitialSnapshotPolicy::ImmutableOnly,
            "Ein allgemeines Image aktivierte implizit beschreibbare Snapshotliterale.");
    image.set_initial_snapshot_policy(InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    require(image.initial_snapshot_policy() ==
                InitialSnapshotPolicy::EntryPointStraightLineQuiescent,
            "Expliziter Anfangssnapshotvertrag ging verloren.");
    require(image.address_model() == ImageAddressModel::Exact &&
                !image.resolve_segment_address(0xAC010002u, 2u).has_value(),
            "Allgemeines Image aktivierte implizit SH-4-Direktsegmente.");
    image.set_address_model(ImageAddressModel::Sh4DirectMapped);
    require(image.resolve_segment_address(0x0C010002u, 2u) == 0x8C010002u &&
                image.resolve_segment_address(0xAC010002u, 2u) == 0x8C010002u &&
                !image.resolve_segment_address(0xE0010002u, 2u).has_value(),
            "SH-4-P0/P1/P2-Codealiase werden nicht auf das Image normalisiert.");

    const auto* code = image.find_segment(0x8C010002u, 2u);
    if (code == nullptr) {
        throw std::runtime_error("Ein gueltiger Codebereich wurde nicht gefunden.");
    }
    require(code->kind == SegmentKind::Code, "Die Segmentklassifikation ging verloren.");
    require(code->permissions.readable && code->permissions.executable &&
                !code->permissions.writable,
            "Codeberechtigungen sind falsch.");
    require(code->file_offset == 0x40u && code->bytes.size() == 4u, "Dateilayout ging verloren.");
    require(code->byte_offset(0x8C010003u) == 3u,
            "Virtuelle Adresse wurde falsch in Dateidaten abgebildet.");
    require(!code->byte_offset(0x8C010004u).has_value(),
            "Zero-Fill-Bereich wurde faelschlich als Dateidaten gemeldet.");
    require(image.find_segment(0x8C0100FFu) == code, "Das letzte Segmentbyte fehlt.");
    require(image.find_segment(0x8C0100FFu, 2u) == nullptr,
            "Ein Zugriff ueber das Segmentende wurde akzeptiert.");
    require(std::string(segment_kind_name(SegmentKind::Unknown)) == "unknown",
            "Unknown-Name ist instabil.");

    ExecutableImage relocated;
    relocated.add_segment(
        {".data", 0x1000u, 0u, 4u, SegmentKind::Data, {true, true, false}, {1u, 2u, 3u, 4u}});
    require(relocated.read_u32_le(0x1000u) == 0x04030201u, "Little-Endian-Lesezugriff ist falsch.");
    relocated.write_u32_le(0x1000u, 0x89ABCDEFu);
    require(relocated.read_u32_le(0x1000u) == 0x89ABCDEFu,
            "Little-Endian-Schreibzugriff ist falsch.");
    relocated.add_relocation(
        {0x1000u, 1u, RelocationKind::Absolute32, "target", 0x2000u, 4, 0x2004u});
    require(relocated.relocations().size() == 1u, "Relocation wurde nicht gespeichert.");
    require(relocated.relocations()[0].applied_value == 0x2004u,
            "Relocation-Ergebnis ging verloren.");
    require(std::string(relocation_kind_name(RelocationKind::PcRelative32)) == "pc-relative32",
            "Relocation-Name ist instabil.");

    ExecutableImage top;
    top.add_segment({"top", 0xFFFFFFFFu, 0u, 1u, SegmentKind::Unknown, {}, {0u}});
    require(top.segments()[0].end_address() == 0x100000000ull,
            "Das obere Adressraumende wurde abgeschnitten.");

    require_throws<std::invalid_argument>(
        [] {
            ExecutableImage invalid;
            invalid.add_segment({"", 0u, 0u, 1u, SegmentKind::Unknown, {}, {0u}});
        },
        "Ein namenloses Segment wurde akzeptiert.");
    require_throws<std::invalid_argument>(
        [] {
            ExecutableImage invalid;
            invalid.add_segment({"short", 0u, 0u, 1u, SegmentKind::Data, {}, {1u, 2u}});
        },
        "Dateidaten groesser als der Speicherbereich wurden akzeptiert.");
    require_throws<std::out_of_range>(
        [] {
            ExecutableImage invalid;
            invalid.add_segment({"wrap", 0xFFFFFFFFu, 0u, 2u, SegmentKind::Data, {}, {}});
        },
        "Ein Segment-Wraparound wurde akzeptiert.");
    require_throws<std::invalid_argument>(
        [] {
            ExecutableImage invalid;
            invalid.add_segment(code_segment());
            invalid.add_segment({"overlap", 0x8C010080u, 0u, 0x20u, SegmentKind::Data, {}, {}});
        },
        "Ueberlappende Segmente wurden akzeptiert.");

    std::cout << "KR-1601 Executable-Image- und Segmentmodell erfolgreich.\n";
    return EXIT_SUCCESS;
}
