#include "katana/runtime/native_port_platform.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace katana::runtime;

static NativePortDevelopmentStateResult handler_result = NativePortDevelopmentStateResult::Deferred;
static bool handler_validate_state = true;
static bool handler_validate_deferred = false;
static NativePortDevelopmentStateResult restore_fixture(
    NativePortContext& context, const NativePortDevelopmentStateRequest&) noexcept {
    if ((handler_result == NativePortDevelopmentStateResult::Loaded || handler_validate_deferred) && handler_validate_state &&
        context.platform->input_initial_state_pending()) {
        const std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        try { context.platform->validate_input_initial_state(bytes); }
        catch (...) { return NativePortDevelopmentStateResult::Rejected; }
    }
    return handler_result;
}

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template <typename Function> static void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const NativePortPlatformError&) { rejected = true; }
    require(rejected, "Checkpoint identity/order violation was accepted");
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("katana-checkpoint-input-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "content");
    const std::array<std::byte, 4> state = {
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    {
        std::ofstream output(root / "initial.state", std::ios::binary);
        output.write(reinterpret_cast<const char*>(state.data()), state.size());
        require(bool(output), "Cannot create isolated checkpoint fixture");
    }
    NativePortPlatformConfig config;
    config.content_root = root / "content";
    config.user_data_root = root / "record-user";
    config.project_id = "checkpoint-input-test";
    config.input_identity = "sonic-checkpoint-input-contract-test";
    config.require_gamepad_backend = false;
    config.input_initial_state_path = root / "initial.state";
    config.input_record_path = root / "route.kat1";
    NativePortInputSnapshot recorded;
    {
        NativePortPlatformServices platform(config);
        rejects([&] { platform.start_input_after_initial_state_load(); });
        for (unsigned poll = 0; poll < 5u; ++poll)
            require(platform.poll_gamepads().poll_sequence == 0u,
                    "Pre-restore poll consumed recording position");
        auto wrong = state;
        wrong[0] = std::byte{9};
        rejects([&] { platform.validate_input_initial_state(wrong); });
        rejects([&] { platform.start_input_after_initial_state_load(); });
        platform.validate_input_initial_state(state);
        require(platform.poll_gamepads().poll_sequence == 0u,
                "Validation alone released input before restore completion");
        platform.start_input_after_initial_state_load();
        recorded = platform.poll_gamepads();
        require(recorded.poll_sequence == 1u, "Recording did not start at record one");
        platform.finalize_clean_shutdown();
    }
    config.input_record_path.clear();
    config.input_replay_path = root / "route.kat1";
    config.user_data_root = root / "replay-user";
    {
        NativePortPlatformServices platform(config);
        for (unsigned poll = 0; poll < 11u; ++poll)
            require(platform.poll_gamepads().poll_sequence == 0u,
                    "Pre-restore replay poll consumed a record");
        NativePortContext context;
        context.platform = &platform;
        context.development_state_handler = restore_fixture;
        NativePortDevelopmentStateRequest request;
        request.operation = NativePortDevelopmentStateOperation::Load;
        request.path = (root / "initial.state").string();
        require(dispatch_native_port_development_state(context, request) == NativePortDevelopmentStateResult::Deferred &&
                    platform.poll_gamepads().poll_sequence == 0u,
                "Deferred generic restore released replay");
        handler_result = NativePortDevelopmentStateResult::Loaded;
        require(dispatch_native_port_development_state(context, request) == NativePortDevelopmentStateResult::Loaded,
                "Generic Loaded handoff failed");
        const auto first = platform.poll_gamepads();
        require(first.poll_sequence == recorded.poll_sequence &&
                    first.connection_generation == recorded.connection_generation &&
                    first.gamepads[0].buttons == recorded.gamepads[0].buttons &&
                    first.gamepads[0].left_stick_x_raw == recorded.gamepads[0].left_stick_x_raw &&
                    first.gamepads[0].left_stick_y_raw == recorded.gamepads[0].left_stick_y_raw,
                "First post-restore replay record drifted");
        rejects([&] { platform.validate_input_initial_state(state); });
        rejects([&] { platform.start_input_after_initial_state_load(); });
        require(dispatch_native_port_development_state(context, request) == NativePortDevelopmentStateResult::Loaded &&
                    context.stop_reason == NativePortStopReason::None,
                "A later ordinary quickload incorrectly re-armed initial input");
    }
    config.user_data_root = root / "rejected-state-user";
    {
        NativePortPlatformServices platform(config);
        NativePortContext context;
        context.platform = &platform;
        context.development_state_handler = restore_fixture;
        NativePortDevelopmentStateRequest request;
        request.operation = NativePortDevelopmentStateOperation::Load;
        request.path = (root / "initial.state").string();
        handler_result = NativePortDevelopmentStateResult::Rejected;
        bool rejected = false;
        try { static_cast<void>(dispatch_native_port_development_state(context, request)); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected && context.stop_reason == NativePortStopReason::AotContractViolation &&
                    platform.poll_gamepads().poll_sequence == 0u,
                "Rejected initial restore ran indefinitely with neutral replay");
    }
    config.user_data_root = root / "unvalidated-state-user";
    {
        NativePortPlatformServices platform(config);
        NativePortContext context;
        context.platform = &platform;
        context.development_state_handler = restore_fixture;
        NativePortDevelopmentStateRequest request;
        request.operation = NativePortDevelopmentStateOperation::Load;
        handler_result = NativePortDevelopmentStateResult::Deferred;
        handler_validate_deferred = true;
        require(dispatch_native_port_development_state(context, request) == NativePortDevelopmentStateResult::Deferred,
                "Validated Deferred attempt did not defer");
        handler_validate_deferred = false;
        handler_result = NativePortDevelopmentStateResult::Loaded;
        handler_validate_state = false;
        rejects([&] { static_cast<void>(dispatch_native_port_development_state(context, request)); });
        require(context.stop_reason == NativePortStopReason::AotContractViolation &&
                    platform.poll_gamepads().poll_sequence == 0u,
                "Loaded without provider identity validation armed initial input");
    }
    {
        std::ofstream output(root / "initial.state", std::ios::binary | std::ios::trunc);
        output << "different state";
    }
    config.user_data_root = root / "wrong-state-user";
    rejects([&] { NativePortPlatformServices wrong_state(config); });
    // Only this test's freshly created directory is removed after all owners close.
    std::filesystem::remove_all(root);
    std::cout << "Checkpoint input binding and first-record handoff passed.\n";
}
