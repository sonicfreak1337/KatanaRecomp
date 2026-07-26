#include "katana/testing/sh4_sst_generated.hpp"

#include <sstream>

namespace katana::testing::sh4_sst {
namespace {

thread_local NativeExecutionTrace* active_trace = nullptr;
thread_local runtime::PlatformServices* active_services = nullptr;

} // namespace

AotDispatchError::AotDispatchError(const std::uint32_t target, const bool call)
    : std::runtime_error([&] {
          std::ostringstream message;
          message << "Unbound precompiled SH-4 " << (call ? "call" : "jump") << " target 0x"
                  << std::hex << target;
          return message.str();
      }()),
      target_(target), call_(call) {}

std::uint32_t AotDispatchError::target() const noexcept {
    return target_;
}

bool AotDispatchError::call() const noexcept {
    return call_;
}

void begin_native_execution_trace(NativeExecutionTrace& trace,
                                  runtime::PlatformServices& services) noexcept {
    trace.instructions.clear();
    trace.block_entries.clear();
    trace.allocation_failed = false;
    active_trace = &trace;
    active_services = &services;
}

void end_native_execution_trace() noexcept {
    active_trace = nullptr;
    active_services = nullptr;
}

void note_native_instruction(const std::uint32_t guest_pc, const bool delay_slot) noexcept {
    if (active_trace == nullptr || active_trace->allocation_failed) return;
    try {
        active_trace->instructions.push_back({guest_pc, delay_slot});
    } catch (...) {
        // Generated observers are deliberately noexcept; the runner promotes
        // this sticky bit to a fatal infrastructure error after leaving native code.
        active_trace->allocation_failed = true;
    }
}

void note_native_block(const std::uint32_t guest_pc) noexcept {
    if (active_trace == nullptr || active_trace->allocation_failed) return;
    try {
        active_trace->block_entries.push_back(guest_pc);
    } catch (...) {
        active_trace->allocation_failed = true;
    }
}

runtime::PlatformServices* active_native_services() noexcept {
    return active_services;
}

[[noreturn]] void
reject_native_dispatch(runtime::CpuState& cpu, const std::uint32_t target, const bool call) {
    cpu.pc = target;
    throw AotDispatchError(target, call);
}

} // namespace katana::testing::sh4_sst
