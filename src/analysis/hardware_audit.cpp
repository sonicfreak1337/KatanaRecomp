#include "katana/analysis/hardware_audit.hpp"

#include "katana/analysis/basic_blocks.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/json_report.hpp"
#include "katana/ir/lower.hpp"
#include "katana/runtime/native_port_semantics.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace katana::analysis {
namespace {

struct AddressDescription {
    std::uint32_t canonical = 0u;
    DreamcastHardwareRegion region = DreamcastHardwareRegion::Unknown;
    bool aperture_mapped = false;
    bool image_backed = false;
    std::string name;
};

bool in_range(const std::uint32_t value,
              const std::uint32_t base,
              const std::uint32_t size) noexcept {
    return value >= base &&
           static_cast<std::uint64_t>(value) < static_cast<std::uint64_t>(base) + size;
}

std::uint32_t canonical_address(const std::uint32_t address) noexcept {
    if ((address & 0xFC000000u) == 0x7C000000u) return address;
    if (address >= 0x80000000u && address < 0xE0000000u) return address & 0x1FFFFFFFu;
    if (address >= 0x1F000000u && address < 0x20000000u) return address | 0xE0000000u;
    return address;
}

std::string pvr_register_name(const std::uint32_t offset) {
    static constexpr std::array names{std::pair{0x000u, "ID"},
                                      std::pair{0x004u, "REVISION"},
                                      std::pair{0x008u, "SOFTRESET"},
                                      std::pair{0x014u, "STARTRENDER"},
                                      std::pair{0x020u, "PARAM_BASE"},
                                      std::pair{0x02Cu, "REGION_BASE"},
                                      std::pair{0x040u, "BORDER_COL"},
                                      std::pair{0x044u, "FB_R_CTRL"},
                                      std::pair{0x048u, "FB_W_CTRL"},
                                      std::pair{0x04Cu, "FB_W_LINESTRIDE"},
                                      std::pair{0x050u, "FB_R_SOF1"},
                                      std::pair{0x054u, "FB_R_SOF2"},
                                      std::pair{0x05Cu, "FB_R_SIZE"},
                                      std::pair{0x060u, "FB_W_SOF1"},
                                      std::pair{0x064u, "FB_W_SOF2"},
                                      std::pair{0x068u, "FB_X_CLIP"},
                                      std::pair{0x06Cu, "FB_Y_CLIP"},
                                      std::pair{0x074u, "FPU_SHAD_SCALE"},
                                      std::pair{0x078u, "FPU_CULL_VAL"},
                                      std::pair{0x07Cu, "FPU_PARAM_CFG"},
                                      std::pair{0x080u, "HALF_OFFSET"},
                                      std::pair{0x084u, "FPU_PERP_VAL"},
                                      std::pair{0x088u, "ISP_BACKGND_D"},
                                      std::pair{0x08Cu, "ISP_BACKGND_T"},
                                      std::pair{0x098u, "ISP_FEED_CFG"},
                                      std::pair{0x0B0u, "FOG_COL_RAM"},
                                      std::pair{0x0B4u, "FOG_COL_VERT"},
                                      std::pair{0x0B8u, "FOG_DENSITY"},
                                      std::pair{0x0C4u, "SPG_TRIGGER_POS"},
                                      std::pair{0x0C8u, "SPG_HBLANK_INT"},
                                      std::pair{0x0CCu, "SPG_VBLANK_INT"},
                                      std::pair{0x0D0u, "SPG_CONTROL"},
                                      std::pair{0x0D4u, "SPG_HBLANK"},
                                      std::pair{0x0D8u, "SPG_LOAD"},
                                      std::pair{0x0DCu, "SPG_VBLANK"},
                                      std::pair{0x0E0u, "SPG_WIDTH"},
                                      std::pair{0x0E4u, "TEXT_CONTROL"},
                                      std::pair{0x0E8u, "VO_CONTROL"},
                                      std::pair{0x0ECu, "VO_STARTX"},
                                      std::pair{0x0F0u, "VO_STARTY"},
                                      std::pair{0x0F4u, "SCALER_CTL"},
                                      std::pair{0x108u, "PAL_RAM_CTRL"},
                                      std::pair{0x10Cu, "SPG_STATUS"},
                                      std::pair{0x114u, "FB_R_SOF_CURRENT"},
                                      std::pair{0x11Cu, "PT_ALPHA_REF"},
                                      std::pair{0x124u, "TA_OL_BASE"},
                                      std::pair{0x128u, "TA_ISP_BASE"},
                                      std::pair{0x12Cu, "TA_OL_LIMIT"},
                                      std::pair{0x130u, "TA_ISP_LIMIT"},
                                      std::pair{0x134u, "TA_NEXT_OPB"},
                                      std::pair{0x138u, "TA_ITP_CURRENT"},
                                      std::pair{0x13Cu, "TA_GLOB_TILE_CLIP"},
                                      std::pair{0x140u, "TA_ALLOC_CTRL"},
                                      std::pair{0x144u, "TA_LIST_INIT"},
                                      std::pair{0x148u, "TA_YUV_TEX_BASE"},
                                      std::pair{0x14Cu, "TA_YUV_TEX_CTRL"},
                                      std::pair{0x150u, "TA_YUV_TEX_CNT"},
                                      std::pair{0x160u, "TA_LIST_CONT"},
                                      std::pair{0x164u, "TA_NEXT_OPB_INIT"}};
    const auto found = std::find_if(
        names.begin(), names.end(), [offset](const auto& item) { return item.first == offset; });
    if (found != names.end()) return found->second;
    if (offset >= 0x200u && offset < 0x400u) return "FOG_TABLE";
    if (offset >= 0x1000u && offset < 0x2000u) return "PALETTE_RAM";
    return {};
}

template <std::size_t Size>
std::string named_offset(const std::array<std::pair<std::uint32_t, const char*>, Size>& names,
                         const std::uint32_t offset) {
    const auto found = std::find_if(
        names.begin(), names.end(), [offset](const auto& item) { return item.first == offset; });
    return found == names.end() ? std::string{} : std::string{found->second};
}

std::string dreamcast_register_name(const DreamcastHardwareRegion region,
                                    const std::uint32_t offset) {
    using Pair = std::pair<std::uint32_t, const char*>;
    switch (region) {
    case DreamcastHardwareRegion::SystemBus: {
        static constexpr std::array names{
            Pair{0x00u, "SB_C2DSTAT"}, Pair{0x04u, "SB_C2DLEN"},  Pair{0x08u, "SB_C2DST"},
            Pair{0x10u, "SB_SDSTAW"},  Pair{0x14u, "SB_SDBAAW"},  Pair{0x18u, "SB_SDWLT"},
            Pair{0x1Cu, "SB_SDLAS"},   Pair{0x20u, "SB_SDST"},    Pair{0x40u, "SB_DBREQM"},
            Pair{0x44u, "SB_BAVLWC"},  Pair{0x48u, "SB_C2DPRYC"}, Pair{0x4Cu, "SB_C2DMAXL"},
            Pair{0x60u, "SB_SDDIV"},   Pair{0x80u, "SB_TFREM"},   Pair{0x84u, "SB_LMMODE0"},
            Pair{0x88u, "SB_LMMODE1"}, Pair{0x8Cu, "SB_FFST"},    Pair{0x90u, "SB_SFRES"},
            Pair{0x9Cu, "SB_SBREV"},   Pair{0xA0u, "SB_RBSPLT"}};
        return named_offset(names, offset);
    }
    case DreamcastHardwareRegion::SystemAsic: {
        static constexpr std::array names{Pair{0x00u, "ISTNRM"},
                                          Pair{0x04u, "ISTEXT"},
                                          Pair{0x08u, "ISTERR"},
                                          Pair{0x10u, "IML2NRM"},
                                          Pair{0x14u, "IML2EXT"},
                                          Pair{0x18u, "IML2ERR"},
                                          Pair{0x20u, "IML4NRM"},
                                          Pair{0x24u, "IML4EXT"},
                                          Pair{0x28u, "IML4ERR"},
                                          Pair{0x30u, "IML6NRM"},
                                          Pair{0x34u, "IML6EXT"},
                                          Pair{0x38u, "IML6ERR"},
                                          Pair{0x40u, "PDTNRM"},
                                          Pair{0x44u, "PDTEXT"},
                                          Pair{0x50u, "G2DTNRM"},
                                          Pair{0x54u, "G2DTEXT"}};
        return named_offset(names, offset);
    }
    case DreamcastHardwareRegion::Maple: {
        static constexpr std::array names{Pair{0x04u, "MDSTAR"},
                                          Pair{0x10u, "MDTSEL"},
                                          Pair{0x14u, "MDEN"},
                                          Pair{0x18u, "MDST"},
                                          Pair{0x80u, "MSYS"},
                                          Pair{0x84u, "MST"},
                                          Pair{0x88u, "MSHTCL"},
                                          Pair{0x8Cu, "MDAPRO"},
                                          Pair{0xE8u, "MMSEL"},
                                          Pair{0xF4u, "MTXDAD"},
                                          Pair{0xF8u, "MRXDAD"},
                                          Pair{0xFCu, "MRXDBD"}};
        return named_offset(names, offset);
    }
    case DreamcastHardwareRegion::GdRom: {
        static constexpr std::array names{Pair{0x80u, "GD_DATA"},
                                          Pair{0x84u, "GD_ERROR"},
                                          Pair{0x88u, "GD_INT_REASON"},
                                          Pair{0x90u, "GD_BYTE_COUNT_LOW"},
                                          Pair{0x94u, "GD_BYTE_COUNT_HIGH"},
                                          Pair{0x9Cu, "GD_COMMAND_STATUS"},
                                          Pair{0xA0u, "GD_ALT_STATUS_CONTROL"}};
        return named_offset(names, offset);
    }
    case DreamcastHardwareRegion::G1Dma: {
        static constexpr std::array names{Pair{0x04u, "GDSTAR"},
                                          Pair{0x08u, "GDLEN"},
                                          Pair{0x0Cu, "GDDIR"},
                                          Pair{0x14u, "GDEN"},
                                          Pair{0x18u, "GDST"},
                                          Pair{0xB0u, "G1SYSM"},
                                          Pair{0xF4u, "GDSTARD"},
                                          Pair{0xF8u, "GDLEND"}};
        return named_offset(names, offset);
    }
    case DreamcastHardwareRegion::G2Dma:
        if (offset < 0x80u && (offset & 3u) == 0u) {
            static constexpr std::array suffixes{
                "STAR", "STAG", "LEN", "DIR", "TSEL", "EN", "ST", "SUSP"};
            return "G2_CH" + std::to_string(offset / 0x20u) + '_' + suffixes[(offset & 0x1Fu) / 4u];
        }
        if (offset == 0xBCu) return "G2APRO";
        return {};
    case DreamcastHardwareRegion::PvrDma: {
        static constexpr std::array names{Pair{0x00u, "PDSTAP"},
                                          Pair{0x04u, "PDSTAR"},
                                          Pair{0x08u, "PDLEN"},
                                          Pair{0x0Cu, "PDDIR"},
                                          Pair{0x10u, "PDTSEL"},
                                          Pair{0x14u, "PDEN"},
                                          Pair{0x18u, "PDST"},
                                          Pair{0x80u, "PDAPRO"},
                                          Pair{0xF0u, "PDSTAPD"},
                                          Pair{0xF4u, "PDSTARD"},
                                          Pair{0xF8u, "PDLEND"}};
        return named_offset(names, offset);
    }
    case DreamcastHardwareRegion::Aica:
        if (offset == 0x2C00u) return "ARM_RESET";
        if (offset >= 0x2890u && offset <= 0x2898u && (offset & 3u) == 0u)
            return "TIMER_" + std::to_string((offset - 0x2890u) / 4u);
        if (offset == 0x28B4u) return "SCIEB";
        if (offset == 0x28B8u) return "SCIPD";
        if (offset == 0x28BCu) return "SCIRE";
        if (offset < 64u * 0x80u) return "CHANNEL_" + std::to_string(offset / 0x80u);
        return {};
    case DreamcastHardwareRegion::AicaRtc:
        return offset == 0u   ? "RTC_HIGH"
               : offset == 4u ? "RTC_LOW"
               : offset == 8u ? "RTC_ENABLE"
                              : std::string{};
    default:
        return {};
    }
}

std::string sh4_register_name(const std::uint32_t address) {
    switch (address) {
    case 0xFF000000u:
        return "PTEH";
    case 0xFF000004u:
        return "PTEL";
    case 0xFF000008u:
        return "TTB";
    case 0xFF00000Cu:
        return "TEA";
    case 0xFF000010u:
        return "MMUCR";
    case 0xFF00001Cu:
        return "CCR";
    case 0xFF000020u:
        return "TRA";
    case 0xFF000024u:
        return "EXPEVT";
    case 0xFF000028u:
        return "INTEVT";
    case 0xFF000034u:
        return "PTEA";
    case 0xFF000038u:
        return "QACR0";
    case 0xFF00003Cu:
        return "QACR1";
    case 0xFF80002Cu:
        return "PCTRA";
    case 0xFF800030u:
        return "PDTRA";
    case 0xFF800040u:
        return "PCTRB";
    case 0xFF800044u:
        return "PDTRB";
    case 0xFF800048u:
        return "GPIOIC";
    case 0xFFD00000u:
        return "ICR";
    case 0xFFD00004u:
        return "IPRA";
    case 0xFFD00008u:
        return "IPRB";
    case 0xFFD0000Cu:
        return "IPRC";
    case 0xFFD80000u:
        return "TOCR";
    case 0xFFD80004u:
        return "TSTR";
    case 0xFFE80000u:
        return "SCSMR2";
    case 0xFFE80004u:
        return "SCBRR2";
    case 0xFFE80008u:
        return "SCSCR2";
    case 0xFFE8000Cu:
        return "SCFTDR2";
    case 0xFFE80010u:
        return "SCFSR2";
    case 0xFFE80014u:
        return "SCFRDR2";
    case 0xFFE80018u:
        return "SCFCR2";
    case 0xFFE8001Cu:
        return "SCFDR2";
    case 0xFFE80020u:
        return "SCSPTR2";
    case 0xFFE80024u:
        return "SCLSR2";
    default:
        break;
    }
    if (in_range(address, 0xFFA00000u, 0x40u)) {
        const auto channel = (address - 0xFFA00000u) / 0x10u;
        const auto offset = (address - 0xFFA00000u) % 0x10u;
        const char* name = offset == 0u   ? "SAR"
                           : offset == 4u ? "DAR"
                           : offset == 8u ? "DMATCR"
                                          : "CHCR";
        return std::string(name) + std::to_string(channel);
    }
    if (address == 0xFFA00040u) return "DMAOR";
    if (in_range(address, 0xFFD80008u, 0x24u)) {
        const auto channel = (address - 0xFFD80008u) / 0x0Cu;
        const auto offset = (address - 0xFFD80008u) % 0x0Cu;
        const char* name = offset == 0u ? "TCOR" : offset == 4u ? "TCNT" : "TCR";
        return std::string(name) + std::to_string(channel);
    }
    return {};
}

AddressDescription describe(const std::uint32_t address) {
    const auto canonical = canonical_address(address);
    AddressDescription result{
        canonical, DreamcastHardwareRegion::Unknown, false, false, {}};
    const auto set = [&](const DreamcastHardwareRegion region,
                         const bool runtime_mapped,
                         std::string name = {}) {
        result.region = region;
        result.aperture_mapped = runtime_mapped;
        result.name = std::move(name);
    };
    if (in_range(canonical, 0x005F6800u, 0xB0u))
        set(DreamcastHardwareRegion::SystemBus, true);
    else if (in_range(canonical, 0x005F6900u, 0x58u))
        set(DreamcastHardwareRegion::SystemAsic, true);
    else if (in_range(canonical, 0x005F6C00u, 0x100u))
        set(DreamcastHardwareRegion::Maple, true);
    else if (in_range(canonical, 0x005F7000u, 0x100u))
        set(DreamcastHardwareRegion::GdRom, true);
    else if (in_range(canonical, 0x005F7400u, 0x100u))
        set(DreamcastHardwareRegion::G1Dma, true);
    else if (in_range(canonical, 0x005F7800u, 0x100u))
        set(DreamcastHardwareRegion::G2Dma, true);
    else if (in_range(canonical, 0x005F7C00u, 0x100u))
        set(DreamcastHardwareRegion::PvrDma, true);
    else if (in_range(canonical, 0x005F8000u, 0x2000u))
        set(DreamcastHardwareRegion::Pvr, true, pvr_register_name(canonical - 0x005F8000u));
    else if (in_range(canonical, 0x00700000u, 0x8000u))
        set(DreamcastHardwareRegion::Aica, true);
    else if (in_range(canonical, 0x00710000u, 0x0Cu))
        set(DreamcastHardwareRegion::AicaRtc, true);
    else if (in_range(canonical, 0x00800000u, 0x00800000u))
        set(DreamcastHardwareRegion::AicaRam, true);
    else if (in_range(canonical, 0x01000000u, 0x00800000u))
        set(DreamcastHardwareRegion::TaFifo, true);
    else if (in_range(canonical, 0x10800000u, 0x00800000u))
        set(DreamcastHardwareRegion::TaYuv, true);
    else if (in_range(canonical, 0x04000000u, 0x00800000u))
        set(DreamcastHardwareRegion::Vram64, true);
    else if (in_range(canonical, 0x05000000u, 0x00800000u))
        set(DreamcastHardwareRegion::Vram32, true);
    else if (in_range(canonical, 0x11000000u, 0x01000000u) ||
             in_range(canonical, 0x13000000u, 0x01000000u))
        set(DreamcastHardwareRegion::TaVram, true);
    else if (in_range(canonical, 0x7C000000u, 0x04000000u))
        set(DreamcastHardwareRegion::Sh4OnChipRam, true);
    else if (in_range(canonical, 0xE0000000u, 0x04000000u))
        set(DreamcastHardwareRegion::StoreQueue, true);
    else if (in_range(canonical, 0xFF000000u, 0x14u))
        set(DreamcastHardwareRegion::Sh4Mmu, true, sh4_register_name(canonical));
    else if (canonical == 0xFF00001Cu)
        set(DreamcastHardwareRegion::Sh4Cache, true, sh4_register_name(canonical));
    else if (in_range(canonical, 0xFF000020u, 0x0Cu))
        set(DreamcastHardwareRegion::Sh4Exception, true, sh4_register_name(canonical));
    else if (canonical == 0xFF000034u)
        set(DreamcastHardwareRegion::Sh4Mmu, true, sh4_register_name(canonical));
    else if (in_range(canonical, 0xFF000038u, 8u))
        set(DreamcastHardwareRegion::Sh4Qacr, true, sh4_register_name(canonical));
    else if (canonical == 0xFF80002Cu || canonical == 0xFF800030u || canonical == 0xFF800040u ||
             canonical == 0xFF800044u || canonical == 0xFF800048u)
        set(DreamcastHardwareRegion::Sh4Io, true, sh4_register_name(canonical));
    else if (in_range(canonical, 0xFFA00000u, 0x44u))
        set(DreamcastHardwareRegion::Sh4Dmac, true, sh4_register_name(canonical));
    else if (in_range(canonical, 0xFFC80000u, 0x40u))
        set(DreamcastHardwareRegion::Sh4Rtc, true);
    else if (in_range(canonical, 0xFFD00000u, 0x14u))
        set(DreamcastHardwareRegion::Sh4Intc, true, sh4_register_name(canonical));
    else if (in_range(canonical, 0xFFD80000u, 0x30u))
        set(DreamcastHardwareRegion::Sh4Tmu, true, sh4_register_name(canonical));
    else if (in_range(canonical, 0xFFE80000u, 0x28u))
        set(DreamcastHardwareRegion::Sh4Scif, true, sh4_register_name(canonical));
    else if (canonical >= 0xFF000000u)
        set(DreamcastHardwareRegion::Sh4P4, false);
    if (result.name.empty() && result.region != DreamcastHardwareRegion::Unknown &&
        canonical < 0xE0000000u) {
        std::uint32_t base = canonical;
        switch (result.region) {
        case DreamcastHardwareRegion::SystemBus:
            base = 0x005F6800u;
            break;
        case DreamcastHardwareRegion::SystemAsic:
            base = 0x005F6900u;
            break;
        case DreamcastHardwareRegion::Maple:
            base = 0x005F6C00u;
            break;
        case DreamcastHardwareRegion::GdRom:
            base = 0x005F7000u;
            break;
        case DreamcastHardwareRegion::G1Dma:
            base = 0x005F7400u;
            break;
        case DreamcastHardwareRegion::G2Dma:
            base = 0x005F7800u;
            break;
        case DreamcastHardwareRegion::PvrDma:
            base = 0x005F7C00u;
            break;
        case DreamcastHardwareRegion::Aica:
            base = 0x00700000u;
            break;
        case DreamcastHardwareRegion::AicaRtc:
            base = 0x00710000u;
            break;
        default:
            break;
        }
        result.name = dreamcast_register_name(result.region, canonical - base);
    }
    return result;
}

AddressDescription describe_image_access(
    const io::ExecutableImage& image,
    const std::uint32_t instruction_address,
    const sh4::InstructionKind instruction_kind,
    const std::uint32_t address,
    const std::uint8_t width) {
    // A latent runtime module is analyzed at an identity-bound synthetic
    // source address until its loader-selected runtime base is known.  A
    // directly PC-relative module-local literal access must therefore retain
    // source-image authority even when that synthetic address happens to
    // alias a Dreamcast hardware aperture after SH-4 canonicalization. Keep
    // the exception narrower than propagated pointer accesses: a hardware
    // address loaded from such a literal must still classify as MMIO when it
    // is dereferenced by a later instruction.
    const auto* instruction_segment =
        image.find_segment(instruction_address, 2u);
    const auto* instruction_identity =
        image.find_immutable_range(instruction_address, 2u);
    const auto* access_identity =
        width == 0u ? nullptr : image.find_immutable_range(address, width);
    if (instruction_segment != nullptr &&
        instruction_segment->source_kind == io::ImageSourceKind::DiscModule &&
        instruction_segment->load_phase == io::ImageLoadPhase::RuntimeModule &&
        (instruction_kind == sh4::InstructionKind::MovWordLoadPcRelative ||
         instruction_kind == sh4::InstructionKind::MovLongLoadPcRelative) &&
        instruction_identity != nullptr && access_identity != nullptr &&
        instruction_identity->generation == access_identity->generation &&
        instruction_identity->identity == access_identity->identity) {
        const auto* access_segment = image.find_segment(address, width);
        if (access_segment == instruction_segment) {
            auto result = describe(address);
            result.canonical = address;
            result.region = DreamcastHardwareRegion::Unknown;
            result.aperture_mapped = false;
            result.image_backed = true;
            result.name = "runtime-module-local-data";
            return result;
        }
    }

    auto result = describe(address);
    // A statically resolved address backed by the identity-bound input image is
    // source/module data, even when its synthetic analysis address (for
    // example 0x88...) canonicalizes outside native main RAM.  Hardware ranges
    // retain precedence: an invalid image segment at a real MMIO aperture must
    // never hide a device dependency.
    if (result.region == DreamcastHardwareRegion::Unknown && width != 0u &&
        image.resolve_segment_address(address, width).has_value()) {
        result.image_backed = true;
        result.name = "source-bound-data";
    }
    return result;
}

template <std::size_t Size>
bool contains(const std::array<std::uint32_t, Size>& values, const std::uint32_t value) noexcept {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool system_asic_offset(const std::uint32_t offset) noexcept {
    if (offset <= 0x08u && (offset & 3u) == 0u) return true;
    if (offset >= 0x10u && offset <= 0x38u && (offset & 3u) == 0u)
        return ((offset - 0x10u) / 4u) % 4u < 3u;
    return offset == 0x40u || offset == 0x44u || offset == 0x50u || offset == 0x54u;
}

HardwareRuntimeSupport system_bus_support(const std::uint32_t offset,
                                          const HardwareAccessKind kind) noexcept {
    static constexpr std::array readable{0x00u,
                                         0x04u,
                                         0x08u,
                                         0x10u,
                                         0x14u,
                                         0x18u,
                                         0x1Cu,
                                         0x20u,
                                         0x40u,
                                         0x44u,
                                         0x48u,
                                         0x4Cu,
                                         0x60u,
                                         0x80u,
                                         0x84u,
                                         0x88u,
                                         0x8Cu,
                                         0x9Cu,
                                         0xA0u};
    static constexpr std::array writable{0x00u,
                                         0x04u,
                                         0x08u,
                                         0x10u,
                                         0x14u,
                                         0x18u,
                                         0x1Cu,
                                         0x20u,
                                         0x40u,
                                         0x44u,
                                         0x48u,
                                         0x4Cu,
                                         0x60u,
                                         0x84u,
                                         0x88u,
                                         0x90u,
                                         0xA0u,
                                         0xA4u,
                                         0xACu};
    if (kind == HardwareAccessKind::Read)
        return contains(readable, offset) ? HardwareRuntimeSupport::Implemented
                                          : HardwareRuntimeSupport::Rejected;
    if (kind == HardwareAccessKind::Write) {
        if (offset == 0x20u) return HardwareRuntimeSupport::KnownGap;
        return contains(writable, offset) ? HardwareRuntimeSupport::Implemented
                                          : HardwareRuntimeSupport::Rejected;
    }
    return HardwareRuntimeSupport::Rejected;
}

HardwareRuntimeSupport maple_support(const std::uint32_t offset,
                                     const HardwareAccessKind kind) noexcept {
    static constexpr std::array readable{
        0x04u, 0x10u, 0x14u, 0x18u, 0x80u, 0x84u, 0xE8u, 0xF4u, 0xF8u, 0xFCu};
    static constexpr std::array writable{0x04u, 0x10u, 0x14u, 0x18u, 0x80u, 0x88u, 0x8Cu, 0xE8u};
    if (kind == HardwareAccessKind::Prefetch) return HardwareRuntimeSupport::Rejected;
    const auto supported =
        kind == HardwareAccessKind::Read ? contains(readable, offset) : contains(writable, offset);
    return supported ? HardwareRuntimeSupport::Implemented : HardwareRuntimeSupport::Rejected;
}

HardwareRuntimeSupport gdrom_support(const std::uint32_t offset,
                                     const HardwareAccessKind kind,
                                     const std::uint8_t width) noexcept {
    if (kind == HardwareAccessKind::Prefetch) return HardwareRuntimeSupport::Rejected;
    if (offset == 0x80u)
        return width == 2u ? HardwareRuntimeSupport::Implemented : HardwareRuntimeSupport::Rejected;
    if (width != 1u) return HardwareRuntimeSupport::Rejected;
    static constexpr std::array readable{0x84u, 0x88u, 0x90u, 0x94u, 0x9Cu, 0xA0u};
    static constexpr std::array writable{0x90u, 0x94u, 0x9Cu, 0xA0u};
    const auto supported =
        kind == HardwareAccessKind::Read ? contains(readable, offset) : contains(writable, offset);
    return supported ? HardwareRuntimeSupport::Implemented : HardwareRuntimeSupport::Rejected;
}

HardwareRuntimeSupport holly_dma_support(const DreamcastHardwareRegion region,
                                         const std::uint32_t offset,
                                         const HardwareAccessKind kind) noexcept {
    if (kind == HardwareAccessKind::Prefetch) return HardwareRuntimeSupport::Rejected;
    if (region == DreamcastHardwareRegion::G2Dma) {
        if (offset < 0x80u && (offset & 3u) == 0u) return HardwareRuntimeSupport::Implemented;
        static constexpr std::array readable{0x80u,
                                             0x90u,
                                             0x94u,
                                             0x98u,
                                             0x9Cu,
                                             0xC0u,
                                             0xC4u,
                                             0xC8u,
                                             0xD0u,
                                             0xD4u,
                                             0xD8u,
                                             0xE0u,
                                             0xE4u,
                                             0xE8u,
                                             0xF0u,
                                             0xF4u,
                                             0xF8u};
        static constexpr std::array writable{0x90u, 0x94u, 0x98u, 0x9Cu, 0xBCu};
        const auto supported = kind == HardwareAccessKind::Read ? contains(readable, offset)
                                                                : contains(writable, offset);
        return supported ? HardwareRuntimeSupport::Implemented : HardwareRuntimeSupport::Rejected;
    }
    if (region == DreamcastHardwareRegion::G1Dma) {
        static constexpr std::array readable{
            0x04u, 0x08u, 0x0Cu, 0x14u, 0x18u, 0xB0u, 0xF4u, 0xF8u};
        static constexpr std::array writable{0x04u, 0x08u, 0x0Cu, 0x14u, 0x18u};
        static constexpr std::array ignored_writes{
            0x80u, 0x84u, 0x88u, 0x8Cu, 0x90u, 0x94u, 0xA0u, 0xA4u, 0xB4u, 0xB8u, 0xE4u};
        if (kind == HardwareAccessKind::Write && contains(ignored_writes, offset))
            return HardwareRuntimeSupport::Partial;
        const auto supported = kind == HardwareAccessKind::Read ? contains(readable, offset)
                                                                : contains(writable, offset);
        return supported ? HardwareRuntimeSupport::Implemented : HardwareRuntimeSupport::Rejected;
    }
    static constexpr std::array readable{
        0x00u, 0x04u, 0x08u, 0x0Cu, 0x10u, 0x14u, 0x18u, 0xF0u, 0xF4u, 0xF8u};
    static constexpr std::array writable{0x00u, 0x04u, 0x08u, 0x0Cu, 0x10u, 0x14u, 0x18u, 0x80u};
    const auto supported =
        kind == HardwareAccessKind::Read ? contains(readable, offset) : contains(writable, offset);
    return supported ? HardwareRuntimeSupport::Implemented : HardwareRuntimeSupport::Rejected;
}

HardwareRuntimeSupport sh4_tmu_support(const std::uint32_t address,
                                       const std::uint8_t width) noexcept {
    const auto offset = address - 0xFFD80000u;
    if ((offset == 0x00u || offset == 0x04u) && width == 1u)
        return HardwareRuntimeSupport::Implemented;
    if (offset >= 0x08u && offset <= 0x28u) {
        const auto local = (offset - 0x08u) % 0x0Cu;
        if ((local == 0u || local == 4u) && width == 4u) return HardwareRuntimeSupport::Implemented;
        if (local == 8u && width == 2u) return HardwareRuntimeSupport::Implemented;
    }
    return HardwareRuntimeSupport::Rejected;
}

HardwareRuntimeSupport sh4_scif_support(const std::uint32_t address,
                                        const HardwareAccessKind kind,
                                        const std::uint8_t width) noexcept {
    if (kind == HardwareAccessKind::Prefetch) return HardwareRuntimeSupport::Rejected;
    const auto offset = address - 0xFFE80000u;
    const auto byte_register = offset == 0x04u || offset == 0x0Cu || offset == 0x14u;
    if (width != (byte_register ? 1u : 2u)) return HardwareRuntimeSupport::Rejected;
    if (kind == HardwareAccessKind::Read && offset == 0x0Cu)
        return HardwareRuntimeSupport::Rejected;
    if (kind == HardwareAccessKind::Write && (offset == 0x14u || offset == 0x1Cu))
        return HardwareRuntimeSupport::Rejected;
    static constexpr std::array offsets{
        0x00u, 0x04u, 0x08u, 0x0Cu, 0x10u, 0x14u, 0x18u, 0x1Cu, 0x20u, 0x24u};
    return contains(offsets, offset) ? HardwareRuntimeSupport::Implemented
                                     : HardwareRuntimeSupport::Rejected;
}

HardwareRuntimeSupport pvr_support(const std::uint32_t offset,
                                   const HardwareAccessKind kind,
                                   const std::uint8_t width) noexcept {
    if (width != 4u || kind == HardwareAccessKind::Prefetch || (offset & 3u) != 0u)
        return HardwareRuntimeSupport::Rejected;
    // These scan-generator and scanout registers have complete product-path
    // semantics: register masks/state, guest-time status and native blank/border output.
    if (offset == 0x040u || offset == 0x0DCu || offset == 0x0E8u)
        return HardwareRuntimeSupport::Implemented;
    if (offset == 0x10Cu)
        return kind == HardwareAccessKind::Read ? HardwareRuntimeSupport::Implemented
                                                : HardwareRuntimeSupport::Rejected;
    return HardwareRuntimeSupport::Partial;
}

HardwareRuntimeSupport assess_support(const AddressDescription& description,
                                      const HardwareAccessKind kind,
                                      const std::uint8_t width) noexcept {
    using Region = DreamcastHardwareRegion;
    if (!description.aperture_mapped) return HardwareRuntimeSupport::Unmapped;
    const auto address = description.canonical;
    switch (description.region) {
    case Region::SystemBus:
        if (width != 4u) return HardwareRuntimeSupport::Rejected;
        return system_bus_support(address - 0x005F6800u, kind);
    case Region::SystemAsic:
        return width == 4u && kind != HardwareAccessKind::Prefetch &&
                       system_asic_offset(address - 0x005F6900u)
                   ? HardwareRuntimeSupport::Implemented
                   : HardwareRuntimeSupport::Rejected;
    case Region::Maple:
        if (width != 4u) return HardwareRuntimeSupport::Rejected;
        return maple_support(address - 0x005F6C00u, kind);
    case Region::GdRom:
        return gdrom_support(address - 0x005F7000u, kind, width);
    case Region::G1Dma:
    case Region::G2Dma:
    case Region::PvrDma:
        if (width != 4u) return HardwareRuntimeSupport::Rejected;
        return holly_dma_support(description.region,
                                 address - (description.region == Region::G1Dma   ? 0x005F7400u
                                            : description.region == Region::G2Dma ? 0x005F7800u
                                                                                  : 0x005F7C00u),
                                 kind);
    case Region::Pvr:
        return pvr_support(address - 0x005F8000u, kind, width);
    case Region::Aica:
        return kind != HardwareAccessKind::Prefetch && (width == 1u || width == 2u || width == 4u)
                   ? HardwareRuntimeSupport::Partial
                   : HardwareRuntimeSupport::Rejected;
    case Region::AicaRtc:
        return kind != HardwareAccessKind::Prefetch &&
                       (width == 1u || width == 2u || width == 4u) &&
                       ((address - 0x00710000u) == 0u || (address - 0x00710000u) == 4u ||
                        (address - 0x00710000u) == 8u)
                   ? HardwareRuntimeSupport::Implemented
                   : HardwareRuntimeSupport::Rejected;
    case Region::AicaRam:
    case Region::Vram64:
    case Region::Vram32:
        return kind != HardwareAccessKind::Prefetch ? HardwareRuntimeSupport::Implemented
                                                    : HardwareRuntimeSupport::Rejected;
    case Region::TaFifo:
    case Region::TaYuv:
    case Region::TaVram:
        return kind == HardwareAccessKind::Write ? HardwareRuntimeSupport::Implemented
                                                 : HardwareRuntimeSupport::Rejected;
    case Region::StoreQueue:
        return HardwareRuntimeSupport::Implemented;
    case Region::Sh4OnChipRam:
        return kind != HardwareAccessKind::Prefetch && (width == 1u || width == 2u || width == 4u)
                   ? HardwareRuntimeSupport::Implemented
                   : HardwareRuntimeSupport::Rejected;
    case Region::Sh4Mmu:
    case Region::Sh4Cache:
    case Region::Sh4Exception:
    case Region::Sh4Qacr:
    case Region::Sh4Dmac:
        return width == 4u && kind != HardwareAccessKind::Prefetch
                   ? HardwareRuntimeSupport::Implemented
                   : HardwareRuntimeSupport::Rejected;
    case Region::Sh4Io:
        return kind != HardwareAccessKind::Prefetch ? HardwareRuntimeSupport::Implemented
                                                    : HardwareRuntimeSupport::Rejected;
    case Region::Sh4Intc:
        return width == 2u && kind != HardwareAccessKind::Prefetch
                   ? HardwareRuntimeSupport::Implemented
                   : HardwareRuntimeSupport::Rejected;
    case Region::Sh4Tmu:
        return kind == HardwareAccessKind::Prefetch ? HardwareRuntimeSupport::Rejected
                                                    : sh4_tmu_support(address, width);
    case Region::Sh4Scif:
        return sh4_scif_support(address, kind, width);
    case Region::Sh4Rtc:
        return kind != HardwareAccessKind::Prefetch &&
                       ((address == 0xFFC8001Cu && width == 2u) || width == 1u)
                   ? HardwareRuntimeSupport::Implemented
                   : HardwareRuntimeSupport::Rejected;
    case Region::Sh4P4:
    case Region::Unknown:
        return HardwareRuntimeSupport::Unmapped;
    }
    return HardwareRuntimeSupport::Unmapped;
}

const char* support_reason(const AddressDescription& description,
                           const HardwareRuntimeSupport support) noexcept {
    if (support == HardwareRuntimeSupport::KnownGap &&
        description.region == DreamcastHardwareRegion::SystemBus)
        return "sort_dma_transfer_path_missing";
    if (support == HardwareRuntimeSupport::Partial) {
        if (description.region == DreamcastHardwareRegion::Pvr)
            return "register_backed_renderer_capability_is_value_dependent";
        if (description.region == DreamcastHardwareRegion::Aica)
            return description.canonical == 0x00702C00u
                       ? "arm_reset_modeled_native_arm7_execution_missing"
                       : "hle_audio_register_subset";
        if (description.region == DreamcastHardwareRegion::G1Dma)
            return "accepted_compatibility_register_has_no_effect";
        return "partial_product_semantics";
    }
    if (support == HardwareRuntimeSupport::Rejected)
        return "runtime_rejects_access_width_direction_or_offset";
    if (support == HardwareRuntimeSupport::Unmapped) return "runtime_aperture_missing";
    return "product_path_implemented";
}

struct EffectiveAccess {
    std::uint32_t address = 0u;
    HardwareAccessKind kind = HardwareAccessKind::Read;
    std::uint8_t width = 0u;
    bool address_known = true;
    std::string address_expression;
};

struct EffectiveAccessSet {
    std::vector<EffectiveAccess> accesses;
    bool complete = false;
};

// A small class of SH-4 startup routines walks an image-owned table of
// {destination,value} pairs and performs one indirect store per row.  The
// ordinary constant lattice quite deliberately does not turn the destination
// loaded from that table into one guessed scalar address.  Keep this audit
// side resolver equally conservative: it only admits a table when the table
// pointer is an immutable, initial-image value, the load/store shape proves
// that the dynamic store consumes the table destination, and a terminator is
// observed within a hard bound.
struct TableDrivenAccessResolution final {
    bool recognized = false;
    EffectiveAccessSet access_set;
};

constexpr std::size_t maximum_hardware_table_entries = 256u;
constexpr std::size_t maximum_hardware_table_lookback = 64u;

bool is_identity_bound_initial_segment(const io::ExecutableImage& image,
                                       const io::ImageSegment& segment) noexcept {
    if (!segment.permissions.readable ||
        segment.load_phase != io::ImageLoadPhase::Initial ||
        segment.source_kind == io::ImageSourceKind::RuntimeMemory)
        return false;
    if (!segment.permissions.writable) return true;
    return image.initial_snapshot_policy() ==
           io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent;
}

std::optional<std::uint32_t> read_identity_bound_table_word(
    const io::ExecutableImage& image,
    const io::ImageSegment& segment,
    const std::uint32_t address) {
    if (!segment.contains(address, 4u) || !is_identity_bound_initial_segment(image, segment))
        return std::nullopt;
    try {
        return image.read_u32_le(address);
    } catch (...) {
        // A malformed or partially committed source segment is not a table
        // proof.  The hardware audit must remain fail-closed here.
        return std::nullopt;
    }
}

TableDrivenAccessResolution scan_identity_bound_hardware_table(
    const io::ExecutableImage& image,
    const std::uint32_t table_address) {
    TableDrivenAccessResolution result;
    const auto* table_segment = image.find_segment(table_address, 8u);
    if (table_segment == nullptr ||
        !is_identity_bound_initial_segment(image, *table_segment))
        return result;

    result.recognized = true;
    result.access_set.complete = false;
    for (std::size_t index = 0u; index < maximum_hardware_table_entries; ++index) {
        const auto entry_offset = static_cast<std::uint64_t>(index) * 8u;
        const auto entry_address = static_cast<std::uint64_t>(table_address) + entry_offset;
        if (entry_address > static_cast<std::uint64_t>(
                                std::numeric_limits<std::uint32_t>::max()) - 4u) {
            result.access_set.accesses.clear();
            return result;
        }
        const auto entry = static_cast<std::uint32_t>(entry_address);
        // Requiring the complete pair to remain in the same identity-bound
        // segment prevents a sentinel in a neighboring/latent image from
        // completing this table accidentally.
        const auto destination = read_identity_bound_table_word(image, *table_segment, entry);
        if (!destination.has_value()) {
            result.access_set.accesses.clear();
            return result;
        }
        if (*destination == 0u) {
            result.access_set.complete = true;
            return result;
        }
        const auto value = read_identity_bound_table_word(image, *table_segment, entry + 4u);
        if (!value.has_value()) {
            result.access_set.accesses.clear();
            return result;
        }
        // The owner contract writes a 32-bit value to a 32-bit destination.
        // An unaligned target is not silently normalized into a different
        // hardware register and therefore invalidates the table proof.
        if ((*destination & 3u) != 0u) {
            result.access_set.accesses.clear();
            result.access_set.complete = false;
            return result;
        }
        result.access_set.accesses.push_back(
            {*destination, HardwareAccessKind::Write, 4u});
    }
    // A bound was reached without a null terminator.  Preserve no partial
    // target set: callers will keep the dynamic site unresolved instead of
    // converting a truncated scan into a positive closure claim.
    result.access_set.accesses.clear();
    result.access_set.complete = false;
    return result;
}

bool is_pc_relative_table_pointer_load(const sh4::DisassemblyLine& line) noexcept {
    return line.instruction.kind == sh4::InstructionKind::MovLongLoadPcRelative;
}

bool is_table_destination_load(const sh4::DisassemblyLine& line,
                               const std::uint8_t table_register,
                               const std::uint8_t destination_register) noexcept {
    return line.instruction.kind == sh4::InstructionKind::MovLongLoad &&
           line.instruction.source_register == table_register &&
           line.instruction.destination_register == destination_register;
}

bool is_table_value_load(const sh4::DisassemblyLine& line,
                         const std::uint8_t table_register,
                         const std::uint8_t value_register) noexcept {
    return line.instruction.kind == sh4::InstructionKind::MovLongLoadDisplacement &&
           line.instruction.source_register == table_register &&
           line.instruction.destination_register == value_register &&
           line.instruction.displacement == 4;
}

bool register_is_unmodified(
    const std::span<const sh4::DisassemblyLine> lines,
    const std::size_t first,
    const std::size_t last,
    const std::uint8_t register_index) noexcept {
    if (first > last || last > lines.size() || register_index >= 16u) return false;
    const auto mask = static_cast<std::uint16_t>(1u << register_index);
    for (std::size_t index = first; index < last; ++index)
        if ((general_register_write_mask(lines[index].instruction) & mask) != 0u)
            return false;
    return true;
}

bool has_table_terminator_guard(
    const std::span<const sh4::DisassemblyLine> lines,
    const std::size_t loop_load_index,
    const std::size_t store_index) noexcept {
    if (loop_load_index >= store_index || store_index >= lines.size()) return false;
    const auto& loop_load = lines[loop_load_index].instruction;
    if (loop_load.kind != sh4::InstructionKind::MovLongLoad) return false;
    const auto terminator_register = loop_load.destination_register;
    for (std::size_t test_index = loop_load_index + 1u;
         test_index + 1u < store_index;
         ++test_index) {
        const auto& test = lines[test_index].instruction;
        if (test.kind != sh4::InstructionKind::TestRegister ||
            test.source_register != terminator_register ||
            test.destination_register != terminator_register ||
            !register_is_unmodified(
                lines, loop_load_index + 1u, test_index, terminator_register))
            continue;
        const auto& branch_line = lines[test_index + 1u];
        if (branch_line.instruction.control_flow !=
                sh4::ControlFlowKind::ConditionalBranch ||
            (branch_line.instruction.kind != sh4::InstructionKind::Bt &&
             branch_line.instruction.kind != sh4::InstructionKind::BtS) ||
            !branch_line.target_address.has_value() ||
            *branch_line.target_address <= lines[store_index].address)
            continue;
        return true;
    }
    return false;
}

bool has_bounded_table_backedge(
    const std::span<const sh4::DisassemblyLine> lines,
    const std::size_t pointer_index,
    const std::size_t store_index,
    const std::uint8_t table_register) noexcept {
    if (store_index + 2u >= lines.size()) return false;
    const auto& branch_line = lines[store_index + 1u];
    const auto& delay_line = lines[store_index + 2u];
    if (branch_line.address != lines[store_index].address + 2u ||
        delay_line.address != branch_line.address + 2u ||
        branch_line.instruction.control_flow !=
            sh4::ControlFlowKind::UnconditionalBranch ||
        !branch_line.target_address.has_value() ||
        *branch_line.target_address <= lines[pointer_index].address ||
        *branch_line.target_address >= lines[store_index].address ||
        !delay_line.is_delay_slot ||
        delay_line.instruction.kind != sh4::InstructionKind::AddImmediate ||
        delay_line.instruction.destination_register != table_register ||
        delay_line.instruction.immediate != 8)
        return false;
    const auto loop_load = std::find_if(
        lines.begin() + static_cast<std::ptrdiff_t>(pointer_index + 1u),
        lines.begin() + static_cast<std::ptrdiff_t>(store_index),
        [&](const auto& line) {
            return line.address == *branch_line.target_address &&
                   line.instruction.kind == sh4::InstructionKind::MovLongLoad &&
                   line.instruction.source_register == table_register;
        });
    if (loop_load == lines.begin() + static_cast<std::ptrdiff_t>(store_index))
        return false;
    const auto loop_load_index =
        static_cast<std::size_t>(loop_load - lines.begin());
    return register_is_unmodified(
               lines, pointer_index + 1u, loop_load_index, table_register) &&
           has_table_terminator_guard(lines, loop_load_index, store_index);
}

bool contiguous_instruction_window(
    const std::span<const sh4::DisassemblyLine> lines,
    const std::size_t first,
    const std::size_t last) noexcept {
    if (first > last || last >= lines.size()) return false;
    for (std::size_t index = first + 1u; index <= last; ++index)
        if (lines[index].address != lines[index - 1u].address + 2u) return false;
    return true;
}

bool table_window_has_only_local_control_flow(
    const std::span<const sh4::DisassemblyLine> lines,
    const std::size_t first,
    const std::size_t last) noexcept {
    for (std::size_t index = first; index <= last; ++index) {
        const auto flow = lines[index].instruction.control_flow;
        if (flow != sh4::ControlFlowKind::None &&
            flow != sh4::ControlFlowKind::ConditionalBranch)
            return false;
    }
    return true;
}

TableDrivenAccessResolution resolve_table_driven_store(
    const io::ExecutableImage& image,
    const std::span<const sh4::DisassemblyLine> lines,
    const std::size_t store_index,
    std::unordered_map<std::uint32_t, TableDrivenAccessResolution>& table_cache) {
    TableDrivenAccessResolution unresolved;
    if (store_index >= lines.size() ||
        lines[store_index].instruction.kind != sh4::InstructionKind::MovLongStore)
        return unresolved;
    const auto first = store_index > maximum_hardware_table_lookback
                           ? store_index - maximum_hardware_table_lookback
                           : 0u;
    for (std::size_t pointer_index = first; pointer_index < store_index; ++pointer_index) {
        if (!is_pc_relative_table_pointer_load(lines[pointer_index])) continue;
        if (!contiguous_instruction_window(lines, pointer_index, store_index)) continue;
        // A table proof may not cross a return, call, or unconditional branch.
        // Conditional loop guards are permitted because the owner pattern
        // tests the destination sentinel before the indirect store.
        if (!table_window_has_only_local_control_flow(lines, pointer_index, store_index))
            continue;
        const auto window = lines.subspan(pointer_index, store_index - pointer_index + 1u);
        const auto trace = propagate_local_constants(window, image);
        if (trace.empty()) continue;
        const auto& pointer_load = lines[pointer_index].instruction;
        const auto table_pointer = trace.front().after.registers[
            pointer_load.destination_register];
        if (!table_pointer.has_value() ||
            trace.front().after.sources[pointer_load.destination_register].find(
                "guarded-writable") != std::string::npos)
            continue;
        const auto cached = table_cache.find(*table_pointer);
        const auto& table = cached != table_cache.end()
                                ? cached->second
                                : table_cache.emplace(
                                      *table_pointer,
                                      scan_identity_bound_hardware_table(image, *table_pointer))
                                      .first->second;
        if (!table.recognized) continue;
        const auto table_register = pointer_load.destination_register;
        if (!has_bounded_table_backedge(
                lines, pointer_index, store_index, table_register))
            continue;
        const auto store_value_register =
            lines[store_index].instruction.source_register;
        for (std::size_t destination_index = pointer_index + 1u;
             destination_index < store_index;
             ++destination_index) {
            const auto destination_register =
                lines[destination_index].instruction.destination_register;
            if (!is_table_destination_load(lines[destination_index],
                                           table_register,
                                           destination_register))
                continue;
            const auto destination_trace_index = destination_index - pointer_index;
            if (destination_trace_index >= trace.size() ||
                trace[destination_trace_index].before.registers[table_register] !=
                    table_pointer ||
                !register_is_unmodified(
                    lines, pointer_index + 1u, destination_index, table_register))
                continue;
            bool has_value_load = false;
            std::size_t value_index = 0u;
            for (std::size_t candidate_index = pointer_index + 1u;
                 candidate_index < store_index;
                 ++candidate_index) {
                if (is_table_value_load(
                        lines[candidate_index], table_register, store_value_register) &&
                    candidate_index - pointer_index < trace.size() &&
                    trace[candidate_index - pointer_index]
                            .before.registers[table_register] == table_pointer &&
                    register_is_unmodified(
                        lines, pointer_index + 1u, candidate_index, table_register)) {
                    has_value_load = true;
                    value_index = candidate_index;
                    break;
                }
            }
            if (!has_value_load ||
                lines[store_index].instruction.destination_register != destination_register ||
                !register_is_unmodified(
                    lines, destination_index + 1u, store_index, destination_register) ||
                !register_is_unmodified(
                    lines, value_index + 1u, store_index, store_value_register))
                continue;
            return table;
        }
    }
    return unresolved;
}

std::uint32_t project_runtime_image_access(
    const io::ExecutableImage& image,
    const std::uint32_t instruction_address,
    const std::uint32_t address,
    const std::uint8_t width) noexcept {
    if (width == 0u) return address;
    const auto instruction =
        static_cast<std::uint64_t>(instruction_address);
    for (const auto& alias : image.address_aliases()) {
        const auto source_begin =
            static_cast<std::uint64_t>(alias.source_start);
        const auto source_end = source_begin + alias.size;
        if (instruction < source_begin || instruction + 2u > source_end)
            continue;

        std::optional<std::uint64_t> offset;
        const auto access_begin = static_cast<std::uint64_t>(address);
        if (access_begin >= source_begin &&
            access_begin + width <= source_end) {
            offset = access_begin - source_begin;
        } else if (address < 0xE0000000u &&
                   alias.source_start < 0xE0000000u) {
            const auto access_physical =
                static_cast<std::uint64_t>(address & 0x1FFFFFFFu);
            const auto source_physical =
                static_cast<std::uint64_t>(
                    alias.source_start & 0x1FFFFFFFu);
            if (access_physical >= source_physical &&
                access_physical + width <=
                    source_physical + alias.size)
                offset = access_physical - source_physical;
        }
        if (!offset.has_value()) return address;
        const auto projected =
            static_cast<std::uint64_t>(alias.runtime_start) + *offset;
        if (projected + width > 0x1'0000'0000ull) return address;
        return static_cast<std::uint32_t>(projected);
    }
    return address;
}

bool is_memory_access_instruction(const sh4::InstructionKind kind) noexcept {
    const auto operation = ir::lowering_operation_for_instruction(kind);
    const auto effects = ir::instruction_memory_effects(operation);
    // PREF is address-dependent and intentionally has no unconditional IR memory effect,
    // but it remains a hardware-aperture access for this audit.
    return effects.access != ir::MemoryAccessKind::None || kind == sh4::InstructionKind::Prefetch;
}

bool is_syntactic_memory_read(sh4::InstructionKind kind) noexcept;

struct MemoryAccessShape final {
    std::uint8_t access_mask = 0u;
    std::uint8_t width_mask = 0u;
};

MemoryAccessShape memory_access_shape(
    const sh4::DecodedInstruction& instruction) noexcept {
    if (instruction.kind == sh4::InstructionKind::Prefetch)
        return {hardware_audit_access_prefetch,
                hardware_audit_width_cache_line_32};
    const auto operation =
        ir::lowering_operation_for_instruction(instruction.kind);
    const auto effects = ir::instruction_memory_effects(
        operation,
        instruction.destination_register,
        instruction.source_register);
    std::uint8_t access_mask = 0u;
    if (effects.access == ir::MemoryAccessKind::Read ||
        is_syntactic_memory_read(instruction.kind))
        access_mask |= hardware_audit_access_read;
    if (effects.access == ir::MemoryAccessKind::Write)
        access_mask |= hardware_audit_access_write;
    std::uint8_t width_mask = 0u;
    switch (effects.width) {
    case ir::OperandWidth::Bits8:
        width_mask = hardware_audit_width_u8;
        break;
    case ir::OperandWidth::Bits16:
        width_mask = hardware_audit_width_u16;
        break;
    case ir::OperandWidth::Bits32:
    case ir::OperandWidth::Bits64:
        // SH-4 FMOV.SZ=1 is represented as two 32-bit bus words.
        width_mask = hardware_audit_width_u32;
        break;
    default:
        break;
    }
    return {access_mask, width_mask};
}

std::optional<std::uint32_t> displaced(const std::optional<std::uint32_t>& base,
                                       const std::uint32_t offset = 0u) {
    if (!base.has_value()) return std::nullopt;
    return *base + offset;
}

EffectiveAccessSet effective_accesses(const sh4::DisassemblyLine& line,
                                      const RegisterConstants& before,
                                      const std::optional<std::uint32_t> gbr) {
    using K = sh4::InstructionKind;
    const auto& instruction = line.instruction;
    std::optional<std::uint32_t> address;
    HardwareAccessKind kind = HardwareAccessKind::Read;
    std::uint8_t width = 0u;
    bool read_modify_write = false;
    bool fmov_pair = false;
    switch (instruction.kind) {
    case K::MovByteStore:
    case K::MovWordStore:
    case K::MovLongStore:
        kind = HardwareAccessKind::Write;
        width = instruction.kind == K::MovByteStore   ? 1u
                : instruction.kind == K::MovWordStore ? 2u
                                                      : 4u;
        address = before.registers[instruction.destination_register];
        break;
    case K::MovByteLoad:
    case K::MovWordLoad:
    case K::MovLongLoad:
        width = instruction.kind == K::MovByteLoad   ? 1u
                : instruction.kind == K::MovWordLoad ? 2u
                                                     : 4u;
        address = before.registers[instruction.source_register];
        break;
    case K::MovByteStoreDisplacement:
    case K::MovWordStoreDisplacement:
    case K::MovLongStoreDisplacement:
        kind = HardwareAccessKind::Write;
        width = instruction.kind == K::MovByteStoreDisplacement   ? 1u
                : instruction.kind == K::MovWordStoreDisplacement ? 2u
                                                                  : 4u;
        address = displaced(before.registers[instruction.destination_register],
                            static_cast<std::uint32_t>(instruction.displacement));
        break;
    case K::MovByteLoadDisplacement:
    case K::MovWordLoadDisplacement:
    case K::MovLongLoadDisplacement:
        width = instruction.kind == K::MovByteLoadDisplacement   ? 1u
                : instruction.kind == K::MovWordLoadDisplacement ? 2u
                                                                 : 4u;
        address = displaced(before.registers[instruction.source_register],
                            static_cast<std::uint32_t>(instruction.displacement));
        break;
    case K::MovByteStoreR0Indexed:
    case K::MovWordStoreR0Indexed:
    case K::MovLongStoreR0Indexed:
        kind = HardwareAccessKind::Write;
        width = instruction.kind == K::MovByteStoreR0Indexed   ? 1u
                : instruction.kind == K::MovWordStoreR0Indexed ? 2u
                                                               : 4u;
        if (before.registers[0u].has_value() &&
            before.registers[instruction.destination_register].has_value())
            address = *before.registers[0u] + *before.registers[instruction.destination_register];
        break;
    case K::MovByteLoadR0Indexed:
    case K::MovWordLoadR0Indexed:
    case K::MovLongLoadR0Indexed:
        width = instruction.kind == K::MovByteLoadR0Indexed   ? 1u
                : instruction.kind == K::MovWordLoadR0Indexed ? 2u
                                                              : 4u;
        if (before.registers[0u].has_value() &&
            before.registers[instruction.source_register].has_value())
            address = *before.registers[0u] + *before.registers[instruction.source_register];
        break;
    case K::MovByteStoreGbrDisplacement:
    case K::MovWordStoreGbrDisplacement:
    case K::MovLongStoreGbrDisplacement:
        kind = HardwareAccessKind::Write;
        width = instruction.kind == K::MovByteStoreGbrDisplacement   ? 1u
                : instruction.kind == K::MovWordStoreGbrDisplacement ? 2u
                                                                     : 4u;
        address = displaced(gbr, static_cast<std::uint32_t>(instruction.displacement));
        break;
    case K::MovByteLoadGbrDisplacement:
    case K::MovWordLoadGbrDisplacement:
    case K::MovLongLoadGbrDisplacement:
        width = instruction.kind == K::MovByteLoadGbrDisplacement   ? 1u
                : instruction.kind == K::MovWordLoadGbrDisplacement ? 2u
                                                                    : 4u;
        address = displaced(gbr, static_cast<std::uint32_t>(instruction.displacement));
        break;
    case K::MovWordLoadPcRelative:
    case K::MovLongLoadPcRelative:
        width = instruction.kind == K::MovWordLoadPcRelative ? 2u : 4u;
        address = (width == 4u ? (line.address + 4u) & ~3u : line.address + 4u) +
                  static_cast<std::uint32_t>(instruction.displacement);
        break;
    case K::TestByteImmediate:
    case K::AndByteImmediate:
    case K::XorByteImmediate:
    case K::OrByteImmediate:
        width = 1u;
        read_modify_write = instruction.kind != K::TestByteImmediate;
        if (gbr.has_value() && before.registers[0u].has_value())
            address = *gbr + *before.registers[0u];
        break;
    case K::MovByteStorePreDecrement:
    case K::MovWordStorePreDecrement:
    case K::MovLongStorePreDecrement:
        kind = HardwareAccessKind::Write;
        width = instruction.kind == K::MovByteStorePreDecrement   ? 1u
                : instruction.kind == K::MovWordStorePreDecrement ? 2u
                                                                  : 4u;
        if (before.registers[instruction.destination_register].has_value())
            address = *before.registers[instruction.destination_register] - width;
        break;
    case K::MovByteLoadPostIncrement:
    case K::MovWordLoadPostIncrement:
    case K::MovLongLoadPostIncrement:
        width = instruction.kind == K::MovByteLoadPostIncrement   ? 1u
                : instruction.kind == K::MovWordLoadPostIncrement ? 2u
                                                                  : 4u;
        address = before.registers[instruction.source_register];
        break;
    case K::StoreSpecialRegisterPreDecrement:
        kind = HardwareAccessKind::Write;
        width = 4u;
        if (before.registers[instruction.destination_register].has_value())
            address = *before.registers[instruction.destination_register] - width;
        break;
    case K::LoadSpecialRegisterPostIncrement:
        width = 4u;
        address = before.registers[instruction.source_register];
        break;
    case K::MultiplyAccumulateWord:
    case K::MultiplyAccumulateLong: {
        width = instruction.kind == K::MultiplyAccumulateWord ? 2u : 4u;
        const auto destination = before.registers[instruction.destination_register];
        const auto source = before.registers[instruction.source_register];
        EffectiveAccessSet result;
        if (destination.has_value())
            result.accesses.push_back({*destination, HardwareAccessKind::Read, width});
        if (source.has_value()) {
            const auto source_address =
                *source +
                (instruction.source_register == instruction.destination_register ? width : 0u);
            result.accesses.push_back({source_address, HardwareAccessKind::Read, width});
        }
        result.complete = destination.has_value() && source.has_value();
        return result;
    }
    case K::MovcaLong:
        kind = HardwareAccessKind::Write;
        width = 4u;
        address = before.registers[instruction.destination_register];
        break;
    case K::TestAndSetByte:
        width = 1u;
        read_modify_write = true;
        address = before.registers[instruction.source_register];
        break;
    case K::Prefetch:
        kind = HardwareAccessKind::Prefetch;
        width = runtime::native_port_store_queue_prefetch_width;
        address = before.registers[instruction.source_register];
        break;
    case K::FmovLoad:
    case K::FmovLoadPostIncrement:
        width = 4u;
        fmov_pair = true;
        address = before.registers[instruction.source_register];
        break;
    case K::FmovLoadR0Indexed:
        width = 4u;
        fmov_pair = true;
        if (before.registers[0u].has_value() &&
            before.registers[instruction.source_register].has_value())
            address = *before.registers[0u] + *before.registers[instruction.source_register];
        break;
    case K::FmovStore:
        kind = HardwareAccessKind::Write;
        width = 4u;
        fmov_pair = true;
        address = before.registers[instruction.destination_register];
        break;
    case K::FmovStorePreDecrement:
        kind = HardwareAccessKind::Write;
        width = 4u;
        fmov_pair = true;
        if (before.registers[instruction.destination_register].has_value())
            address = *before.registers[instruction.destination_register] - 8u;
        break;
    case K::FmovStoreR0Indexed:
        kind = HardwareAccessKind::Write;
        width = 4u;
        fmov_pair = true;
        if (before.registers[0u].has_value() &&
            before.registers[instruction.destination_register].has_value())
            address = *before.registers[0u] + *before.registers[instruction.destination_register];
        break;
    default:
        return {};
    }
    if (!address.has_value()) return {};
    if (read_modify_write)
        return {{{*address, HardwareAccessKind::Read, width},
                 {*address, HardwareAccessKind::Write, width}},
                true};
    // FPSCR.SZ is not part of local constant propagation.  Enumerate the conservative
    // union of its 32-bit bus words: SZ=0 uses the first word, while SZ=1 additionally
    // uses the second.  Predecrement starts at base-8, so the pair also retains the
    // SZ=0 base-4 address.
    if (fmov_pair) return {{{*address, kind, width}, {*address + 4u, kind, width}}, true};
    return {{{*address, kind, width}}, true};
}

std::vector<std::optional<std::uint32_t>>
propagate_local_gbr(const std::span<const sh4::DisassemblyLine> lines,
                    const std::span<const ConstantTraceEntry> trace) {
    std::vector<std::optional<std::uint32_t>> before;
    before.reserve(lines.size());
    std::optional<std::uint32_t> gbr;
    for (std::size_t index = 0u; index < lines.size(); ++index) {
        before.push_back(gbr);
        const auto& instruction = lines[index].instruction;
        if (instruction.special_register != sh4::SpecialRegister::Gbr) continue;
        if (instruction.kind == sh4::InstructionKind::LoadSpecialRegister)
            gbr = trace[index].before.registers[instruction.source_register];
        else if (instruction.kind == sh4::InstructionKind::LoadSpecialRegisterPostIncrement)
            gbr.reset();
    }
    return before;
}

std::string hex8(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return output.str();
}

std::optional<EffectiveAccess> bounded_store_queue_prefetch_access(
    const std::span<const sh4::DisassemblyLine> lines,
    const std::span<const ConstantTraceEntry> trace,
    const std::size_t prefetch_index) {
    using K = sh4::InstructionKind;
    if (prefetch_index >= lines.size() || prefetch_index >= trace.size() ||
        lines[prefetch_index].instruction.kind != K::Prefetch)
        return std::nullopt;

    const auto address_register =
        lines[prefetch_index].instruction.source_register;
    if (address_register >= 16u) return std::nullopt;
    const auto register_mask =
        static_cast<std::uint16_t>(1u << address_register);
    std::int64_t delta = 0;
    std::optional<std::uint32_t> or_value;
    std::optional<std::uint32_t> and_mask;

    for (std::size_t cursor = prefetch_index; cursor != 0u;) {
        --cursor;
        const auto& instruction = lines[cursor].instruction;
        if ((general_register_write_mask(instruction) & register_mask) == 0u)
            continue;

        if ((instruction.kind == K::MovByteStorePreDecrement ||
             instruction.kind == K::MovWordStorePreDecrement ||
             instruction.kind == K::MovLongStorePreDecrement) &&
            instruction.destination_register == address_register) {
            delta -= instruction.kind == K::MovByteStorePreDecrement   ? 1
                     : instruction.kind == K::MovWordStorePreDecrement ? 2
                                                                       : 4;
            continue;
        }
        if (instruction.kind == K::AddImmediate &&
            instruction.destination_register == address_register) {
            delta += instruction.immediate;
            continue;
        }
        if (instruction.kind == K::OrRegister &&
            instruction.destination_register == address_register &&
            !or_value.has_value()) {
            or_value = trace[cursor].before.registers[
                instruction.source_register];
            if (!or_value.has_value()) return std::nullopt;
            continue;
        }
        if (instruction.kind == K::AndRegister &&
            instruction.destination_register == address_register &&
            or_value.has_value()) {
            and_mask = trace[cursor].before.registers[
                instruction.source_register];
            break;
        }
        return std::nullopt;
    }
    if (!or_value.has_value() || !and_mask.has_value())
        return std::nullopt;

    const auto minimum64 = static_cast<std::int64_t>(*or_value) + delta;
    const auto maximum64 =
        static_cast<std::int64_t>(*or_value | *and_mask) + delta;
    if (minimum64 < 0 || maximum64 < minimum64 ||
        maximum64 > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    const auto minimum = static_cast<std::uint32_t>(minimum64);
    const auto maximum = static_cast<std::uint32_t>(maximum64);
    const auto minimum_description = describe(minimum);
    const auto maximum_description = describe(maximum);
    if (minimum_description.region != DreamcastHardwareRegion::StoreQueue ||
        maximum_description.region != DreamcastHardwareRegion::StoreQueue ||
        !minimum_description.aperture_mapped ||
        !maximum_description.aperture_mapped)
        return std::nullopt;

    EffectiveAccess access;
    access.address = minimum;
    access.kind = HardwareAccessKind::Prefetch;
    access.width = runtime::native_port_store_queue_prefetch_width;
    access.address_known = false;
    access.address_expression =
        "canonical-range:" + hex8(minimum) + "-" + hex8(maximum);
    return access;
}

std::string hex4(const std::uint16_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << value;
    return output.str();
}

std::size_t controlling_instruction_index(const BasicBlock& block) noexcept {
    if (block.lines.empty()) return 0u;
    const auto last = block.lines.size() - 1u;
    return block.lines[last].is_delay_slot && last != 0u &&
                   block.lines[last - 1u].instruction.has_delay_slot &&
                   block.lines[last].address == block.lines[last - 1u].address + 2u
               ? last - 1u
               : last;
}

constexpr std::uint16_t loop_register_bit(const std::uint8_t index) noexcept {
    return static_cast<std::uint16_t>(1u << index);
}

bool is_potential_memory_load(const sh4::InstructionKind kind) noexcept {
    using K = sh4::InstructionKind;
    switch (kind) {
    case K::MovByteLoad:
    case K::MovWordLoad:
    case K::MovLongLoad:
    case K::MovByteLoadPostIncrement:
    case K::MovWordLoadPostIncrement:
    case K::MovLongLoadPostIncrement:
    case K::MovByteLoadDisplacement:
    case K::MovWordLoadDisplacement:
    case K::MovLongLoadDisplacement:
    case K::MovByteLoadR0Indexed:
    case K::MovWordLoadR0Indexed:
    case K::MovLongLoadR0Indexed:
    case K::MovByteLoadGbrDisplacement:
    case K::MovWordLoadGbrDisplacement:
    case K::MovLongLoadGbrDisplacement:
    case K::MovWordLoadPcRelative:
    case K::MovLongLoadPcRelative:
        return true;
    default:
        return false;
    }
}

bool is_postincrement_memory_load(const sh4::InstructionKind kind) noexcept {
    using K = sh4::InstructionKind;
    switch (kind) {
    case K::MovByteLoadPostIncrement:
    case K::MovWordLoadPostIncrement:
    case K::MovLongLoadPostIncrement:
    case K::LoadSpecialRegisterPostIncrement:
    case K::FmovLoadPostIncrement:
    case K::MultiplyAccumulateWord:
    case K::MultiplyAccumulateLong:
        return true;
    default:
        return false;
    }
}

std::uint16_t memory_read_address_register_mask(
    const sh4::DecodedInstruction& instruction) noexcept {
    using K = sh4::InstructionKind;
    const auto source = loop_register_bit(instruction.source_register);
    const auto destination = loop_register_bit(instruction.destination_register);
    switch (instruction.kind) {
    case K::MovByteLoad:
    case K::MovWordLoad:
    case K::MovLongLoad:
    case K::MovByteLoadPostIncrement:
    case K::MovWordLoadPostIncrement:
    case K::MovLongLoadPostIncrement:
    case K::MovByteLoadDisplacement:
    case K::MovWordLoadDisplacement:
    case K::MovLongLoadDisplacement:
    case K::TestAndSetByte:
    case K::Prefetch:
    case K::LoadSpecialRegisterPostIncrement:
    case K::FmovLoad:
    case K::FmovLoadPostIncrement:
        return source;
    case K::MovByteLoadR0Indexed:
    case K::MovWordLoadR0Indexed:
    case K::MovLongLoadR0Indexed:
    case K::FmovLoadR0Indexed:
        return static_cast<std::uint16_t>(source | loop_register_bit(0u));
    case K::TestByteImmediate:
    case K::AndByteImmediate:
    case K::XorByteImmediate:
    case K::OrByteImmediate:
        return loop_register_bit(0u);
    case K::MultiplyAccumulateWord:
    case K::MultiplyAccumulateLong:
        return static_cast<std::uint16_t>(source | destination);
    default:
        return 0u;
    }
}

std::optional<HardwareLoopLocalProgressKind> local_progress_kind(
    const sh4::DisassemblyLine& line,
    const std::uint8_t register_index,
    const RegisterConstants& before) noexcept {
    using K = sh4::InstructionKind;
    const auto& instruction = line.instruction;
    if (instruction.kind == K::AddImmediate &&
        instruction.destination_register == register_index &&
        instruction.immediate != 0)
        return HardwareLoopLocalProgressKind::IntegerInduction;
    if ((instruction.kind == K::AddRegister ||
         instruction.kind == K::SubRegister) &&
        instruction.destination_register == register_index &&
        instruction.source_register != register_index) {
        const auto step = before.registers[instruction.source_register];
        if (step.has_value() && *step != 0u)
            return HardwareLoopLocalProgressKind::IntegerInduction;
    }
    if (is_postincrement_memory_load(instruction.kind) &&
        instruction.source_register == register_index) {
        const bool same_integer_load_register =
            (instruction.kind == K::MovByteLoadPostIncrement ||
             instruction.kind == K::MovWordLoadPostIncrement ||
             instruction.kind == K::MovLongLoadPostIncrement) &&
            instruction.destination_register == register_index;
        if (!same_integer_load_register)
            return HardwareLoopLocalProgressKind::AddressPostIncrement;
    }
    if (is_potential_memory_load(instruction.kind) &&
        instruction.destination_register == register_index &&
        (memory_read_address_register_mask(instruction) &
         loop_register_bit(register_index)) != 0u)
        return HardwareLoopLocalProgressKind::PointerTraversal;
    return std::nullopt;
}

bool is_syntactic_memory_read(const sh4::InstructionKind kind) noexcept {
    using K = sh4::InstructionKind;
    if (is_potential_memory_load(kind)) return true;
    switch (kind) {
    case K::TestByteImmediate:
    case K::AndByteImmediate:
    case K::XorByteImmediate:
    case K::OrByteImmediate:
    case K::TestAndSetByte:
    case K::LoadSpecialRegisterPostIncrement:
    case K::MultiplyAccumulateWord:
    case K::MultiplyAccumulateLong:
    case K::FmovLoad:
    case K::FmovLoadPostIncrement:
    case K::FmovLoadR0Indexed:
        return true;
    default:
        return false;
    }
}

bool instruction_writes_t(const sh4::DecodedInstruction& instruction) noexcept {
    using K = sh4::InstructionKind;
    switch (instruction.kind) {
    case K::Unknown:
    case K::AddWithCarry:
    case K::AddWithOverflow:
    case K::SubWithCarry:
    case K::SubWithOverflow:
    case K::NegateWithCarry:
    case K::DecrementAndTest:
    case K::ShiftLogicalLeftOne:
    case K::ShiftLogicalRightOne:
    case K::ShiftArithmeticLeftOne:
    case K::ShiftArithmeticRightOne:
    case K::RotateLeft:
    case K::RotateRight:
    case K::RotateLeftThroughT:
    case K::RotateRightThroughT:
    case K::DivideInitializeUnsigned:
    case K::DivideInitializeSigned:
    case K::DivideStep:
    case K::ClearT:
    case K::SetT:
    case K::CompareEqualImmediate:
    case K::CompareEqualRegister:
    case K::CompareHigherOrSame:
    case K::CompareGreaterOrEqual:
    case K::CompareHigher:
    case K::CompareGreaterThan:
    case K::ComparePositiveOrZero:
    case K::ComparePositive:
    case K::CompareString:
    case K::TestImmediate:
    case K::TestRegister:
    case K::TestByteImmediate:
    case K::TestAndSetByte:
    case K::FcmpEqual:
    case K::FcmpGreater:
    case K::TrapAlways:
    case K::ReturnFromException:
        return true;
    case K::LoadSpecialRegister:
    case K::LoadSpecialRegisterPostIncrement:
        return instruction.special_register == sh4::SpecialRegister::Sr;
    default:
        return false;
    }
}

std::uint16_t condition_register_mask(const sh4::DecodedInstruction& instruction) noexcept {
    using K = sh4::InstructionKind;
    switch (instruction.kind) {
    case K::CompareEqualImmediate:
    case K::TestImmediate:
        return 1u;
    case K::CompareEqualRegister:
    case K::CompareHigherOrSame:
    case K::CompareGreaterOrEqual:
    case K::CompareHigher:
    case K::CompareGreaterThan:
    case K::CompareString:
    case K::TestRegister:
        return static_cast<std::uint16_t>((1u << instruction.destination_register) |
                                          (1u << instruction.source_register));
    case K::ComparePositiveOrZero:
    case K::ComparePositive:
        return static_cast<std::uint16_t>(1u << instruction.destination_register);
    default:
        return 0u;
    }
}

std::optional<std::uint16_t>
guard_input_registers(const sh4::DecodedInstruction& instruction) noexcept {
    using K = sh4::InstructionKind;
    const auto destination = loop_register_bit(instruction.destination_register);
    const auto source = loop_register_bit(instruction.source_register);
    switch (instruction.kind) {
    case K::MovImmediate:
    case K::MovWordLoadPcRelative:
    case K::MovLongLoadPcRelative:
    case K::MoveAddressPcRelative:
        return static_cast<std::uint16_t>(0u);
    case K::MovRegister:
    case K::NegateRegister:
    case K::NotRegister:
    case K::ExtendUnsignedByte:
    case K::ExtendUnsignedWord:
    case K::ExtendSignedByte:
    case K::ExtendSignedWord:
    case K::SwapBytes:
    case K::SwapWords:
        return source;
    case K::AddImmediate:
    case K::AndImmediate:
    case K::OrImmediate:
    case K::XorImmediate:
    case K::DecrementAndTest:
    case K::ShiftLogicalLeftOne:
    case K::ShiftLogicalRightOne:
    case K::ShiftArithmeticLeftOne:
    case K::ShiftArithmeticRightOne:
    case K::ShiftLogicalLeftTwo:
    case K::ShiftLogicalLeftEight:
    case K::ShiftLogicalLeftSixteen:
    case K::ShiftLogicalRightTwo:
    case K::ShiftLogicalRightEight:
    case K::ShiftLogicalRightSixteen:
    case K::RotateLeft:
    case K::RotateRight:
        return destination;
    case K::AddRegister:
    case K::SubRegister:
    case K::AndRegister:
    case K::OrRegister:
    case K::XorRegister:
    case K::ExtractMiddle:
    case K::ShiftArithmeticDynamic:
    case K::ShiftLogicalDynamic:
        return static_cast<std::uint16_t>(destination | source);
    case K::MovByteStorePreDecrement:
    case K::MovWordStorePreDecrement:
    case K::MovLongStorePreDecrement:
    case K::StoreSpecialRegisterPreDecrement:
        return destination;
    default:
        return std::nullopt;
    }
}

bool is_linear_loop_memory(const AddressDescription& description) noexcept {
    if (description.image_backed) return true;
    if (in_range(description.canonical, 0x0C000000u, 0x04000000u)) return true;
    using R = DreamcastHardwareRegion;
    switch (description.region) {
    case R::AicaRam:
    case R::TaVram:
    case R::Vram64:
    case R::Vram32:
        return true;
    default:
        return false;
    }
}

std::uint32_t loop_canonical_address(const AddressDescription& description) noexcept {
    if (in_range(description.canonical, 0x0C000000u, 0x04000000u))
        return 0x0C000000u | (description.canonical & 0x00FFFFFFu);
    return description.canonical;
}

enum class LoopReadStorage : std::uint8_t { Ram, Mmio, Unknown };

LoopReadStorage loop_read_storage(const HardwareLoopAccessEvidence& access) noexcept {
    if (access.linear_memory) return LoopReadStorage::Ram;
    if (access.region != DreamcastHardwareRegion::Unknown && access.aperture_mapped &&
        access.runtime_support != HardwareRuntimeSupport::Unmapped)
        return LoopReadStorage::Mmio;
    return LoopReadStorage::Unknown;
}

HardwareLoopClassification classify_loop(const HardwareNaturalLoop& loop) noexcept {
    bool counter = !loop.counter_instruction_addresses.empty();
    bool ram_poll = false;
    bool mmio_poll = false;
    bool unknown_poll = loop.unresolved_guard_access;
    for (const auto& access : loop.accesses) {
        if (!access.guards_loop || access.kind != HardwareAccessKind::Read) continue;
        switch (loop_read_storage(access)) {
        case LoopReadStorage::Ram:
            ram_poll = true;
            break;
        case LoopReadStorage::Mmio:
            mmio_poll = true;
            break;
        case LoopReadStorage::Unknown:
            unknown_poll = true;
            break;
        }
    }
    if (unknown_poll) return HardwareLoopClassification::Unknown;
    const auto evidence_classes = static_cast<unsigned>(counter) + static_cast<unsigned>(ram_poll) +
                                  static_cast<unsigned>(mmio_poll);
    if (evidence_classes > 1u) return HardwareLoopClassification::Mixed;
    if (counter) return HardwareLoopClassification::Counter;
    if (ram_poll) return HardwareLoopClassification::RamPoll;
    if (mmio_poll) return HardwareLoopClassification::MmioPoll;
    return HardwareLoopClassification::Unknown;
}

void add_loop_cfg_successor(BasicBlock& block, const std::uint32_t address) {
    if (std::find(block.successors.begin(), block.successors.end(), address) ==
        block.successors.end())
        block.successors.push_back(address);
}

void repair_contextual_delay_slot_edges(std::vector<BasicBlock>& blocks,
                                        const ControlFlowAnalysisResult& analysis) {
    for (auto& block : blocks) {
        if (block.lines.empty()) continue;
        const auto control_index = controlling_instruction_index(block);
        const auto& control_line = block.lines[control_index];
        const auto& instruction = control_line.instruction;
        if (!instruction.has_delay_slot) continue;
        const bool paired = control_index + 1u < block.lines.size() &&
                            block.lines[control_index + 1u].address == control_line.address + 2u &&
                            block.lines[control_index + 1u].is_delay_slot;
        if (paired) continue;

        const auto delay_context =
            std::find_if(analysis.recursive.contextual_instructions.begin(),
                         analysis.recursive.contextual_instructions.end(),
                         [&control_line](const auto& context) {
                             return context.line.address == control_line.address + 2u &&
                                    context.delay_slot_owner == control_line.address &&
                                    control_flow_evidence_proven(context.evidence) &&
                                    context.line.instruction.is_known() &&
                                    !context.line.instruction.changes_control_flow();
                         });
        if (delay_context == analysis.recursive.contextual_instructions.end()) continue;

        const auto fallthrough = control_line.address + 4u;
        if (control_line.target_address.has_value() &&
            instruction.control_flow != sh4::ControlFlowKind::Call)
            add_loop_cfg_successor(block, *control_line.target_address);
        switch (instruction.control_flow) {
        case sh4::ControlFlowKind::ConditionalBranch:
        case sh4::ControlFlowKind::Call:
        case sh4::ControlFlowKind::IndirectCall:
            add_loop_cfg_successor(block, fallthrough);
            break;
        case sh4::ControlFlowKind::IndirectBranch:
            block.has_indirect_successor = true;
            break;
        default:
            break;
        }
        for (const auto& edge : analysis.resolved_edges) {
            if (edge.instruction_address != control_line.address ||
                edge.kind != ResolvedControlFlowKind::Jump)
                continue;
            add_loop_cfg_successor(block, edge.target_address);
            if (control_flow_evidence_complete(resolved_edge_evidence(edge)))
                block.has_indirect_successor = false;
        }
        std::sort(block.successors.begin(), block.successors.end());
        if (control_index + 1u < block.lines.size() &&
            block.lines[control_index + 1u].address == control_line.address + 2u) {
            block.lines[control_index + 1u].is_delay_slot = true;
        } else {
            auto delay_line = delay_context->line;
            delay_line.is_delay_slot = true;
            block.lines.push_back(std::move(delay_line));
        }
    }
}

std::vector<RegisterConstants> propagate_cfg_block_constants(
    const std::vector<BasicBlock>& blocks,
    const std::vector<std::vector<std::size_t>>& successors,
    const std::vector<bool>& roots,
    const io::ExecutableImage& image) {
    struct EntryState final {
        RegisterConstants constants;
        bool initialized = false;
    };

    std::vector<EntryState> entries(blocks.size());
    std::deque<std::size_t> worklist;
    std::vector<bool> queued(blocks.size(), false);
    for (std::size_t index = 0u; index < blocks.size(); ++index) {
        if (!roots[index]) continue;
        entries[index].initialized = true;
        worklist.push_back(index);
        queued[index] = true;
    }

    const auto join = [](EntryState& destination,
                         const RegisterConstants& incoming) {
        if (!destination.initialized) {
            destination.constants = incoming;
            destination.initialized = true;
            return true;
        }
        bool changed = false;
        for (std::size_t reg = 0u;
             reg < destination.constants.registers.size(); ++reg) {
            auto& current = destination.constants.registers[reg];
            if (!current.has_value()) continue;
            if (!incoming.registers[reg].has_value() ||
                *incoming.registers[reg] != *current) {
                current.reset();
                destination.constants.sources[reg].clear();
                changed = true;
            } else if (destination.constants.sources[reg] !=
                       incoming.sources[reg]) {
                destination.constants.sources[reg] = "cfg-join";
            }
        }
        return changed;
    };

    // The must-constant lattice can initialize a block once and can then only
    // discard at most sixteen register constants.  The explicit budget makes
    // malformed or unexpectedly cyclic CFG input fail closed instead of
    // leaving a speculative predecessor value in the hardware audit.
    const auto budget = std::max<std::size_t>(
        1u, blocks.size() * (RegisterConstants{}.registers.size() + 2u));
    std::size_t evaluations = 0u;
    while (!worklist.empty() && evaluations++ < budget) {
        const auto block_index = worklist.front();
        worklist.pop_front();
        queued[block_index] = false;

        auto output = entries[block_index].constants;
        if (!blocks[block_index].lines.empty()) {
            const auto trace = propagate_basic_block_constants(
                blocks[block_index].lines, image, output);
            output = trace.back().after;
        }
        for (const auto successor : successors[block_index]) {
            // A callable/external entry always has an unknown incoming ABI
            // state even if the same address is also reached internally.
            if (roots[successor]) continue;
            if (!join(entries[successor], output) || queued[successor])
                continue;
            worklist.push_back(successor);
            queued[successor] = true;
        }
    }
    if (!worklist.empty()) return std::vector<RegisterConstants>(blocks.size());

    std::vector<RegisterConstants> result(blocks.size());
    for (std::size_t index = 0u; index < blocks.size(); ++index) {
        if (entries[index].initialized)
            result[index] = std::move(entries[index].constants);
    }
    return result;
}

std::vector<HardwareNaturalLoop>
find_natural_hardware_loops(const io::ExecutableImage& image,
                            const ControlFlowAnalysisResult& analysis) {
    std::vector<std::uint32_t> function_entries;
    std::vector<std::uint32_t> block_leaders;
    function_entries.reserve(analysis.recursive.functions.size());
    block_leaders.reserve(analysis.recursive.functions.size() * 2u);
    for (const auto& function : analysis.recursive.functions) {
        function_entries.push_back(function.address);
        block_leaders.push_back(function.address);
        if (function.size != 0u) {
            const auto end = static_cast<std::uint64_t>(function.address) +
                             function.size;
            if (end <= std::numeric_limits<std::uint32_t>::max())
                block_leaders.push_back(
                    static_cast<std::uint32_t>(end));
        }
    }
    auto blocks = build_basic_blocks(
        analysis.recursive.instructions,
        analysis.resolved_edges,
        block_leaders);
    if (blocks.empty()) return {};
    repair_contextual_delay_slot_edges(blocks, analysis);

    std::unordered_map<std::uint32_t, std::size_t> block_by_address;
    block_by_address.reserve(blocks.size());
    for (std::size_t index = 0u; index < blocks.size(); ++index)
        block_by_address.emplace(blocks[index].start_address, index);

    std::vector<std::vector<std::size_t>> successors(blocks.size());
    std::vector<std::vector<std::size_t>> predecessors(blocks.size());
    for (std::size_t source = 0u; source < blocks.size(); ++source) {
        for (const auto successor_address : blocks[source].successors) {
            const auto found = block_by_address.find(successor_address);
            if (found == block_by_address.end()) continue;
            successors[source].push_back(found->second);
            predecessors[found->second].push_back(source);
        }
    }

    std::vector<bool> roots(blocks.size(), false);
    for (const auto entry : function_entries) {
        const auto found = block_by_address.find(entry);
        if (found != block_by_address.end()) roots[found->second] = true;
    }
    for (std::size_t index = 0u; index < blocks.size(); ++index) {
        if (predecessors[index].empty()) roots[index] = true;
    }

    std::vector<bool> reachable(blocks.size(), false);
    const auto mark_reachable = [&](const std::size_t root) {
        std::vector<std::size_t> worklist{root};
        while (!worklist.empty()) {
            const auto current = worklist.back();
            worklist.pop_back();
            if (reachable[current]) continue;
            reachable[current] = true;
            for (const auto successor : successors[current]) {
                if (!reachable[successor]) worklist.push_back(successor);
            }
        }
    };
    for (std::size_t index = 0u; index < blocks.size(); ++index) {
        if (roots[index]) mark_reachable(index);
    }
    for (std::size_t index = 0u; index < blocks.size(); ++index) {
        if (!reachable[index]) roots[index] = true;
    }
    const auto block_entry_constants = propagate_cfg_block_constants(
        blocks, successors, roots, image);

    std::vector<HardwareLoopWriteCandidate> resolved_linear_writes;
    for (std::size_t block_index = 0u;
         block_index < blocks.size(); ++block_index) {
        const auto& block = blocks[block_index];
        const auto trace = propagate_basic_block_constants(
            block.lines, image, block_entry_constants[block_index]);
        const auto gbr_trace = propagate_local_gbr(block.lines, trace);
        for (std::size_t line_index = 0u;
             line_index < block.lines.size(); ++line_index) {
            if (!is_memory_access_instruction(
                    block.lines[line_index].instruction.kind))
                continue;
            const auto access_set = effective_accesses(
                block.lines[line_index], trace[line_index].before,
                gbr_trace[line_index]);
            for (const auto& access : access_set.accesses) {
                if (access.kind != HardwareAccessKind::Write) continue;
                const auto projected_address = project_runtime_image_access(
                    image, block.lines[line_index].address,
                    access.address, access.width);
                const auto description = describe_image_access(
                    image, block.lines[line_index].address,
                    block.lines[line_index].instruction.kind,
                    projected_address, access.width);
                if (!is_linear_loop_memory(description)) continue;
                resolved_linear_writes.push_back({
                    block.lines[line_index].address,
                    projected_address,
                    loop_canonical_address(description),
                    access.width});
            }
        }
    }
    std::sort(resolved_linear_writes.begin(), resolved_linear_writes.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.instruction_address,
                                  left.canonical_address,
                                  left.width) <
                         std::tie(right.instruction_address,
                                  right.canonical_address,
                                  right.width);
              });
    resolved_linear_writes.erase(
        std::unique(resolved_linear_writes.begin(),
                    resolved_linear_writes.end(),
                    [](const auto& left, const auto& right) {
                        return left.instruction_address ==
                                   right.instruction_address &&
                               left.canonical_address ==
                                   right.canonical_address &&
                               left.width == right.width;
                    }),
        resolved_linear_writes.end());

    const auto synthetic_root = blocks.size();
    std::vector<std::vector<std::size_t>> dominator_successors = successors;
    std::vector<std::vector<std::size_t>> dominator_predecessors = predecessors;
    dominator_successors.emplace_back();
    dominator_predecessors.emplace_back();
    for (std::size_t index = 0u; index < blocks.size(); ++index) {
        if (!roots[index]) continue;
        dominator_successors[synthetic_root].push_back(index);
        dominator_predecessors[index].push_back(synthetic_root);
    }

    struct DfsFrame {
        std::size_t node = 0u;
        std::size_t next_successor = 0u;
    };
    std::vector<bool> visited(blocks.size() + 1u, false);
    std::vector<std::size_t> postorder;
    postorder.reserve(blocks.size() + 1u);
    std::vector<DfsFrame> dfs{{synthetic_root, 0u}};
    visited[synthetic_root] = true;
    while (!dfs.empty()) {
        auto& frame = dfs.back();
        const auto& outgoing = dominator_successors[frame.node];
        if (frame.next_successor < outgoing.size()) {
            const auto successor = outgoing[frame.next_successor++];
            if (!visited[successor]) {
                visited[successor] = true;
                dfs.push_back({successor, 0u});
            }
            continue;
        }
        postorder.push_back(frame.node);
        dfs.pop_back();
    }
    std::reverse(postorder.begin(), postorder.end());
    std::vector<std::size_t> rpo_position(blocks.size() + 1u, 0u);
    for (std::size_t index = 0u; index < postorder.size(); ++index)
        rpo_position[postorder[index]] = index;

    constexpr auto no_dominator = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> immediate_dominator(blocks.size() + 1u, no_dominator);
    immediate_dominator[synthetic_root] = synthetic_root;
    const auto intersect = [&](std::size_t left, std::size_t right) {
        while (left != right) {
            while (rpo_position[left] > rpo_position[right])
                left = immediate_dominator[left];
            while (rpo_position[right] > rpo_position[left])
                right = immediate_dominator[right];
        }
        return left;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t rpo_index = 1u; rpo_index < postorder.size(); ++rpo_index) {
            const auto block_index = postorder[rpo_index];
            auto next = no_dominator;
            for (const auto predecessor : dominator_predecessors[block_index]) {
                if (immediate_dominator[predecessor] == no_dominator) continue;
                next = next == no_dominator ? predecessor : intersect(predecessor, next);
            }
            if (next != no_dominator && next != immediate_dominator[block_index]) {
                immediate_dominator[block_index] = next;
                changed = true;
            }
        }
    }

