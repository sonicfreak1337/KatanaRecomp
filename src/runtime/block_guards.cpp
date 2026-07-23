#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/cache_control.hpp"

#include <algorithm>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace katana::runtime {
namespace {
constexpr std::uint32_t fpscr_guard_mask =
    fpscr_rounding_mode_mask | fpscr_pr_mask | fpscr_sz_mask | fpscr_fr_mask;
constexpr std::uint32_t mmucr_sv_mask = 0x00000100u;
constexpr std::uint32_t mmucr_sqmd_mask = 0x00000200u;
constexpr std::uint32_t store_queue_window_mask = 0xFC000000u;
constexpr std::uint32_t store_queue_window = 0xE0000000u;
}

BlockVariantKey block_variant_key(const BlockStateGuard& guard,
                                  const std::uint64_t runtime_generation) noexcept {
    return {guard.address_space_generation,
            guard.mmu_generation,
            guard.watchpoint_generation,
            guard.fpscr_mode,
            runtime_generation};
}

TranslationError::TranslationError(const TranslationAccess access,
                                   const std::uint32_t address,
                                   const ExceptionCause cause)
    : std::runtime_error([&] {
          std::ostringstream out;
          out << "SH-4-Adressuebersetzungsfehler access=" << static_cast<unsigned>(access)
              << " address=0x" << std::hex << std::setw(8) << std::setfill('0') << address;
          return out.str();
      }()),
      cause_(cause), access_(access), address_(address) {}
ExceptionCause TranslationError::cause() const noexcept {
    return cause_;
}
TranslationAccess TranslationError::access() const noexcept {
    return access_;
}
std::uint32_t TranslationError::address() const noexcept {
    return address_;
}

void RuntimeAddressSpace::set_mode(const AddressTranslationMode mode) noexcept {
    if (mode_ != mode) {
        mode_ = mode;
        ++address_space_generation_;
    }
}
void RuntimeAddressSpace::write_mmucr(const std::uint32_t value) noexcept {
    if (mmucr_ != value) {
        mmucr_ = value;
        ++mmu_generation_;
    }
}
void RuntimeAddressSpace::write_pteh(const std::uint32_t value) noexcept {
    const auto asid = static_cast<std::uint8_t>(value & 0xFFu);
    if (asid_ != asid) {
        asid_ = asid;
        ++mmu_generation_;
    }
}
void RuntimeAddressSpace::ldtlb(TlbMapping mapping) {
    if (mapping.page_size != 1024u && mapping.page_size != 4096u &&
        mapping.page_size != 65536u && mapping.page_size != 1048576u)
        throw std::invalid_argument("SH-4-TLB-Seitengroesse ist ungueltig.");
    mapping.virtual_page = mapping.virtual_page / mapping.page_size * mapping.page_size;
    mapping.physical_page =
        canonical_physical_address(mapping.physical_page) / mapping.page_size * mapping.page_size;
    const auto found = std::find_if(mappings_.begin(), mappings_.end(), [&](const auto& value) {
        return value.slot == mapping.slot;
    });
    if (found == mappings_.end()) {
        mappings_.push_back(mapping);
    } else {
        *found = mapping;
    }
    ++mmu_generation_;
}
void RuntimeAddressSpace::clear_tlb() noexcept {
    mappings_.clear();
    ++mmu_generation_;
}
void RuntimeAddressSpace::bump_address_space() noexcept {
    ++address_space_generation_;
}
void RuntimeAddressSpace::bump_watchpoints() noexcept {
    ++watchpoint_generation_;
}

TranslationResult RuntimeAddressSpace::translate(const std::uint32_t address,
                                                 const TranslationAccess access,
                                                 const bool privileged) const {
    const auto read_cause = access == TranslationAccess::Write ? ExceptionCause::AddressErrorWrite
                                                               : ExceptionCause::AddressErrorRead;
    if (!privileged && address >= 0x80000000u)
        throw TranslationError(access, address, read_cause);

    if ((address & 0xFC000000u) == sh4_on_chip_ram_address)
        return {address, address, mmu_generation_, true};

    const auto segment = address >> 29u;
    if (segment == 4u || segment == 5u) {
        if (!privileged) throw TranslationError(access, address, read_cause);
        return {address, canonical_physical_address(address), mmu_generation_, true};
    }
    if (segment >= 7u) {
        if (!privileged || access == TranslationAccess::Instruction)
            throw TranslationError(access, address, read_cause);
        return {address, address, mmu_generation_, true};
    }
    if (mode_ == AddressTranslationMode::NoMmu) {
        return {address, canonical_physical_address(address), mmu_generation_, true};
    }
    if (segment == 6u && !privileged)
        throw TranslationError(access, address, read_cause);

    return translate_mapped(address, access, privileged);
}

