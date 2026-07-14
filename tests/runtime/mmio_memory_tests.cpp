#include "katana/runtime/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception, typename Function>
bool throws(Function&& function) {
    try {
        function();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

std::size_t width_bytes(const katana::runtime::MemoryAccessWidth width) {
    return static_cast<std::size_t>(width);
}

struct Access {
    bool write = false;
    std::uint32_t offset = 0u;
    std::uint32_t value = 0u;
    katana::runtime::MemoryAccessWidth width =
        katana::runtime::MemoryAccessWidth::Byte;
};

class ByteOnlyMemoryDevice final : public katana::runtime::MemoryDevice {
public:
    [[nodiscard]] std::size_t size() const noexcept override {
        return bytes_.size();
    }

    [[nodiscard]] std::uint8_t read_u8(
        const std::uint32_t offset
    ) const override {
        if (offset >= bytes_.size()) {
            throw std::out_of_range("Byte-only read ausserhalb der Region.");
        }
        return bytes_[offset];
    }

    void write_u8(
        const std::uint32_t offset,
        const std::uint8_t value
    ) override {
        if (offset >= bytes_.size()) {
            throw std::out_of_range("Byte-only write ausserhalb der Region.");
        }
        bytes_[offset] = value;
        ++byte_writes;
    }

    std::size_t byte_writes = 0u;

private:
    std::array<std::uint8_t, 8> bytes_{};
};

} // namespace

int main() {
    using katana::runtime::Memory;
    using katana::runtime::MemoryAccessWidth;
    using katana::runtime::MemoryRegionAccess;
    using katana::runtime::MmioMemoryDevice;

    std::array<std::uint8_t, 32> registers{};
    std::vector<Access> accesses;

    const auto read_handler = [&registers, &accesses](
        const std::uint32_t offset,
        const MemoryAccessWidth width
    ) {
        const auto count = width_bytes(width);
        std::uint32_t value = 0u;
        for (std::size_t index = 0u; index < count; ++index) {
            value |= static_cast<std::uint32_t>(
                registers[offset + index]
            ) << (index * 8u);
        }
        accesses.push_back(Access{false, offset, value, width});
        return value;
    };

    const auto write_handler = [&registers, &accesses](
        const std::uint32_t offset,
        const std::uint32_t value,
        const MemoryAccessWidth width
    ) {
        accesses.push_back(Access{true, offset, value, width});
        const auto count = width_bytes(width);
        for (std::size_t index = 0u; index < count; ++index) {
            registers[offset + index] = static_cast<std::uint8_t>(
                (value >> (index * 8u)) & 0xFFu
            );
        }
    };

    auto mmio = std::make_shared<MmioMemoryDevice>(
        registers.size(),
        read_handler,
        write_handler
    );

    Memory bus(0u);
    bus.map_region("test-mmio", 0x005F0000u, mmio);
    bus.map_region("test-mmio-alias", 0x205F0000u, mmio);

    bus.write_u8(0x005F0001u, 0xA5u);
    bus.write_u16(0x005F0002u, 0xBEEFu);
    bus.write_u32(0x005F0004u, 0x89ABCDEFu);

    require(
        accesses.size() == 3u &&
        accesses[0].write &&
        accesses[0].offset == 1u &&
        accesses[0].value == 0xA5u &&
        accesses[0].width == MemoryAccessWidth::Byte &&
        accesses[1].write &&
        accesses[1].offset == 2u &&
        accesses[1].value == 0xBEEFu &&
        accesses[1].width == MemoryAccessWidth::Halfword &&
        accesses[2].write &&
        accesses[2].offset == 4u &&
        accesses[2].value == 0x89ABCDEFu &&
        accesses[2].width == MemoryAccessWidth::Word,
        "MMIO-Schreibzugriffe erreichen den Handler nicht exakt einmal mit Breite."
    );

    accesses.clear();
    require(
        bus.read_u8(0x205F0001u) == 0xA5u &&
        bus.read_u16(0x205F0002u) == 0xBEEFu &&
        bus.read_u32(0x205F0004u) == 0x89ABCDEFu,
        "MMIO-Lesehandler liefert ueber Aliase falsche Werte."
    );
    require(
        accesses.size() == 3u &&
        !accesses[0].write &&
        accesses[0].offset == 1u &&
        accesses[0].width == MemoryAccessWidth::Byte &&
        !accesses[1].write &&
        accesses[1].offset == 2u &&
        accesses[1].width == MemoryAccessWidth::Halfword &&
        !accesses[2].write &&
        accesses[2].offset == 4u &&
        accesses[2].width == MemoryAccessWidth::Word,
        "MMIO-Lesezugriffe werden weiterhin in Bytezugriffe zerlegt."
    );

    accesses.clear();
    registers[8u] = 0xDDu;
    registers[9u] = 0xCCu;
    registers[10u] = 0xBBu;
    registers[11u] = 0xAAu;
    require(
        mmio->read_u8(8u) == 0xDDu &&
        mmio->read_u16(8u) == 0xCCDDu &&
        mmio->read_u32(8u) == 0xAABBCCDDu,
        "MMIO-Rueckgabewerte werden nicht passend zur Zugriffsbreite maskiert."
    );

    auto byte_only = std::make_shared<ByteOnlyMemoryDevice>();
    bus.map_region("byte-only", 0x00001000u, byte_only);
    bus.write_u32(0x00001000u, 0x11223344u);
    require(
        byte_only->byte_writes == 4u &&
        bus.read_u32(0x00001000u) == 0x11223344u,
        "Bestehende bytebasierte Speichergeraete verlieren den Little-Endian-Fallback."
    );

    std::size_t forbidden_writes = 0u;
    auto read_only_mmio = std::make_shared<MmioMemoryDevice>(
        4u,
        [](const std::uint32_t, const MemoryAccessWidth) {
            return 0x12345678u;
        },
        [&forbidden_writes](
            const std::uint32_t,
            const std::uint32_t,
            const MemoryAccessWidth
        ) {
            ++forbidden_writes;
        }
    );
    bus.map_region(
        "read-only-mmio",
        0x00002000u,
        read_only_mmio,
        MemoryRegionAccess::ReadOnly
    );
    require(
        throws<std::runtime_error>([&bus] {
            bus.write_u32(0x00002000u, 0xFFFFFFFFu);
        }) && forbidden_writes == 0u,
        "Read-only-Regionsschutz muss vor dem MMIO-Schreibhandler greifen."
    );

    auto write_only_mmio = std::make_shared<MmioMemoryDevice>(
        4u,
        katana::runtime::MmioReadHandler{},
        [](const std::uint32_t, const std::uint32_t, const MemoryAccessWidth) {}
    );
    bus.map_region("write-only-mmio", 0x00003000u, write_only_mmio);
    require(
        throws<std::runtime_error>([&bus] {
            static_cast<void>(bus.read_u8(0x00003000u));
        }),
        "Ein fehlender MMIO-Lesehandler wird nicht sichtbar gemeldet."
    );

    auto read_only_handler_mmio = std::make_shared<MmioMemoryDevice>(
        4u,
        [](const std::uint32_t, const MemoryAccessWidth) {
            return 0u;
        },
        katana::runtime::MmioWriteHandler{}
    );
    bus.map_region(
        "handler-read-only-mmio",
        0x00004000u,
        read_only_handler_mmio
    );
    require(
        throws<std::runtime_error>([&bus] {
            bus.write_u8(0x00004000u, 1u);
        }),
        "Ein fehlender MMIO-Schreibhandler wird nicht sichtbar gemeldet."
    );

    const auto callback_count_before_error = accesses.size();
    require(
        throws<std::out_of_range>([&mmio] {
            static_cast<void>(mmio->read_u32(30u));
        }) && accesses.size() == callback_count_before_error,
        "Ueberlaufende MMIO-Zugriffe duerfen den Handler nicht aufrufen."
    );
    require(
        throws<std::invalid_argument>([] {
            static_cast<void>(MmioMemoryDevice(
                0u,
                [](const std::uint32_t, const MemoryAccessWidth) {
                    return 0u;
                },
                katana::runtime::MmioWriteHandler{}
            ));
        }),
        "Leere MMIO-Regionen werden akzeptiert."
    );
    require(
        throws<std::invalid_argument>([] {
            static_cast<void>(MmioMemoryDevice(
                4u,
                katana::runtime::MmioReadHandler{},
                katana::runtime::MmioWriteHandler{}
            ));
        }),
        "MMIO-Regionen ohne irgendeinen Handler werden akzeptiert."
    );

    std::cout << "Breitenbewusste MMIO-Handler erfolgreich.\n";
    return EXIT_SUCCESS;
}