    std::vector<std::vector<std::size_t>> dominator_children(blocks.size() + 1u);
    for (std::size_t node = 0u; node < blocks.size(); ++node)
        dominator_children[immediate_dominator[node]].push_back(node);
    std::vector<std::size_t> dominator_preorder(blocks.size() + 1u, 0u);
    std::vector<std::size_t> dominator_subtree_end(blocks.size() + 1u, 0u);
    std::size_t dominator_clock = 0u;
    std::vector<DfsFrame> dominator_dfs{{synthetic_root, 0u}};
    dominator_preorder[synthetic_root] = dominator_clock++;
    while (!dominator_dfs.empty()) {
        auto& frame = dominator_dfs.back();
        const auto& children = dominator_children[frame.node];
        if (frame.next_successor < children.size()) {
            const auto child = children[frame.next_successor++];
            dominator_preorder[child] = dominator_clock++;
            dominator_dfs.push_back({child, 0u});
            continue;
        }
        dominator_subtree_end[frame.node] = dominator_clock;
        dominator_dfs.pop_back();
    }
    const auto dominates = [&](const std::size_t dominator, const std::size_t node) noexcept {
        return dominator_preorder[dominator] <= dominator_preorder[node] &&
               dominator_preorder[node] < dominator_subtree_end[dominator];
    };

