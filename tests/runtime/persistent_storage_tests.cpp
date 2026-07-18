#include "katana/runtime/dreamcast_memory.hpp"
#include "katana/runtime/maple.hpp"
#include "katana/runtime/persistent_storage.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

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

void write_file(const std::filesystem::path& path, const std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("Testdatei konnte nicht geschrieben werden.");
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Testdatei konnte nicht gelesen werden.");
    const auto size = input.tellg();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty())
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

void flash_unlock(katana::runtime::Memory& memory) {
    memory.write_u8(0x00200000u + katana::runtime::dreamcast_flash_unlock_address_1, 0xAAu);
    memory.write_u8(0x00200000u + katana::runtime::dreamcast_flash_unlock_address_2, 0x55u);
}

} // namespace

int main() {
    using namespace katana::runtime;
    static_assert(persistent_image_contract_version == 1u);
    const auto root = std::filesystem::temp_directory_path() / "katana-kr4704-persistence";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto source_path = root / "source.bin";
    const auto working_path = root / "working.katana-work";
    std::vector<std::uint8_t> source(128u, 0xFFu);
    source[7u] = 0x5Au;
    write_file(source_path, source);
    const auto source_snapshot = read_file(source_path);

    auto image = PersistentImage::open({"test-image", source_path, working_path, source.size()});
    require(image->recovery() == PersistentImageRecovery::CreatedFromSource &&
                image->read_byte(7u) == 0x5Au && !image->dirty() &&
                std::filesystem::is_regular_file(working_path),
            "Arbeitskopie wird nicht getrennt aus der Quelle erzeugt.");
    image->write_byte(7u, 0xA5u);
    image->save();
    auto recovery_path = working_path;
    recovery_path += ".recovery";
    require(!image->dirty() && image->save_count() == 1u &&
                std::filesystem::is_regular_file(recovery_path) &&
                read_file(source_path) == source_snapshot,
            "Atomischer Save veraendert Quelle oder erzeugt keine Recovery.");

    const auto primary_after_save = read_file(working_path);
    auto loaded = PersistentImage::open({"test-image", source_path, working_path, source.size()});
    require(loaded->recovery() == PersistentImageRecovery::LoadedPrimary &&
                loaded->read_byte(7u) == 0xA5u,
            "Gueltige Primaerarbeitskopie wird nicht reproduzierbar geladen.");

    auto corrupted = primary_after_save;
    corrupted.front() ^= 0xFFu;
    write_file(working_path, corrupted);
    auto restored = PersistentImage::open({"test-image", source_path, working_path, source.size()});
    require(restored->recovery() == PersistentImageRecovery::RestoredRecovery &&
                restored->read_byte(7u) == 0x5Au && read_file(source_path) == source_snapshot,
            "Defekte Primaerdatei wird nicht aus validierter Recovery restauriert.");

    std::filesystem::remove(working_path);
    restored = PersistentImage::open({"test-image", source_path, working_path, source.size()});
    require(restored->recovery() == PersistentImageRecovery::RestoredRecovery &&
                read_file(source_path) == source_snapshot,
            "Fehlende Primaerdatei verwendet keine gueltige Recovery.");

    write_file(working_path, corrupted);
    write_file(recovery_path, corrupted);
    require(throws<std::runtime_error>([&] {
                static_cast<void>(PersistentImage::open(
                    {"test-image", source_path, working_path, source.size()}));
            }) &&
                read_file(source_path) == source_snapshot,
            "Zwei defekte Kopien werden akzeptiert oder veraendern die Quelle.");

    std::filesystem::remove(working_path);
    std::filesystem::remove(recovery_path);
    image = PersistentImage::open({"test-image", source_path, working_path, source.size()});
    image->write_byte(9u, 0x11u);
    image->save();
    std::filesystem::remove(recovery_path);
    std::filesystem::create_directories(recovery_path / "occupied");
    image->write_byte(10u, 0x22u);
    require(throws<std::runtime_error>([&] { image->save(); }) && image->dirty() &&
                read_file(working_path) != std::vector<std::uint8_t>{} &&
                read_file(source_path) == source_snapshot,
            "Publishfehler verliert Dirty-Zustand, Primaerkopie oder Quellschutz.");
    std::filesystem::remove_all(recovery_path);
    const auto reloaded_after_failure =
        PersistentImage::open({"test-image", source_path, working_path, source.size()});
    require(reloaded_after_failure->read_byte(9u) == 0x11u &&
                reloaded_after_failure->read_byte(10u) == 0xFFu,
            "Publishfehler veraendert die letzte gueltige Primaerkopie.");

    require(throws<std::invalid_argument>([&] {
                static_cast<void>(
                    PersistentImage::open({"same-file", source_path, source_path, source.size()}));
            }),
            "Identische Quell- und Arbeitsdatei wird akzeptiert.");

    const auto flash_working = root / "flash.katana-work";
    auto flash_image = PersistentImage::open(
        {"dreamcast-flash", std::nullopt, flash_working, dreamcast_flash_size, 0xFFu});
    Memory memory(0u);
    const auto flash = map_dreamcast_command_flash(memory, flash_image);
    flash_unlock(memory);
    memory.write_u8(0x00200000u + dreamcast_flash_unlock_address_1, 0xA0u);
    memory.write_u8(0x00200020u, 0x7Fu);
    require(flash->persistent_working_copy() && flash->working_copy_dirty() &&
                flash->source_byte(0x20u) == 0xFFu,
            "Command-Flash schreibt nicht ausschliesslich in die persistente Arbeitskopie.");
    flash->save_working_copy();
    const auto flash_reload = PersistentImage::open(
        {"dreamcast-flash", std::nullopt, flash_working, dreamcast_flash_size, 0xFFu});
    require(flash_reload->read_byte(0x20u) == 0x7Fu,
            "Persistentes Command-Flash wird nach Save nicht wieder geladen.");

    const auto vmu_working = root / "vmu.katana-work";
    auto vmu_image = PersistentImage::open(
        {"dreamcast-vmu", std::nullopt, vmu_working, vmu_storage_size, 0xFFu});
    MapleVmuDevice vmu(vmu_image);
    std::vector<std::uint32_t> payload(1u + vmu_block_size / 4u, 0xA5A5A5A5u);
    payload[0] = 1u;
    require(vmu.transact({MapleCommand::BlockWrite, payload}).code == MapleResponseCode::Ack &&
                vmu.persistent_working_copy() && vmu.working_copy_dirty(),
            "VMU-Blockwrite markiert die persistente Arbeitskopie nicht dirty.");
    vmu.save_working_copy();
    const auto vmu_reload = PersistentImage::open(
        {"dreamcast-vmu", std::nullopt, vmu_working, vmu_storage_size, 0xFFu});
    require(vmu_reload->read_byte(vmu_block_size) == 0xA5u,
            "Persistente VMU wird nach Save nicht wieder geladen.");

    const auto status = image->serialize_status_json();
    require(status.find("katana-persistent-image-v1") != std::string::npos &&
                status.find(root.string()) == std::string::npos &&
                status.find("sha256") == std::string::npos,
            "Arbeitskopiendiagnose enthaelt Pfade oder Hashidentitaeten.");

    std::filesystem::remove_all(root);
    std::cout << "KR-4703 persistente Arbeitskopien und Recovery erfolgreich.\n";
}
