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
constexpr std::uint32_t mmucr_at_mask = 0x00000001u;
constexpr std::uint32_t mmucr_sv_mask = 0x00000100u;
constexpr std::uint32_t mmucr_sqmd_mask = 0x00000200u;
constexpr std::uint32_t mmucr_translation_mask =
    mmucr_at_mask | mmucr_sv_mask | mmucr_sqmd_mask;
constexpr std::uint32_t mmucr_itlb_context_mask = mmucr_at_mask | mmucr_sv_mask;
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
        const bool translation_changed =
            ((mmucr_ ^ value) & mmucr_translation_mask) != 0u;
        const bool itlb_context_changed =
            ((mmucr_ ^ value) & mmucr_itlb_context_mask) != 0u;
        mmucr_ = value;
        if (itlb_context_changed) clear_itlb();
        if (translation_changed) ++mmu_generation_;
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
    // UTLB writes can introduce an overlap with any cached translation. Keeping one
    // source slot alive would hide a subsequent architectural multiple hit.
    clear_itlb();
    ++mmu_generation_;
}
void RuntimeAddressSpace::clear_tlb() noexcept {
    mappings_.clear();
    clear_itlb();
    ++mmu_generation_;
}
void RuntimeAddressSpace::clear_itlb() noexcept {
    itlb_valid_.fill(false);
    itlb_lru_.fill(0u);
    itlb_source_slots_.fill(0xFFu);
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
    return translate_impl(address, access, privileged, true);
}

TranslationResult RuntimeAddressSpace::inspect_translation(
    const std::uint32_t address,
    const TranslationAccess access,
    const bool privileged) const {
    return translate_impl(address, access, privileged, false);
}

TranslationResult RuntimeAddressSpace::translate_impl(
    const std::uint32_t address,
    const TranslationAccess access,
    const bool privileged,
    const bool update_instruction_tlb) const {
    const auto read_cause = access == TranslationAccess::Write ? ExceptionCause::AddressErrorWrite
                                                               : ExceptionCause::AddressErrorRead;
    if (access == TranslationAccess::Instruction) {
        if ((address & 1u) != 0u)
            throw TranslationError(access, address, ExceptionCause::AddressErrorRead);
        switch (instruction_translation_path(address, privileged)) {
        case InstructionTranslationPath::Direct:
            return {address, canonical_physical_address(address), mmu_generation_, true};
        case InstructionTranslationPath::Mapped:
            return translate_mapped(address, access, privileged, update_instruction_tlb);
        case InstructionTranslationPath::Invalid:
            throw TranslationError(access, address, read_cause);
        }
    }

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
        if (!privileged) throw TranslationError(access, address, read_cause);
        return {address, address, mmu_generation_, true};
    }
    if (mode_ == AddressTranslationMode::NoMmu) {
        return {address, canonical_physical_address(address), mmu_generation_, true};
    }
    if (segment == 6u && !privileged)
        throw TranslationError(access, address, read_cause);

    return translate_mapped(address, access, privileged, update_instruction_tlb);
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
    const auto translated =
        translate_mapped(address, TranslationAccess::Write, privileged, false);
    return {address,
            translated.physical_address & ~std::uint32_t{31u},
            StoreQueueAddressingMode::Utlb};
}