    std::vector<HardwareNaturalLoop> loops;
    std::vector<std::size_t> loop_membership(blocks.size(), 0u);
    std::size_t loop_generation = 0u;
    for (std::size_t latch = 0u; latch < blocks.size(); ++latch) {
        for (const auto header : successors[latch]) {
            if (!reachable[header] || !reachable[latch]) continue;
            if (!dominates(header, latch)) continue;
            if (++loop_generation == 0u) {
                std::fill(loop_membership.begin(), loop_membership.end(), 0u);
                loop_generation = 1u;
            }
            const auto is_member = [&](const std::size_t block) noexcept {
                return loop_membership[block] == loop_generation;
            };
            loop_membership[header] = loop_generation;
            std::vector<std::size_t> member_indices{header};
            if (latch != header) {
                loop_membership[latch] = loop_generation;
                member_indices.push_back(latch);
            }
            std::vector<std::size_t> worklist;
            if (latch != header) worklist.push_back(latch);
            while (!worklist.empty()) {
                const auto current = worklist.back();
                worklist.pop_back();
                for (const auto predecessor : predecessors[current]) {
                    if (is_member(predecessor) || !dominates(header, predecessor)) continue;
                    loop_membership[predecessor] = loop_generation;
                    member_indices.push_back(predecessor);
                    if (predecessor != header) worklist.push_back(predecessor);
                }
            }
            std::sort(member_indices.begin(), member_indices.end());

            HardwareNaturalLoop loop;
            loop.header_address = blocks[header].start_address;
            loop.latch_address = blocks[latch].start_address;
            loop.backedge_instruction_address =
                blocks[latch].lines[controlling_instruction_index(blocks[latch])].address;
            for (const auto index : member_indices)
                loop.block_addresses.push_back(blocks[index].start_address);

            std::unordered_map<std::uint32_t, std::vector<std::size_t>>
                read_accesses_by_instruction;
            std::unordered_map<std::uint32_t, bool> syntactic_read_by_instruction;
            for (const auto block_index : member_indices) {
                const auto& block = blocks[block_index];
                const auto trace = propagate_basic_block_constants(
                    block.lines, image, block_entry_constants[block_index]);
                const auto gbr_trace = propagate_local_gbr(block.lines, trace);
                for (std::size_t line_index = 0u; line_index < block.lines.size(); ++line_index) {
                    if (!is_memory_access_instruction(block.lines[line_index].instruction.kind))
                        continue;
                    if (is_syntactic_memory_read(block.lines[line_index].instruction.kind))
                        syntactic_read_by_instruction.emplace(block.lines[line_index].address,
                                                              true);
                    const auto access_set = effective_accesses(
                        block.lines[line_index], trace[line_index].before, gbr_trace[line_index]);
                    for (const auto& access : access_set.accesses) {
                        const auto projected_address =
                            project_runtime_image_access(
                                image,
                                block.lines[line_index].address,
                                access.address,
                                access.width);
                        const auto description = describe_image_access(
                            image, block.lines[line_index].address,
                            block.lines[line_index].instruction.kind,
                            projected_address, access.width);
                        HardwareLoopAccessEvidence evidence;
                        evidence.instruction_address = block.lines[line_index].address;
                        evidence.guest_address = projected_address;
                        evidence.canonical_address = loop_canonical_address(description);
                        evidence.region = description.region;
                        evidence.kind = access.kind;
                        evidence.width = access.width;
                        evidence.linear_memory = is_linear_loop_memory(description);
                        evidence.aperture_mapped = description.aperture_mapped;
                        evidence.runtime_support =
                            assess_support(description, access.kind, access.width);
                        if (evidence.kind == HardwareAccessKind::Read)
                            read_accesses_by_instruction[evidence.instruction_address].push_back(
                                loop.accesses.size());
                        loop.accesses.push_back(std::move(evidence));
                    }
                }
            }

            const auto append_local_progress =
                [&](const std::uint32_t condition_address,
                    const sh4::DisassemblyLine& progress_line,
                    const std::uint8_t register_index,
                    const HardwareLoopLocalProgressKind kind) {
                    loop.local_progress_evidence.push_back(
                        {condition_address,
                         progress_line.address,
                         register_index,
                         kind});
                };
            const auto record_guard_address_progress =
                [&](const std::uint32_t condition_address,
                    const std::uint32_t guard_read_address) {
                    // External-progress admission currently consumes only
                    // one-block loops. For that exact shape, a bounded cyclic
                    // backward slice over the guard-address expression proves
                    // whether the guarded read advances locally between
                    // iterations. Unsupported definitions stop their slice;
                    // unrelated arithmetic cannot satisfy the proof.
                    if (member_indices.size() != 1u) return;
                    const auto block_index = member_indices.front();
                    const auto& block = blocks[block_index];
                    const auto found = std::ranges::find_if(
                        block.lines,
                        [&](const auto& line) {
                            return line.address == guard_read_address;
                        });
                    if (found == block.lines.end()) return;
                    const auto read_index = static_cast<std::size_t>(
                        std::distance(block.lines.begin(), found));
                    const auto address_registers =
                        memory_read_address_register_mask(found->instruction);
                    if (address_registers == 0u) return;
                    const auto trace = propagate_basic_block_constants(
                        block.lines,
                        image,
                        block_entry_constants[block_index]);

                    // Follow the guard address through the complete local
                    // register expression, rather than considering only the
                    // register named by the final load.  Table scans commonly
                    // form an address through MOV/SHLL/ADD before advancing an
                    // index, while linked-record traversals load the next
                    // index/pointer through a second register.  Both are local
                    // progress, but neither is visible at the final address
                    // register alone.
                    //
                    // The walk is cyclic because a loop-carried update may be
                    // in the backedge delay slot, after the guarded read in
                    // linear address order.  Every accepted proof is still
                    // tied to an exact writer inside this one-block natural
                    // loop.  Exhausting the bounded state budget merely leaves
                    // the loop classified as externally driven.
                    struct AddressProgressState final {
                        std::uint8_t register_index = 0u;
                        std::size_t cursor = 0u;
                        std::size_t pointer_load_index = 0u;

                        bool operator==(const AddressProgressState&) const =
                            default;
                    };
                    const auto no_pointer_load = block.lines.size();
                    std::deque<AddressProgressState> pending;
                    std::vector<AddressProgressState> visited;
                    for (std::uint8_t register_index = 0u;
                         register_index < 16u; ++register_index) {
                        const auto register_mask =
                            loop_register_bit(register_index);
                        if ((address_registers & register_mask) == 0u)
                            continue;
                        pending.push_back(
                            {register_index, read_index, no_pointer_load});
                    }

                    constexpr std::size_t max_progress_states = 1024u;
                    while (!pending.empty() &&
                           visited.size() < max_progress_states) {
                        const auto state = pending.front();
                        pending.pop_front();
                        if (std::ranges::find(visited, state) != visited.end())
                            continue;
                        visited.push_back(state);

                        if (state.pointer_load_index != no_pointer_load) {
                            const auto& pointer_load =
                                block.lines[state.pointer_load_index];
                            if (pointer_load.instruction.destination_register ==
                                state.register_index) {
                                append_local_progress(
                                    condition_address,
                                    pointer_load,
                                    state.register_index,
                                    HardwareLoopLocalProgressKind::
                                        PointerTraversal);
                                continue;
                            }
                        }

                        std::optional<std::size_t> writer_index;
                        for (std::size_t distance = 0u;
                             distance < block.lines.size(); ++distance) {
                            const auto candidate_index =
                                (state.cursor + block.lines.size() - distance) %
                                block.lines.size();
                            if ((general_register_write_mask(
                                     block.lines[candidate_index].instruction) &
                                 loop_register_bit(state.register_index)) == 0u)
                                continue;
                            writer_index = candidate_index;
                            break;
                        }
                        if (!writer_index.has_value()) continue;

                        const auto& writer = block.lines[*writer_index];
                        const auto kind = local_progress_kind(
                            writer,
                            state.register_index,
                            trace[*writer_index].before);
                        if (kind.has_value()) {
                            append_local_progress(condition_address,
                                                  writer,
                                                  state.register_index,
                                                  *kind);
                            continue;
                        }

                        const auto previous_index =
                            (*writer_index + block.lines.size() - 1u) %
                            block.lines.size();
                        if (is_potential_memory_load(
                                writer.instruction.kind) &&
                            writer.instruction.destination_register ==
                                state.register_index) {
                            const auto pointer_address_registers =
                                memory_read_address_register_mask(
                                    writer.instruction);
                            for (std::uint8_t input_register = 0u;
                                 input_register < 16u; ++input_register) {
                                if ((pointer_address_registers &
                                     loop_register_bit(input_register)) == 0u)
                                    continue;
                                pending.push_back(
                                    {input_register,
                                     previous_index,
                                     *writer_index});
                            }
                            continue;
                        }

                        const auto inputs =
                            guard_input_registers(writer.instruction);
                        if (!inputs.has_value()) continue;
                        for (std::uint8_t input_register = 0u;
                             input_register < 16u; ++input_register) {
                            if ((*inputs & loop_register_bit(input_register)) ==
                                0u)
                                continue;
                            pending.push_back(
                                {input_register,
                                 previous_index,
                                 state.pointer_load_index});
                        }
                    }
                };

            enum class GuardReadResolution : std::uint8_t {
                NotARead,
                ResolvedAddress,
                UnresolvedAddress
            };
            const auto mark_direct_guard_access = [&](const std::uint32_t address) {
                const auto found = read_accesses_by_instruction.find(address);
                if (found != read_accesses_by_instruction.end()) {
                    for (const auto index : found->second)
                        loop.accesses[index].guards_loop = true;
                    return GuardReadResolution::ResolvedAddress;
                }
                if (syntactic_read_by_instruction.contains(address)) {
                    loop.unresolved_guard_read_instruction_addresses.push_back(address);
                    return GuardReadResolution::UnresolvedAddress;
                }
                return GuardReadResolution::NotARead;
            };
            bool has_unresolved_fcmp_guard = false;
            for (const auto block_index : member_indices) {
                const auto& block = blocks[block_index];
                if (block.lines.empty()) continue;
                const auto control_index = controlling_instruction_index(block);
                const auto& control = block.lines[control_index].instruction;
                if (control.control_flow != sh4::ControlFlowKind::ConditionalBranch) continue;
                bool successor_inside = false;
                bool successor_outside = false;
                for (const auto successor_address : block.successors) {
                    const auto found = block_by_address.find(successor_address);
                    if (found != block_by_address.end() && is_member(found->second))
                        successor_inside = true;
                    else
                        successor_outside = true;
                }
                if (!successor_inside || !successor_outside) continue;

                std::size_t condition_block = block_index;
                std::size_t condition_index = control_index;
                std::vector<bool> visited_condition_blocks(blocks.size(), false);
                bool condition_found = false;
                while (!condition_found) {
                    while (condition_index != 0u) {
                        --condition_index;
                        if (instruction_writes_t(
                                blocks[condition_block].lines[condition_index].instruction)) {
                            condition_found = true;
                            break;
                        }
                    }
                    if (condition_found) break;
                    visited_condition_blocks[condition_block] = true;
                    if (predecessors[condition_block].size() != 1u) break;
                    const auto predecessor = predecessors[condition_block].front();
                    if (!is_member(predecessor) || visited_condition_blocks[predecessor]) break;
                    condition_block = predecessor;
                    condition_index = blocks[condition_block].lines.size();
                }
                if (!condition_found) {
                    loop.unresolved_guard_access = true;
                    continue;
                }

                const auto& condition_line = blocks[condition_block].lines[condition_index];
                const auto& condition = condition_line.instruction;
                if (condition.kind == sh4::InstructionKind::DecrementAndTest) {
                    loop.counter_instruction_addresses.push_back(condition_line.address);
                    continue;
                }
                if (condition.kind == sh4::InstructionKind::TestByteImmediate ||
                    condition.kind == sh4::InstructionKind::TestAndSetByte ||
                    (condition.kind == sh4::InstructionKind::LoadSpecialRegisterPostIncrement &&
                     condition.special_register == sh4::SpecialRegister::Sr)) {
                    if (mark_direct_guard_access(condition_line.address) !=
                        GuardReadResolution::ResolvedAddress)
                        loop.unresolved_guard_access = true;
                    record_guard_address_progress(condition_line.address,
                                                  condition_line.address);
                    continue;
                }
                if (condition.kind == sh4::InstructionKind::FcmpEqual ||
                    condition.kind == sh4::InstructionKind::FcmpGreater) {
                    // FPSCR.PR/SZ/FR, FR/XF banking, FPUL transfers and vector operations make
                    // scalar-only backward slicing unsound. Keep every loop-local memory read as
                    // an explicit unresolved FCMP guard candidate until that full state is modeled.
                    if (!has_unresolved_fcmp_guard) {
                        for (const auto& [instruction_address, is_read] :
                             syntactic_read_by_instruction) {
                            if (is_read) {
                                loop.unresolved_guard_read_instruction_addresses.push_back(
                                    instruction_address);
                                record_guard_address_progress(
                                    condition_line.address,
                                    instruction_address);
                            }
                        }
                        has_unresolved_fcmp_guard = true;
                    }
                    loop.unresolved_guard_access = true;
                    continue;
                }
                auto required_registers = condition_register_mask(condition);
                if (required_registers == 0u) {
                    loop.unresolved_guard_access = true;
                    continue;
                }

                std::size_t writer_block = condition_block;
                std::size_t writer_position = condition_index;
                std::vector<bool> visited_writer_blocks(blocks.size(), false);
                bool provenance_complete = true;
                while (required_registers != 0u) {
                    while (writer_position != 0u && required_registers != 0u) {
                        --writer_position;
                        const auto& writer = blocks[writer_block].lines[writer_position];
                        const auto writes = static_cast<std::uint16_t>(
                            general_register_write_mask(writer.instruction) & required_registers);
                        if (writes == 0u) continue;

                        if (is_potential_memory_load(writer.instruction.kind)) {
                            const auto loaded_register =
                                loop_register_bit(writer.instruction.destination_register);
                            const auto loaded_outputs =
                                static_cast<std::uint16_t>(writes & loaded_register);
                            if (loaded_outputs != 0u) {
                                required_registers = static_cast<std::uint16_t>(required_registers &
                                                                                ~loaded_outputs);
                                if (mark_direct_guard_access(writer.address) !=
                                    GuardReadResolution::ResolvedAddress) {
                                    provenance_complete = false;
                                }
                                record_guard_address_progress(
                                    condition_line.address,
                                    writer.address);
                            }
                            const auto non_value_outputs =
                                static_cast<std::uint16_t>(writes & ~loaded_register);
                            const auto postincrement =
                                writer.instruction.kind ==
                                    sh4::InstructionKind::MovByteLoadPostIncrement ||
                                writer.instruction.kind ==
                                    sh4::InstructionKind::MovWordLoadPostIncrement ||
                                writer.instruction.kind ==
                                    sh4::InstructionKind::MovLongLoadPostIncrement;
                            if (non_value_outputs != 0u && !postincrement) {
                                required_registers = static_cast<std::uint16_t>(required_registers &
                                                                                ~non_value_outputs);
                                provenance_complete = false;
                            }
                            continue;
                        }

                        const auto writer_trace =
                            propagate_basic_block_constants(
                                blocks[writer_block].lines,
                                image,
                                block_entry_constants[writer_block]);
                        for (std::uint8_t register_index = 0u;
                             register_index < 16u; ++register_index) {
                            const auto register_mask =
                                loop_register_bit(register_index);
                            if ((writes & register_mask) == 0u) continue;
                            const auto kind = local_progress_kind(
                                writer,
                                register_index,
                                writer_trace[writer_position].before);
                            if (kind.has_value())
                                append_local_progress(condition_line.address,
                                                      writer,
                                                      register_index,
                                                      *kind);
                        }
                        const auto inputs = guard_input_registers(writer.instruction);
                        required_registers =
                            static_cast<std::uint16_t>(required_registers & ~writes);
                        if (inputs.has_value())
                            required_registers =
                                static_cast<std::uint16_t>(required_registers | *inputs);
                        else
                            provenance_complete = false;
                    }
                    if (required_registers == 0u) break;
                    visited_writer_blocks[writer_block] = true;
                    if (predecessors[writer_block].size() != 1u) {
                        provenance_complete = false;
                        break;
                    }
                    const auto predecessor = predecessors[writer_block].front();
                    if (!is_member(predecessor) || visited_writer_blocks[predecessor]) {
                        provenance_complete = false;
                        break;
                    }
                    writer_block = predecessor;
                    writer_position = blocks[writer_block].lines.size();
                }
                if (!provenance_complete || required_registers != 0u)
                    loop.unresolved_guard_access = true;
            }

            std::sort(loop.block_addresses.begin(), loop.block_addresses.end());
            std::sort(loop.counter_instruction_addresses.begin(),
                      loop.counter_instruction_addresses.end());
            std::sort(loop.local_progress_evidence.begin(),
                      loop.local_progress_evidence.end(),
                      [](const auto& left, const auto& right) {
                          return std::tie(
                                     left.condition_instruction_address,
                                     left.progress_instruction_address,
                                     left.register_index,
                                     left.kind) <
                                 std::tie(
                                     right.condition_instruction_address,
                                     right.progress_instruction_address,
                                     right.register_index,
                                     right.kind);
                      });
            loop.local_progress_evidence.erase(
                std::unique(loop.local_progress_evidence.begin(),
                            loop.local_progress_evidence.end(),
                            [](const auto& left, const auto& right) {
                                return left.condition_instruction_address ==
                                           right.condition_instruction_address &&
                                       left.progress_instruction_address ==
                                           right.progress_instruction_address &&
                                       left.register_index == right.register_index &&
                                       left.kind == right.kind;
                            }),
                loop.local_progress_evidence.end());
            std::sort(loop.unresolved_guard_read_instruction_addresses.begin(),
                      loop.unresolved_guard_read_instruction_addresses.end());
            loop.unresolved_guard_read_instruction_addresses.erase(
                std::unique(loop.unresolved_guard_read_instruction_addresses.begin(),
                            loop.unresolved_guard_read_instruction_addresses.end()),
                loop.unresolved_guard_read_instruction_addresses.end());
            std::sort(loop.accesses.begin(),
                      loop.accesses.end(),
                      [](const auto& left, const auto& right) {
                          return std::tie(left.instruction_address,
                                          left.guest_address,
                                          left.kind,
                                          left.width) < std::tie(right.instruction_address,
                                                                 right.guest_address,
                                                                 right.kind,
                                                                 right.width);
                      });
            loop.classification = classify_loop(loop);
            constexpr std::size_t maximum_matching_write_candidates = 256u;
            for (const auto& guard : loop.accesses) {
                if (!guard.guards_loop || !guard.linear_memory ||
                    guard.kind != HardwareAccessKind::Read)
                    continue;
                const auto guard_begin =
                    static_cast<std::uint64_t>(guard.canonical_address);
                const auto guard_end = guard_begin + guard.width;
                for (const auto& write : resolved_linear_writes) {
                    const auto write_begin =
                        static_cast<std::uint64_t>(write.canonical_address);
                    const auto write_end = write_begin + write.width;
                    if (guard_begin >= write_end || write_begin >= guard_end)
                        continue;
                    if (loop.matching_write_candidates.size() ==
                        maximum_matching_write_candidates) {
                        loop.matching_write_candidates_truncated = true;
                        break;
                    }
                    loop.matching_write_candidates.push_back(write);
                }
                if (loop.matching_write_candidates_truncated) break;
            }
            std::sort(loop.matching_write_candidates.begin(),
                      loop.matching_write_candidates.end(),
                      [](const auto& left, const auto& right) {
                          return std::tie(left.instruction_address,
                                          left.canonical_address,
                                          left.width) <
                                 std::tie(right.instruction_address,
                                          right.canonical_address,
                                          right.width);
                      });
            loop.matching_write_candidates.erase(
                std::unique(loop.matching_write_candidates.begin(),
                            loop.matching_write_candidates.end(),
                            [](const auto& left, const auto& right) {
                                return left.instruction_address ==
                                           right.instruction_address &&
                                       left.canonical_address ==
                                           right.canonical_address &&
                                       left.width == right.width;
                            }),
                loop.matching_write_candidates.end());
            loops.push_back(std::move(loop));
        }
    }
    std::sort(loops.begin(), loops.end(), [](const auto& left, const auto& right) {
        return std::tie(
                   left.header_address, left.latch_address, left.backedge_instruction_address) <
               std::tie(
                   right.header_address, right.latch_address, right.backedge_instruction_address);
    });
    return loops;
}

} // namespace