StoreQueuePrefetchTranslation RuntimeAddressSpace::translate_store_queue_prefetch(
    const std::uint32_t address,
    const bool privileged) const {
    if ((address & store_queue_window_mask) != store_queue_window) {
        throw std::invalid_argument(
            "Store-Queue-PREF-Uebersetzung verlangt eine Adresse im P4-SQ-Fenster.");
    }
    if ((address & 3u) != 0u || (!privileged && (mmucr_ & mmucr_sqmd_mask) != 0u)) {
        throw TranslationError(
            TranslationAccess::Write, address, ExceptionCause::AddressErrorWrite);
    }
    if (mode_ == AddressTranslationMode::NoMmu) {
        return {address, 0u, StoreQueueAddressingMode::Qacr};
    }
    const auto translated = translate_mapped(address, TranslationAccess::Write, privileged);
    return {address,
            translated.physical_address & ~std::uint32_t{31u},
            StoreQueueAddressingMode::Utlb};
}

TranslationResult RuntimeAddressSpace::translate_mapped(const std::uint32_t address,
                                                        const TranslationAccess access,
                                                        const bool privileged) const {
    const auto matches = [&](const auto& value) {
        const auto start = static_cast<std::uint64_t>(value.virtual_page);
        const auto end = start + value.page_size;
        const bool asid_match = value.shared ||
                                (privileged && (mmucr_ & mmucr_sv_mask) != 0u) ||
                                value.asid == asid_;
        return value.valid && address >= start && static_cast<std::uint64_t>(address) < end &&
               asid_match;
    };
    const auto found = std::find_if(mappings_.begin(), mappings_.end(), matches);
    if (found == mappings_.end())
        throw TranslationError(access,
                               address,
                               access == TranslationAccess::Write ? ExceptionCause::TlbMissWrite
                                                                  : ExceptionCause::TlbMissRead);
    if (std::find_if(std::next(found), mappings_.end(), matches) != mappings_.end())
        throw TranslationError(access, address, ExceptionCause::TlbMultipleHit);
    if (!privileged && !found->user_access)
        throw TranslationError(access,
                               address,
                               access == TranslationAccess::Write
                                   ? ExceptionCause::TlbProtectionWrite
                                   : ExceptionCause::TlbProtectionRead);
    if (access == TranslationAccess::Write && (!found->writable || !found->dirty))
        throw TranslationError(access,
                               address,
                               found->writable ? ExceptionCause::InitialPageWrite
                                               : ExceptionCause::TlbProtectionWrite);
    if ((access == TranslationAccess::Instruction && !found->executable) ||
        (access == TranslationAccess::Read && !found->readable))
        throw TranslationError(access, address, ExceptionCause::TlbProtectionRead);
    return {address,
            found->physical_page + (address - found->virtual_page),
            mmu_generation_,
            false};
}

BlockStateGuard RuntimeAddressSpace::guard_for(const std::uint32_t virtual_address,
                                               const std::uint32_t fpscr) const {
    const auto translated = translate(virtual_address, TranslationAccess::Instruction);
    return {mode_,
            mmucr_,
            address_space_generation_,
            mmu_generation_,
            watchpoint_generation_,
            fpscr & fpscr_guard_mask,
            translated.physical_address / page_size * page_size};
}

bool RuntimeAddressSpace::block_fits_translation_page(const std::uint32_t virtual_start,
                                                      const std::uint32_t size) const noexcept {
    if (size == 0u) {
        return false;
    }
    if (mode_ == AddressTranslationMode::NoMmu) {
        return true;
    }
    const auto last = static_cast<std::uint64_t>(virtual_start) + size - 1u;
    if (last > 0xFFFFFFFFull) return false;
    try {
        const auto first = translate(virtual_start, TranslationAccess::Instruction);
        const auto final =
            translate(static_cast<std::uint32_t>(last), TranslationAccess::Instruction);
        return final.physical_address - first.physical_address == size - 1u;
    } catch (const TranslationError&) {
        return false;
    }
}

} // namespace katana::runtime