TranslationResult RuntimeAddressSpace::translate_mapped(const std::uint32_t address,
                                                         const TranslationAccess access,
                                                         const bool privileged,
                                                         const bool update_instruction_tlb) const {
    const auto matches = [&](const auto& value) {
        const auto start = static_cast<std::uint64_t>(value.virtual_page);
        const auto end = start + value.page_size;
        const bool asid_match = value.shared ||
                                (privileged && (mmucr_ & mmucr_sv_mask) != 0u) ||
                                value.asid == asid_;
        return value.valid && address >= start && static_cast<std::uint64_t>(address) < end &&
               asid_match;
    };
    const auto finish_translation = [&](const TlbMapping& mapping,
                                        const std::uint8_t utlb_slot,
                                        const std::uint8_t itlb_slot,
                                        const bool itlb_refilled) {
        if (!privileged && !mapping.user_access)
            throw TranslationError(access,
                                   address,
                                   access == TranslationAccess::Write
                                       ? ExceptionCause::TlbProtectionWrite
                                       : ExceptionCause::TlbProtectionRead);
        if (access == TranslationAccess::Write && (!mapping.writable || !mapping.dirty))
            throw TranslationError(access,
                                   address,
                                   mapping.writable ? ExceptionCause::InitialPageWrite
                                                    : ExceptionCause::TlbProtectionWrite);
        if ((access == TranslationAccess::Instruction && !mapping.executable) ||
            (access == TranslationAccess::Read && !mapping.readable))
            throw TranslationError(access, address, ExceptionCause::TlbProtectionRead);
        return TranslationResult{address,
                                 mapping.physical_page + (address - mapping.virtual_page),
                                 mmu_generation_,
                                 false,
                                 utlb_slot,
                                 itlb_slot,
                                 itlb_refilled};
    };

    if (access == TranslationAccess::Instruction && update_instruction_tlb) {
        const auto touch_itlb = [&](const std::size_t touched) {
            const auto old_rank = itlb_lru_[touched];
            const auto valid_count =
                static_cast<std::uint8_t>(std::count(itlb_valid_.begin(), itlb_valid_.end(), true));
            for (std::size_t index = 0u; index < itlb_.size(); ++index) {
                if (index != touched && itlb_valid_[index] &&
                    itlb_lru_[index] > old_rank)
                    --itlb_lru_[index];
            }
            itlb_lru_[touched] = valid_count == 0u ? 0u : valid_count - 1u;
        };
        const auto itlb_matches = [&](const std::size_t index) {
            return itlb_valid_[index] && matches(itlb_[index]);
        };
        for (std::size_t index = 0u; index < itlb_.size(); ++index) {
            if (!itlb_matches(index)) continue;
            touch_itlb(index);
            return finish_translation(
                itlb_[index], 0xFFu, static_cast<std::uint8_t>(index), false);
        }
    }

    const auto found = std::find_if(mappings_.begin(), mappings_.end(), matches);
    if (found == mappings_.end())
        throw TranslationError(access,
                               address,
                               access == TranslationAccess::Write ? ExceptionCause::TlbMissWrite
                                                                  : ExceptionCause::TlbMissRead);
    if (std::find_if(std::next(found), mappings_.end(), matches) != mappings_.end())
        throw TranslationError(access, address, ExceptionCause::TlbMultipleHit);
    if (access != TranslationAccess::Instruction)
        return finish_translation(*found, found->slot, 0xFFu, false);
    if (!update_instruction_tlb)
        return finish_translation(*found, found->slot, 0xFFu, false);

    std::size_t replacement = itlb_.size();
    for (std::size_t index = 0u; index < itlb_.size(); ++index) {
        if (!itlb_valid_[index]) {
            replacement = index;
            break;
        }
    }
    if (replacement == itlb_.size()) {
        replacement = static_cast<std::size_t>(
            std::min_element(itlb_lru_.begin(), itlb_lru_.end()) - itlb_lru_.begin());
    }
    const bool replacing_valid_entry = itlb_valid_[replacement];
    const auto old_rank = itlb_lru_[replacement];
    itlb_[replacement] = *found;
    itlb_valid_[replacement] = true;
    itlb_source_slots_[replacement] = found->slot;
    const auto valid_count =
        static_cast<std::uint8_t>(std::count(itlb_valid_.begin(), itlb_valid_.end(), true));
    if (replacing_valid_entry) {
        for (std::size_t index = 0u; index < itlb_.size(); ++index) {
            if (index != replacement && itlb_valid_[index] &&
                itlb_lru_[index] > old_rank)
                --itlb_lru_[index];
        }
    }
    itlb_lru_[replacement] = valid_count - 1u;
    return finish_translation(
        *found, found->slot, static_cast<std::uint8_t>(replacement), true);
}

BlockStateGuard RuntimeAddressSpace::guard_for(const std::uint32_t virtual_address,
                                               const std::uint32_t fpscr,
                                               const bool privileged) const {
    const auto translated =
        inspect_translation(virtual_address, TranslationAccess::Instruction, privileged);
    return {mode_,
            mmucr_,
            address_space_generation_,
            mmu_generation_,
            watchpoint_generation_,
            fpscr & fpscr_guard_mask,
            translated.physical_address / page_size * page_size};
}