DreamcastHardwareAudit audit_dreamcast_hardware(const io::ExecutableImage& image,
                                                const ControlFlowAnalysisResult& analysis) {
    DreamcastHardwareAudit result;
    for (const auto& segment : image.segments())
        result.image_bytes += segment.bytes.size();
    result.reachable_instructions = analysis.recursive.instructions.size();
    result.reachable_functions = analysis.recursive.functions.size();
    result.unknown_instructions = analysis.recursive.diagnostics.size();
    for (const auto& source : analysis.recursive.diagnostics) {
        HardwareInstructionDiagnostic diagnostic;
        diagnostic.address = source.address;
        diagnostic.opcode = source.opcode;
        diagnostic.reason = source.reason;
        diagnostic.evidence = source.evidence;
        for (const auto& context : analysis.recursive.contextual_instructions) {
            if (context.line.address != source.address) continue;
            diagnostic.incoming_addresses.push_back(context.incoming_address);
            if (context.delay_slot_owner)
                diagnostic.delay_slot_owners.push_back(*context.delay_slot_owner);
        }
        std::sort(diagnostic.incoming_addresses.begin(), diagnostic.incoming_addresses.end());
        diagnostic.incoming_addresses.erase(
            std::unique(diagnostic.incoming_addresses.begin(), diagnostic.incoming_addresses.end()),
            diagnostic.incoming_addresses.end());
        std::sort(diagnostic.delay_slot_owners.begin(), diagnostic.delay_slot_owners.end());
        diagnostic.delay_slot_owners.erase(
            std::unique(diagnostic.delay_slot_owners.begin(), diagnostic.delay_slot_owners.end()),
            diagnostic.delay_slot_owners.end());
        result.instruction_diagnostics.push_back(std::move(diagnostic));
    }
    struct AggregatedReference {
        HardwareAccessReference reference;
        std::size_t multiplicity = 0u;
    };
    using ReferenceKey =
        std::tuple<std::uint32_t, std::uint32_t, HardwareAccessKind,
                   std::uint8_t, bool, std::string>;

    struct MemorySiteAggregate final {
        bool complete = true;
        std::uint8_t access_mask = 0u;
        std::uint8_t width_mask = 0u;
    };
    std::map<std::uint32_t, MemorySiteAggregate> complete_memory_sites;
    std::map<ReferenceKey, AggregatedReference> aggregated_references;
    std::unordered_map<std::uint32_t, TableDrivenAccessResolution>
        table_driven_store_resolutions;
    std::unordered_map<std::uint32_t, TableDrivenAccessResolution> table_scan_cache;
    table_scan_cache.reserve(64u);
    if (analysis.instruction_arena) {
        const auto instructions = std::span<const sh4::DisassemblyLine>(
            analysis.recursive.instructions.data(),
            analysis.recursive.instructions.size());
        table_driven_store_resolutions.reserve(instructions.size() / 32u + 1u);
        for (std::size_t index = 0u; index < instructions.size(); ++index) {
            if (instructions[index].instruction.kind != sh4::InstructionKind::MovLongStore)
                continue;
            auto resolution = resolve_table_driven_store(
                image, instructions, index, table_scan_cache);
            if (!resolution.recognized) continue;
            const auto [existing, inserted] = table_driven_store_resolutions.try_emplace(
                instructions[index].address);
            if (inserted) {
                existing->second = std::move(resolution);
                continue;
            }
            // Multiple CFG contexts may expose the same store.  A complete
            // table proof survives only when every context is complete; the
            // target union remains useful for reporting possible hardware
            // sites, but never upgrades an incomplete scan.
            existing->second.access_set.complete =
                existing->second.access_set.complete && resolution.access_set.complete;
            for (const auto& access : resolution.access_set.accesses) {
                const auto duplicate = std::find_if(
                    existing->second.access_set.accesses.begin(),
                    existing->second.access_set.accesses.end(),
                    [&access](const auto& current) {
                        return current.address == access.address &&
                               current.kind == access.kind && current.width == access.width;
                    });
                if (duplicate == existing->second.access_set.accesses.end())
                    existing->second.access_set.accesses.push_back(access);
            }
        }
    }
    if (analysis.instruction_arena) {
        for (const auto& span : analysis.block_spans) {
            const auto lines = span.view(*analysis.instruction_arena);
            const auto trace = propagate_local_constants(lines, image);
            const auto gbr_trace = propagate_local_gbr(lines, trace);
            for (std::size_t index = 0u; index < lines.size(); ++index) {
                if (!is_memory_access_instruction(lines[index].instruction.kind)) continue;
                auto access_set =
                    effective_accesses(lines[index], trace[index].before, gbr_trace[index]);
                if (!access_set.complete) {
                    const auto bounded_prefetch =
                        bounded_store_queue_prefetch_access(lines, trace, index);
                    if (bounded_prefetch.has_value()) {
                        access_set.accesses = {*bounded_prefetch};
                        access_set.complete = true;
                    }
                }
                if (!access_set.complete) {
                    const auto table = table_driven_store_resolutions.find(lines[index].address);
                    if (table != table_driven_store_resolutions.end() &&
                        table->second.recognized)
                        access_set = table->second.access_set;
                }
                const auto shape =
                    memory_access_shape(lines[index].instruction);
                const auto [site, inserted] = complete_memory_sites.emplace(
                    lines[index].address,
                    MemorySiteAggregate{access_set.complete,
                                        shape.access_mask,
                                        shape.width_mask});
                if (!inserted) {
                    site->second.complete =
                        site->second.complete && access_set.complete;
                    site->second.access_mask |= shape.access_mask;
                    site->second.width_mask |= shape.width_mask;
                }
                std::map<ReferenceKey, AggregatedReference> context_references;
                for (const auto& access : access_set.accesses) {
                    const auto projected_address =
                        project_runtime_image_access(
                            image,
                            lines[index].address,
                            access.address,
                            access.width);
                    const auto description = describe_image_access(
                        image, lines[index].address,
                        lines[index].instruction.kind,
                        projected_address, access.width);
                    // Unknown is ordinary memory only inside the verified
                    // linear main-RAM aperture. A statically resolved access
                    // anywhere else is an unmapped native-product dependency,
                    // not evidence that the hardware closure is complete.
                    if (description.region ==
                            DreamcastHardwareRegion::Unknown &&
                        is_linear_loop_memory(description))
                        continue;
                    HardwareAccessReference reference;
                    reference.instruction_address = lines[index].address;
                    reference.guest_address = projected_address;
                    reference.canonical_address_known = access.address_known;
                    reference.canonical_address = description.canonical;
                    reference.address_expression = access.address_expression;
                    reference.region = description.region;
                    reference.kind = access.kind;
                    reference.width = access.width;
                    reference.aperture_mapped = description.aperture_mapped;
                    reference.runtime_support =
                        assess_support(description, access.kind, access.width);
                    reference.support_reason =
                        support_reason(description, reference.runtime_support);
                    reference.register_name = description.name;
                    const ReferenceKey key{reference.instruction_address,
                                           reference.guest_address,
                                           reference.kind,
                                           reference.width,
                                           reference.canonical_address_known,
                                           reference.address_expression};
                    const auto [context, context_inserted] =
                        context_references.try_emplace(key, AggregatedReference{reference, 0u});
                    static_cast<void>(context_inserted);
                    ++context->second.multiplicity;
                }
                for (auto& [key, context] : context_references) {
                    const auto [aggregate, aggregate_inserted] =
                        aggregated_references.try_emplace(key, context);
                    if (!aggregate_inserted &&
                        context.multiplicity > aggregate->second.multiplicity) {
                        aggregate->second = std::move(context);
                    }
                }
            }
        }
    }
    result.memory_access_sites = complete_memory_sites.size();
    for (const auto& site : complete_memory_sites) {
        if (site.second.complete)
            ++result.resolved_memory_access_sites;
        else {
            ++result.unresolved_memory_access_sites;
            result.unresolved_memory_instruction_sites.push_back(
                {site.first,
                 site.second.access_mask,
                 site.second.width_mask});
        }
    }
    for (const auto& entry : aggregated_references) {
        for (std::size_t occurrence = 0u; occurrence < entry.second.multiplicity; ++occurrence)
            result.references.push_back(entry.second.reference);
    }
    std::sort(
        result.references.begin(),
        result.references.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.guest_address,
                            left.instruction_address,
                            left.kind,
                            left.width,
                            left.canonical_address_known,
                            left.canonical_address,
                            left.address_expression) <
                   std::tie(right.guest_address,
                            right.instruction_address,
                            right.kind,
                            right.width,
                            right.canonical_address_known,
                            right.canonical_address,
                            right.address_expression);
        });
    for (const auto& reference : result.references) {
        if (result.addresses.empty() ||
            result.addresses.back().guest_address != reference.guest_address) {
            HardwareAddressSummary summary;
            summary.guest_address = reference.guest_address;
            summary.canonical_address = reference.canonical_address;
            summary.region = reference.region;
            summary.aperture_mapped = reference.aperture_mapped;
            summary.runtime_support = reference.runtime_support;
            summary.support_reason = reference.support_reason;
            summary.register_name = reference.register_name;
            result.addresses.push_back(std::move(summary));
        }
        auto& summary = result.addresses.back();
        if (static_cast<std::uint8_t>(reference.runtime_support) >
            static_cast<std::uint8_t>(summary.runtime_support)) {
            summary.runtime_support = reference.runtime_support;
            summary.support_reason = reference.support_reason;
        }
        if (reference.kind == HardwareAccessKind::Read)
            ++summary.reads;
        else if (reference.kind == HardwareAccessKind::Write)
            ++summary.writes;
        else
            ++summary.prefetches;
        if (std::find(summary.widths.begin(), summary.widths.end(), reference.width) ==
            summary.widths.end())
            summary.widths.push_back(reference.width);
        summary.instruction_addresses.push_back(reference.instruction_address);
    }
    for (auto& summary : result.addresses) {
        std::sort(summary.widths.begin(), summary.widths.end());
        switch (summary.runtime_support) {
        case HardwareRuntimeSupport::Implemented:
            ++result.implemented_addresses;
            break;
        case HardwareRuntimeSupport::Partial:
            ++result.partial_addresses;
            break;
        case HardwareRuntimeSupport::KnownGap:
            ++result.known_gap_addresses;
            break;
        case HardwareRuntimeSupport::Rejected:
            ++result.rejected_addresses;
            break;
        case HardwareRuntimeSupport::Unmapped:
            ++result.unmapped_addresses;
            break;
        }
    }
    result.loops = find_natural_hardware_loops(image, analysis);
    result.unresolved_poll_guard_loops = count_unresolved_poll_guard_loops(result.loops);
    return result;
}

