#include "katana/codegen/latent_aot_registry.hpp"

#include "katana/analysis/abi.hpp"
#include "katana/codegen/cache.hpp"
#include "katana/codegen/latent_aot_analysis_cache.hpp"
#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t sector_size = 2048u;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void both32(std::vector<std::uint8_t>& image,
            const std::size_t offset,
            const std::uint32_t value) {
    for (std::size_t byte = 0u; byte < 4u; ++byte) {
        image[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
        image[offset + 4u + byte] =
            static_cast<std::uint8_t>(value >> ((3u - byte) * 8u));
    }
}

std::size_t record(std::vector<std::uint8_t>& image,
                   const std::size_t offset,
                   const std::uint32_t lba,
                   const std::uint32_t size,
                   const std::string& name,
                   const bool directory) {
    const auto length =
        static_cast<std::uint8_t>(33u + name.size() + (name.size() % 2u == 0u ? 1u : 0u));
    image[offset] = length;
    both32(image, offset + 2u, lba);
    both32(image, offset + 10u, size);
    image[offset + 25u] = directory ? 0x02u : 0u;
    image[offset + 28u] = 1u;
    image[offset + 31u] = 1u;
    image[offset + 32u] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(),
              name.end(),
              image.begin() + static_cast<std::ptrdiff_t>(offset + 33u));
    return length;
}

struct FixtureFile {
    std::uint32_t lba = 0u;
    std::string name;
    std::vector<std::uint8_t> bytes;
};

std::vector<std::uint8_t>
fixture_iso_with_files(const std::vector<FixtureFile>& files) {
    std::size_t image_sectors = 24u;
    for (const auto& file : files) {
        image_sectors = std::max(
            image_sectors,
            static_cast<std::size_t>(file.lba) + 1u);
    }
    std::vector<std::uint8_t> image(image_sectors * sector_size);
    const auto pvd = 16u * sector_size;
    image[pvd] = 1u;
    std::copy_n("CD001", 5u, image.begin() + static_cast<std::ptrdiff_t>(pvd + 1u));
    image[pvd + 6u] = 1u;
    static_cast<void>(
        record(image, pvd + 156u, 20u, sector_size, std::string(1u, '\0'), true));

    auto directory = 20u * sector_size;
    directory +=
        record(image, directory, 20u, sector_size, std::string(1u, '\0'), true);
    directory +=
        record(image, directory, 20u, sector_size, std::string(1u, '\1'), true);
    for (const auto& file : files) {
        directory += record(image,
                            directory,
                            file.lba,
                            static_cast<std::uint32_t>(file.bytes.size()),
                            file.name,
                            false);
        std::copy(file.bytes.begin(),
                  file.bytes.end(),
                  image.begin() +
                      static_cast<std::ptrdiff_t>(file.lba * sector_size));
    }
    return image;
}

std::vector<std::uint8_t> fixture_iso() {
    const std::vector<std::uint8_t> module_bytes{
        0x0Bu, 0x00u, 0x09u, 0x00u};
    return fixture_iso_with_files(
        {{21u, "MODULE.BIN;1", module_bytes},
         {22u, "COPY.BIN;1", module_bytes},
         {23u, "DATA.DAT;1", {0xFFu, 0xFFu, 0xFFu, 0xFFu}}});
}

std::vector<std::uint8_t> local_descriptor_module(
    const bool include_pointer_producer,
    const std::uint32_t descriptor_stride,
    const std::size_t callback_target_count) {
    require(descriptor_stride == 44u || descriptor_stride == 48u,
            "Lokale Descriptor-Fixture erhielt einen unerwarteten Stride.");
    require(callback_target_count >= 1u && callback_target_count <= 2u,
            "Lokale Descriptor-Fixture erhielt eine ungueltige Zielzahl.");
    constexpr std::uint32_t runtime_base = 0x8C900000u;
    std::vector<std::uint8_t> bytes(0x400u, 0u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };

    // Entry -> producer(r4 object) -> consumer(r4 object, r5 mutable index).
    put_u16(0x00u, 0xB01Eu); // bsr 0x40
    put_u16(0x02u, 0x0009u); // nop (delay)
    put_u16(0x04u, 0xB03Cu); // bsr 0x80
    put_u16(0x06u, 0x0009u); // nop (delay)
    put_u16(0x08u, 0x000Bu); // rts
    put_u16(0x0Au, 0x0009u); // nop (delay)

    // Store the local table pointer into incoming object field +68.
    put_u16(0x40u, 0xD107u); // mov.l @(0x60,pc),r1
    put_u16(0x42u, 0xE044u); // mov #68,r0
    put_u16(0x44u,
            include_pointer_producer ? 0x0416u : 0x0009u); // mov.l r1,@(r0,r4)
    put_u16(0x46u, 0x000Bu); // rts
    put_u16(0x48u, 0x0009u); // nop (delay)
    put_u32(0x60u, runtime_base + 0x200u);

    // Form the proven descriptor address from one mutable index and the
    // persisted object field, then load callback +28.  The positive sequence
    // mirrors the bounded 3*16 shape used by the title descriptor walker.
    if (descriptor_stride == 48u) {
        put_u16(0x80u, 0x6353u); // mov r5,r3
        put_u16(0x82u, 0x4300u); // shll r3 -> 2*i
        put_u16(0x84u, 0x6253u); // mov r5,r2
        put_u16(0x86u, 0x332Cu); // add r2,r3 -> 3*i
        put_u16(0x88u, 0x4308u); // shll2 r3 -> 12*i
        put_u16(0x8Au, 0x4308u); // shll2 r3 -> 48*i
        put_u16(0x8Cu, 0xE044u); // mov #68,r0
        put_u16(0x8Eu, 0x014Eu); // mov.l @(r0,r4),r1
        put_u16(0x90u, 0x331Cu); // add r1,r3
        put_u16(0x92u, 0x5237u); // mov.l @(28,r3),r2
        put_u16(0x94u, 0x420Bu); // jsr @r2
        put_u16(0x96u, 0x0009u); // nop (delay)
        put_u16(0x98u, 0x000Bu); // rts
        put_u16(0x9Au, 0x0009u); // nop (delay)
    } else {
        put_u16(0x80u, 0x6253u); // mov r5,r2
        put_u16(0x82u, 0x4228u); // shll2 r2 -> 4*i
        put_u16(0x84u, 0x6323u); // mov r2,r3
        put_u16(0x86u, 0x4220u); // shll r2 -> 8*i
        put_u16(0x88u, 0x4220u); // shll r2 -> 16*i
        put_u16(0x8Au, 0x4220u); // shll r2 -> 32*i
        put_u16(0x8Cu, 0x323Cu); // add r3,r2 -> 36*i
        put_u16(0x8Eu, 0x323Cu); // add r3,r2 -> 40*i
        put_u16(0x90u, 0x323Cu); // add r3,r2 -> 44*i
        put_u16(0x92u, 0xE044u); // mov #68,r0
        put_u16(0x94u, 0x014Eu); // mov.l @(r0,r4),r1
        put_u16(0x96u, 0x312Cu); // add r2,r1
        put_u16(0x98u, 0xE01Cu); // mov #28,r0
        put_u16(0x9Au, 0x031Eu); // mov.l @(r0,r1),r3
        put_u16(0x9Cu, 0x430Bu); // jsr @r3
        put_u16(0x9Eu, 0x0009u); // nop (delay)
        put_u16(0xA0u, 0x000Bu); // rts
        put_u16(0xA2u, 0x0009u); // nop (delay)
    }

    put_u16(0x100u, 0x000Bu);
    put_u16(0x102u, 0x0009u);
    put_u16(0x120u, 0x000Bu);
    put_u16(0x122u, 0x0009u);
    put_u32(0x200u + 28u, runtime_base + 0x100u);
    put_u32(0x230u + 28u,
            callback_target_count == 2u ? runtime_base + 0x120u
                                         : 0xDEADBEEFu);
    put_u32(0x260u + 28u, 0xDEADBEEFu);

    // Two independent, already reachable function pointers bind the runtime
    // alias without making either descriptor callback a pre-existing root.
    put_u32(0x300u, runtime_base + 0x40u);
    put_u32(0x304u, runtime_base + 0x80u);
    return bytes;
}

std::vector<std::uint8_t> unrelated_record_field_module(
    const bool complete_callback) {
    constexpr std::uint32_t runtime_base = 0x8C900000u;
    std::vector<std::uint8_t> bytes(complete_callback ? 0xA10u : 0x140u,
                                    0u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };

    // An unrelated external factory returns a receiver in r0. The module
    // stores a local pointer at receiver+44, which deliberately shares only
    // the field displacement of an external callback consumer.
    put_u16(0x00u, 0xD307u); // mov.l @(0x20,pc),r3
    put_u16(0x02u, 0x430Bu); // jsr @r3
    put_u16(0x04u, 0x0009u); // nop (delay)
    put_u16(0x06u, 0x6B03u); // mov r0,r11
    put_u16(0x08u, 0xD306u); // mov.l @(0x24,pc),r3
    put_u16(0x0Au, 0x1B3Bu); // mov.l r3,@(44,r11)
    put_u16(0x0Cu, 0x000Bu); // rts
    put_u16(0x0Eu, 0x0009u); // nop (delay)
    put_u32(0x20u, 0x8C018120u);
    put_u32(0x24u, runtime_base + 0x100u);

    if (!complete_callback) {
        // The negative shape starts with a plausible BSR+delay pair but its
        // call continuation is data. The old early-control-flow probe accepted
        // it.
        put_u16(0x100u, 0xB00Eu); // bsr 0x120
        put_u16(0x102u, 0x0009u); // nop (delay)
        put_u16(0x104u, 0xFFFFu); // invalid continuation
        put_u16(0x106u, 0x0009u);
        put_u16(0x120u, 0x000Bu);
        put_u16(0x122u, 0x0009u);
    } else {
        // A complete callback wrapper may tail-transfer into a large shared
        // body. The wrapper proof must validate the tail entry without
        // charging the shared body's full extent to its 1024-instruction
        // budget. The full CFA still owns and validates that body after the
        // wrapper is admitted.
        put_u16(0x100u, 0xA07Eu); // bra 0x200
        put_u16(0x102u, 0x0009u); // nop (delay)
        put_u16(0x1F0u, 0x000Bu); // short conditional exit
        put_u16(0x1F2u, 0x0009u); // nop (delay)
        put_u16(0x200u, 0x89F6u); // bt 0x1F0
        for (std::size_t offset = 0x202u; offset < 0xA04u;
             offset += 2u)
            put_u16(offset, 0x0009u); // bounded shared body
        put_u16(0xA04u, 0x000Bu);
        put_u16(0xA06u, 0x0009u);
    }
    return bytes;
}

