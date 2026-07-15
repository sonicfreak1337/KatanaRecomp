#include "katana/runtime/pvr.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <stdexcept>
#include <string>

namespace katana::runtime {

std::size_t PvrRegisterFile::index(const std::uint32_t offset) {
    if (offset >= pvr_register_size || (offset & 3u) != 0u) {
        throw std::out_of_range("Ungueltiger oder nicht ausgerichteter PVR-Registeroffset.");
    }
    return offset / 4u;
}

std::uint32_t PvrRegisterFile::read(const std::uint32_t offset) const {
    if (offset == pvr_register::Id) { return pvr_id; }
    if (offset == pvr_register::Revision) { return pvr_revision; }
    return registers_[index(offset)];
}

void PvrRegisterFile::write(const std::uint32_t offset, const std::uint32_t value) {
    static_cast<void>(index(offset));
    if (offset == pvr_register::Id || offset == pvr_register::Revision) {
        throw std::runtime_error("PVR-ID und Revision sind read-only.");
    }
    if (offset == pvr_register::SoftReset) {
        if ((value & 0x7u) != 0u) { reset(); }
        return;
    }
    if (offset == pvr_register::StartRender) {
        ++render_requests_;
        return;
    }
    registers_[index(offset)] = value;
}

void PvrRegisterFile::reset() noexcept {
    registers_.fill(0u);
    ++resets_;
}

std::uint64_t PvrRegisterFile::render_request_count() const noexcept { return render_requests_; }
std::uint64_t PvrRegisterFile::reset_count() const noexcept { return resets_; }

std::shared_ptr<PvrRegisterFile> map_pvr_registers(Memory& memory) {
    auto registers = std::make_shared<PvrRegisterFile>();
    auto device = std::make_shared<MmioMemoryDevice>(
        pvr_register_size,
        [registers](const std::uint32_t offset, const MemoryAccessWidth width) {
            if (width != MemoryAccessWidth::Word) {
                throw std::runtime_error("PVR-Register erfordern 32-Bit-Zugriffe.");
            }
            return registers->read(offset);
        },
        [registers](const std::uint32_t offset, const std::uint32_t value, const MemoryAccessWidth width) {
            if (width != MemoryAccessWidth::Word) {
                throw std::runtime_error("PVR-Register erfordern 32-Bit-Zugriffe.");
            }
            registers->write(offset, value);
        }
    );
    for (const auto segment : dreamcast_direct_segment_bases) {
        const auto base = segment + pvr_register_physical_base;
        memory.map_region("dreamcast-pvr-registers-" + std::to_string(base), base, device);
    }
    return registers;
}

} // namespace katana::runtime