const char* dreamcast_hardware_region_name(const DreamcastHardwareRegion region) noexcept {
    switch (region) {
    case DreamcastHardwareRegion::SystemBus:
        return "system_bus";
    case DreamcastHardwareRegion::SystemAsic:
        return "system_asic";
    case DreamcastHardwareRegion::Maple:
        return "maple";
    case DreamcastHardwareRegion::GdRom:
        return "gdrom";
    case DreamcastHardwareRegion::G1Dma:
        return "g1_dma";
    case DreamcastHardwareRegion::G2Dma:
        return "g2_dma";
    case DreamcastHardwareRegion::PvrDma:
        return "pvr_dma";
    case DreamcastHardwareRegion::Pvr:
        return "pvr";
    case DreamcastHardwareRegion::Aica:
        return "aica";
    case DreamcastHardwareRegion::AicaRtc:
        return "aica_rtc";
    case DreamcastHardwareRegion::AicaRam:
        return "aica_ram";
    case DreamcastHardwareRegion::TaFifo:
        return "ta_fifo";
    case DreamcastHardwareRegion::TaYuv:
        return "ta_yuv";
    case DreamcastHardwareRegion::TaVram:
        return "ta_vram";
    case DreamcastHardwareRegion::Vram64:
        return "vram_64";
    case DreamcastHardwareRegion::Vram32:
        return "vram_32";
    case DreamcastHardwareRegion::StoreQueue:
        return "store_queue";
    case DreamcastHardwareRegion::Sh4OnChipRam:
        return "sh4_on_chip_ram";
    case DreamcastHardwareRegion::Sh4Mmu:
        return "sh4_mmu";
    case DreamcastHardwareRegion::Sh4Cache:
        return "sh4_cache";
    case DreamcastHardwareRegion::Sh4Exception:
        return "sh4_exception";
    case DreamcastHardwareRegion::Sh4Qacr:
        return "sh4_qacr";
    case DreamcastHardwareRegion::Sh4Io:
        return "sh4_io";
    case DreamcastHardwareRegion::Sh4Dmac:
        return "sh4_dmac";
    case DreamcastHardwareRegion::Sh4Rtc:
        return "sh4_rtc";
    case DreamcastHardwareRegion::Sh4Intc:
        return "sh4_intc";
    case DreamcastHardwareRegion::Sh4Tmu:
        return "sh4_tmu";
    case DreamcastHardwareRegion::Sh4Scif:
        return "sh4_scif";
    case DreamcastHardwareRegion::Sh4P4:
        return "sh4_p4_unmapped";
    case DreamcastHardwareRegion::Unknown:
        return "unknown";
    }
    return "unknown";
}

