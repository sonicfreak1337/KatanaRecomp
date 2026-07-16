#include "katana/runtime/block_guards.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace katana::runtime {
namespace {
constexpr std::uint32_t fpscr_guard_mask =
    fpscr_rounding_mode_mask | fpscr_pr_mask | fpscr_sz_mask | fpscr_fr_mask;
}

TranslationError::TranslationError(
    const TranslationAccess access,
    const std::uint32_t address,
    const ExceptionCause cause
) : std::runtime_error([&] {
        std::ostringstream out;
        out << "SH-4-Adressuebersetzungsfehler access=" << static_cast<unsigned>(access)
            << " address=0x" << std::hex << std::setw(8) << std::setfill('0') << address;
        return out.str();
    }()), cause_(cause), access_(access), address_(address) {}
ExceptionCause TranslationError::cause() const noexcept { return cause_; }
TranslationAccess TranslationError::access() const noexcept { return access_; }
std::uint32_t TranslationError::address() const noexcept { return address_; }

void RuntimeAddressSpace::set_mode(const AddressTranslationMode mode) noexcept {
    if (mode_ != mode) { mode_ = mode; ++address_space_generation_; }
}
void RuntimeAddressSpace::write_mmucr(const std::uint32_t value) noexcept {
    if (mmucr_ != value) { mmucr_ = value; ++mmu_generation_; }
}
void RuntimeAddressSpace::ldtlb(TlbMapping mapping) {
    mapping.virtual_page = mapping.virtual_page / page_size * page_size;
    mapping.physical_page = canonical_physical_address(mapping.physical_page) / page_size * page_size;
    const auto found = std::find_if(mappings_.begin(), mappings_.end(), [&](const auto& value) {
        return value.virtual_page == mapping.virtual_page;
    });
    if (found == mappings_.end()) { mappings_.push_back(mapping); } else { *found = mapping; }
    ++mmu_generation_;
}
void RuntimeAddressSpace::clear_tlb() noexcept { mappings_.clear(); ++mmu_generation_; }
void RuntimeAddressSpace::bump_address_space() noexcept { ++address_space_generation_; }
void RuntimeAddressSpace::bump_watchpoints() noexcept { ++watchpoint_generation_; }

TranslationResult RuntimeAddressSpace::translate(
    const std::uint32_t address,
    const TranslationAccess access,
    const bool privileged
) const {
    if (mode_ == AddressTranslationMode::NoMmu) {
        return {address, canonical_physical_address(address), mmu_generation_, true};
    }
    const auto page = address / page_size * page_size;
    const auto found = std::find_if(mappings_.begin(), mappings_.end(), [&](const auto& value) {
        return value.virtual_page == page;
    });
    const bool permitted = found != mappings_.end() && (privileged || found->user_access) &&
        (access != TranslationAccess::Instruction || found->executable) &&
        (access != TranslationAccess::Read || found->readable) &&
        (access != TranslationAccess::Write || found->writable);
    if (!permitted) {
        throw TranslationError(
            access, address,
            access == TranslationAccess::Write ? ExceptionCause::AddressErrorWrite : ExceptionCause::AddressErrorRead
        );
    }
    return {address, found->physical_page + (address - page), mmu_generation_, false};
}

BlockStateGuard RuntimeAddressSpace::guard_for(
    const std::uint32_t virtual_address,
    const std::uint32_t fpscr
) const {
    const auto translated = translate(virtual_address, TranslationAccess::Instruction);
    return {
        mode_, mmucr_, address_space_generation_, mmu_generation_, watchpoint_generation_,
        fpscr & fpscr_guard_mask, translated.physical_address / page_size * page_size
    };
}

bool RuntimeAddressSpace::block_fits_translation_page(
    const std::uint32_t virtual_start,
    const std::uint32_t size
) const noexcept {
    if (size == 0u) { return false; }
    if (mode_ == AddressTranslationMode::NoMmu) { return true; }
    const auto last = static_cast<std::uint64_t>(virtual_start) + size - 1u;
    return last <= 0xFFFFFFFFull && virtual_start / page_size == last / page_size;
}

} // namespace katana::runtime
