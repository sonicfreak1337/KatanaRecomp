#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t abi_version = 1u;

class Memory {
public:
    explicit Memory(std::size_t size = 1024u * 1024u);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint8_t read_u8(std::uint32_t address) const;
    [[nodiscard]] std::uint16_t read_u16(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t read_u32(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t read_s8(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t read_s16(std::uint32_t address) const;

    void write_u8(std::uint32_t address, std::uint8_t value);
    void write_u16(std::uint32_t address, std::uint16_t value);
    void write_u32(std::uint32_t address, std::uint32_t value);

private:
    void check(std::uint32_t address, std::size_t width) const;

    std::vector<std::uint8_t> bytes_;
};

struct CpuState {
    std::array<std::uint32_t, 16> r{};
    std::array<std::uint32_t, 8> r_bank{};
    std::uint32_t pc = 0u;
    std::uint32_t pr = 0u;
    std::uint32_t gbr = 0u;
    std::uint32_t vbr = 0u;
    std::uint32_t ssr = 0u;
    std::uint32_t spc = 0u;
    std::uint32_t sgr = 0u;
    std::uint32_t dbr = 0u;
    std::uint32_t tra = 0u;
    std::uint32_t expevt = 0u;
    std::uint32_t mach = 0u;
    std::uint32_t macl = 0u;
    std::uint32_t fpul = 0u;
    std::uint32_t fpscr = 0u;
    std::uint32_t sr = 0u;
    bool t = false;
    bool s = false;
    bool q = false;
    bool m = false;
    bool trap_pending = false;
    bool sleeping = false;
    Memory memory{};

    [[nodiscard]] std::uint32_t read_sr() const noexcept;
    void write_sr(std::uint32_t value) noexcept;
};

[[noreturn]] void unresolved_call(CpuState& cpu, std::uint32_t target);
[[noreturn]] void unresolved_jump(CpuState& cpu, std::uint32_t target);

}