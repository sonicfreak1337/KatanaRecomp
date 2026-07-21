#include "katana/analysis/hardware_audit.hpp"

#include "katana/analysis/value_analysis.hpp"
#include "katana/io/json_report.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <tuple>
#include <utility>

namespace katana::analysis {
namespace {

struct AddressDescription {
    std::uint32_t canonical = 0u;
    DreamcastHardwareRegion region = DreamcastHardwareRegion::Unknown;
    bool aperture_mapped = false;
    std::string name;
};

bool in_range(const std::uint32_t value,
              const std::uint32_t base,
              const std::uint32_t size) noexcept {
    return value >= base && static_cast<std::uint64_t>(value) <
                                static_cast<std::uint64_t>(base) + size;
}

std::uint32_t canonical_address(const std::uint32_t address) noexcept {
    if (address >= 0x80000000u && address < 0xE0000000u) return address & 0x1FFFFFFFu;
    if (address >= 0x1F000000u && address < 0x20000000u) return address | 0xE0000000u;
    return address;
}

std::string pvr_register_name(const std::uint32_t offset) {
    static constexpr std::array names{
        std::pair{0x000u, "ID"}, std::pair{0x004u, "REVISION"},
        std::pair{0x008u, "SOFTRESET"}, std::pair{0x014u, "STARTRENDER"},
        std::pair{0x020u, "PARAM_BASE"}, std::pair{0x02Cu, "REGION_BASE"},
        std::pair{0x040u, "BORDER_COL"}, std::pair{0x044u, "FB_R_CTRL"},
        std::pair{0x048u, "FB_W_CTRL"}, std::pair{0x04Cu, "FB_W_LINESTRIDE"},
        std::pair{0x050u, "FB_R_SOF1"}, std::pair{0x054u, "FB_R_SOF2"},
        std::pair{0x05Cu, "FB_R_SIZE"}, std::pair{0x060u, "FB_W_SOF1"},
        std::pair{0x064u, "FB_W_SOF2"}, std::pair{0x068u, "FB_X_CLIP"},
        std::pair{0x06Cu, "FB_Y_CLIP"}, std::pair{0x074u, "FPU_SHAD_SCALE"},
        std::pair{0x078u, "FPU_CULL_VAL"}, std::pair{0x07Cu, "FPU_PARAM_CFG"},
        std::pair{0x080u, "HALF_OFFSET"}, std::pair{0x084u, "FPU_PERP_VAL"},
        std::pair{0x088u, "ISP_BACKGND_D"}, std::pair{0x08Cu, "ISP_BACKGND_T"},
        std::pair{0x098u, "ISP_FEED_CFG"}, std::pair{0x0B0u, "FOG_COL_RAM"},
        std::pair{0x0B4u, "FOG_COL_VERT"}, std::pair{0x0B8u, "FOG_DENSITY"},
        std::pair{0x0C4u, "SPG_TRIGGER_POS"}, std::pair{0x0C8u, "SPG_HBLANK_INT"},
        std::pair{0x0CCu, "SPG_VBLANK_INT"}, std::pair{0x0D0u, "SPG_CONTROL"},
        std::pair{0x0D4u, "SPG_HBLANK"}, std::pair{0x0D8u, "SPG_LOAD"},
        std::pair{0x0DCu, "SPG_VBLANK"}, std::pair{0x0E0u, "SPG_WIDTH"},
        std::pair{0x0E4u, "TEXT_CONTROL"}, std::pair{0x0E8u, "VO_CONTROL"},
        std::pair{0x0ECu, "VO_STARTX"}, std::pair{0x0F0u, "VO_STARTY"},
        std::pair{0x0F4u, "SCALER_CTL"}, std::pair{0x108u, "PAL_RAM_CTRL"},
        std::pair{0x10Cu, "SPG_STATUS"}, std::pair{0x114u, "FB_R_SOF_CURRENT"},
        std::pair{0x11Cu, "PT_ALPHA_REF"}, std::pair{0x124u, "TA_OL_BASE"},
        std::pair{0x128u, "TA_ISP_BASE"}, std::pair{0x12Cu, "TA_OL_LIMIT"},
        std::pair{0x130u, "TA_ISP_LIMIT"}, std::pair{0x134u, "TA_NEXT_OPB"},
        std::pair{0x138u, "TA_ITP_CURRENT"}, std::pair{0x13Cu, "TA_GLOB_TILE_CLIP"},
        std::pair{0x140u, "TA_ALLOC_CTRL"}, std::pair{0x144u, "TA_LIST_INIT"},
        std::pair{0x148u, "TA_YUV_TEX_BASE"}, std::pair{0x14Cu, "TA_YUV_TEX_CTRL"},
        std::pair{0x150u, "TA_YUV_TEX_CNT"}, std::pair{0x160u, "TA_LIST_CONT"},
        std::pair{0x164u, "TA_NEXT_OPB_INIT"}};
    const auto found = std::find_if(names.begin(), names.end(),
                                    [offset](const auto& item) { return item.first == offset; });
    if (found != names.end()) return found->second;
    if (offset >= 0x200u && offset < 0x400u) return "FOG_TABLE";
    if (offset >= 0x1000u && offset < 0x2000u) return "PALETTE_RAM";
    return {};
}

template <std::size_t Size>
std::string named_offset(const std::array<std::pair<std::uint32_t, const char*>, Size>& names,
                         const std::uint32_t offset) {
    const auto found = std::find_if(names.begin(), names.end(),
                                    [offset](const auto& item) { return item.first == offset; });
    return found == names.end() ? std::string{} : std::string{found->second};
}

std::string dreamcast_register_name(const DreamcastHardwareRegion region,
                                    const std::uint32_t offset) {
    using Pair = std::pair<std::uint32_t, const char*>;
    switch (region) {
    case DreamcastHardwareRegion::SystemBus: {
        static constexpr std::array names{
            Pair{0x00u, "SB_C2DSTAT"}, Pair{0x04u, "SB_C2DLEN"},
            Pair{0x08u, "SB_C2DST"}, Pair{0x10u, "SB_SDSTAW"},
            Pair{0x14u, "SB_SDBAAW"}, Pair{0x18u, "SB_SDWLT"},
            Pair{0x1Cu, "SB_SDLAS"}, Pair{0x20u, "SB_SDST"},
            Pair{0x40u, "SB_DBREQM"}, Pair{0x44u, "SB_BAVLWC"},
            Pair{0x48u, "SB_C2DPRYC"}, Pair{0x4Cu, "SB_C2DMAXL"},
            Pair{0x60u, "SB_SDDIV"}, Pair{0x80u, "SB_TFREM"},
            Pair{0x84u, "SB_LMMODE0"}, Pair{0x88u, "SB_LMMODE1"},
            Pair{0x8Cu, "SB_FFST"}, Pair{0x90u, "SB_SFRES"},
            Pair{0x9Cu, "SB_SBREV"}, Pair{0xA0u, "SB_RBSPLT"}};
        return named_offset(names, offset);
    }
    case DreamcastHardwareRegion::SystemAsic: {
        static constexpr std::array names{
            Pair{0x00u, "ISTNRM"}, Pair{0x04u, "ISTEXT"}, Pair{0x08u, "ISTERR"},
            Pair{0x10u, "IML2NRM"}, Pair{0x14u, "IML2EXT"}, Pair{0x18u, "IML2ERR"},
            Pair{0x20u, "IML4NRM"}, Pair{0x24u, "IML4EXT"}, Pair{0x28u, "IML4ERR"},
            Pair{0x30u, "IML6NRM"}, Pair{0x34u, "IML6EXT"}, Pair{0x38u, "IML6ERR"},
            Pair{0x40u, "PDTNRM"}, Pair{0x44u, "PDTEXT"},
            Pair{0x50u, "G2DTNRM"}, Pair{0x54u, "G2DTEXT"}};
        return named_offset(names, offset);
    }
    case DreamcastHardwareRegion::Maple: {
        static constexpr std::array names{
            Pair{0x04u, "MDSTAR"}, Pair{0x10u, "MDTSEL"}, Pair{0x14u, "MDEN"},
            Pair{0x18u, "MDST"}, Pair{0x80u, "MSYS"}, Pair{0x84u, "MST"},
            Pair{0x88u, "MSHTCL"}, Pair{0x8Cu, "MDAPRO"}, Pair{0xE8u, "MMSEL"},
            Pair{0xF4u, "MTXDAD"}, Pair{0xF8u, "MRXDAD"}, Pair{0xFCu, "MRXDBD"}};
        return named_offset(names, offset);
    }
    case DreamcastHardwareRegion::GdRom: {
        static constexpr std::array names{
            Pair{0x80u, "GD_DATA"}, Pair{0x84u, "GD_ERROR"},
            Pair{0x88u, "GD_INT_REASON"}, Pair{0x90u, "GD_BYTE_COUNT_LOW"},
            Pair{0x94u, "GD_BYTE_COUNT_HIGH"}, Pair{0x9Cu, "GD_COMMAND_STATUS"},
            Pair{0xA0u, "GD_ALT_STATUS_CONTROL"}};
        return named_offset(names, offset);
    }
    case DreamcastHardwareRegion::G1Dma: {
        static constexpr std::array names{
            Pair{0x04u, "GDSTAR"}, Pair{0x08u, "GDLEN"}, Pair{0x0Cu, "GDDIR"},
            Pair{0x14u, "GDEN"}, Pair{0x18u, "GDST"}, Pair{0xB0u, "G1SYSM"},
            Pair{0xF4u, "GDSTARD"}, Pair{0xF8u, "GDLEND"}};
        return named_offset(names, offset);
    }
    case DreamcastHardwareRegion::G2Dma:
        if (offset < 0x80u && (offset & 3u) == 0u) {
            static constexpr std::array suffixes{"STAR", "STAG", "LEN", "DIR",
                                                  "TSEL", "EN", "ST", "SUSP"};
            return "G2_CH" + std::to_string(offset / 0x20u) + '_' + suffixes[(offset & 0x1Fu) / 4u];
        }
        if (offset == 0xBCu) return "G2APRO";
        return {};
    case DreamcastHardwareRegion::PvrDma: {
        static constexpr std::array names{
            Pair{0x00u, "PDSTAP"}, Pair{0x04u, "PDSTAR"}, Pair{0x08u, "PDLEN"},
            Pair{0x0Cu, "PDDIR"}, Pair{0x10u, "PDTSEL"}, Pair{0x14u, "PDEN"},
            Pair{0x18u, "PDST"}, Pair{0x80u, "PDAPRO"}, Pair{0xF0u, "PDSTAPD"},
            Pair{0xF4u, "PDSTARD"}, Pair{0xF8u, "PDLEND"}};
        return named_offset(names, offset);
    }
    case DreamcastHardwareRegion::Aica:
        if (offset == 0x2C00u) return "ARM_RESET";
        if (offset >= 0x2890u && offset <= 0x2898u && (offset & 3u) == 0u)
            return "TIMER_" + std::to_string((offset - 0x2890u) / 4u);
        if (offset == 0x28B4u) return "SCIEB";
        if (offset == 0x28B8u) return "SCIPD";
        if (offset == 0x28BCu) return "SCIRE";
        if (offset < 64u * 0x80u)
            return "CHANNEL_" + std::to_string(offset / 0x80u);
        return {};
    case DreamcastHardwareRegion::AicaRtc:
        return offset == 0u ? "RTC_HIGH" : offset == 4u ? "RTC_LOW" :
               offset == 8u ? "RTC_ENABLE" : std::string{};
    default:
        return {};
    }
}

std::string sh4_register_name(const std::uint32_t address) {
    switch (address) {
    case 0xFF000000u: return "PTEH";
    case 0xFF000004u: return "PTEL";
    case 0xFF000008u: return "TTB";
    case 0xFF00000Cu: return "TEA";
    case 0xFF000010u: return "MMUCR";
    case 0xFF00001Cu: return "CCR";
    case 0xFF000034u: return "PTEA";
    case 0xFF000038u: return "QACR0";
    case 0xFF00003Cu: return "QACR1";
    case 0xFF80002Cu: return "PCTRA";
    case 0xFF800030u: return "PDTRA";
    case 0xFF800040u: return "PCTRB";
    case 0xFF800044u: return "PDTRB";
    case 0xFF800048u: return "GPIOIC";
    case 0xFFD00000u: return "ICR";
    case 0xFFD00004u: return "IPRA";
    case 0xFFD00008u: return "IPRB";
    case 0xFFD0000Cu: return "IPRC";
    case 0xFFD80000u: return "TOCR";
    case 0xFFD80004u: return "TSTR";
    case 0xFFE80000u: return "SCSMR2";
    case 0xFFE80004u: return "SCBRR2";
    case 0xFFE80008u: return "SCSCR2";
    case 0xFFE8000Cu: return "SCFTDR2";
    case 0xFFE80010u: return "SCFSR2";
    case 0xFFE80014u: return "SCFRDR2";
    case 0xFFE80018u: return "SCFCR2";
    case 0xFFE8001Cu: return "SCFDR2";
    case 0xFFE80020u: return "SCSPTR2";
    case 0xFFE80024u: return "SCLSR2";
    default: break;
    }
    if (in_range(address, 0xFFA00000u, 0x40u)) {
        const auto channel = (address - 0xFFA00000u) / 0x10u;
        const auto offset = (address - 0xFFA00000u) % 0x10u;
        const char* name = offset == 0u ? "SAR" : offset == 4u ? "DAR" :
                           offset == 8u ? "DMATCR" : "CHCR";
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
    AddressDescription result{canonical, DreamcastHardwareRegion::Unknown, false, {}};
    const auto set = [&](const DreamcastHardwareRegion region,
                         const bool runtime_mapped,
                         std::string name = {}) {
        result.region = region;
        result.aperture_mapped = runtime_mapped;
        result.name = std::move(name);
    };
    if (in_range(canonical, 0x005F6800u, 0xB0u)) set(DreamcastHardwareRegion::SystemBus, true);
    else if (in_range(canonical, 0x005F6900u, 0x58u)) set(DreamcastHardwareRegion::SystemAsic, true);
    else if (in_range(canonical, 0x005F6C00u, 0x100u)) set(DreamcastHardwareRegion::Maple, true);
    else if (in_range(canonical, 0x005F7000u, 0x100u)) set(DreamcastHardwareRegion::GdRom, true);
    else if (in_range(canonical, 0x005F7400u, 0x100u)) set(DreamcastHardwareRegion::G1Dma, true);
    else if (in_range(canonical, 0x005F7800u, 0x100u)) set(DreamcastHardwareRegion::G2Dma, true);
    else if (in_range(canonical, 0x005F7C00u, 0x100u)) set(DreamcastHardwareRegion::PvrDma, true);
    else if (in_range(canonical, 0x005F8000u, 0x2000u))
        set(DreamcastHardwareRegion::Pvr, true, pvr_register_name(canonical - 0x005F8000u));
    else if (in_range(canonical, 0x00700000u, 0x8000u)) set(DreamcastHardwareRegion::Aica, true);
    else if (in_range(canonical, 0x00710000u, 0x0Cu)) set(DreamcastHardwareRegion::AicaRtc, true);
    else if (in_range(canonical, 0x00800000u, 0x00800000u)) set(DreamcastHardwareRegion::AicaRam, true);
    else if (in_range(canonical, 0x01000000u, 0x00800000u)) set(DreamcastHardwareRegion::TaFifo, true);
    else if (in_range(canonical, 0x10800000u, 0x00800000u)) set(DreamcastHardwareRegion::TaYuv, true);
    else if (in_range(canonical, 0x04000000u, 0x00800000u)) set(DreamcastHardwareRegion::Vram64, true);
    else if (in_range(canonical, 0x05000000u, 0x00800000u)) set(DreamcastHardwareRegion::Vram32, true);
    else if (in_range(canonical, 0x11000000u, 0x01000000u) ||
             in_range(canonical, 0x13000000u, 0x01000000u)) set(DreamcastHardwareRegion::TaVram, true);
    else if (in_range(canonical, 0xE0000000u, 0x04000000u)) set(DreamcastHardwareRegion::StoreQueue, true);
    else if (in_range(canonical, 0xFF000000u, 0x14u))
        set(DreamcastHardwareRegion::Sh4Mmu, true, sh4_register_name(canonical));
    else if (canonical == 0xFF00001Cu)
        set(DreamcastHardwareRegion::Sh4Cache, true, sh4_register_name(canonical));
    else if (canonical == 0xFF000034u)
        set(DreamcastHardwareRegion::Sh4Mmu, true, sh4_register_name(canonical));
    else if (in_range(canonical, 0xFF000038u, 8u))
        set(DreamcastHardwareRegion::Sh4Qacr, true, sh4_register_name(canonical));
    else if (canonical == 0xFF80002Cu || canonical == 0xFF800030u ||
             canonical == 0xFF800040u || canonical == 0xFF800044u || canonical == 0xFF800048u)
        set(DreamcastHardwareRegion::Sh4Io, true, sh4_register_name(canonical));
    else if (in_range(canonical, 0xFFA00000u, 0x44u))
        set(DreamcastHardwareRegion::Sh4Dmac, true, sh4_register_name(canonical));
    else if (in_range(canonical, 0xFFC80000u, 0x40u)) set(DreamcastHardwareRegion::Sh4Rtc, true);
    else if (in_range(canonical, 0xFFD00000u, 0x14u))
        set(DreamcastHardwareRegion::Sh4Intc, true, sh4_register_name(canonical));
    else if (in_range(canonical, 0xFFD80000u, 0x30u))
        set(DreamcastHardwareRegion::Sh4Tmu, true, sh4_register_name(canonical));
    else if (in_range(canonical, 0xFFE80000u, 0x28u))
        set(DreamcastHardwareRegion::Sh4Scif, true, sh4_register_name(canonical));
    else if (canonical >= 0xFF000000u) set(DreamcastHardwareRegion::Sh4P4, false);
    if (result.name.empty() && result.region != DreamcastHardwareRegion::Unknown &&
        canonical < 0xE0000000u) {
        std::uint32_t base = canonical;
        switch (result.region) {
        case DreamcastHardwareRegion::SystemBus: base = 0x005F6800u; break;
        case DreamcastHardwareRegion::SystemAsic: base = 0x005F6900u; break;
        case DreamcastHardwareRegion::Maple: base = 0x005F6C00u; break;
        case DreamcastHardwareRegion::GdRom: base = 0x005F7000u; break;
        case DreamcastHardwareRegion::G1Dma: base = 0x005F7400u; break;
        case DreamcastHardwareRegion::G2Dma: base = 0x005F7800u; break;
        case DreamcastHardwareRegion::PvrDma: base = 0x005F7C00u; break;
        case DreamcastHardwareRegion::Aica: base = 0x00700000u; break;
        case DreamcastHardwareRegion::AicaRtc: base = 0x00710000u; break;
        default: break;
        }
        result.name = dreamcast_register_name(result.region, canonical - base);
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
    static constexpr std::array readable{0x00u, 0x04u, 0x08u, 0x10u, 0x14u, 0x18u,
                                         0x1Cu, 0x20u, 0x40u, 0x44u, 0x48u, 0x4Cu,
                                         0x60u, 0x80u, 0x84u, 0x88u, 0x8Cu, 0x9Cu,
                                         0xA0u};
    static constexpr std::array writable{0x00u, 0x04u, 0x08u, 0x10u, 0x14u, 0x18u,
                                         0x1Cu, 0x20u, 0x40u, 0x44u, 0x48u, 0x4Cu,
                                         0x60u, 0x84u, 0x88u, 0x90u, 0xA0u, 0xA4u,
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
    static constexpr std::array readable{0x04u, 0x10u, 0x14u, 0x18u, 0x80u, 0x84u,
                                         0xE8u, 0xF4u, 0xF8u, 0xFCu};
    static constexpr std::array writable{0x04u, 0x10u, 0x14u, 0x18u,
                                         0x80u, 0x88u, 0x8Cu, 0xE8u};
    if (kind == HardwareAccessKind::Prefetch) return HardwareRuntimeSupport::Rejected;
    const auto supported = kind == HardwareAccessKind::Read ? contains(readable, offset)
                                                            : contains(writable, offset);
    return supported ? HardwareRuntimeSupport::Implemented : HardwareRuntimeSupport::Rejected;
}

HardwareRuntimeSupport gdrom_support(const std::uint32_t offset,
                                     const HardwareAccessKind kind,
                                     const std::uint8_t width) noexcept {
    if (kind == HardwareAccessKind::Prefetch) return HardwareRuntimeSupport::Rejected;
    if (offset == 0x80u)
        return width == 2u ? HardwareRuntimeSupport::Implemented
                           : HardwareRuntimeSupport::Rejected;
    if (width != 1u) return HardwareRuntimeSupport::Rejected;
    static constexpr std::array readable{0x84u, 0x88u, 0x90u, 0x94u, 0x9Cu, 0xA0u};
    static constexpr std::array writable{0x90u, 0x94u, 0x9Cu, 0xA0u};
    const auto supported = kind == HardwareAccessKind::Read ? contains(readable, offset)
                                                            : contains(writable, offset);
    return supported ? HardwareRuntimeSupport::Implemented : HardwareRuntimeSupport::Rejected;
}

HardwareRuntimeSupport holly_dma_support(const DreamcastHardwareRegion region,
                                         const std::uint32_t offset,
                                         const HardwareAccessKind kind) noexcept {
    if (kind == HardwareAccessKind::Prefetch) return HardwareRuntimeSupport::Rejected;
    if (region == DreamcastHardwareRegion::G2Dma) {
        if (offset < 0x80u && (offset & 3u) == 0u) return HardwareRuntimeSupport::Implemented;
        static constexpr std::array readable{0x80u, 0x90u, 0x94u, 0x98u, 0x9Cu,
                                             0xC0u, 0xC4u, 0xC8u, 0xD0u, 0xD4u,
                                             0xD8u, 0xE0u, 0xE4u, 0xE8u, 0xF0u,
                                             0xF4u, 0xF8u};
        static constexpr std::array writable{0x90u, 0x94u, 0x98u, 0x9Cu, 0xBCu};
        const auto supported = kind == HardwareAccessKind::Read ? contains(readable, offset)
                                                                : contains(writable, offset);
        return supported ? HardwareRuntimeSupport::Implemented : HardwareRuntimeSupport::Rejected;
    }
    if (region == DreamcastHardwareRegion::G1Dma) {
        static constexpr std::array readable{0x04u, 0x08u, 0x0Cu, 0x14u,
                                             0x18u, 0xB0u, 0xF4u, 0xF8u};
        static constexpr std::array writable{0x04u, 0x08u, 0x0Cu, 0x14u, 0x18u};
        static constexpr std::array ignored_writes{0x80u, 0x84u, 0x88u, 0x8Cu, 0x90u, 0x94u,
                                                   0xA0u, 0xA4u, 0xB4u, 0xB8u, 0xE4u};
        if (kind == HardwareAccessKind::Write && contains(ignored_writes, offset))
            return HardwareRuntimeSupport::Partial;
        const auto supported = kind == HardwareAccessKind::Read ? contains(readable, offset)
                                                                : contains(writable, offset);
        return supported ? HardwareRuntimeSupport::Implemented : HardwareRuntimeSupport::Rejected;
    }
    static constexpr std::array readable{0x00u, 0x04u, 0x08u, 0x0Cu, 0x10u,
                                         0x14u, 0x18u, 0xF0u, 0xF4u, 0xF8u};
    static constexpr std::array writable{0x00u, 0x04u, 0x08u, 0x0Cu,
                                         0x10u, 0x14u, 0x18u, 0x80u};
    const auto supported = kind == HardwareAccessKind::Read ? contains(readable, offset)
                                                            : contains(writable, offset);
    return supported ? HardwareRuntimeSupport::Implemented : HardwareRuntimeSupport::Rejected;
}

HardwareRuntimeSupport sh4_tmu_support(const std::uint32_t address,
                                       const std::uint8_t width) noexcept {
    const auto offset = address - 0xFFD80000u;
    if ((offset == 0x00u || offset == 0x04u) && width == 1u)
        return HardwareRuntimeSupport::Implemented;
    if (offset >= 0x08u && offset <= 0x28u) {
        const auto local = (offset - 0x08u) % 0x0Cu;
        if ((local == 0u || local == 4u) && width == 4u)
            return HardwareRuntimeSupport::Implemented;
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
    if (kind == HardwareAccessKind::Write &&
        (offset == 0x14u || offset == 0x1Cu))
        return HardwareRuntimeSupport::Rejected;
    static constexpr std::array offsets{0x00u, 0x04u, 0x08u, 0x0Cu, 0x10u,
                                        0x14u, 0x18u, 0x1Cu, 0x20u, 0x24u};
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
                                 address - (description.region == Region::G1Dma ? 0x005F7400u :
                                            description.region == Region::G2Dma ? 0x005F7800u :
                                                                                 0x005F7C00u),
                                 kind);
    case Region::Pvr:
        return pvr_support(address - 0x005F8000u, kind, width);
    case Region::Aica:
        return kind != HardwareAccessKind::Prefetch && (width == 1u || width == 2u || width == 4u)
                   ? HardwareRuntimeSupport::Partial
                   : HardwareRuntimeSupport::Rejected;
    case Region::AicaRtc:
        return kind != HardwareAccessKind::Prefetch ? HardwareRuntimeSupport::Implemented
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
    case Region::Sh4Mmu:
    case Region::Sh4Cache:
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
};

bool is_memory_access_instruction(const sh4::InstructionKind kind) noexcept {
    using K = sh4::InstructionKind;
    switch (kind) {
    case K::MovByteStore:
    case K::MovWordStore:
    case K::MovLongStore:
    case K::MovByteLoad:
    case K::MovWordLoad:
    case K::MovLongLoad:
    case K::MovByteStoreDisplacement:
    case K::MovWordStoreDisplacement:
    case K::MovLongStoreDisplacement:
    case K::MovByteLoadDisplacement:
    case K::MovWordLoadDisplacement:
    case K::MovLongLoadDisplacement:
    case K::MovByteStoreR0Indexed:
    case K::MovWordStoreR0Indexed:
    case K::MovLongStoreR0Indexed:
    case K::MovByteLoadR0Indexed:
    case K::MovWordLoadR0Indexed:
    case K::MovLongLoadR0Indexed:
    case K::MovByteStorePreDecrement:
    case K::MovWordStorePreDecrement:
    case K::MovLongStorePreDecrement:
    case K::MovByteLoadPostIncrement:
    case K::MovWordLoadPostIncrement:
    case K::MovLongLoadPostIncrement:
    case K::MovcaLong:
    case K::TestAndSetByte:
    case K::Prefetch:
        return true;
    default:
        return false;
    }
}

std::optional<std::uint32_t> displaced(const std::optional<std::uint32_t>& base,
                                       const std::uint32_t offset = 0u) {
    if (!base.has_value()) return std::nullopt;
    return *base + offset;
}

std::optional<EffectiveAccess> effective_access(const sh4::DisassemblyLine& line,
                                                const RegisterConstants& before) {
    using K = sh4::InstructionKind;
    const auto& instruction = line.instruction;
    std::optional<std::uint32_t> address;
    HardwareAccessKind kind = HardwareAccessKind::Read;
    std::uint8_t width = 0u;
    switch (instruction.kind) {
    case K::MovByteStore:
    case K::MovWordStore:
    case K::MovLongStore:
        kind = HardwareAccessKind::Write;
        width = instruction.kind == K::MovByteStore ? 1u :
                instruction.kind == K::MovWordStore ? 2u : 4u;
        address = before.registers[instruction.destination_register];
        break;
    case K::MovByteLoad:
    case K::MovWordLoad:
    case K::MovLongLoad:
        width = instruction.kind == K::MovByteLoad ? 1u :
                instruction.kind == K::MovWordLoad ? 2u : 4u;
        address = before.registers[instruction.source_register];
        break;
    case K::MovByteStoreDisplacement:
    case K::MovWordStoreDisplacement:
    case K::MovLongStoreDisplacement:
        kind = HardwareAccessKind::Write;
        width = instruction.kind == K::MovByteStoreDisplacement ? 1u :
                instruction.kind == K::MovWordStoreDisplacement ? 2u : 4u;
        address = displaced(before.registers[instruction.destination_register],
                            static_cast<std::uint32_t>(instruction.displacement));
        break;
    case K::MovByteLoadDisplacement:
    case K::MovWordLoadDisplacement:
    case K::MovLongLoadDisplacement:
        width = instruction.kind == K::MovByteLoadDisplacement ? 1u :
                instruction.kind == K::MovWordLoadDisplacement ? 2u : 4u;
        address = displaced(before.registers[instruction.source_register],
                            static_cast<std::uint32_t>(instruction.displacement));
        break;
    case K::MovByteStoreR0Indexed:
    case K::MovWordStoreR0Indexed:
    case K::MovLongStoreR0Indexed:
        kind = HardwareAccessKind::Write;
        width = instruction.kind == K::MovByteStoreR0Indexed ? 1u :
                instruction.kind == K::MovWordStoreR0Indexed ? 2u : 4u;
        if (before.registers[0u].has_value() &&
            before.registers[instruction.destination_register].has_value())
            address = *before.registers[0u] + *before.registers[instruction.destination_register];
        break;
    case K::MovByteLoadR0Indexed:
    case K::MovWordLoadR0Indexed:
    case K::MovLongLoadR0Indexed:
        width = instruction.kind == K::MovByteLoadR0Indexed ? 1u :
                instruction.kind == K::MovWordLoadR0Indexed ? 2u : 4u;
        if (before.registers[0u].has_value() &&
            before.registers[instruction.source_register].has_value())
            address = *before.registers[0u] + *before.registers[instruction.source_register];
        break;
    case K::MovByteStorePreDecrement:
    case K::MovWordStorePreDecrement:
    case K::MovLongStorePreDecrement:
        kind = HardwareAccessKind::Write;
        width = instruction.kind == K::MovByteStorePreDecrement ? 1u :
                instruction.kind == K::MovWordStorePreDecrement ? 2u : 4u;
        if (before.registers[instruction.destination_register].has_value())
            address = *before.registers[instruction.destination_register] - width;
        break;
    case K::MovByteLoadPostIncrement:
    case K::MovWordLoadPostIncrement:
    case K::MovLongLoadPostIncrement:
        width = instruction.kind == K::MovByteLoadPostIncrement ? 1u :
                instruction.kind == K::MovWordLoadPostIncrement ? 2u : 4u;
        address = before.registers[instruction.source_register];
        break;
    case K::MovcaLong:
        kind = HardwareAccessKind::Write;
        width = 4u;
        address = before.registers[instruction.destination_register];
        break;
    case K::TestAndSetByte:
        kind = HardwareAccessKind::Write;
        width = 1u;
        address = before.registers[instruction.source_register];
        break;
    case K::Prefetch:
        kind = HardwareAccessKind::Prefetch;
        width = 32u;
        address = before.registers[instruction.source_register];
        break;
    default: return std::nullopt;
    }
    if (!address.has_value()) return std::nullopt;
    return EffectiveAccess{*address, kind, width};
}

std::string hex8(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return output.str();
}

std::string hex4(const std::uint16_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << value;
    return output.str();
}

} // namespace

DreamcastHardwareAudit audit_dreamcast_hardware(const io::ExecutableImage& image,
                                                const ControlFlowAnalysisResult& analysis) {
    DreamcastHardwareAudit result;
    for (const auto& segment : image.segments()) result.image_bytes += segment.bytes.size();
    result.reachable_instructions = analysis.recursive.instructions.size();
    result.reachable_functions = analysis.recursive.functions.size();
    result.unknown_instructions = analysis.recursive.diagnostics.size();
    for (const auto& source : analysis.recursive.diagnostics) {
        HardwareInstructionDiagnostic diagnostic;
        diagnostic.address = source.address;
        diagnostic.opcode = source.opcode;
        diagnostic.reason = source.reason;
        for (const auto& context : analysis.recursive.contextual_instructions) {
            if (context.line.address != source.address) continue;
            if (control_flow_evidence_strength(context.evidence) >
                control_flow_evidence_strength(diagnostic.evidence))
                diagnostic.evidence = context.evidence;
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
    if (analysis.instruction_arena) {
        for (const auto& span : analysis.block_spans) {
            const auto lines = span.view(*analysis.instruction_arena);
            const auto trace = propagate_local_constants(lines, image);
            for (std::size_t index = 0u; index < lines.size(); ++index) {
                if (!is_memory_access_instruction(lines[index].instruction.kind)) continue;
                ++result.memory_access_sites;
                const auto access = effective_access(lines[index], trace[index].before);
                if (!access.has_value()) {
                    ++result.unresolved_memory_access_sites;
                    continue;
                }
                ++result.resolved_memory_access_sites;
                const auto description = describe(access->address);
                if (description.region == DreamcastHardwareRegion::Unknown) continue;
                HardwareAccessReference reference;
                reference.instruction_address = lines[index].address;
                reference.guest_address = access->address;
                reference.canonical_address = description.canonical;
                reference.region = description.region;
                reference.kind = access->kind;
                reference.width = access->width;
                reference.aperture_mapped = description.aperture_mapped;
                reference.runtime_support =
                    assess_support(description, access->kind, access->width);
                reference.support_reason = support_reason(description, reference.runtime_support);
                reference.register_name = description.name;
                result.references.push_back(std::move(reference));
            }
        }
    }
    std::sort(result.references.begin(), result.references.end(), [](const auto& left, const auto& right) {
        return std::tie(left.guest_address, left.instruction_address, left.kind, left.width) <
               std::tie(right.guest_address, right.instruction_address, right.kind, right.width);
    });
    for (const auto& reference : result.references) {
        if (result.addresses.empty() || result.addresses.back().guest_address != reference.guest_address) {
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
        if (reference.kind == HardwareAccessKind::Read) ++summary.reads;
        else if (reference.kind == HardwareAccessKind::Write) ++summary.writes;
        else ++summary.prefetches;
        if (std::find(summary.widths.begin(), summary.widths.end(), reference.width) ==
            summary.widths.end())
            summary.widths.push_back(reference.width);
        summary.instruction_addresses.push_back(reference.instruction_address);
    }
    for (auto& summary : result.addresses) {
        std::sort(summary.widths.begin(), summary.widths.end());
        switch (summary.runtime_support) {
        case HardwareRuntimeSupport::Implemented: ++result.implemented_addresses; break;
        case HardwareRuntimeSupport::Partial: ++result.partial_addresses; break;
        case HardwareRuntimeSupport::KnownGap: ++result.known_gap_addresses; break;
        case HardwareRuntimeSupport::Rejected: ++result.rejected_addresses; break;
        case HardwareRuntimeSupport::Unmapped: ++result.unmapped_addresses; break;
        }
    }
    return result;
}

const char* dreamcast_hardware_region_name(const DreamcastHardwareRegion region) noexcept {
    switch (region) {
    case DreamcastHardwareRegion::SystemBus: return "system_bus";
    case DreamcastHardwareRegion::SystemAsic: return "system_asic";
    case DreamcastHardwareRegion::Maple: return "maple";
    case DreamcastHardwareRegion::GdRom: return "gdrom";
    case DreamcastHardwareRegion::G1Dma: return "g1_dma";
    case DreamcastHardwareRegion::G2Dma: return "g2_dma";
    case DreamcastHardwareRegion::PvrDma: return "pvr_dma";
    case DreamcastHardwareRegion::Pvr: return "pvr";
    case DreamcastHardwareRegion::Aica: return "aica";
    case DreamcastHardwareRegion::AicaRtc: return "aica_rtc";
    case DreamcastHardwareRegion::AicaRam: return "aica_ram";
    case DreamcastHardwareRegion::TaFifo: return "ta_fifo";
    case DreamcastHardwareRegion::TaYuv: return "ta_yuv";
    case DreamcastHardwareRegion::TaVram: return "ta_vram";
    case DreamcastHardwareRegion::Vram64: return "vram_64";
    case DreamcastHardwareRegion::Vram32: return "vram_32";
    case DreamcastHardwareRegion::StoreQueue: return "store_queue";
    case DreamcastHardwareRegion::Sh4Mmu: return "sh4_mmu";
    case DreamcastHardwareRegion::Sh4Cache: return "sh4_cache";
    case DreamcastHardwareRegion::Sh4Qacr: return "sh4_qacr";
    case DreamcastHardwareRegion::Sh4Io: return "sh4_io";
    case DreamcastHardwareRegion::Sh4Dmac: return "sh4_dmac";
    case DreamcastHardwareRegion::Sh4Rtc: return "sh4_rtc";
    case DreamcastHardwareRegion::Sh4Intc: return "sh4_intc";
    case DreamcastHardwareRegion::Sh4Tmu: return "sh4_tmu";
    case DreamcastHardwareRegion::Sh4Scif: return "sh4_scif";
    case DreamcastHardwareRegion::Sh4P4: return "sh4_p4_unmapped";
    case DreamcastHardwareRegion::Unknown: return "unknown";
    }
    return "unknown";
}

const char* hardware_access_kind_name(const HardwareAccessKind kind) noexcept {
    switch (kind) {
    case HardwareAccessKind::Read: return "read";
    case HardwareAccessKind::Write: return "write";
    case HardwareAccessKind::Prefetch: return "prefetch";
    }
    return "read";
}

const char* hardware_runtime_support_name(const HardwareRuntimeSupport support) noexcept {
    switch (support) {
    case HardwareRuntimeSupport::Implemented: return "implemented";
    case HardwareRuntimeSupport::Partial: return "partial";
    case HardwareRuntimeSupport::KnownGap: return "known_gap";
    case HardwareRuntimeSupport::Rejected: return "rejected";
    case HardwareRuntimeSupport::Unmapped: return "unmapped";
    }
    return "unmapped";
}

std::string format_hardware_audit_text(const DreamcastHardwareAudit& audit) {
    std::ostringstream output;
    output << "Scope: initial boot executable\n"
           << "Image bytes: " << audit.image_bytes << "\n"
           << "Reachable instructions: " << audit.reachable_instructions << "\n"
           << "Reachable functions: " << audit.reachable_functions << "\n"
           << "Unknown instructions: " << audit.unknown_instructions << "\n"
           << "Memory access sites: " << audit.memory_access_sites << " (constant="
           << audit.resolved_memory_access_sites << ", dynamic="
           << audit.unresolved_memory_access_sites << ")\n"
           << "Hardware addresses: " << audit.addresses.size() << " (implemented="
           << audit.implemented_addresses << ", partial=" << audit.partial_addresses
           << ", known_gap=" << audit.known_gap_addresses
           << ", rejected=" << audit.rejected_addresses
           << ", unmapped=" << audit.unmapped_addresses << ")\n";
    for (const auto& diagnostic : audit.instruction_diagnostics) {
        output << "Instruction diagnostic: address=" << hex8(diagnostic.address)
               << " opcode=" << hex4(diagnostic.opcode)
               << " reason=" << diagnostic.reason
               << " evidence=" << control_flow_evidence_name(diagnostic.evidence)
               << " incoming=";
        for (std::size_t index = 0u; index < diagnostic.incoming_addresses.size(); ++index) {
            if (index != 0u) output << ',';
            output << hex8(diagnostic.incoming_addresses[index]);
        }
        output << '\n';
    }
    for (const auto& address : audit.addresses) {
        output << hex8(address.guest_address) << " canonical=" << hex8(address.canonical_address)
               << " region=" << dreamcast_hardware_region_name(address.region)
               << " aperture=" << (address.aperture_mapped ? "mapped" : "unmapped")
               << " support=" << hardware_runtime_support_name(address.runtime_support)
               << " reason=" << address.support_reason
               << " reads=" << address.reads << " writes=" << address.writes
               << " prefetches=" << address.prefetches;
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
    io::write_json_report_header(output, "katana.hardware-audit.v2", "dreamcast_hardware_audit");
    output << ",\"scope\":\"initial_boot_executable\",\"image_bytes\":" << audit.image_bytes
           << ",\"reachable_instructions\":" << audit.reachable_instructions
           << ",\"reachable_functions\":" << audit.reachable_functions
           << ",\"unknown_instructions\":" << audit.unknown_instructions
           << ",\"memory_access_sites\":" << audit.memory_access_sites
           << ",\"resolved_memory_access_sites\":" << audit.resolved_memory_access_sites
           << ",\"unresolved_memory_access_sites\":" << audit.unresolved_memory_access_sites
           << ",\"implemented_addresses\":" << audit.implemented_addresses
           << ",\"partial_addresses\":" << audit.partial_addresses
           << ",\"known_gap_addresses\":" << audit.known_gap_addresses
           << ",\"rejected_addresses\":" << audit.rejected_addresses
           << ",\"unmapped_addresses\":" << audit.unmapped_addresses
           << ",\"instruction_diagnostics\":[";
    for (std::size_t index = 0u; index < audit.instruction_diagnostics.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& diagnostic = audit.instruction_diagnostics[index];
        output << "{\"address\":" << io::quote_json(hex8(diagnostic.address))
               << ",\"opcode\":" << io::quote_json(hex4(diagnostic.opcode))
               << ",\"reason\":" << io::quote_json(diagnostic.reason)
               << ",\"evidence\":" << io::quote_json(control_flow_evidence_name(diagnostic.evidence))
               << ",\"incoming_addresses\":[";
        for (std::size_t incoming = 0u; incoming < diagnostic.incoming_addresses.size(); ++incoming) {
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
                   << ",\"canonical_address\":"
                   << io::quote_json(hex8(reference.canonical_address))
                   << ",\"region\":"
                   << io::quote_json(dreamcast_hardware_region_name(reference.region))
                   << ",\"kind\":" << io::quote_json(hardware_access_kind_name(reference.kind))
                   << ",\"width\":" << static_cast<unsigned>(reference.width)
                   << ",\"aperture_mapped\":"
                   << (reference.aperture_mapped ? "true" : "false")
                   << ",\"runtime_support\":"
                   << io::quote_json(hardware_runtime_support_name(reference.runtime_support))
                   << ",\"support_reason\":" << io::quote_json(reference.support_reason)
                   << "}";
        }
        output << ']';
    }
    output << '}';
    return output.str();
}

} // namespace katana::analysis
