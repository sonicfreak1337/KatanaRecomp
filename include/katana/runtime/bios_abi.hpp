#pragma once

#include "katana/runtime/block_table.hpp"
#include "katana/runtime/firmware_handoff.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace katana::runtime {

inline constexpr std::uint32_t bios_abi_contract_version = 3u;
enum class BiosAbiVectorKind : std::uint8_t { SysInfo, RomFont, Flash, MiscGdrom, Gdrom2, System };
enum class BiosAbiServiceStatus : std::uint8_t { Completed, ServiceUnavailable };

struct BiosAbiVector {
    BiosAbiVectorKind kind = BiosAbiVectorKind::SysInfo;
    std::string_view name;
    std::uint32_t slot_address = 0u;
    std::uint32_t handler_address = 0u;
};
struct BiosAbiCall {
    BiosAbiVectorKind vector = BiosAbiVectorKind::SysInfo;
    std::uint32_t selector = 0u;
    std::uint32_t super_selector = 0u;
    std::string_view service;
    BiosAbiServiceStatus status = BiosAbiServiceStatus::ServiceUnavailable;
};

class BiosAbiDispatchError final : public std::runtime_error {
  public:
    BiosAbiDispatchError(std::uint32_t handler_address,
                         std::uint32_t selector,
                         std::uint32_t super_selector,
                         std::uint32_t return_address,
                         std::string reason);
};

[[nodiscard]] std::span<const BiosAbiVector> hle_bios_abi_vectors() noexcept;
[[nodiscard]] BiosAbiCall route_hle_bios_abi_call(const CpuState& cpu);
void install_hle_bios_abi(Memory& memory,
                          RuntimeBlockTable& blocks,
                          FirmwareHandoffMap& handoff,
                          const BlockVariantKey& variant = {},
                          std::uint64_t guest_cycle = 0u,
                          ExecutableCodeTracker* code_tracker = nullptr);
[[nodiscard]] const char* bios_abi_service_status_name(BiosAbiServiceStatus status) noexcept;
[[nodiscard]] std::string format_hle_bios_abi_contract_json();

} // namespace katana::runtime
