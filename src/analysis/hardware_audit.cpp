#include "katana/analysis/hardware_audit.hpp"

#include "katana/analysis/basic_blocks.hpp"
#include "katana/analysis/value_analysis.hpp"
#include "katana/io/json_report.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <array>
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
    std::string name;
};

bool in_range(const std::uint32_t value,
              const std::uint32_t base,
              const std::uint32_t size) noexcept {
    return value >= base && static_cast<std::uint64_t>(value) <
                                static_cast<std::uint64_t>(base) + size;
}

std::uint32_t canonical_address(const std::uint32_t address) noexcept {
    if ((address & 0xFC000000u) == 0x7C000000u) return address;
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
    case 0xFF000020u: return "TRA";
    case 0xFF000024u: return "EXPEVT";
    case 0xFF000028u: return "INTEVT";
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
        return kind != HardwareAccessKind::Prefetch &&
                       (width == 1u || width == 2u || width == 4u) &&
                       ((address - 0x00710000u) == 0u ||
                        (address - 0x00710000u) == 4u ||
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
        return kind != HardwareAccessKind::Prefetch &&
                       (width == 1u || width == 2u || width == 4u)
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
};

struct EffectiveAccessSet {
    std::vector<EffectiveAccess> accesses;
    bool complete = false;
};

bool is_memory_access_instruction(const sh4::InstructionKind kind) noexcept {
    const auto operation = ir::lowering_operation_for_instruction(kind);
    const auto effects = ir::instruction_memory_effects(operation);
    // PREF is address-dependent and intentionally has no unconditional IR memory effect,
    // but it remains a hardware-aperture access for this audit.
    return effects.access != ir::MemoryAccessKind::None ||
           kind == sh4::InstructionKind::Prefetch;
}

std::optional<std::uint32_t> displaced(const std::optional<std::uint32_t>& base,
                                       const std::uint32_t offset = 0u) {
    if (!base.has_value()) return std::nullopt;
    return *base + offset;
}

EffectiveAccessSet
effective_accesses(const sh4::DisassemblyLine& line,
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
    case K::MovByteStoreGbrDisplacement:
    case K::MovWordStoreGbrDisplacement:
    case K::MovLongStoreGbrDisplacement:
        kind = HardwareAccessKind::Write;
        width = instruction.kind == K::MovByteStoreGbrDisplacement ? 1u :
                instruction.kind == K::MovWordStoreGbrDisplacement ? 2u : 4u;
        address = displaced(gbr, static_cast<std::uint32_t>(instruction.displacement));
        break;
    case K::MovByteLoadGbrDisplacement:
    case K::MovWordLoadGbrDisplacement:
    case K::MovLongLoadGbrDisplacement:
        width = instruction.kind == K::MovByteLoadGbrDisplacement ? 1u :
                instruction.kind == K::MovWordLoadGbrDisplacement ? 2u : 4u;
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
                *source + (instruction.source_register == instruction.destination_register ? width
                                                                                           : 0u);
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
        width = 32u;
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
            address =
                *before.registers[0u] + *before.registers[instruction.destination_register];
        break;
    default: return {};
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
    if (fmov_pair)
        return {{{*address, kind, width}, {*address + 4u, kind, width}}, true};
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
        return static_cast<std::uint16_t>(
            (1u << instruction.destination_register) | (1u << instruction.source_register));
    case K::ComparePositiveOrZero:
    case K::ComparePositive:
        return static_cast<std::uint16_t>(1u << instruction.destination_register);
    default:
        return 0u;
    }
}

constexpr std::uint16_t loop_register_bit(const std::uint8_t index) noexcept {
    return static_cast<std::uint16_t>(1u << index);
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
    default: return std::nullopt;
    }
}