std::optional<BlockStateGuard>
RuntimeAddressSpace::direct_p1_p2_instruction_guard(const std::uint32_t address,
                                                    const std::uint32_t fpscr,
                                                    const bool privileged) const noexcept {
    const auto segment = address >> 29u;
    if (!privileged || (segment != 4u && segment != 5u)) return std::nullopt;
    return BlockStateGuard{
        mode_,
        mmucr_,
        address_space_generation_,
        mmu_generation_,
        watchpoint_generation_,
        fpscr & fpscr_guard_mask,
        canonical_physical_address(address) / page_size * page_size,
    };
}

bool RuntimeAddressSpace::direct_p1_p2_dispatch_guard_current(
    const std::uint32_t address,
    const std::uint32_t fpscr,
    const bool privileged,
    const BlockVariantKey& expected,
    const std::uint64_t runtime_generation) const noexcept {
    const auto segment = address >> 29u;
    return privileged && (segment == 4u || segment == 5u) &&
           expected.address_space_generation == address_space_generation_ &&
           expected.mmu_generation == mmu_generation_ &&
           expected.watchpoint_generation == watchpoint_generation_ &&
           expected.fpscr_mode == (fpscr & fpscr_guard_mask) &&
           expected.runtime_generation == runtime_generation;
}

bool RuntimeAddressSpace::prove_instruction_mapping(const std::uint32_t virtual_start,
                                                    const std::uint32_t physical_start,
                                                    const std::uint32_t size,
                                                    const bool privileged) const noexcept {
    if (size < 2u || (size & 1u) != 0u || (virtual_start & 1u) != 0u ||
        (physical_start & 1u) != 0u)
        return false;
    const auto virtual_end = static_cast<std::uint64_t>(virtual_start) + size;
    const auto physical_end = static_cast<std::uint64_t>(physical_start) + size;
    if (virtual_end > 0x1'0000'0000ull || physical_end > 0x1'0000'0000ull)
        return false;
    const auto translated_at = [&](const std::uint32_t offset) {
        try {
            return inspect_translation(virtual_start + offset,
                                       TranslationAccess::Instruction,
                                       privileged)
                       .physical_address ==
                   physical_start + offset;
        } catch (...) {
            return false;
        }
    };
    if (!translated_at(0u) || !translated_at(size - 2u)) return false;
    constexpr std::uint64_t smallest_tlb_page = 1024u;
    auto boundary =
        (static_cast<std::uint64_t>(virtual_start) / smallest_tlb_page + 1u) *
        smallest_tlb_page;
    while (boundary < virtual_end) {
        const auto offset = static_cast<std::uint32_t>(boundary - virtual_start);
        if (!translated_at(offset) || !translated_at(offset - 2u)) return false;
        boundary += smallest_tlb_page;
    }
    return true;
}

RuntimeAddressSpaceSnapshot RuntimeAddressSpace::snapshot() const {
    return {
        mode_,
        mmucr_,
        asid_,
        address_space_generation_,
        mmu_generation_,
        watchpoint_generation_,
        mappings_,
        itlb_,
        itlb_valid_,
        itlb_lru_,
        itlb_source_slots_,
    };
}

bool RuntimeAddressSpace::block_fits_translation_page(const std::uint32_t virtual_start,
                                                      const std::uint32_t size) const noexcept {
    if (size < 2u || (virtual_start & 1u) != 0u || (size & 1u) != 0u) return false;
    const auto final_instruction =
        static_cast<std::uint64_t>(virtual_start) + size - 2u;
    if (final_instruction > 0xFFFFFFFFull) return false;
    try {
        const auto first =
            inspect_translation(virtual_start, TranslationAccess::Instruction);
        const auto final = inspect_translation(
            static_cast<std::uint32_t>(final_instruction), TranslationAccess::Instruction);
        return first.utlb_slot == final.utlb_slot &&
               static_cast<std::uint64_t>(final.physical_address) ==
                   static_cast<std::uint64_t>(first.physical_address) + size - 2u;
    } catch (...) {
        return false;
    }
}

} // namespace katana::runtime
