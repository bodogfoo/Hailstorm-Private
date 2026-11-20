#include <doctest/doctest.h>

#include <chrono>

#include "app/execution_state.h"
#include "app/frame_pace.h"

using namespace hailstone::app;

TEST_CASE("ExecutionController transitions and GPU/UI suppression flags") {
    ExecutionController ctrl;
    ctrl.configure(true, 25, GpuRetentionMode::WarmCache);
    CHECK(ctrl.state() == ExecutionState::BackgroundHeadless);
    CHECK(ctrl.is_headless());
    CHECK_FALSE(ctrl.is_foreground());
    CHECK_FALSE(ctrl.should_render());
    CHECK(ctrl.should_suppress_gpu_and_ui());
    CHECK(ctrl.background_tick_hz() == 25);
    CHECK(ctrl.gpu_mode() == GpuRetentionMode::WarmCache);

    const auto t0 = ExecutionController::Clock::now();
    ctrl.request_foreground(t0);
    CHECK(ctrl.state() == ExecutionState::Transitioning);
    CHECK(ctrl.is_transitioning());
    CHECK(ctrl.should_render());
    CHECK_FALSE(ctrl.should_suppress_gpu_and_ui());

    ctrl.complete_foreground_transition(t0 + std::chrono::milliseconds(18));
    CHECK(ctrl.state() == ExecutionState::ForegroundFull);
    CHECK(ctrl.is_foreground());
    CHECK(ctrl.metrics().last_switch_ms == 18);

    ctrl.enter_background_headless();
    CHECK(ctrl.is_headless());
    CHECK(ctrl.should_suppress_gpu_and_ui());

    ctrl.set_background_tick_hz(5);
    CHECK(ctrl.background_tick_hz() == 10);
    ctrl.set_background_tick_hz(120);
    CHECK(ctrl.background_tick_hz() == 60);
}

TEST_CASE("FramePace decouples background simulation Hz from visual rendering") {
    const auto bg = frame_pace_for_state(
        ExecutionState::BackgroundHeadless, 60, kBackgroundNoDraw, 25, false, std::chrono::seconds(5));
    CHECK(bg.cap == 25);
    CHECK_FALSE(bg.draw);
    CHECK(bg.skip_ui_build);

    const auto trans = frame_pace_for_state(
        ExecutionState::Transitioning, 60, kBackgroundNoDraw, 25, false, std::chrono::milliseconds(0));
    CHECK(trans.cap == 0);
    CHECK(trans.draw);
    CHECK_FALSE(trans.skip_ui_build);

    const auto fg = frame_pace_for_state(
        ExecutionState::ForegroundFull, 60, kBackgroundNoDraw, 25, true, std::chrono::milliseconds(0));
    CHECK(fg.cap == 60);
    CHECK(fg.draw);
    CHECK_FALSE(fg.skip_ui_build);

    CHECK(parse_gpu_retention_mode("deferred") == GpuRetentionMode::Deferred);
    CHECK(parse_gpu_retention_mode("cached") == GpuRetentionMode::WarmCache);
    CHECK(parse_gpu_retention_mode("shared-broker") == GpuRetentionMode::SharedBroker);
}