bool is_linear_loop_memory(const AddressDescription& description) noexcept {
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
        case LoopReadStorage::Ram: ram_poll = true; break;
        case LoopReadStorage::Mmio: mmio_poll = true; break;
        case LoopReadStorage::Unknown: unknown_poll = true; break;
        }
    }
    if (unknown_poll) return HardwareLoopClassification::Unknown;
    const auto evidence_classes =
        static_cast<unsigned>(counter) + static_cast<unsigned>(ram_poll) +
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
        const bool paired =
            control_index + 1u < block.lines.size() &&
            block.lines[control_index + 1u].address == control_line.address + 2u &&
            block.lines[control_index + 1u].is_delay_slot;
        if (paired) continue;

        const auto delay_context = std::find_if(
            analysis.recursive.contextual_instructions.begin(),
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
        case sh4::ControlFlowKind::IndirectBranch: block.has_indirect_successor = true; break;
        default: break;
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

std::vector<HardwareNaturalLoop>
find_natural_hardware_loops(const io::ExecutableImage& image,
                            const ControlFlowAnalysisResult& analysis) {
    std::vector<std::uint32_t> function_entries;
    function_entries.reserve(analysis.recursive.functions.size());
    for (const auto& function : analysis.recursive.functions)
        function_entries.push_back(function.address);
    auto blocks = build_basic_blocks(
        analysis.recursive.instructions, analysis.resolved_edges, function_entries);
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
                const auto trace = propagate_local_constants(block.lines, image);
                const auto gbr_trace = propagate_local_gbr(block.lines, trace);
                for (std::size_t line_index = 0u; line_index < block.lines.size(); ++line_index) {
                    if (!is_memory_access_instruction(block.lines[line_index].instruction.kind))
                        continue;
                    if (is_syntactic_memory_read(block.lines[line_index].instruction.kind))
                        syntactic_read_by_instruction.emplace(block.lines[line_index].address, true);
                    const auto access_set = effective_accesses(
                        block.lines[line_index], trace[line_index].before, gbr_trace[line_index]);
                    for (const auto& access : access_set.accesses) {
                        const auto description = describe(access.address);
                        HardwareLoopAccessEvidence evidence;
                        evidence.instruction_address = block.lines[line_index].address;
                        evidence.guest_address = access.address;
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
                if (control.control_flow != sh4::ControlFlowKind::ConditionalBranch)
                    continue;
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
                    (condition.kind ==
                         sh4::InstructionKind::LoadSpecialRegisterPostIncrement &&
                     condition.special_register == sh4::SpecialRegister::Sr)) {
                    if (mark_direct_guard_access(condition_line.address) !=
                        GuardReadResolution::ResolvedAddress)
                        loop.unresolved_guard_access = true;
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
                            if (is_read)
                                loop.unresolved_guard_read_instruction_addresses.push_back(
                                    instruction_address);
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
                                required_registers = static_cast<std::uint16_t>(
                                    required_registers & ~loaded_outputs);
                                if (mark_direct_guard_access(writer.address) !=
                                    GuardReadResolution::ResolvedAddress) {
                                    provenance_complete = false;
                                }
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
                                required_registers = static_cast<std::uint16_t>(
                                    required_registers & ~non_value_outputs);
                                provenance_complete = false;
                            }
                            continue;
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
            std::sort(loop.unresolved_guard_read_instruction_addresses.begin(),
                      loop.unresolved_guard_read_instruction_addresses.end());
            loop.unresolved_guard_read_instruction_addresses.erase(
                std::unique(loop.unresolved_guard_read_instruction_addresses.begin(),
                            loop.unresolved_guard_read_instruction_addresses.end()),
                loop.unresolved_guard_read_instruction_addresses.end());
            std::sort(loop.accesses.begin(), loop.accesses.end(), [](const auto& left,
                                                                    const auto& right) {
                return std::tie(left.instruction_address,
                                left.guest_address,
                                left.kind,
                                left.width) <
                       std::tie(right.instruction_address,
                                right.guest_address,
                                right.kind,
                                right.width);
            });
            loop.classification = classify_loop(loop);
            loops.push_back(std::move(loop));
        }
    }
    std::sort(loops.begin(), loops.end(), [](const auto& left, const auto& right) {
        return std::tie(left.header_address,
                        left.latch_address,
                        left.backedge_instruction_address) <
               std::tie(right.header_address,
                        right.latch_address,
                        right.backedge_instruction_address);
    });
    return loops;
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
            const auto gbr_trace = propagate_local_gbr(lines, trace);
            for (std::size_t index = 0u; index < lines.size(); ++index) {
                if (!is_memory_access_instruction(lines[index].instruction.kind)) continue;
                ++result.memory_access_sites;
                const auto access_set =
                    effective_accesses(lines[index], trace[index].before, gbr_trace[index]);
                if (!access_set.complete) {
                    ++result.unresolved_memory_access_sites;
                } else {
                    ++result.resolved_memory_access_sites;
                }
                for (const auto& access : access_set.accesses) {
                    const auto description = describe(access.address);
                    if (description.region == DreamcastHardwareRegion::Unknown) continue;
                    HardwareAccessReference reference;
                    reference.instruction_address = lines[index].address;
                    reference.guest_address = access.address;
                    reference.canonical_address = description.canonical;
                    reference.region = description.region;
                    reference.kind = access.kind;
                    reference.width = access.width;
                    reference.aperture_mapped = description.aperture_mapped;
                    reference.runtime_support =
                        assess_support(description, access.kind, access.width);
                    reference.support_reason =
                        support_reason(description, reference.runtime_support);
                    reference.register_name = description.name;
                    result.references.push_back(std::move(reference));
                }
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
    result.loops = find_natural_hardware_loops(image, analysis);
    result.unresolved_poll_guard_loops = count_unresolved_poll_guard_loops(result.loops);
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
    case DreamcastHardwareRegion::Sh4OnChipRam: return "sh4_on_chip_ram";
    case DreamcastHardwareRegion::Sh4Mmu: return "sh4_mmu";
    case DreamcastHardwareRegion::Sh4Cache: return "sh4_cache";
    case DreamcastHardwareRegion::Sh4Exception: return "sh4_exception";
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

const char*
hardware_loop_classification_name(const HardwareLoopClassification classification) noexcept {
    switch (classification) {
    case HardwareLoopClassification::Counter: return "counter";
    case HardwareLoopClassification::RamPoll: return "ram_poll";
    case HardwareLoopClassification::MmioPoll: return "mmio_poll";
    case HardwareLoopClassification::Mixed: return "mixed";
    case HardwareLoopClassification::Unknown: return "unknown";
    }
    return "unknown";
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
    output << "Scope: " << audit.scope << "\n"
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
           << ", unmapped=" << audit.unmapped_addresses << ")\n"
           << "Natural loops: " << audit.loops.size() << "\n"
           << "Unresolved poll/guard loops: " << audit.unresolved_poll_guard_loops << "\n";
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
    for (const auto& loop : audit.loops) {
        output << "Loop: header=" << hex8(loop.header_address)
               << " latch=" << hex8(loop.latch_address)
               << " backedge=" << hex8(loop.backedge_instruction_address)
               << " classification=" << hardware_loop_classification_name(loop.classification)
               << " unresolved_guard_access="
               << (loop.unresolved_guard_access ? "yes" : "no")
               << " unresolved_guard_read_sites=";
        for (std::size_t index = 0u;
             index < loop.unresolved_guard_read_instruction_addresses.size();
             ++index) {
            if (index != 0u) output << ',';
            output << hex8(loop.unresolved_guard_read_instruction_addresses[index]);
        }
        output
               << " blocks=";
        for (std::size_t index = 0u; index < loop.block_addresses.size(); ++index) {
            if (index != 0u) output << ',';
            output << hex8(loop.block_addresses[index]);
        }
        output << " counter_sites=";
        for (std::size_t index = 0u; index < loop.counter_instruction_addresses.size(); ++index) {
            if (index != 0u) output << ',';
            output << hex8(loop.counter_instruction_addresses[index]);
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
    io::write_json_report_header(output, "katana.hardware-audit.v4", "dreamcast_hardware_audit");
    output << ",\"scope\":" << io::quote_json(audit.scope) << ",\"image_bytes\":"
           << audit.image_bytes
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
           << ",\"natural_loops\":" << audit.loops.size()
           << ",\"unresolved_poll_guard_loops\":" << audit.unresolved_poll_guard_loops
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
           << ",\"loops\":[";
    for (std::size_t index = 0u; index < audit.loops.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& loop = audit.loops[index];
        output << "{\"header_address\":" << io::quote_json(hex8(loop.header_address))
               << ",\"latch_address\":" << io::quote_json(hex8(loop.latch_address))
               << ",\"backedge_instruction_address\":"
               << io::quote_json(hex8(loop.backedge_instruction_address))
               << ",\"classification\":"
               << io::quote_json(hardware_loop_classification_name(loop.classification))
               << ",\"unresolved_guard_access\":"
               << (loop.unresolved_guard_access ? "true" : "false")
               << ",\"unresolved_guard_read_instruction_addresses\":[";
        for (std::size_t site = 0u;
             site < loop.unresolved_guard_read_instruction_addresses.size();
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
        output << "],\"accesses\":[";
        for (std::size_t access_index = 0u; access_index < loop.accesses.size();
             ++access_index) {
            if (access_index != 0u) output << ',';
            const auto& access = loop.accesses[access_index];
            output << "{\"instruction_address\":"
                   << io::quote_json(hex8(access.instruction_address))
                   << ",\"guest_address\":" << io::quote_json(hex8(access.guest_address))
                   << ",\"canonical_address\":"
                   << io::quote_json(hex8(access.canonical_address))
                   << ",\"region\":"
                   << io::quote_json(dreamcast_hardware_region_name(access.region))
                   << ",\"kind\":" << io::quote_json(hardware_access_kind_name(access.kind))
                   << ",\"width\":" << static_cast<unsigned>(access.width)
                   << ",\"linear_memory\":" << (access.linear_memory ? "true" : "false")
                   << ",\"aperture_mapped\":"
                   << (access.aperture_mapped ? "true" : "false")
                   << ",\"runtime_support\":"
                   << io::quote_json(hardware_runtime_support_name(access.runtime_support))
                   << ",\"guards_loop\":" << (access.guards_loop ? "true" : "false")
                   << '}';
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
