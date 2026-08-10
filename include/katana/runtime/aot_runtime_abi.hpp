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

// Narrow generated-code view of the executable tracker. The tracker remains
// opaque to AOT translation units; a missing tracker is deliberately not a
// proof that a store cannot alias executable guest bytes.
[[nodiscard]] std::uint64_t
native_aot_code_tracker_generation(
    const ExecutableCodeTracker* tracker) noexcept;
[[nodiscard]] bool
native_aot_code_tracker_tracks_address(
    const ExecutableCodeTracker* tracker,
    std::uint32_t physical_address,
    std::size_t size) noexcept;

// Native guest calls deliberately use the host stack, but an arbitrary guest
// call graph must never be allowed to exhaust it. Reaching this limit is not a
// guest-visible failure: generated code leaves cpu.pc at the already prepared
// callee and unwinds to the central dispatcher, which resumes the same call
// without a native fallback, runtime decoding, or emulation.
// Generated owners can have materially larger host frames than ordinary C++
// functions.  A depth of 128 can therefore exhaust the 1 MiB default Windows
// executable stack before this guard ever gets a chance to hand control back
// to the iterative dispatcher.  Keep both recursive entry paths shallow; the
// prepared cpu.pc makes hitting either limit a transparent dispatch boundary.
inline constexpr std::uint32_t native_aot_call_depth_limit = 32u;
inline constexpr std::uint32_t native_aot_dispatch_depth_limit = 32u;

namespace detail {
inline thread_local std::uint32_t native_aot_call_depth = 0u;
inline thread_local std::uint32_t native_aot_dispatch_depth = 0u;
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

// Runtime-only/guarded dynamic calls use the validated central dispatcher but
// may enter it directly from an outer generated block. Keep that host
// recursion independently bounded: unlike NativeAotCallDepthGuard this guard
// must not suppress BlockExit construction in the blocks selected by the
// nested dispatcher.
class NativeAotDispatchDepthGuard final {
  public:
    NativeAotDispatchDepthGuard() noexcept
        : acquired_(detail::native_aot_dispatch_depth <
                    native_aot_dispatch_depth_limit) {
        if (acquired_) ++detail::native_aot_dispatch_depth;
    }

    ~NativeAotDispatchDepthGuard() noexcept {
        if (acquired_) --detail::native_aot_dispatch_depth;
    }

    NativeAotDispatchDepthGuard(const NativeAotDispatchDepthGuard&) = delete;
    NativeAotDispatchDepthGuard&
    operator=(const NativeAotDispatchDepthGuard&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return acquired_; }

  private:
    bool acquired_ = false;
};

// A product backend entry is also the runtime block-table function. Native
// guest calls enter that same function while their depth guard is alive; only
// the outer table invocation must build and finalize a BlockExit.
[[nodiscard]] inline bool native_aot_call_is_nested() noexcept {
    return detail::native_aot_call_depth != 0u;
}

// Product owner entries share their active BlockExit metadata with the central
// dispatcher. Native calls enter another owner on the same host thread, so
// each entry must preserve the caller's metadata until that native call level
// returns. The frame also gives every level a fresh tail-dispatch flag.
class NativeAotCallExitStateFrame final {
  public:
    NativeAotCallExitStateFrame(
        BlockAddress& active_source,
        BlockEndKind& active_kind,
        DynamicDispatchSiteClass& active_site_class,
        bool& tail_dispatch_completed,
        const BlockAddress entry_source) noexcept
        : active_source_(active_source),
          active_kind_(active_kind),
          active_site_class_(active_site_class),
          tail_dispatch_completed_(tail_dispatch_completed),
          saved_source_(active_source),
          saved_kind_(active_kind),
          saved_site_class_(active_site_class),
          saved_tail_dispatch_completed_(tail_dispatch_completed) {
        active_source_ = entry_source;
        active_kind_ = BlockEndKind::Fallthrough;
        active_site_class_ = DynamicDispatchSiteClass::NotDynamic;
        tail_dispatch_completed_ = false;
    }

    ~NativeAotCallExitStateFrame() noexcept {
        if (!restore_on_exit_) return;
        active_source_ = saved_source_;
        active_kind_ = saved_kind_;
        active_site_class_ = saved_site_class_;
        tail_dispatch_completed_ = saved_tail_dispatch_completed_;
    }

    // The outer block-table invocation publishes its completed site class to
    // the central dispatcher after returning. Nested owner calls do not release
    // their frame and therefore restore the caller level normally.
    void release() noexcept { restore_on_exit_ = false; }

    NativeAotCallExitStateFrame(const NativeAotCallExitStateFrame&) = delete;
    NativeAotCallExitStateFrame&
    operator=(const NativeAotCallExitStateFrame&) = delete;