const char* hardware_access_kind_name(const HardwareAccessKind kind) noexcept {
    switch (kind) {
    case HardwareAccessKind::Read:
        return "read";
    case HardwareAccessKind::Write:
        return "write";
    case HardwareAccessKind::Prefetch:
        return "prefetch";
    }
    return "read";
}

const char*
hardware_loop_classification_name(const HardwareLoopClassification classification) noexcept {
    switch (classification) {
    case HardwareLoopClassification::Counter:
        return "counter";
    case HardwareLoopClassification::RamPoll:
        return "ram_poll";
    case HardwareLoopClassification::MmioPoll:
        return "mmio_poll";
    case HardwareLoopClassification::Mixed:
        return "mixed";
    case HardwareLoopClassification::Unknown:
        return "unknown";
    }
    return "unknown";
}

const char* hardware_loop_local_progress_kind_name(
    const HardwareLoopLocalProgressKind kind) noexcept {
    switch (kind) {
    case HardwareLoopLocalProgressKind::IntegerInduction:
        return "integer_induction";
    case HardwareLoopLocalProgressKind::AddressPostIncrement:
        return "address_postincrement";
    case HardwareLoopLocalProgressKind::PointerTraversal:
        return "pointer_traversal";
    }
    return "integer_induction";
}

const char* hardware_runtime_support_name(const HardwareRuntimeSupport support) noexcept {
    switch (support) {
    case HardwareRuntimeSupport::Implemented:
        return "implemented";
    case HardwareRuntimeSupport::Partial:
        return "partial";
    case HardwareRuntimeSupport::KnownGap:
        return "known_gap";
    case HardwareRuntimeSupport::Rejected:
        return "rejected";
    case HardwareRuntimeSupport::Unmapped:
        return "unmapped";
    }
    return "unmapped";
}