std::vector<std::uint8_t> direct_record_field_module(
    const bool complete_callback,
    const std::uint8_t receiver_register = 4u) {
    require(receiver_register >= 4u && receiver_register <= 7u,
            "Direkte Record-Fixture erhielt einen ungueltigen ABI-Receiver.");
    constexpr std::uint32_t runtime_base = 0x8C900000u;
    std::vector<std::uint8_t> bytes(0x340u, 0u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };

    // Entry -> direct producer.  The producer receives the callback record in
    // the canonical incoming ABI argument; no external factory return is
    // involved.
    put_u16(0x00u, 0xB01Eu); // bsr 0x40
    put_u16(0x02u, 0x0009u); // nop (delay)
    put_u16(0x04u, 0x000Bu); // rts
    put_u16(0x06u, 0x0009u); // nop (delay)

    put_u16(0x40u, 0xD307u); // mov.l @(0x60,pc),r3
    // mov.l r3,@(16,rN) is the callback-record field store.  Production
    // admission only seeds canonical r4; r5..r7 exercise the negative lane.
    const auto store_opcode = static_cast<std::uint16_t>(
        0x1000u | (static_cast<std::uint16_t>(receiver_register) << 8u) |
        (3u << 4u) | 4u);
    put_u16(0x42u, complete_callback ? store_opcode : 0x0009u);
    put_u16(0x44u, 0x000Bu); // rts
    put_u16(0x46u, 0x0009u); // nop (delay)
    put_u32(0x60u, runtime_base + 0x100u);

    // A valid local callback entry has an early architectural terminator.
    put_u16(0x100u, 0x000Bu); // rts
    put_u16(0x102u, 0x0009u); // nop (delay)

    // Two distinct runtime aliases establish the module load base without a
    // title-specific address or a runtime fallback.
    put_u32(0x300u, runtime_base + 0x40u);
    put_u32(0x304u, runtime_base + 0x100u);
    return bytes;
}

std::vector<std::uint8_t> published_record_field_module() {
    constexpr std::uint32_t runtime_base = 0x8C900000u;
    constexpr std::uint32_t persistent_pointer_sink = 0x8C030000u;
    std::vector<std::uint8_t> bytes(0x340u, 0u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };

    // The authoritative root calls a local producer with an ordinary r5
    // value.  This deliberately kills the root's canonical incoming-r4
    // record marker before the call, so only the publication proof below can
    // establish the producer's receiver.
    put_u16(0x00u, 0x6453u); // mov r5,r4
    put_u16(0x02u, 0xB01Du); // bsr 0x40
    put_u16(0x04u, 0x0009u); // nop (delay)
    put_u16(0x06u, 0x000Bu); // rts
    put_u16(0x08u, 0x0009u); // nop (delay)

    // The producer preserves its input record in r12, publishes that exact
    // alias as r5 to a separately proven persistent-pointer sink, then writes
    // a local callback literal into the independently proven +16 field.
    put_u16(0x40u, 0x6C43u); // mov r4,r12
    put_u16(0x42u, 0xD30Fu); // mov.l @(0x80,pc),r3
    put_u16(0x44u, 0x430Bu); // jsr @r3
    put_u16(0x46u, 0x65C3u); // mov r12,r5 (delay)
    put_u16(0x48u, 0xD20Eu); // mov.l @(0x84,pc),r2
    put_u16(0x4Au, 0x1C24u); // mov.l r2,@(16,r12)
    put_u16(0x4Cu, 0x000Bu); // rts
    put_u16(0x4Eu, 0x0009u); // nop (delay)
    put_u32(0x80u, persistent_pointer_sink);
    put_u32(0x84u, runtime_base + 0x100u);

    put_u16(0x100u, 0x000Bu); // callback: rts
    put_u16(0x102u, 0x0009u); // delay slot

    // Two exact aliases establish the preferred runtime base without a
    // title-specific address or runtime observation.
    put_u32(0x300u, runtime_base + 0x40u);
    put_u32(0x304u, runtime_base + 0x100u);
    return bytes;
}

std::vector<std::uint8_t> registered_record_callback_module() {
    constexpr std::uint32_t runtime_base = 0x8C900000u;
    constexpr std::uint32_t registrar = 0x8C040000u;
    std::vector<std::uint8_t> bytes(0x340u, 0u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };

    // The authoritative module root registers a local initializer as r6.
    // The primary-image contract independently proves that this callback is
    // later invoked with its persistent record in r4.
    put_u16(0x00u, 0xD307u); // mov.l @(0x20,pc),r3
    put_u16(0x02u, 0xD608u); // mov.l @(0x24,pc),r6
    put_u16(0x04u, 0x430Bu); // jsr @r3
    put_u16(0x06u, 0xE402u); // mov #2,r4 (delay)
    put_u16(0x08u, 0x000Bu); // rts
    put_u16(0x0Au, 0x0009u); // nop (delay)
    put_u32(0x20u, registrar);
    put_u32(0x24u, runtime_base + 0x40u);

    // The initializer receives the record in r4, preserves it in r11 and
    // publishes a second local callback through the proven +16 field.
    put_u16(0x40u, 0x6B43u); // mov r4,r11
    put_u16(0x42u, 0xD20Fu); // mov.l @(0x80,pc),r2
    put_u16(0x44u, 0x1B24u); // mov.l r2,@(16,r11)
    put_u16(0x46u, 0x000Bu); // rts
    put_u16(0x48u, 0x0009u); // nop (delay)
    put_u32(0x80u, runtime_base + 0x100u);

    put_u16(0x100u, 0x000Bu); // second callback: rts
    put_u16(0x102u, 0x0009u); // delay slot

    put_u32(0x300u, runtime_base + 0x40u);
    put_u32(0x304u, runtime_base + 0x100u);
    return bytes;
}

std::vector<std::uint8_t> mutual_record_table_module(
    const std::size_t record_count = 3u,
    const bool constant_stride = true,
    const bool matching_backreference = true) {
    constexpr std::uint32_t runtime_base = 0x8C900000u;
    std::vector<std::uint8_t> bytes(0x400u, 0u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };

    put_u16(0x00u, 0x000Bu); // authoritative root: rts
    put_u16(0x02u, 0x0009u); // delay slot

    // The missing callback identifies its record family in its bounded entry
    // prefix. At 0x100, D207 reads the literal at 0x120.
    put_u16(0x100u, 0xD207u); // mov.l @(28,pc),r2
    put_u16(0x102u, 0x000Bu); // rts
    put_u16(0x104u, 0x0009u); // delay slot
    put_u32(0x120u,
            runtime_base + (matching_backreference ? 0x200u : 0x204u));

    constexpr std::array record_bases{0x200u, 0x228u, 0x250u};
    for (std::size_t index = 0u;
         index < std::min(record_count, record_bases.size()); ++index) {
        auto base = record_bases[index];
        if (!constant_stride && index == 2u) base += 4u;
        put_u32(base + 0x10u, runtime_base + 0x100u);
    }
    return bytes;
}

std::string byte_identity(const std::vector<std::uint8_t>& bytes) {
    return "sha256:" + katana::io::sha256_bytes(std::string_view(
                           reinterpret_cast<const char*>(bytes.data()),
                           bytes.size()));
}

struct AnalysisCacheFixture {
    std::filesystem::path path;

    explicit AnalysisCacheFixture(
        const std::string_view suffix = {})
        : path(
              std::filesystem::current_path() /
              ("katana-latent-aot-analysis-cache-fixture" +
               std::string(suffix))) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    ~AnalysisCacheFixture() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    [[nodiscard]] std::size_t corrupt_all_artifacts() const {
        std::size_t count = 0u;
        if (!std::filesystem::exists(path))
            return count;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(path)) {
            if (!entry.is_regular_file() ||
                entry.path().filename() != "module-analysis.bin")
                continue;
            std::ofstream output(
                entry.path(),
                std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error(
                    "Analysecache-Fixture konnte nicht korrumpiert werden.");
            output << "corrupt";
            output.close();
            if (!output)
                throw std::runtime_error(
                    "Analysecache-Fixture wurde nicht vollstaendig korrumpiert.");
            ++count;
        }
        return count;
    }

    [[nodiscard]] bool replace_positive_with_source_mismatch(
        const std::string& positive_key) const {
        if (!std::filesystem::exists(path))
            return false;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(path)) {
            if (!entry.is_regular_file() ||
                entry.path().filename() != "module-analysis.bin")
                continue;
            std::ifstream input(entry.path(), std::ios::binary);
            const std::vector<std::uint8_t> artifact{
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
            if (input.bad() || artifact.empty())
                throw std::runtime_error(
                    "Analysecache-Fixture konnte ein Artefakt nicht lesen.");
            auto parsed =
                katana::codegen::parse_latent_aot_analysis_cache(
                    positive_key, artifact);
            if (parsed.state !=
                katana::codegen::LatentAotAnalysisCacheState::Positive)
                continue;
            auto& instruction =
                parsed.program.front().blocks.front().instructions.front();
            // Keep the graph and IR structurally valid while making its
            // claimed delayed opcode disagree with the exact RTS bytes. BRA
            // retains an Owner role, so the ordinary IR verifier still accepts
            // this checksum-consistent foreign payload.
            instruction.original_opcode = 0xA000u;
            const auto replaced =
                katana::codegen::serialize_latent_aot_positive_cache(
                    positive_key, parsed.program);
            std::ofstream output(
                entry.path(),
                std::ios::binary | std::ios::trunc);
            output.write(
                reinterpret_cast<const char*>(replaced.data()),
                static_cast<std::streamsize>(replaced.size()));
            output.close();
            if (!output)
                throw std::runtime_error(
                    "Analysecache-Fixture konnte Fremd-IR nicht schreiben.");
            return true;
        }
        return false;
    }

    [[nodiscard]] bool replace_positive_with_instruction_gap(
        const std::string& positive_key) const {
        if (!std::filesystem::exists(path))
            return false;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(path)) {
            if (!entry.is_regular_file() ||
                entry.path().filename() != "module-analysis.bin")
                continue;
            std::ifstream input(entry.path(), std::ios::binary);
            const std::vector<std::uint8_t> artifact{
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
            if (input.bad() || artifact.empty())
                throw std::runtime_error(
                    "Analysecache-Fixture konnte ein Artefakt nicht lesen.");
            auto parsed =
                katana::codegen::parse_latent_aot_analysis_cache(
                    positive_key, artifact);
            if (parsed.state !=
                katana::codegen::LatentAotAnalysisCacheState::Positive)
                continue;
            auto& instructions =
                parsed.program.front().blocks.front().instructions;
            if (instructions.size() < 4u)
                continue;
            // Every retained instruction still decodes exactly from the
            // current bytes and the ordinary verifier accepts the delayed
            // return pair. The missing middle NOP nevertheless makes this an
            // impossible basic block and must invalidate the untrusted hit.
            instructions.erase(instructions.begin() + 1);
            const auto replaced =
                katana::codegen::serialize_latent_aot_positive_cache(
                    positive_key, parsed.program);
            std::ofstream output(
                entry.path(),
                std::ios::binary | std::ios::trunc);
            output.write(
                reinterpret_cast<const char*>(replaced.data()),
                static_cast<std::streamsize>(replaced.size()));
            output.close();
            if (!output)
                throw std::runtime_error(
                    "Analysecache-Fixture konnte Gap-IR nicht schreiben.");
            return true;
        }
        return false;
    }

