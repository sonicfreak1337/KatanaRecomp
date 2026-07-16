#pragma once

#include "katana/runtime/block_table.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace katana::runtime {

enum class AddressTranslationMode : std::uint8_t { NoMmu, Mmu };
enum class TranslationAccess : std::uint8_t { Instruction, Read, Write };

struct TlbMapping {
    std::uint32_t virtual_page = 0u;
    std::uint32_t physical_page = 0u;
    bool readable = true;
    bool writable = true;
    bool executable = true;
    bool user_access = true;
};

struct TranslationResult {
    std::uint32_t virtual_address = 0u;
    std::uint32_t physical_address = 0u;
    std::uint64_t mmu_generation = 0u;
    bool no_mmu_fastpath = false;
};

struct BlockStateGuard {
    AddressTranslationMode mode = AddressTranslationMode::NoMmu;
    std::uint32_t mmucr = 0u;
    std::uint64_t address_space_generation = 0u;
    std::uint64_t mmu_generation = 0u;
    std::uint64_t watchpoint_generation = 0u;
    std::uint32_t fpscr_mode = 0u;
    std::uint32_t physical_page = 0u;

    [[nodiscard]] bool operator==(const BlockStateGuard&) const noexcept = default;
};

[[nodiscard]] BlockVariantKey block_variant_key(const BlockStateGuard& guard,
                                                std::uint64_t runtime_generation = 0u) noexcept;

class TranslationError final : public std::runtime_error {
  public:
    TranslationError(TranslationAccess access, std::uint32_t address, ExceptionCause cause);
    [[nodiscard]] ExceptionCause cause() const noexcept;
    [[nodiscard]] TranslationAccess access() const noexcept;
    [[nodiscard]] std::uint32_t address() const noexcept;

  private:
    ExceptionCause cause_;
    TranslationAccess access_;
    std::uint32_t address_;
};

class RuntimeAddressSpace {
  public:
    static constexpr std::uint32_t page_size = 4096u;
    void set_mode(AddressTranslationMode mode) noexcept;
    void write_mmucr(std::uint32_t value) noexcept;
    void ldtlb(TlbMapping mapping);
    void clear_tlb() noexcept;
    void bump_address_space() noexcept;
    void bump_watchpoints() noexcept;
    [[nodiscard]] TranslationResult
    translate(std::uint32_t address, TranslationAccess access, bool privileged = true) const;
    [[nodiscard]] BlockStateGuard guard_for(std::uint32_t virtual_address,
                                            std::uint32_t fpscr) const;
    [[nodiscard]] bool block_fits_translation_page(std::uint32_t virtual_start,
                                                   std::uint32_t size) const noexcept;

  private:
    AddressTranslationMode mode_ = AddressTranslationMode::NoMmu;
    std::uint32_t mmucr_ = 0u;
    std::uint64_t address_space_generation_ = 0u;
    std::uint64_t mmu_generation_ = 0u;
    std::uint64_t watchpoint_generation_ = 0u;
    std::vector<TlbMapping> mappings_;
};

} // namespace katana::runtime