std::string format_hardware_audit_text(const DreamcastHardwareAudit& audit) {
    std::ostringstream output;
    output << "Scope: " << audit.scope << "\n"
           << "Image bytes: " << audit.image_bytes << "\n"
           << "Reachable instructions: " << audit.reachable_instructions << "\n"
           << "Reachable functions: " << audit.reachable_functions << "\n"
           << "Unknown instructions: " << audit.unknown_instructions << "\n"
           << "Memory access sites: " << audit.memory_access_sites
           << " (constant=" << audit.resolved_memory_access_sites
           << ", dynamic=" << audit.unresolved_memory_access_sites << ")\n"
           << "Unresolved memory instructions: "
           << audit.unresolved_memory_instruction_sites.size() << "\n"
           << "Hardware addresses: " << audit.addresses.size()
           << " (implemented=" << audit.implemented_addresses
           << ", partial=" << audit.partial_addresses << ", known_gap=" << audit.known_gap_addresses
           << ", rejected=" << audit.rejected_addresses << ", unmapped=" << audit.unmapped_addresses
           << ")\n"
           << "Natural loops: " << audit.loops.size() << "\n"
           << "Unresolved poll/guard loops: " << audit.unresolved_poll_guard_loops << "\n";
    for (const auto& diagnostic : audit.instruction_diagnostics) {
        output << "Instruction diagnostic: address=" << hex8(diagnostic.address)
               << " opcode=" << hex4(diagnostic.opcode) << " reason=" << diagnostic.reason
               << " evidence=" << control_flow_evidence_name(diagnostic.evidence) << " incoming=";
        for (std::size_t index = 0u; index < diagnostic.incoming_addresses.size(); ++index) {
            if (index != 0u) output << ',';
            output << hex8(diagnostic.incoming_addresses[index]);
        }
        output << '\n';
    }
    for (const auto& loop : audit.loops) {
        output << "Loop: header=" << hex8(loop.header_address)
               << " latch=" << hex8(loop.latch_address)
               << " backedge=" << hex8(loop.backedge_instruction_address)
               << " classification=" << hardware_loop_classification_name(loop.classification)
               << " unresolved_guard_access=" << (loop.unresolved_guard_access ? "yes" : "no")
               << " unresolved_guard_read_sites=";
        for (std::size_t index = 0u;
             index < loop.unresolved_guard_read_instruction_addresses.size();
             ++index) {
            if (index != 0u) output << ',';
            output << hex8(loop.unresolved_guard_read_instruction_addresses[index]);
        }
        output << " blocks=";
        for (std::size_t index = 0u; index < loop.block_addresses.size(); ++index) {
            if (index != 0u) output << ',';
            output << hex8(loop.block_addresses[index]);
        }
        output << " counter_sites=";
        for (std::size_t index = 0u; index < loop.counter_instruction_addresses.size(); ++index) {
            if (index != 0u) output << ',';
            output << hex8(loop.counter_instruction_addresses[index]);
        }
        output << " local_progress=";
        for (std::size_t index = 0u;
             index < loop.local_progress_evidence.size(); ++index) {
            if (index != 0u) output << ',';
            const auto& evidence = loop.local_progress_evidence[index];
            output << hardware_loop_local_progress_kind_name(evidence.kind)
                   << '@' << hex8(evidence.progress_instruction_address)
                   << ":r" << static_cast<unsigned>(evidence.register_index)
                   << ":condition="
                   << hex8(evidence.condition_instruction_address);
        }
        output << '\n';
        for (const auto& access : loop.accesses) {
            output << "  access=" << hex8(access.instruction_address)
                   << " guest=" << hex8(access.guest_address)
                   << " canonical=" << hex8(access.canonical_address)
                   << " region=" << dreamcast_hardware_region_name(access.region)
                   << " kind=" << hardware_access_kind_name(access.kind)
                   << " width=" << static_cast<unsigned>(access.width)
                   << " storage=" << (access.linear_memory ? "linear" : "device")
                   << " aperture=" << (access.aperture_mapped ? "mapped" : "unmapped")
                   << " support=" << hardware_runtime_support_name(access.runtime_support)
                   << " guards_loop=" << (access.guards_loop ? "yes" : "no") << '\n';
        }
    }
    for (const auto& address : audit.addresses) {
        output << hex8(address.guest_address) << " canonical=" << hex8(address.canonical_address)
               << " region=" << dreamcast_hardware_region_name(address.region)
               << " aperture=" << (address.aperture_mapped ? "mapped" : "unmapped")
               << " support=" << hardware_runtime_support_name(address.runtime_support)
               << " reason=" << address.support_reason << " reads=" << address.reads
               << " writes=" << address.writes << " prefetches=" << address.prefetches;
        if (!address.register_name.empty()) output << " register=" << address.register_name;
        output << " widths=";
        for (std::size_t index = 0u; index < address.widths.size(); ++index) {
            if (index != 0u) output << ',';
            output << static_cast<unsigned>(address.widths[index]);
        }
        output << " sites=";
        for (std::size_t index = 0u; index < address.instruction_addresses.size(); ++index) {
            if (index != 0u) output << ',';
            output << hex8(address.instruction_addresses[index]);
        }
        output << '\n';
    }
    return output.str();
}