    [[nodiscard]] bool replace_positive_with_negative(
        const std::string& positive_key) const {
        if (!std::filesystem::exists(path))
            return false;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(path)) {
            if (!entry.is_regular_file() ||
                entry.path().filename() != "module-analysis.bin")
                continue;
            std::ifstream input(entry.path(), std::ios::binary);
            const std::vector<std::uint8_t> artifact{
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
            if (input.bad() || artifact.empty())
                throw std::runtime_error(
                    "Analysecache-Fixture konnte ein Artefakt nicht lesen.");
            const auto parsed =
                katana::codegen::parse_latent_aot_analysis_cache(
                    positive_key, artifact);
            if (parsed.state !=
                katana::codegen::LatentAotAnalysisCacheState::Positive)
                continue;
            const auto replaced =
                katana::codegen::serialize_latent_aot_negative_cache(
                    positive_key,
                    katana::codegen::LatentAotAnalysisRejection::
                        ProgramInvalid);
            std::ofstream output(
                entry.path(),
                std::ios::binary | std::ios::trunc);
            output.write(
                reinterpret_cast<const char*>(replaced.data()),
                static_cast<std::streamsize>(replaced.size()));
            output.close();
            if (!output)
                throw std::runtime_error(
                    "Analysecache-Fixture konnte keinen negativen "
                    "Eintrag schreiben.");
            return true;
        }
        return false;
    }

    [[nodiscard]] bool replace_positive_with_forged_dynamic_target(
        const std::string& positive_key) const {
        if (!std::filesystem::exists(path))
            return false;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(path)) {
            if (!entry.is_regular_file() ||
                entry.path().filename() != "module-analysis.bin")
                continue;
            std::ifstream input(entry.path(), std::ios::binary);
            const std::vector<std::uint8_t> artifact{
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
            if (input.bad() || artifact.empty())
                throw std::runtime_error(
                    "Analysecache-Fixture konnte ein Artefakt nicht lesen.");
            auto parsed =
                katana::codegen::parse_latent_aot_analysis_cache(
                    positive_key, artifact);
            if (parsed.state !=
                katana::codegen::LatentAotAnalysisCacheState::Positive)
                continue;
            auto& block =
                parsed.program.front().blocks.front();
            auto& instruction = block.instructions.front();
            if (instruction.operation !=
                katana::ir::Operation::JumpRegister)
                continue;
            instruction.resolved_targets = {
                block.start_address};
            instruction.dynamic_target_class =
                katana::ir::DynamicTargetClass::GuardedComplete;
            block.successors = {block.start_address};
            block.has_indirect_successor = false;
            const auto replaced =
                katana::codegen::serialize_latent_aot_positive_cache(
                    positive_key, parsed.program);
            std::ofstream output(
                entry.path(),
                std::ios::binary | std::ios::trunc);
            output.write(
                reinterpret_cast<const char*>(replaced.data()),
                static_cast<std::streamsize>(replaced.size()));
            output.close();
            if (!output)
                throw std::runtime_error(
                    "Analysecache-Fixture konnte dynamisches Fremdziel "
                    "nicht schreiben.");
            return true;
        }
        return false;
    }
};

std::string analysis_cache_key_for_module(
    const katana::codegen::PreparedLatentAotModule& module,
    const katana::codegen::LatentAotDiscoveryOptions& options,
    const bool exact_candidate = false) {
    katana::codegen::LatentAotAnalysisCacheKeyInputs inputs;
    inputs.byte_sha256 = module.byte_identity.substr(7u);
    inputs.byte_size = module.byte_size;
    inputs.entry_offsets = module.entry_offsets;
    inputs.exact_candidate = exact_candidate;
    inputs.source_address = module.source_address;
    inputs.maximum_entry_scan_instructions =
        options.maximum_entry_scan_instructions;
    inputs.maximum_native_instructions =
        options.maximum_native_instructions_per_module;
    inputs.maximum_blocks = options.maximum_blocks_per_module;
    inputs.maximum_functions = options.maximum_functions_per_module;
    inputs.maximum_analysis_iterations =
        options.maximum_analysis_iterations;
    inputs.maximum_analysis_contexts =
        options.maximum_analysis_contexts;
    inputs.analyzer_abi = katana::analysis::abi_version;
    std::ostringstream external_contract;
    const std::string_view cache_implementation_identity =
        !options.analysis_cache_implementation_identity.empty()
            ? std::string_view{options.analysis_cache_implementation_identity}
            : std::string_view{options.analysis_implementation_identity};
    const std::string_view product_implementation_identity =
        !options.ir_product_implementation_identity.empty()
            ? std::string_view{options.ir_product_implementation_identity}
            : cache_implementation_identity;
    external_contract << 's' << cache_implementation_identity.size() << ':'
                      << cache_implementation_identity << ';' << 'o'
                      << product_implementation_identity.size() << ':'
                      << product_implementation_identity << ';' << 't'
                      << options.external_code_targets.size() << ';';
    for (const auto target : options.external_code_targets)
        external_contract << target << ';';
    if (!options.external_data_targets.empty()) {
        external_contract << 'd' << options.external_data_targets.size()
                          << ';';
        for (const auto target : options.external_data_targets)
            external_contract << target << ';';
    }
    external_contract << 'c' << options.external_callback_sinks.size() << ';';
    for (const auto& sink : options.external_callback_sinks)
        external_contract << sink.function_address << ':'
                          << +sink.argument_mask << ':'
                          << +sink.record_argument_mask << ';';
    external_contract << 'p'
                      << options.external_persistent_pointer_sinks.size()
                      << ';';
    for (const auto& sink : options.external_persistent_pointer_sinks)
        external_contract << sink.function_address << ':'
                          << +sink.argument_mask << ';';
    external_contract << 'f'
                      << options.external_callback_field_sinks.size() << ';';
    for (const auto& sink : options.external_callback_field_sinks)
        external_contract << sink.function_address << ':'
                          << sink.call_instruction_address << ':'
                          << sink.load_instruction_address << ':'
                          << sink.displacement << ':' << +sink.width << ':'
                          << sink.call << ':'
                          << +sink.receiver_argument_mask << ';';
    if (!options.external_callback_record_tables.empty()) {
        external_contract << 'r'
                          << options.external_callback_record_tables.size()
                          << ';';
        for (const auto& table : options.external_callback_record_tables)
            external_contract << table.function_address << ':'
                              << table.call_instruction_address << ':'
                              << table.callback_load_instruction_address
                              << ':' << table.callback_sink_address << ':'
                              << table.header_table_pointer_displacement
                              << ':' << table.record_stride << ':'
                              << table.callback_displacement << ':'
                              << +table.callback_argument << ':'
                              << +table.width << ';';
    }
    inputs.analyzer_implementation_id =
        std::string(
            katana::codegen::latent_aot_analysis_implementation_id) +
        "-" +
        katana::io::sha256_bytes(external_contract.str());
    return katana::codegen::make_latent_aot_analysis_cache_key(inputs);
}

} // namespace

