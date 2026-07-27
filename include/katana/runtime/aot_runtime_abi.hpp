#pragma once

// Stable, intentionally narrow include surface for generated native AOT
// translation units. Product codegen depends on this ABI-facing contract
// instead of including the wider Katana source tree directly.
#include "katana/runtime/block_abi.hpp"
#include "katana/runtime/exception.hpp"
#include "katana/runtime/fpu.hpp"
#include "katana/runtime/indirect_dispatch.hpp"
#include "katana/runtime/platform_services.hpp"
#include "katana/runtime/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace katana::runtime {

// Native guest calls deliberately use the host stack, but an arbitrary guest
// call graph must never be allowed to exhaust it. Reaching this limit is not a
// guest-visible failure: generated code leaves cpu.pc at the already prepared
// callee and unwinds to the central dispatcher, which resumes the same call
// without a native fallback, runtime decoding, or emulation.
inline constexpr std::uint32_t native_aot_call_depth_limit = 128u;

namespace detail {
inline thread_local std::uint32_t native_aot_call_depth = 0u;
}

class NativeAotCallDepthGuard final {
  public:
    NativeAotCallDepthGuard() noexcept
        : acquired_(detail::native_aot_call_depth < native_aot_call_depth_limit) {
        if (acquired_) ++detail::native_aot_call_depth;
    }

    ~NativeAotCallDepthGuard() {
        if (acquired_) --detail::native_aot_call_depth;
    }

    NativeAotCallDepthGuard(const NativeAotCallDepthGuard&) = delete;
    NativeAotCallDepthGuard& operator=(const NativeAotCallDepthGuard&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return acquired_; }

  private:
    bool acquired_ = false;
};

// Keeps only a compile-time selected subset of the SH-4 general registers in
// native locals. Generated code uses this object only for pure leaf functions:
// memory, host-service, exception, SR/register-bank and native-call boundaries
// remain on CpuState. Destruction is the function/dispatcher boundary and makes
// every selected register architecturally visible, including while unwinding a
// host exception such as a guest-cycle budget stop.
template <std::uint16_t RegisterMask>
class NativeAotRegisterFile final {
  public:
    explicit NativeAotRegisterFile(CpuState& cpu) noexcept : cpu_(cpu) {
        load(std::make_index_sequence<16u>{});
    }

    ~NativeAotRegisterFile() { flush(); }

    NativeAotRegisterFile(const NativeAotRegisterFile&) = delete;
    NativeAotRegisterFile& operator=(const NativeAotRegisterFile&) = delete;

    [[nodiscard]] std::uint32_t& operator[](const std::size_t index) noexcept {
        return values_[index];
    }

    [[nodiscard]] const std::uint32_t& operator[](const std::size_t index) const noexcept {
        return values_[index];
    }

    void flush() noexcept { store(std::make_index_sequence<16u>{}); }

  private:
    template <std::size_t Index>
    void load_one() noexcept {
        if constexpr ((RegisterMask & (std::uint16_t{1u} << Index)) != 0u)
            values_[Index] = cpu_.r[Index];
    }

    template <std::size_t... Indexes>
    void load(std::index_sequence<Indexes...>) noexcept {
        (load_one<Indexes>(), ...);
    }

    template <std::size_t Index>
    void store_one() noexcept {
        if constexpr ((RegisterMask & (std::uint16_t{1u} << Index)) != 0u)
            cpu_.r[Index] = values_[Index];
    }

    template <std::size_t... Indexes>
    void store(std::index_sequence<Indexes...>) noexcept {
        (store_one<Indexes>(), ...);
    }

    CpuState& cpu_;
    std::array<std::uint32_t, 16u> values_{};
};

} // namespace katana::runtime