std::string format_hardware_audit_json(const DreamcastHardwareAudit& audit,
                                       const bool include_accesses) {
    std::ostringstream output;
    io::write_json_report_header(output, "katana.hardware-audit.v6", "dreamcast_hardware_audit");
    output << ",\"scope\":" << io::quote_json(audit.scope)
           << ",\"image_bytes\":" << audit.image_bytes
           << ",\"reachable_instructions\":" << audit.reachable_instructions
           << ",\"reachable_functions\":" << audit.reachable_functions
           << ",\"unknown_instructions\":" << audit.unknown_instructions
           << ",\"memory_access_sites\":" << audit.memory_access_sites
           << ",\"resolved_memory_access_sites\":" << audit.resolved_memory_access_sites
           << ",\"unresolved_memory_access_sites\":" << audit.unresolved_memory_access_sites
           << ",\"unresolved_memory_instruction_sites\":[";
    for (std::size_t index = 0u;
         index < audit.unresolved_memory_instruction_sites.size();
         ++index) {
        if (index != 0u) output << ',';
        const auto& site = audit.unresolved_memory_instruction_sites[index];
        output << "{\"instruction_address\":"
               << io::quote_json(hex8(site.instruction_address))
               << ",\"access_mask\":"
               << static_cast<unsigned>(site.access_mask)
               << ",\"width_mask\":"
               << static_cast<unsigned>(site.width_mask) << '}';
    }
    output << "]"
           << ",\"implemented_addresses\":" << audit.implemented_addresses
           << ",\"partial_addresses\":" << audit.partial_addresses
           << ",\"known_gap_addresses\":" << audit.known_gap_addresses
           << ",\"rejected_addresses\":" << audit.rejected_addresses
           << ",\"unmapped_addresses\":" << audit.unmapped_addresses
           << ",\"natural_loops\":" << audit.loops.size()
           << ",\"unresolved_poll_guard_loops\":" << audit.unresolved_poll_guard_loops
           << ",\"instruction_diagnostics\":[";
    for (std::size_t index = 0u; index < audit.instruction_diagnostics.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& diagnostic = audit.instruction_diagnostics[index];
        output << "{\"address\":" << io::quote_json(hex8(diagnostic.address))
               << ",\"opcode\":" << io::quote_json(hex4(diagnostic.opcode))
               << ",\"reason\":" << io::quote_json(diagnostic.reason) << ",\"evidence\":"
               << io::quote_json(control_flow_evidence_name(diagnostic.evidence))
               << ",\"incoming_addresses\":[";
        for (std::size_t incoming = 0u; incoming < diagnostic.incoming_addresses.size();
             ++incoming) {
            if (incoming != 0u) output << ',';
            output << io::quote_json(hex8(diagnostic.incoming_addresses[incoming]));
        }
        output << "],\"delay_slot_owners\":[";
        for (std::size_t owner = 0u; owner < diagnostic.delay_slot_owners.size(); ++owner) {
            if (owner != 0u) output << ',';
            output << io::quote_json(hex8(diagnostic.delay_slot_owners[owner]));
        }
        output << "]}";
    }
    output << "]"
           << ",\"loops\":[";
    for (std::size_t index = 0u; index < audit.loops.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& loop = audit.loops[index];
        output << "{\"header_address\":" << io::quote_json(hex8(loop.header_address))
               << ",\"latch_address\":" << io::quote_json(hex8(loop.latch_address))
               << ",\"backedge_instruction_address\":"
               << io::quote_json(hex8(loop.backedge_instruction_address)) << ",\"classification\":"
               << io::quote_json(hardware_loop_classification_name(loop.classification))
               << ",\"unresolved_guard_access\":"
               << (loop.unresolved_guard_access ? "true" : "false")
               << ",\"unresolved_guard_read_instruction_addresses\":[";
        for (std::size_t site = 0u; site < loop.unresolved_guard_read_instruction_addresses.size();
             ++site) {
            if (site != 0u) output << ',';
            output << io::quote_json(hex8(loop.unresolved_guard_read_instruction_addresses[site]));
        }
        output << "]"
               << ",\"block_addresses\":[";
        for (std::size_t block = 0u; block < loop.block_addresses.size(); ++block) {
            if (block != 0u) output << ',';
            output << io::quote_json(hex8(loop.block_addresses[block]));
        }
        output << "],\"counter_instruction_addresses\":[";
        for (std::size_t site = 0u; site < loop.counter_instruction_addresses.size(); ++site) {
            if (site != 0u) output << ',';
            output << io::quote_json(hex8(loop.counter_instruction_addresses[site]));
        }
        output << "],\"local_progress_evidence\":[";
        for (std::size_t evidence_index = 0u;
             evidence_index < loop.local_progress_evidence.size();
             ++evidence_index) {
            if (evidence_index != 0u) output << ',';
            const auto& evidence =
                loop.local_progress_evidence[evidence_index];
            output << "{\"condition_instruction_address\":"
                   << io::quote_json(
                          hex8(evidence.condition_instruction_address))
                   << ",\"progress_instruction_address\":"
                   << io::quote_json(
                          hex8(evidence.progress_instruction_address))
                   << ",\"register_index\":"
                   << static_cast<unsigned>(evidence.register_index)
                   << ",\"kind\":"
                   << io::quote_json(
                          hardware_loop_local_progress_kind_name(
                              evidence.kind))
                   << '}';
        }
        output << "],\"accesses\":[";
        for (std::size_t access_index = 0u; access_index < loop.accesses.size(); ++access_index) {
            if (access_index != 0u) output << ',';
            const auto& access = loop.accesses[access_index];
            output << "{\"instruction_address\":"
                   << io::quote_json(hex8(access.instruction_address))
                   << ",\"guest_address\":" << io::quote_json(hex8(access.guest_address))
                   << ",\"canonical_address\":" << io::quote_json(hex8(access.canonical_address))
                   << ",\"region\":"
                   << io::quote_json(dreamcast_hardware_region_name(access.region))
                   << ",\"kind\":" << io::quote_json(hardware_access_kind_name(access.kind))
                   << ",\"width\":" << static_cast<unsigned>(access.width)
                   << ",\"linear_memory\":" << (access.linear_memory ? "true" : "false")
                   << ",\"aperture_mapped\":" << (access.aperture_mapped ? "true" : "false")
                   << ",\"runtime_support\":"
                   << io::quote_json(hardware_runtime_support_name(access.runtime_support))
                   << ",\"guards_loop\":" << (access.guards_loop ? "true" : "false") << '}';
        }
        output << "]}";
    }
    output << "]"
           << ",\"addresses\":[";
    for (std::size_t index = 0u; index < audit.addresses.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& address = audit.addresses[index];
        output << "{\"guest_address\":\"" << hex8(address.guest_address)
               << "\",\"canonical_address\":\"" << hex8(address.canonical_address)
               << "\",\"region\":" << io::quote_json(dreamcast_hardware_region_name(address.region))
               << ",\"aperture_mapped\":" << (address.aperture_mapped ? "true" : "false")
               << ",\"runtime_support\":"
               << io::quote_json(hardware_runtime_support_name(address.runtime_support))
               << ",\"support_reason\":" << io::quote_json(address.support_reason)
               << ",\"register\":" << io::quote_json(address.register_name)
               << ",\"reads\":" << address.reads << ",\"writes\":" << address.writes
               << ",\"prefetches\":" << address.prefetches << ",\"widths\":[";
        for (std::size_t width = 0u; width < address.widths.size(); ++width) {
            if (width != 0u) output << ',';
            output << static_cast<unsigned>(address.widths[width]);
        }
        output << "]"
               << ",\"instruction_addresses\":[";
        for (std::size_t site = 0u; site < address.instruction_addresses.size(); ++site) {
            if (site != 0u) output << ',';
            output << io::quote_json(hex8(address.instruction_addresses[site]));
        }
        output << "]}";
    }
    output << ']';
    if (include_accesses) {
        output << ",\"accesses\":[";
        for (std::size_t index = 0u; index < audit.references.size(); ++index) {
            if (index != 0u) output << ',';
            const auto& reference = audit.references[index];
            output << "{\"instruction_address\":"
                   << io::quote_json(hex8(reference.instruction_address))
                   << ",\"guest_address\":" << io::quote_json(hex8(reference.guest_address))
                   << ",\"canonical_address\":" << io::quote_json(hex8(reference.canonical_address))
                   << ",\"canonical_address_known\":"
                   << (reference.canonical_address_known ? "true" : "false")
                   << ",\"address_expression\":"
                   << io::quote_json(reference.address_expression)
                   << ",\"region\":"
                   << io::quote_json(dreamcast_hardware_region_name(reference.region))
                   << ",\"kind\":" << io::quote_json(hardware_access_kind_name(reference.kind))
                   << ",\"width\":" << static_cast<unsigned>(reference.width)
                   << ",\"aperture_mapped\":" << (reference.aperture_mapped ? "true" : "false")
                   << ",\"runtime_support\":"
                   << io::quote_json(hardware_runtime_support_name(reference.runtime_support))
                   << ",\"support_reason\":" << io::quote_json(reference.support_reason) << "}";
        }
        output << ']';
    }
    output << '}';
    return output.str();
}

} // namespace katana::analysis