  private:
    BlockAddress& active_source_;
    BlockEndKind& active_kind_;
    DynamicDispatchSiteClass& active_site_class_;
    bool& tail_dispatch_completed_;
    BlockAddress saved_source_;
    BlockEndKind saved_kind_;
    DynamicDispatchSiteClass saved_site_class_;
    bool saved_tail_dispatch_completed_ = false;
    bool restore_on_exit_ = true;
};

enum class NativeAotScalarRegister : std::uint8_t {
    T,
    Pr,
    Gbr,
    Mach,
    Macl,
    Fpul
};

using NativeAotScalarRegisterMask = std::uint8_t;

[[nodiscard]] constexpr NativeAotScalarRegisterMask
native_aot_scalar_register_bit(const NativeAotScalarRegister value) noexcept {
    return static_cast<NativeAotScalarRegisterMask>(
        NativeAotScalarRegisterMask{1u} << static_cast<std::uint8_t>(value));
}

// Keeps compile-time selected SH-4 GPR and scalar special-register values in
// native locals. FPU register arrays deliberately remain outside this
// contract. A generated architectural boundary first calls flush_release();
// after the boundary, reload_acquire() observes the potentially new register
// bank and exception/SR state. A released object never writes stale locals from
// its destructor.
template <std::uint16_t RegisterMask,
          NativeAotScalarRegisterMask ScalarRegisterMask = 0u>
class NativeAotRegisterFile final {
  public:
    explicit NativeAotRegisterFile(CpuState& cpu) noexcept : cpu_(cpu) {
        load(std::make_index_sequence<16u>{});
        load_scalars();
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

    [[nodiscard]] bool& t() noexcept {
        static_assert(scalar_selected(NativeAotScalarRegister::T));
        return t_;
    }
    [[nodiscard]] const bool& t() const noexcept {
        static_assert(scalar_selected(NativeAotScalarRegister::T));
        return t_;
    }
    [[nodiscard]] std::uint32_t& pr() noexcept {
        static_assert(scalar_selected(NativeAotScalarRegister::Pr));
        return pr_;
    }
    [[nodiscard]] const std::uint32_t& pr() const noexcept {
        static_assert(scalar_selected(NativeAotScalarRegister::Pr));
        return pr_;
    }
    [[nodiscard]] std::uint32_t& gbr() noexcept {
        static_assert(scalar_selected(NativeAotScalarRegister::Gbr));
        return gbr_;
    }
    [[nodiscard]] const std::uint32_t& gbr() const noexcept {
        static_assert(scalar_selected(NativeAotScalarRegister::Gbr));
        return gbr_;
    }
    [[nodiscard]] std::uint32_t& mach() noexcept {
        static_assert(scalar_selected(NativeAotScalarRegister::Mach));
        return mach_;
    }
    [[nodiscard]] const std::uint32_t& mach() const noexcept {
        static_assert(scalar_selected(NativeAotScalarRegister::Mach));
        return mach_;
    }
    [[nodiscard]] std::uint32_t& macl() noexcept {
        static_assert(scalar_selected(NativeAotScalarRegister::Macl));
        return macl_;
    }
    [[nodiscard]] const std::uint32_t& macl() const noexcept {
        static_assert(scalar_selected(NativeAotScalarRegister::Macl));
        return macl_;
    }
    [[nodiscard]] std::uint32_t& fpul() noexcept {
        static_assert(scalar_selected(NativeAotScalarRegister::Fpul));
        return fpul_;
    }
    [[nodiscard]] const std::uint32_t& fpul() const noexcept {
        static_assert(scalar_selected(NativeAotScalarRegister::Fpul));
        return fpul_;
    }

    [[nodiscard]] bool owns_registers() const noexcept { return owns_registers_; }

    // Compatibility flush for native exits and exceptional unwinding. It keeps
    // ownership, so a later mutation remains visible at destruction.
    void flush() noexcept {
        if (!owns_registers_) return;
        store(std::make_index_sequence<16u>{});
        store_scalars();
    }

    void flush_release() noexcept {
        if (!owns_registers_) return;
        store(std::make_index_sequence<16u>{});
        store_scalars();
        owns_registers_ = false;
    }

    void reload_acquire() noexcept {
        if (owns_registers_) return;
        load(std::make_index_sequence<16u>{});
        load_scalars();
        owns_registers_ = true;
    }

  private:
    [[nodiscard]] static constexpr bool
    scalar_selected(const NativeAotScalarRegister value) noexcept {
        return (ScalarRegisterMask & native_aot_scalar_register_bit(value)) != 0u;
    }

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

    void load_scalars() noexcept {
        if constexpr (scalar_selected(NativeAotScalarRegister::T)) t_ = cpu_.t;
        if constexpr (scalar_selected(NativeAotScalarRegister::Pr)) pr_ = cpu_.pr;
        if constexpr (scalar_selected(NativeAotScalarRegister::Gbr)) gbr_ = cpu_.gbr;
        if constexpr (scalar_selected(NativeAotScalarRegister::Mach)) mach_ = cpu_.mach;
        if constexpr (scalar_selected(NativeAotScalarRegister::Macl)) macl_ = cpu_.macl;
        if constexpr (scalar_selected(NativeAotScalarRegister::Fpul)) fpul_ = cpu_.fpul;
    }

    void store_scalars() noexcept {
        if constexpr (scalar_selected(NativeAotScalarRegister::T)) cpu_.t = t_;
        if constexpr (scalar_selected(NativeAotScalarRegister::Pr)) cpu_.pr = pr_;
        if constexpr (scalar_selected(NativeAotScalarRegister::Gbr)) cpu_.gbr = gbr_;
        if constexpr (scalar_selected(NativeAotScalarRegister::Mach)) cpu_.mach = mach_;
        if constexpr (scalar_selected(NativeAotScalarRegister::Macl)) cpu_.macl = macl_;
        if constexpr (scalar_selected(NativeAotScalarRegister::Fpul)) cpu_.fpul = fpul_;
    }

    CpuState& cpu_;
    std::array<std::uint32_t, 16u> values_{};
    bool t_ = false;
    std::uint32_t pr_ = 0u;
    std::uint32_t gbr_ = 0u;
    std::uint32_t mach_ = 0u;
    std::uint32_t macl_ = 0u;
    std::uint32_t fpul_ = 0u;
    bool owns_registers_ = true;
};

} // namespace katana::runtime