int main() {
    try {
        constexpr std::uint32_t relocation_base = 0x89000000u;
        katana::ir::Instruction mova;
        mova.source_address = relocation_base;
        mova.operation = katana::ir::Operation::MoveAddressPcRelative;
        mova.effective_address = relocation_base + 12u;
        katana::ir::Instruction branch;
        branch.source_address = relocation_base + 2u;
        branch.operation = katana::ir::Operation::Branch;
        branch.target_address = relocation_base + 8u;
        katana::ir::BasicBlock relocation_block;
        relocation_block.start_address = relocation_base;
        relocation_block.instructions = {mova, branch};
        relocation_block.successors = {relocation_base + 8u};
        katana::ir::Function relocation_function;
        relocation_function.entry_address = relocation_base;
        relocation_function.blocks = {relocation_block};
        relocation_function.direct_callees = {relocation_base + 8u};
        const std::array relocation_program{relocation_function};
        require(katana::codegen::latent_aot_program_is_relocation_closed(
                    relocation_program, relocation_base, 16u),
                "Interne MOVA-/Branchadressen wurden nicht als relocation-closed erkannt.");
        auto external_target = relocation_program;
        external_target[0].blocks[0].instructions[1].target_address =
            relocation_base + 16u;
        require(!katana::codegen::latent_aot_program_is_relocation_closed(
                    external_target, relocation_base, 16u),
                "Externes direktes Sprungziel wurde an synthetischer Basis akzeptiert.");
        auto external_pc_relative = relocation_program;
        external_pc_relative[0].blocks[0].instructions[0].effective_address =
            relocation_base + 16u;
        require(!katana::codegen::latent_aot_program_is_relocation_closed(
                    external_pc_relative, relocation_base, 16u),
                "Externe PC-relative/MOVA-Adresse wurde an synthetischer Basis akzeptiert.");

        auto source = std::make_shared<katana::runtime::MemoryDiscSource>(
            fixture_iso(), "synthetic-latent-aot-disc");
        const auto discovered =
            katana::codegen::discover_latent_aot_modules(source, 0u, 0u);
        require(discovered.examined_files == 3u && discovered.duplicate_files == 1u &&
                    discovered.rejected_files == 1u && discovered.modules.size() == 1u,
                "Deterministische Discdatei-Discovery klassifizierte die Fixture falsch.");
        const auto& module = discovered.modules.front();
        require(module.source_bindings.size() == 2u &&
                    module.source_bindings[0].disc_byte_offset ==
                        21u * sector_size &&
                    module.source_bindings[1].disc_byte_offset ==
                        22u * sector_size &&
                    module.source_bindings[0].byte_size == 4u &&
                    module.source_bindings[1].byte_size == 4u &&
                    module.source_bindings[0].id.starts_with(
                        "latent-aot-source-") &&
                    module.source_bindings[1].id.starts_with(
                        "latent-aot-source-") &&
                    module.byte_size == 4u &&
                    module.source_address == 0x88000000u && !module.program.empty() &&
                    module.id.starts_with("latent-aot-") &&
                    module.byte_identity.starts_with("sha256:") &&
                    module.id.find("MODULE") == std::string::npos &&
                    module.id.find("COPY") == std::string::npos &&
                    module.id.find("DATA") == std::string::npos,
                "Latente Registry verlor Offset/AOT oder exportierte einen Discdateinamen.");
        require(
            module.block_identities.size() == 1u &&
                module.block_identities.front().source_offset == 0u &&
                module.block_identities.front().size == 4u &&
                module.block_identities.front().sha256 ==
                    module.byte_identity,
            "Latente Registry exportierte nicht die exakte, sortierte "
            "DispatchBlock-Identitaet.");

        const auto discover_local_descriptor =
            [](const std::vector<std::uint8_t>& bytes,
               const std::string& identity) {
                auto disc = std::make_shared<
                    katana::runtime::MemoryDiscSource>(
                    fixture_iso_with_files(
                        {{21u, "DESCRIPTOR.BIN;1", bytes}}),
                    identity);
                return katana::codegen::discover_latent_aot_modules(
                    disc, 0u, 0u);
            };
        const auto has_dispatch_entry = [](const auto& result,
                                           const std::uint32_t offset) {
            if (result.modules.size() != 1u) return false;
            const auto& module = result.modules.front();
            const auto address = module.source_address + offset;
            const bool has_block = std::any_of(
                module.program.begin(), module.program.end(),
                [&](const auto& function) {
                    return std::any_of(
                        function.blocks.begin(), function.blocks.end(),
                        [&](const auto& block) {
                            return block.start_address == address;
                        });
                });
            const bool has_identity = std::any_of(
                module.block_identities.begin(), module.block_identities.end(),
                [&](const auto& identity) {
                    return identity.source_offset == offset;
                });
            return has_block && has_identity;
        };
        const auto local_descriptor_positive = discover_local_descriptor(
            local_descriptor_module(true, 48u, 2u),
            "synthetic-latent-local-descriptor-positive");
        require(
            local_descriptor_positive.modules.size() == 1u &&
                has_dispatch_entry(local_descriptor_positive, 0x100u) &&
                has_dispatch_entry(local_descriptor_positive, 0x120u),
            "Lokale persistierte 48-Byte-Deskriptortabelle wurde nicht als "
            "gebundene Callback-Rootfamilie materialisiert.");

        const auto local_descriptor_without_producer =
            discover_local_descriptor(
                local_descriptor_module(false, 48u, 2u),
                "synthetic-latent-local-descriptor-no-producer");
        require(
            local_descriptor_without_producer.modules.size() == 1u &&
                !has_dispatch_entry(local_descriptor_without_producer,
                                    0x100u) &&
                !has_dispatch_entry(local_descriptor_without_producer,
                                    0x120u),
            "Lokale Deskriptortabelle wurde ohne beweisbaren "
            "Pointer-Producer zur Callback-Rootfamilie erhoben.");

        const auto local_descriptor_wrong_stride = discover_local_descriptor(
            local_descriptor_module(true, 44u, 2u),
            "synthetic-latent-local-descriptor-wrong-stride");
        require(
            local_descriptor_wrong_stride.modules.size() == 1u &&
                !has_dispatch_entry(local_descriptor_wrong_stride, 0x100u) &&
                !has_dispatch_entry(local_descriptor_wrong_stride, 0x120u),
            "Nicht passende Deskriptor-Strides wurden zu einer Callback-"
            "Rootfamilie verschmolzen.");

        const auto local_descriptor_single_target = discover_local_descriptor(
            local_descriptor_module(true, 48u, 1u),
            "synthetic-latent-local-descriptor-single-target");
        require(
            local_descriptor_single_target.modules.size() == 1u &&
                !has_dispatch_entry(local_descriptor_single_target, 0x100u) &&
                !has_dispatch_entry(local_descriptor_single_target, 0x120u),
            "Ein einzelnes Descriptorziel wurde trotz Mindestfamilie als "
            "Callbacktabelle akzeptiert.");

        const std::array unrelated_record_external_targets{
            0x8C018120u, 0x8C020000u};
        const std::array unrelated_record_field_sinks{
            katana::codegen::LatentAotExternalCallbackFieldSink{
                0x8C020000u, 0x8C020006u, 0x8C020004u, 44, 4u, true}};
        const std::array unrelated_record_roots{0u};
        katana::codegen::LatentAotDiscoveryOptions
            unrelated_record_options;
        unrelated_record_options.mode =
            katana::codegen::LatentAotDiscoveryMode::ExactOnly;
        unrelated_record_options.completeness_policy =
            katana::codegen::LatentAotCompletenessPolicy::
                ExactRuntimeOnlyStopOnMiss;
        unrelated_record_options.external_code_targets =
            unrelated_record_external_targets;
        unrelated_record_options.external_callback_field_sinks =
            unrelated_record_field_sinks;

        const auto unrelated_record_data =
            katana::codegen::audit_latent_aot_module(
                unrelated_record_field_module(false), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                unrelated_record_options);
        require(
            unrelated_record_data.admitted &&
                !std::binary_search(
                    unrelated_record_data.final_entry_offsets.begin(),
                    unrelated_record_data.final_entry_offsets.end(),
                    0x100u),
            "Globale Callback-Feldverschiebung erhob BSR-aehnliche Daten "
            "aus einem fremden Factory-Record zum Code-Root.");

        const auto unrelated_record_callback =
            katana::codegen::audit_latent_aot_module(
                unrelated_record_field_module(true), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                unrelated_record_options);
        require(
            unrelated_record_callback.admitted &&
                std::binary_search(
                    unrelated_record_callback.final_entry_offsets.begin(),
                    unrelated_record_callback.final_entry_offsets.end(),
                    0x100u),
            "Vollstaendig lokal decodierbarer Callback verlor seine "
            "heuristische Root-Evidence.");

        const std::array direct_record_external_targets{
            0x8C018120u, 0x8C020000u};
        const std::array direct_record_field_sinks{
            katana::codegen::LatentAotExternalCallbackFieldSink{
                0x8C020000u, 0x8C020006u, 0x8C020004u, 16, 4u, true}};
        katana::codegen::LatentAotDiscoveryOptions direct_record_options;
        direct_record_options.mode =
            katana::codegen::LatentAotDiscoveryMode::ExactOnly;
        direct_record_options.completeness_policy =
            katana::codegen::LatentAotCompletenessPolicy::
                ExactRuntimeOnlyStopOnMiss;
        direct_record_options.external_code_targets =
            direct_record_external_targets;
        direct_record_options.external_callback_field_sinks =
            direct_record_field_sinks;

        const auto direct_record_callback =
            katana::codegen::audit_latent_aot_module(
                direct_record_field_module(true), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                direct_record_options);
        require(
            direct_record_callback.admitted &&
                std::binary_search(
                    direct_record_callback.final_entry_offsets.begin(),
                    direct_record_callback.final_entry_offsets.end(),
                    0x100u),
            "Direkter r4-Record-Receiver verlor die lokale Callback-Root-"
            "Evidence.");

        const auto direct_record_without_sink =
            katana::codegen::audit_latent_aot_module(
                direct_record_field_module(true), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                unrelated_record_options);
        require(
            direct_record_without_sink.admitted &&
                !std::binary_search(
                    direct_record_without_sink.final_entry_offsets.begin(),
                    direct_record_without_sink.final_entry_offsets.end(),
                    0x100u),
            "Direkter r4-Store wurde ohne StaticCallbackFieldSinkContract "
            "als Callback-Root akzeptiert.");

        const auto direct_record_wrong_receiver =
            katana::codegen::audit_latent_aot_module(
                direct_record_field_module(true, 5u), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                direct_record_options);
        require(
            direct_record_wrong_receiver.admitted &&
                !std::binary_search(
                    direct_record_wrong_receiver.final_entry_offsets.begin(),
                    direct_record_wrong_receiver.final_entry_offsets.end(),
                    0x100u),
            "Nichtkanonischer r5-Record-Receiver wurde als Callback-Root "
            "akzeptiert.");

        const auto direct_record_without_store =
            katana::codegen::audit_latent_aot_module(
                direct_record_field_module(false), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                direct_record_options);
        require(
            direct_record_without_store.admitted &&
                !std::binary_search(
                    direct_record_without_store.final_entry_offsets.begin(),
                    direct_record_without_store.final_entry_offsets.end(),
                    0x100u),
            "Ein lokales Literal ohne Record-Feld-Store wurde als Callback-"
            "Root akzeptiert.");

        const std::array published_record_external_targets{
            0x8C020000u, 0x8C030000u};
        const std::array published_record_field_sinks{
            katana::codegen::LatentAotExternalCallbackFieldSink{
                0x8C020000u, 0x8C020006u, 0x8C020004u, 16, 4u, true}};
        const std::array published_record_pointer_sinks{
            katana::codegen::LatentAotExternalPersistentPointerSink{
                0x8C030000u, 0x02u}};
        katana::codegen::LatentAotDiscoveryOptions
            published_record_options;
        published_record_options.mode =
            katana::codegen::LatentAotDiscoveryMode::ExactOnly;
        published_record_options.completeness_policy =
            katana::codegen::LatentAotCompletenessPolicy::
                ExactRuntimeOnlyStopOnMiss;
        published_record_options.external_code_targets =
            published_record_external_targets;
        published_record_options.external_persistent_pointer_sinks =
            published_record_pointer_sinks;
        published_record_options.external_callback_field_sinks =
            published_record_field_sinks;

        const auto published_record_callback =
            katana::codegen::audit_latent_aot_module(
                published_record_field_module(), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                published_record_options);
        require(
            published_record_callback.admitted &&
                std::binary_search(
                    published_record_callback.final_entry_offsets.begin(),
                    published_record_callback.final_entry_offsets.end(),
                    0x100u) &&
                std::binary_search(
                    published_record_callback.emitted_block_offsets.begin(),
                    published_record_callback.emitted_block_offsets.end(),
                    0x100u),
            "Publizierter Record-Alias verlor seinen identity-bound +16-"
            "Callback-Root oder dessen Blockidentitaet.");

        auto unpublished_record_options = published_record_options;
        unpublished_record_options.external_persistent_pointer_sinks = {};
        const auto unpublished_record_callback =
            katana::codegen::audit_latent_aot_module(
                published_record_field_module(), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                unpublished_record_options);
        require(
            unpublished_record_callback.admitted &&
                !std::binary_search(
                    unpublished_record_callback.final_entry_offsets.begin(),
                    unpublished_record_callback.final_entry_offsets.end(),
                    0x100u),
            "Nicht publizierter Input-Alias wurde allein aus einer globalen "
            "Callback-Feldform zum Root erhoben.");

        const std::array wrong_published_record_pointer_sinks{
            katana::codegen::LatentAotExternalPersistentPointerSink{
                0x8C030000u, 0x04u}};
        auto wrong_published_record_options = published_record_options;
        wrong_published_record_options.external_persistent_pointer_sinks =
            wrong_published_record_pointer_sinks;
        const auto wrong_published_record_callback =
            katana::codegen::audit_latent_aot_module(
                published_record_field_module(), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                wrong_published_record_options);
        require(
            wrong_published_record_callback.admitted &&
                !std::binary_search(
                    wrong_published_record_callback.final_entry_offsets.begin(),
                    wrong_published_record_callback.final_entry_offsets.end(),
                    0x100u),
            "Persistenter Sink fuer das falsche ABI-Argument publizierte "
            "einen fremden Record-Alias.");

        const std::array registered_record_external_targets{
            0x8C020000u, 0x8C040000u};
        const std::array registered_record_sinks{
            katana::codegen::LatentAotExternalCallbackSink{
                0x8C040000u, 0x04u, 0x04u}};
        const std::array registered_record_field_sinks{
            katana::codegen::LatentAotExternalCallbackFieldSink{
                0x8C020000u, 0x8C020006u, 0x8C020004u,
                16, 4u, true, 0x01u}};
        katana::codegen::LatentAotDiscoveryOptions
            registered_record_options;
        registered_record_options.mode =
            katana::codegen::LatentAotDiscoveryMode::ExactOnly;
        registered_record_options.completeness_policy =
            katana::codegen::LatentAotCompletenessPolicy::
                ExactRuntimeOnlyStopOnMiss;
        registered_record_options.external_code_targets =
            registered_record_external_targets;
        registered_record_options.external_callback_sinks =
            registered_record_sinks;
        registered_record_options.external_callback_field_sinks =
            registered_record_field_sinks;

        const auto registered_record_callback =
            katana::codegen::audit_latent_aot_module(
                registered_record_callback_module(), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                registered_record_options);
        require(
            registered_record_callback.admitted &&
                std::binary_search(
                    registered_record_callback.final_entry_offsets.begin(),
                    registered_record_callback.final_entry_offsets.end(),
                    0x40u) &&
                std::binary_search(
                    registered_record_callback.final_entry_offsets.begin(),
                    registered_record_callback.final_entry_offsets.end(),
                    0x100u),
            "Ein ueber einen persistenten Primary-Record registrierter "
            "Initializer verlor seinen lokalen +16-Folgecallback.");

        const std::array untyped_registered_record_sinks{
            katana::codegen::LatentAotExternalCallbackSink{
                0x8C040000u, 0x04u, 0x00u}};
        auto untyped_registered_record_options =
            registered_record_options;
        untyped_registered_record_options.external_callback_sinks =
            untyped_registered_record_sinks;
        const auto untyped_registered_record_callback =
            katana::codegen::audit_latent_aot_module(
                registered_record_callback_module(), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                untyped_registered_record_options);
        require(
            untyped_registered_record_callback.admitted &&
                std::binary_search(
                    untyped_registered_record_callback.final_entry_offsets
                        .begin(),
                    untyped_registered_record_callback.final_entry_offsets
                        .end(),
                    0x40u) &&
                !std::binary_search(
                    untyped_registered_record_callback.final_entry_offsets
                        .begin(),
                    untyped_registered_record_callback.final_entry_offsets
                        .end(),
                    0x100u),
            "Ein Callback-Sink ohne Record-ABI-Provenienz erfand einen "
            "lokalen Folgecallback.");

        const std::array mutual_record_external_targets{0x8C020000u};
        const std::array mutual_record_field_sinks{
            katana::codegen::LatentAotExternalCallbackFieldSink{
                0x8C020000u, 0x8C020006u, 0x8C020004u, 16, 4u, true}};
        katana::codegen::LatentAotDiscoveryOptions mutual_record_options;
        mutual_record_options.mode =
            katana::codegen::LatentAotDiscoveryMode::ExactOnly;
        mutual_record_options.completeness_policy =
            katana::codegen::LatentAotCompletenessPolicy::
                ExactRuntimeOnlyStopOnMiss;
        mutual_record_options.external_code_targets =
            mutual_record_external_targets;
        mutual_record_options.external_callback_field_sinks =
            mutual_record_field_sinks;

        const auto mutual_record_callback =
            katana::codegen::audit_latent_aot_module(
                mutual_record_table_module(), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                mutual_record_options);
        require(
            mutual_record_callback.admitted &&
                std::binary_search(
                    mutual_record_callback.final_entry_offsets.begin(),
                    mutual_record_callback.final_entry_offsets.end(),
                    0x100u),
            "Mutual-Record-Tabelle verlor ihren dreifach stride- und "
            "Rueckreferenz-bewiesenen Callback-Root.");

        const auto mutual_record_two_records =
            katana::codegen::audit_latent_aot_module(
                mutual_record_table_module(2u), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                mutual_record_options);
        require(
            mutual_record_two_records.admitted &&
                !std::binary_search(
                    mutual_record_two_records.final_entry_offsets.begin(),
                    mutual_record_two_records.final_entry_offsets.end(),
                    0x100u),
            "Nur zwei Record-Zellen wurden als statische Callback-Familie "
            "promotet.");

        const auto mutual_record_broken_stride =
            katana::codegen::audit_latent_aot_module(
                mutual_record_table_module(3u, false), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                mutual_record_options);
        require(
            mutual_record_broken_stride.admitted &&
                !std::binary_search(
                    mutual_record_broken_stride.final_entry_offsets.begin(),
                    mutual_record_broken_stride.final_entry_offsets.end(),
                    0x100u),
            "Nichtkonstante Record-Schritte wurden als Callback-Tabelle "
            "promotet.");

        const auto mutual_record_without_backreference =
            katana::codegen::audit_latent_aot_module(
                mutual_record_table_module(3u, true, false), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                mutual_record_options);
        require(
            mutual_record_without_backreference.admitted &&
                !std::binary_search(
                    mutual_record_without_backreference.final_entry_offsets.begin(),
                    mutual_record_without_backreference.final_entry_offsets.end(),
                    0x100u),
            "Record-Pointer ohne Callback-Rueckreferenz wurde als Root "
            "promotet.");

        auto wrong_mutual_record_options = mutual_record_options;
        const std::array wrong_mutual_record_field_sinks{
            katana::codegen::LatentAotExternalCallbackFieldSink{
                0x8C020000u, 0x8C020006u, 0x8C020004u, 20, 4u, true}};
        wrong_mutual_record_options.external_callback_field_sinks =
            wrong_mutual_record_field_sinks;
        const auto mutual_record_wrong_field =
            katana::codegen::audit_latent_aot_module(
                mutual_record_table_module(), 0x88000000u,
                unrelated_record_roots, 0x8C900000u,
                wrong_mutual_record_options);
        require(
            mutual_record_wrong_field.admitted &&
                !std::binary_search(
                    mutual_record_wrong_field.final_entry_offsets.begin(),
                    mutual_record_wrong_field.final_entry_offsets.end(),
                    0x100u),
            "Falsche Callback-Feldform nutzte eine fremde Record-Basis als "
            "Mutual-Root.");

        const auto repeated =
            katana::codegen::discover_latent_aot_modules(source, 0u, 0u);
        require(repeated.modules.size() == discovered.modules.size() &&
                    repeated.modules.front().id == module.id &&
                    repeated.modules.front().byte_identity == module.byte_identity &&
                    repeated.modules.front().source_bindings ==
                        module.source_bindings &&
                    repeated.modules.front().source_address == module.source_address &&
                    repeated.modules.front().program.size() == module.program.size() &&
                    repeated.modules.front().block_identities ==
                        module.block_identities,
                "Latente Registry ist bei identischer Disc nicht deterministisch geordnet.");

        AnalysisCacheFixture analysis_cache_fixture;
        auto cached_options =
            katana::codegen::LatentAotDiscoveryOptions{};
        std::mutex latent_progress_mutex;
        std::vector<katana::ProgressEvent>
            latent_progress_events;
        cached_options.progress = katana::ProgressReporter(
            [&](const katana::ProgressEvent& event) {
                const std::lock_guard lock(
                    latent_progress_mutex);
                latent_progress_events.push_back(event);
            },
            std::chrono::milliseconds(0),
            std::chrono::milliseconds(1000));
        cached_options.analysis_cache_root =
            analysis_cache_fixture.path;
        const auto unproven_cache_disabled =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, cached_options);
        require(
            unproven_cache_disabled.modules.size() == 1u &&
                unproven_cache_disabled.analysis_cache_positive_hits == 0u &&
                unproven_cache_disabled.analysis_cache_negative_hits == 0u &&
                unproven_cache_disabled.analysis_cache_misses == 0u &&
                unproven_cache_disabled.analysis_cache_stores == 0u &&
                unproven_cache_disabled.analysis_full_pipeline_runs != 0u &&
                !std::filesystem::exists(analysis_cache_fixture.path),
            "Latent-AOT-Analysecache lief ohne beweisbare genaue "
            "Analyzer-/Exporter-Identitaet.");
        cached_options.analysis_implementation_identity =
            "latent-registry-test-implementation-a";
        const auto cache_cold =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, cached_options);
        require(
            cached_options.progress.flush(),
            "Latent-AOT-Progress konnte vor der Telemetriepruefung nicht flushen.");
        bool saw_module_control_flow_update = false;
        bool saw_module_function_values = false;
        bool saw_candidate_executor_telemetry = false;
        {
            const std::lock_guard lock(
                latent_progress_mutex);
            saw_module_control_flow_update =
                std::any_of(
                    latent_progress_events.begin(),
                    latent_progress_events.end(),
                    [](const katana::ProgressEvent& event) {
                        return event.operation ==
                                   katana::ProgressOperation::
                                       ControlFlowAnalysis &&
                               event.label.starts_with(
                                   "latent-aot-module-") &&
                               event.counters.iteration
                                   .has_value() &&
                               event.counters.started
                                   .has_value();
                    });
            saw_module_function_values =
                std::any_of(
                    latent_progress_events.begin(),
                    latent_progress_events.end(),
                    [](const katana::ProgressEvent& event) {
                        return event.operation ==
                                   katana::ProgressOperation::
                                       FunctionValueAnalysis &&
                               event.counters.iteration
                                   .has_value();
                    });
            saw_candidate_executor_telemetry =
                std::any_of(
                    latent_progress_events.begin(),
                    latent_progress_events.end(),
                    [](const katana::ProgressEvent& event) {
                        const auto& counters = event.counters;
                        return event.operation ==
                                   katana::ProgressOperation::
                                       CandidateResolution &&
                               event.label ==
                                   "latent-aot-candidates" &&
                               counters.active_workers ==
                                   counters.executor_running_workers &&
                               counters.executor_waiting_workers
                                   .has_value() &&
                               counters.executor_idle_workers
                                   .has_value() &&
                               counters.executor_queued_work
                                   .has_value() &&
                               counters.executor_memory_blocked_work
                                   .has_value() &&
                               counters.executor_continuations
                                   .has_value() &&
                               counters.analysis_memory_capacity_bytes
                                   .has_value() &&
                               counters.analysis_memory_used_bytes
                                   .has_value() &&
                               counters.analysis_memory_peak_bytes
                                   .has_value() &&
                               katana::progress_event_telemetry_complete(
                                   event);
                    });
            latent_progress_events.clear();
        }
        require(
            cache_cold.modules.size() == 1u &&
                cache_cold.rejected_files == 1u &&
                cache_cold.analysis_candidate_duration_ms.size() == 1u &&
                cache_cold.analysis_cache_positive_hits == 0u &&
                cache_cold.analysis_cache_negative_hits == 0u &&
                cache_cold.analysis_cache_misses == 1u &&
                cache_cold.analysis_cache_stores == 0u &&
                cache_cold.analysis_full_pipeline_runs == 1u &&
                saw_module_control_flow_update &&
                saw_module_function_values &&
                saw_candidate_executor_telemetry,
            "Kalter Latent-AOT-Analysecache analysierte einen sicher "
            "source-derived abweisbaren Kandidaten oder "
                "meldete keinen inneren CFA/FVA-/Executor-Fortschritt: modules=" +
                std::to_string(cache_cold.modules.size()) + " rejected=" +
                std::to_string(cache_cold.rejected_files) + " candidates=" +
                std::to_string(cache_cold.analysis_candidate_duration_ms.size()) +
                " positive=" +
                std::to_string(cache_cold.analysis_cache_positive_hits) +
                " negative=" +
                std::to_string(cache_cold.analysis_cache_negative_hits) +
                " misses=" + std::to_string(cache_cold.analysis_cache_misses) +
                " stores=" + std::to_string(cache_cold.analysis_cache_stores) +
                " pipelines=" +
                std::to_string(cache_cold.analysis_full_pipeline_runs) +
                " cfa=" + std::to_string(saw_module_control_flow_update) +
                " fva=" + std::to_string(saw_module_function_values) +
                " executor=" + std::to_string(saw_candidate_executor_telemetry));
        const auto cache_warm =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, cached_options);
        {
            const std::lock_guard lock(
                latent_progress_mutex);
            latent_progress_events.clear();
        }
        require(
            cache_warm.modules.size() == 1u &&
                cache_warm.analysis_candidate_duration_ms.size() == 1u &&
                cache_warm.modules.front().program.size() ==
                    cache_cold.modules.front().program.size() &&
                cache_warm.modules.front().program.front().entry_address ==
                    cache_cold.modules.front().program.front().entry_address &&
                cache_warm.modules.front().program.front().blocks.size() ==
                    cache_cold.modules.front().program.front().blocks.size() &&
                cache_warm.modules.front().block_identities ==
                    cache_cold.modules.front().block_identities &&
                cache_warm.rejected_files == 1u &&
                cache_warm.analysis_cache_positive_hits == 0u &&
                cache_warm.analysis_cache_negative_hits == 0u &&
                cache_warm.analysis_cache_misses == 1u &&
                cache_warm.analysis_cache_stores == 0u &&
                cache_warm.analysis_full_pipeline_runs == 1u &&
                katana::codegen::serialize_latent_aot_positive_cache(
                    std::string(64u, 'a'),
                    cache_warm.modules.front().program) ==
                    katana::codegen::serialize_latent_aot_positive_cache(
                        std::string(64u, 'a'),
                        cache_cold.modules.front().program),
            "Warmer Latent-AOT-Lauf war nicht kanonisch identisch oder "
            "analysierte einen sicheren negativen Shape-Treffer. "
            "positive=" +
                std::to_string(
                    cache_warm.analysis_cache_positive_hits) +
                ", negative=" +
                std::to_string(
                    cache_warm.analysis_cache_negative_hits) +
                ", misses=" +
                std::to_string(
                    cache_warm.analysis_cache_misses) +
                ", stores=" +
                std::to_string(
                    cache_warm.analysis_cache_stores) +
                ", pipelines=" +
                std::to_string(
                    cache_warm.analysis_full_pipeline_runs));

        const auto positive_cache_key =
            analysis_cache_key_for_module(
                cache_cold.modules.front(), cached_options);
        auto subset_program = cache_cold.modules.front().program;
        require(
            !subset_program.empty() &&
                !subset_program.front().blocks.empty() &&
                subset_program.front().blocks.front().instructions.size() >
                    1u,
            "Subset-Forge-Fixture besitzt keinen entfernbaren Quellbefehl.");
        subset_program.front().blocks.front().instructions.pop_back();
        const auto subset_artifact =
            katana::codegen::serialize_latent_aot_positive_cache(
                positive_cache_key, subset_program);
        katana::codegen::CodegenCache(
            analysis_cache_fixture.path)
            .store_bounded(
                positive_cache_key,
                "module-analysis.bin",
                std::string_view(
                    reinterpret_cast<const char*>(
                        subset_artifact.data()),
                    subset_artifact.size()),
                katana::codegen::
                    maximum_latent_aot_analysis_cache_artifact_bytes);
        const auto cache_subset_forge =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, cached_options);
        require(
            cache_subset_forge.modules.size() == 1u &&
                cache_subset_forge.rejected_files == 1u &&
                cache_subset_forge.analysis_cache_positive_hits == 0u &&
                cache_subset_forge.analysis_cache_negative_hits == 0u &&
                cache_subset_forge.analysis_cache_misses == 1u &&
                cache_subset_forge.analysis_cache_stores == 0u &&
                cache_subset_forge.analysis_full_pipeline_runs == 1u &&
                cache_subset_forge.modules.front().block_identities ==
                    cache_cold.modules.front().block_identities,
            "Checksum-konsistenter positiver Subset-Forge wurde nicht "
            "fail-closed als Miss vollstaendig neu analysiert.");

        auto changed_implementation_options = cached_options;
        changed_implementation_options.analysis_implementation_identity =
            "latent-registry-test-implementation-b";
        const auto implementation_key_miss =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, changed_implementation_options);
        require(
            implementation_key_miss.modules.size() == 1u &&
                implementation_key_miss.rejected_files == 1u &&
                implementation_key_miss.analysis_cache_positive_hits == 0u &&
                implementation_key_miss.analysis_cache_negative_hits == 0u &&
                implementation_key_miss.analysis_cache_misses == 1u &&
                implementation_key_miss.analysis_cache_stores == 0u &&
                implementation_key_miss.analysis_full_pipeline_runs == 1u,
            "Geaenderte genaue Analyzer-/Exporter-Implementierung "
            "invalidierte den zugelassenen Cacheeintrag nicht.");

        auto changed_cache_options = cached_options;
        ++changed_cache_options.maximum_analysis_contexts;
        const auto cache_key_miss =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, changed_cache_options);
        require(
            cache_key_miss.modules.size() == 1u &&
                cache_key_miss.rejected_files == 1u &&
                cache_key_miss.analysis_cache_positive_hits == 0u &&
                cache_key_miss.analysis_cache_negative_hits == 0u &&
                cache_key_miss.analysis_cache_misses == 1u &&
                cache_key_miss.analysis_cache_stores == 0u &&
                cache_key_miss.analysis_full_pipeline_runs == 1u,
            "Analyse-relevantes Budget invalidierte den "
            "Latent-AOT-Analysecache nicht.");

        katana::codegen::CodegenCache(
            analysis_cache_fixture.path)
            .store_bounded(
                positive_cache_key,
                "module-analysis.bin",
                std::string_view(
                    reinterpret_cast<const char*>(
                        subset_artifact.data()),
                    subset_artifact.size()),
                katana::codegen::
                    maximum_latent_aot_analysis_cache_artifact_bytes);
        const auto corrupted_analysis_cache_artifacts =
            analysis_cache_fixture.corrupt_all_artifacts();
        require(
            corrupted_analysis_cache_artifacts == 1u,
            "Analysecache-Fixture fand das zugelassene positive "
            "Artefakt nicht: count=" +
                std::to_string(corrupted_analysis_cache_artifacts));
        const auto cache_corrupt =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, cached_options);
        require(
            cache_corrupt.modules.size() == 1u &&
                cache_corrupt.modules.front().program.size() ==
                    cache_cold.modules.front().program.size() &&
                cache_corrupt.modules.front().program.front().entry_address ==
                    cache_cold.modules.front().program.front().entry_address &&
                cache_corrupt.rejected_files == 1u &&
                cache_corrupt.analysis_cache_positive_hits == 0u &&
                cache_corrupt.analysis_cache_negative_hits == 0u &&
                cache_corrupt.analysis_cache_misses == 1u &&
                cache_corrupt.analysis_cache_corrupt_entries == 1u &&
                cache_corrupt.analysis_cache_stores == 0u &&
                cache_corrupt.analysis_full_pipeline_runs == 1u,
            "Korrupter Latent-AOT-Analysecache wurde nicht fail-closed "
            "als Miss neu analysiert und begrenzt repariert.");

        auto exact_only_options =
            katana::codegen::LatentAotDiscoveryOptions{};
        exact_only_options.mode =
            katana::codegen::LatentAotDiscoveryMode::ExactOnly;
        const std::array duplicate_extent_hint{
            katana::codegen::LatentAotEntryHint{
                module.byte_identity, 22u * sector_size, 4u, 0u}};
        const auto exact_duplicate_extent =
            katana::codegen::discover_latent_aot_modules(
                source,
                0u,
                0u,
                {},
                exact_only_options,
                {},
                duplicate_extent_hint);
        require(
            exact_duplicate_extent.modules.size() == 1u &&
                exact_duplicate_extent.modules.front().id == module.id &&
                exact_duplicate_extent.modules.front().source_bindings.size() ==
                    1u &&
                exact_duplicate_extent.modules.front()
                        .source_bindings.front()
                        .disc_byte_offset == 22u * sector_size &&
                exact_duplicate_extent.modules.front().entry_offsets ==
                    std::vector<std::uint32_t>{0u},
            "Byteidentischer erster ISO-Extent konsumierte die exakte Bindung "
            "des zweiten Extents.");

        const std::vector<std::uint8_t> multi_entry_bytes{
            0x0Bu, 0x00u, 0x09u, 0x00u,
            0x0Bu, 0x00u, 0x09u, 0x00u};
        auto multi_entry_source =
            std::make_shared<katana::runtime::MemoryDiscSource>(
                fixture_iso_with_files(
                    {{21u, "MULTI_A.BIN;1", multi_entry_bytes},
                     {22u, "MULTI_B.BIN;1", multi_entry_bytes}}),
                "synthetic-latent-aot-multi-entry-disc");
        const std::array multi_extent_hints{
            katana::codegen::LatentAotEntryHint{
                byte_identity(multi_entry_bytes),
                21u * sector_size,
                static_cast<std::uint32_t>(multi_entry_bytes.size()),
                0u},
            katana::codegen::LatentAotEntryHint{
                byte_identity(multi_entry_bytes),
                22u * sector_size,
                static_cast<std::uint32_t>(multi_entry_bytes.size()),
                4u},
        };
        const auto multi_extent =
            katana::codegen::discover_latent_aot_modules(
                multi_entry_source,
                0u,
                0u,
                {},
                exact_only_options,
                {},
                multi_extent_hints);
        require(
            multi_extent.examined_files == 2u &&
                multi_extent.modules.size() == 1u &&
                multi_extent.modules.front().source_bindings.size() == 2u &&
                multi_extent.modules.front().source_bindings[0].disc_byte_offset ==
                    21u * sector_size &&
                multi_extent.modules.front().source_bindings[1].disc_byte_offset ==
                    22u * sector_size &&
                multi_extent.modules.front().entry_offsets ==
                    (std::vector<std::uint32_t>{0u, 4u}),
            "Byteidentische Exact-Hints an verschiedenen Disc-Extents "
            "wurden nicht als ein Template mit zwei Source-Bindings und "
            "vereinigtem Entryset gruppiert.");

        std::vector<std::uint8_t> reverse_prologue_bytes(0x4Eu, 0u);
        const auto put_reverse_prologue_opcode =
            [&reverse_prologue_bytes](const std::size_t offset,
                                      const std::uint16_t opcode) {
                reverse_prologue_bytes[offset] =
                    static_cast<std::uint8_t>(opcode);
                reverse_prologue_bytes[offset + 1u] =
                    static_cast<std::uint8_t>(opcode >> 8u);
            };
        put_reverse_prologue_opcode(0x00u, 0xB01Eu); // bsr 0x40
        put_reverse_prologue_opcode(0x02u, 0x0009u); // delay nop
        put_reverse_prologue_opcode(0x04u, 0x000Bu); // rts
        put_reverse_prologue_opcode(0x06u, 0x0009u); // delay nop
        put_reverse_prologue_opcode(0x0Cu, 0x000Bu); // previous rts
        put_reverse_prologue_opcode(0x0Eu, 0x0009u); // previous delay
        put_reverse_prologue_opcode(0x10u, 0x2FE6u); // push r14
        put_reverse_prologue_opcode(0x12u, 0x6E43u); // mov r4,r14
        put_reverse_prologue_opcode(0x14u, 0x2FD6u); // push r13
        put_reverse_prologue_opcode(0x16u, 0x4F22u); // push pr
        put_reverse_prologue_opcode(0x18u, 0x4F26u); // pop pr
        put_reverse_prologue_opcode(0x1Au, 0x6DF6u); // pop r13
        put_reverse_prologue_opcode(0x1Cu, 0x000Bu); // rts
        put_reverse_prologue_opcode(0x1Eu, 0x6EF6u); // pop r14 delay
        put_reverse_prologue_opcode(0x40u, 0x2FE6u); // push r14
        put_reverse_prologue_opcode(0x42u, 0x2FD6u); // push r13
        put_reverse_prologue_opcode(0x44u, 0x4F22u); // push pr
        put_reverse_prologue_opcode(0x46u, 0x4F26u); // pop pr
        put_reverse_prologue_opcode(0x48u, 0x6DF6u); // pop r13
        put_reverse_prologue_opcode(0x4Au, 0xAFE1u); // bra 0x10
        put_reverse_prologue_opcode(0x4Cu, 0x6EF6u); // pop r14 delay
        auto reverse_prologue_source =
            std::make_shared<katana::runtime::MemoryDiscSource>(
                fixture_iso_with_files(
                    {{21u, "REVERSE.BIN;1", reverse_prologue_bytes}}),
                "synthetic-latent-aot-reverse-prologue-disc");
        const std::array reverse_prologue_hints{
            katana::codegen::LatentAotEntryHint{
                byte_identity(reverse_prologue_bytes),
                21u * sector_size,
                static_cast<std::uint32_t>(reverse_prologue_bytes.size()),
                0u},
            katana::codegen::LatentAotEntryHint{
                byte_identity(reverse_prologue_bytes),
                21u * sector_size,
                static_cast<std::uint32_t>(reverse_prologue_bytes.size()),
                0x40u},
        };
        const auto reverse_prologue =
            katana::codegen::discover_latent_aot_modules(
                reverse_prologue_source,
                0u,
                0u,
                {},
                exact_only_options,
                {},
                reverse_prologue_hints);
        require(
            reverse_prologue.modules.size() == 1u &&
                reverse_prologue.modules.front().entry_offsets ==
                    (std::vector<std::uint32_t>{0u, 0x10u, 0x40u}) &&
                std::any_of(
                    reverse_prologue.modules.front().program.begin(),
                    reverse_prologue.modules.front().program.end(),
                    [&](const auto& function) {
                        return function.entry_address ==
                            reverse_prologue.modules.front().source_address +
                                0x10u;
                    }),
            "Exakter Tail-Callback verlor den rueckwaerts normalisierten "
            "Callee-Saved-Prolog vor STS.L PR.");

        bool rejected = false;

        const std::vector<std::uint8_t> first_cap_bytes{
            0x0Bu, 0x00u, 0x09u, 0x00u};
        const std::vector<std::uint8_t> hinted_cap_bytes{
            0x0Bu, 0x00u, 0x08u, 0x00u};
        auto cap_source =
            std::make_shared<katana::runtime::MemoryDiscSource>(
                fixture_iso_with_files(
                    {{21u, "FIRST.BIN;1", first_cap_bytes},
                     {22u, "HINTED.BIN;1", hinted_cap_bytes}}),
                "synthetic-latent-aot-cap-disc");
        auto cap_options = katana::codegen::LatentAotDiscoveryOptions{};
        cap_options.mode =
            katana::codegen::LatentAotDiscoveryMode::ExactOnly;
        cap_options.maximum_candidate_files = 0u;
        const std::array behind_cap_hint{
            katana::codegen::LatentAotEntryHint{
                byte_identity(hinted_cap_bytes),
                22u * sector_size,
                static_cast<std::uint32_t>(hinted_cap_bytes.size()),
                0u}};
        const auto exact_behind_cap =
            katana::codegen::discover_latent_aot_modules(
                cap_source,
                0u,
                0u,
                {},
                cap_options,
                {},
                behind_cap_hint);
        require(
            exact_behind_cap.examined_files == 1u &&
                exact_behind_cap.modules.size() == 1u &&
                exact_behind_cap.modules.front().source_bindings.size() == 1u &&
                exact_behind_cap.modules.front()
                        .source_bindings.front()
                        .disc_byte_offset == 22u * sector_size &&
                exact_behind_cap.modules.front().entry_offsets ==
                    std::vector<std::uint32_t>{0u},
            "Exakter Latent-AOT-Hint hinter dem Heuristik-Kandidatenlimit "
            "blieb ungebunden oder loeste eine unangeforderte Heuristik aus.");

        const std::array pinned_source_hints{
            katana::codegen::LatentAotEntryHint{
                byte_identity(first_cap_bytes),
                21u * sector_size,
                static_cast<std::uint32_t>(first_cap_bytes.size()),
                0u},
            katana::codegen::LatentAotEntryHint{
                byte_identity(hinted_cap_bytes),
                22u * sector_size,
                static_cast<std::uint32_t>(hinted_cap_bytes.size()),
                0u,
                0x88002000u}};
        const auto pinned_sources =
            katana::codegen::discover_latent_aot_modules(
                cap_source,
                0u,
                0u,
                {},
                exact_only_options,
                {},
                pinned_source_hints);
        const auto pinned_module = std::ranges::find_if(
            pinned_sources.modules,
            [&](const auto& candidate) {
                return candidate.byte_identity ==
                       byte_identity(hinted_cap_bytes);
            });
        const auto automatic_module = std::ranges::find_if(
            pinned_sources.modules,
            [&](const auto& candidate) {
                return candidate.byte_identity ==
                       byte_identity(first_cap_bytes);
            });
        require(
            pinned_sources.modules.size() == 2u &&
                pinned_module != pinned_sources.modules.end() &&
                pinned_module->source_address == 0x88002000u &&
                automatic_module != pinned_sources.modules.end() &&
                automatic_module->source_address == 0x88000000u,
            "Exakt gebundene Modulbasis verschob einen ungebundenen "
            "Nachbarkandidaten oder verlor ihre identity-bound Adresse.");

        const std::array runtime_bound_hint{
            katana::codegen::LatentAotEntryHint{
                byte_identity(hinted_cap_bytes),
                22u * sector_size,
                static_cast<std::uint32_t>(hinted_cap_bytes.size()),
                0u,
                0u,
                0x8CB80000u}};
        const auto runtime_bound =
            katana::codegen::discover_latent_aot_modules(
                cap_source,
                0u,
                0u,
                {},
                exact_only_options,
                {},
                runtime_bound_hint);
        require(
            runtime_bound.modules.size() == 1u &&
                runtime_bound.modules.front().source_address ==
                    0x88000000u,
            "Loader-bewiesene Runtime-Basis veraenderte die unabhaengige "
            "synthetische Analyseplatzierung oder verlor die Modulbindung.");

        rejected = false;
        auto invalid_runtime_base_hint = runtime_bound_hint.front();
        invalid_runtime_base_hint.proven_runtime_base = 0x8D000000u;
        try {
            static_cast<void>(
                katana::codegen::discover_latent_aot_modules(
                    cap_source,
                    0u,
                    0u,
                    {},
                    exact_only_options,
                    {},
                    std::span{&invalid_runtime_base_hint, 1u}));
        } catch (const std::runtime_error& error) {
            rejected =
                std::string_view{error.what()} ==
                "latent-aot-entry-hint-runtime-base-invalid";
        }
        require(
            rejected,
            "Loader-bewiesene Runtime-Basis ausserhalb des gebundenen "
            "Runtime-Fensters wurde akzeptiert.");

        rejected = false;
        const std::array conflicting_runtime_base_hints{
            runtime_bound_hint.front(),
            katana::codegen::LatentAotEntryHint{
                byte_identity(hinted_cap_bytes),
                22u * sector_size,
                static_cast<std::uint32_t>(hinted_cap_bytes.size()),
                2u,
                0u,
                0x8CB90000u}};
        try {
            static_cast<void>(
                katana::codegen::discover_latent_aot_modules(
                    cap_source,
                    0u,
                    0u,
                    {},
                    exact_only_options,
                    {},
                    conflicting_runtime_base_hints));
        } catch (const std::runtime_error& error) {
            rejected =
                std::string_view{error.what()} ==
                "latent-aot-entry-hint-runtime-base-conflict";
        }
        require(
            rejected,
            "Widerspruechliche identity-bound Runtime-Basen wurden nicht "
            "fail-closed abgelehnt.");

        rejected = false;
        const std::array pinned_collision{
            katana::codegen::LatentAotOccupiedRange{
                0x88002000u, 4096u}};
        try {
            static_cast<void>(
                katana::codegen::discover_latent_aot_modules(
                    cap_source,
                    0u,
                    0u,
                    {},
                    exact_only_options,
                    pinned_collision,
                    std::span{
                        pinned_source_hints}.subspan(1u)));
        } catch (const std::runtime_error& error) {
            rejected =
                std::string_view{error.what()} ==
                "latent-aot-entry-hint-source-address-collision";
        }
        require(
            rejected,
            "Exakt gebundene Modulbasis durfte eine belegte Source-Range "
            "ueberlappen.");

        rejected = false;
        auto out_of_range_hint = pinned_source_hints[1];
        out_of_range_hint.source_address =
            exact_only_options.source_address_end;
        try {
            static_cast<void>(
                katana::codegen::discover_latent_aot_modules(
                    cap_source,
                    0u,
                    0u,
                    {},
                    exact_only_options,
                    {},
                    std::span{&out_of_range_hint, 1u}));
        } catch (const std::runtime_error& error) {
            rejected =
                std::string_view{error.what()} ==
                "latent-aot-entry-hint-source-address-invalid";
        }
        require(
            rejected,
            "Exakt gebundene Modulbasis ausserhalb des Latent-Ranges wurde "
            "akzeptiert.");

        rejected = false;
        const std::array conflicting_source_hints{
            pinned_source_hints[1],
            katana::codegen::LatentAotEntryHint{
                byte_identity(hinted_cap_bytes),
                22u * sector_size,
                static_cast<std::uint32_t>(hinted_cap_bytes.size()),
                2u,
                0x88003000u}};
        try {
            static_cast<void>(
                katana::codegen::discover_latent_aot_modules(
                    cap_source,
                    0u,
                    0u,
                    {},
                    exact_only_options,
                    {},
                    conflicting_source_hints));
        } catch (const std::runtime_error& error) {
            rejected =
                std::string_view{error.what()} ==
                "latent-aot-entry-hint-source-address-conflict";
        }
        require(
            rejected,
            "Widerspruechliche identity-bound Modulbasen wurden nicht "
            "fail-closed abgelehnt.");

        const std::vector<std::uint8_t> header_module_bytes{
            0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x0Bu, 0x00u, 0x09u, 0x00u};
        auto header_source =
            std::make_shared<katana::runtime::MemoryDiscSource>(
                fixture_iso_with_files(
                    {{21u, "HEADER.BIN;1", header_module_bytes}}),
                "synthetic-latent-aot-header-disc");
        const std::array nonzero_entry_hint{
            katana::codegen::LatentAotEntryHint{
                byte_identity(header_module_bytes),
                21u * sector_size,
                static_cast<std::uint32_t>(header_module_bytes.size()),
                4u}};
        const auto exact_nonzero_entry =
            katana::codegen::discover_latent_aot_modules(
                header_source,
                0u,
                0u,
                {},
                exact_only_options,
                {},
                nonzero_entry_hint);
        require(
            exact_nonzero_entry.modules.size() == 1u &&
                exact_nonzero_entry.modules.front().entry_offsets ==
                    std::vector<std::uint32_t>{4u},
            "Exakter Nonzero-Entry wurde durch einen synthetischen Entry 0 "
            "oder dessen Datenheader abgelehnt.");

        const std::array hybrid_entry_hint{
            katana::codegen::LatentAotEntryHint{
                byte_identity(multi_entry_bytes),
                21u * sector_size,
                static_cast<std::uint32_t>(multi_entry_bytes.size()),
                4u}};
        const auto hybrid_multi_entry =
            katana::codegen::discover_latent_aot_modules(
                multi_entry_source,
                0u,
                0u,
                {},
                katana::codegen::LatentAotDiscoveryOptions{},
                {},
                hybrid_entry_hint);
        require(
            hybrid_multi_entry.modules.size() == 1u &&
                hybrid_multi_entry.modules.front().entry_offsets ==
                    (std::vector<std::uint32_t>{0u, 4u}),
            "Zusaetzlicher Exact-Hint ersetzte im Heuristikmodus die bereits "
            "gefundene konventionelle Modulwurzel statt sie zu erweitern.");

        // Four literal bytes (RTS/NOP) followed by the strict zero-offset
        // terminator.  The seven-byte encoded extent is intentionally odd:
        // entry offsets and identities belong to the decoded executable,
        // while the hint size binds the compressed disc source.
        const std::vector<std::uint8_t> prs_entry_source{
            0x2Fu, 0x0Bu, 0x00u, 0x09u, 0x00u, 0x00u, 0x00u};
        const std::vector<std::uint8_t> prs_entry_decoded{
            0x0Bu, 0x00u, 0x09u, 0x00u};
        auto prs_entry_disc =
            std::make_shared<katana::runtime::MemoryDiscSource>(
                fixture_iso_with_files(
                    {{21u, "ENTRY.PRS;1", prs_entry_source}}),
                "synthetic-latent-aot-prs-entry-disc");
        const std::array prs_entry_hint{
            katana::codegen::LatentAotEntryHint{
                byte_identity(prs_entry_decoded),
                21u * sector_size,
                static_cast<std::uint32_t>(prs_entry_source.size()),
                0u}};
        const auto exact_prs_entry =
            katana::codegen::discover_latent_aot_modules(
                prs_entry_disc,
                0u,
                0u,
                {},
                exact_only_options,
                {},
                prs_entry_hint);
        require(
            exact_prs_entry.modules.size() == 1u &&
                exact_prs_entry.modules.front().byte_identity ==
                    byte_identity(prs_entry_decoded) &&
                exact_prs_entry.modules.front().byte_size ==
                    prs_entry_decoded.size() &&
                exact_prs_entry.modules.front().entry_offsets ==
                    std::vector<std::uint32_t>{0u} &&
                exact_prs_entry.modules.front().source_bindings.size() == 1u &&
                exact_prs_entry.modules.front().source_bindings.front().byte_size ==
                    prs_entry_source.size(),
            "Exact-Hint band den kodierten PRS-Extent nicht an die "
            "dekodierte Modulidentitaet und Entry-Sicht.");

        const std::vector<std::uint8_t> six_byte_module{
            0x0Bu, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u};
        auto six_byte_source =
            std::make_shared<katana::runtime::MemoryDiscSource>(
                fixture_iso_with_files(
                    {{21u, "SIX.BIN;1", six_byte_module}}),
                "synthetic-latent-aot-six-byte-disc");
        const std::array six_byte_hint{
            katana::codegen::LatentAotEntryHint{
                byte_identity(six_byte_module),
                21u * sector_size,
                static_cast<std::uint32_t>(six_byte_module.size()),
                0u}};
        const auto exact_six_byte =
            katana::codegen::discover_latent_aot_modules(
                six_byte_source,
                0u,
                0u,
                {},
                exact_only_options,
                {},
                six_byte_hint);
        require(exact_six_byte.modules.size() == 1u &&
                    exact_six_byte.modules.front().byte_size == 6u,
                "Gerader exakter Sechs-Byte-Modulbound wurde wie ein "
                "Vierbyte-Heuristikkandidat abgelehnt.");

        auto exact_two_byte_limits = exact_only_options;
        exact_two_byte_limits.maximum_candidate_files = 0u;
        exact_two_byte_limits.maximum_file_bytes = 2u;
        exact_two_byte_limits.maximum_total_file_bytes = 2u;
        const auto empty_exact_only =
            katana::codegen::discover_latent_aot_modules(
                source,
                0u,
                0u,
                {},
                exact_two_byte_limits);
        require(empty_exact_only.examined_files == 0u &&
                    empty_exact_only.modules.empty(),
                "Leerer ExactOnly-Vertrag verlangte faelschlich "
                "Vierbyte-Heuristikbudgets oder untersuchte Dateien.");

        auto exact_file_bounded =
            exact_only_options;
        exact_file_bounded.maximum_file_bytes = 4u;
        rejected = false;
        try {
            static_cast<void>(
                katana::codegen::discover_latent_aot_modules(
                    six_byte_source,
                    0u,
                    0u,
                    {},
                    exact_file_bounded,
                    {},
                    six_byte_hint));
        } catch (const std::runtime_error& error) {
            rejected =
                std::string_view{error.what()} ==
                "latent-aot-entry-hint-file-budget";
        }
        require(
            rejected,
            "Exact-Hint umging das einzelne Modul-/Binderbudget.");

        auto exact_total_bounded =
            exact_only_options;
        exact_total_bounded.maximum_file_bytes = 8u;
        exact_total_bounded.maximum_total_file_bytes = 4u;
        rejected = false;
        try {
            static_cast<void>(
                katana::codegen::discover_latent_aot_modules(
                    six_byte_source,
                    0u,
                    0u,
                    {},
                    exact_total_bounded,
                    {},
                    six_byte_hint));
        } catch (const std::runtime_error& error) {
            rejected =
                std::string_view{error.what()} ==
                "latent-aot-entry-hint-total-budget";
        }
        require(
            rejected,
            "Exact-Hint umging das globale Dateilesebudget.");

        const std::array occupied{
            katana::codegen::LatentAotOccupiedRange{0x88000000u, 4096u}};
        const auto collision_free = katana::codegen::discover_latent_aot_modules(
            source,
            0u,
            0u,
            {},
            katana::codegen::LatentAotDiscoveryOptions{},
            occupied);
        require(collision_free.modules.size() == 1u &&
                    collision_free.modules.front().source_address == 0x88001000u,
                "Belegte native Source-Range wurde nicht deterministisch uebersprungen.");

        const std::array excluded{module.byte_identity};
        const auto excluded_result = katana::codegen::discover_latent_aot_modules(
            source, 0u, 0u, excluded);
        require(excluded_result.modules.empty() &&
                    excluded_result.duplicate_files == 2u &&
                    excluded_result.rejected_files == 1u,
                "Ausgeschlossene/duplizierte Byteidentitaet wurde erneut analysiert.");

        auto bounded = katana::codegen::LatentAotDiscoveryOptions{};
        bounded.maximum_native_instructions_per_module = 1u;
        const auto bounded_result = katana::codegen::discover_latent_aot_modules(
            source, 0u, 0u, {}, bounded);
        require(bounded_result.modules.empty() && bounded_result.rejected_files >= 1u,
                "Instruktionsbudget verwarf ein zu grosses natives Modul nicht lokal.");

        auto context_bounded =
            katana::codegen::LatentAotDiscoveryOptions{};
        context_bounded.maximum_analysis_contexts = 1u;
        const auto context_bounded_result =
            katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, context_bounded);
        require(
            context_bounded_result.modules.empty() &&
                context_bounded_result.rejected_files >= 1u,
            "Latent-AOT verdrahtete das CFA-Kontextbudget nicht mit dem "
            "nicht-beobachtenden Analyseabbruch.");

        auto invalid = katana::codegen::LatentAotDiscoveryOptions{};
        invalid.maximum_workers = 0u;
        rejected = false;
        try {
            static_cast<void>(
                katana::codegen::discover_latent_aot_modules(source, 0u, 0u, {}, invalid));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "Ungueltiges Discovery-Budget wurde akzeptiert.");

        auto oversized_runtime_template =
            katana::codegen::LatentAotDiscoveryOptions{};
        oversized_runtime_template.maximum_file_bytes =
            static_cast<std::size_t>(
                katana::runtime::maximum_native_aot_template_extent) +
            1u;
        rejected = false;
        try {
            static_cast<void>(
                katana::codegen::discover_latent_aot_modules(
                    source,
                    0u,
                    0u,
                    {},
                    oversized_runtime_template));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(
            rejected,
            "Discovery akzeptierte ein Modulbudget oberhalb des "
            "Runtime-Template-Limits.");

        auto directory_bounded = katana::codegen::LatentAotDiscoveryOptions{};
        directory_bounded.maximum_directory_bytes = 1024u;
        directory_bounded.maximum_total_directory_bytes = 1024u;
        rejected = false;
        try {
            static_cast<void>(katana::codegen::discover_latent_aot_modules(
                source, 0u, 0u, {}, directory_bounded));
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected,
                "Directory wurde trotz vorangestelltem Bytebudget vollstaendig gelesen.");

        std::cout << "Latente native Disc-AOT-Registry erfolgreich.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
