#pragma once

// The main loop's frame cap and whether a frame is drawn (app/client_app.cpp): display.max_fps,
// display.background_fps, and Hailstone 24's decoupled ExecutionState pacing.
// Never throttles game simulation or Lua/MQ automation to the rate of visual rendering:
// in BACKGROUND_HEADLESS, simulation/network/nav/RG Mercs run at `bg_tick_hz` (default 25 Hz)
// while visual rendering, ImGui build, and particle updates are suppressed (draw = false, skip_ui_build = true).
// On login and loading screens (including zone-in settle) an unlimited max_fps counts as kLightScreenCap.

#include <algorithm>
#include <chrono>

#include "app/execution_state.h"

namespace hailstone::app {

inline constexpr int kBackgroundNoDraw = -1; // display.background_fps: nothing drawn while unfocused
inline constexpr int kBackgroundTickHz = 10;
inline constexpr int kLightScreenCap = 240; // display.max_fps 0 on a login/loading screen counts as this

struct FramePace {
    int cap = 0;                // the loop's ticks a second (0 none)
    bool draw = true;           // this frame is rendered via Vulkan
    bool skip_ui_build = false; // true in BACKGROUND_HEADLESS: skip ImGui/Lua UI/particles CPU work
};

// display.background_fps as it is kept: 0 (as max_fps), kBackgroundNoDraw, or 1 .. 1000.
inline int background_fps_setting(int value) {
    return value < 0 ? kBackgroundNoDraw : value == 0 ? 0 : std::min(value, 1000);
}

// The light-screen condition, kept here so it is testable without the heavy client_app_internal.h. in_game is
// screen_ == Screen::InGame; settling is zone_settling(). The last stretch of a zone-in still draws the loading
// overlay while in_game is already true, so settling must keep the screen light.
inline bool is_light_screen(bool in_game, bool settling) {
    return !in_game || settling;
}

// max_fps 0: none. background_fps 0: as max_fps; kBackgroundNoDraw: none drawn; else drawn frames a second.
// since_drawn: the time since the last drawn frame.
// light_screen: a login/loading screen, where an unlimited cap would spin; max_fps 0 counts as kLightScreenCap there.
inline FramePace frame_pace(int max_fps, int background_fps, bool focused, std::chrono::microseconds since_drawn,
                            bool light_screen = false) {
    const int cap = light_screen && max_fps == 0 ? kLightScreenCap : max_fps;
    FramePace pace{cap, true, false};
    if (focused || background_fps == 0) return pace;
    const int loop = std::max(background_fps, kBackgroundTickHz);
    pace.cap = cap > 0 ? std::min(cap, loop) : loop;
    if (background_fps == kBackgroundNoDraw) {
        pace.draw = false;
        pace.skip_ui_build = true;
    } else if (background_fps < kBackgroundTickHz) {
        // Half a tick early: the loop's ticks land on the interval or just after it, not a whole tick late.
        pace.draw = since_drawn >= std::chrono::microseconds(1000000 / background_fps - 500000 / pace.cap);
    }
    return pace;
}

// ExecutionState-aware pacing: explicitly decouples simulation tick Hz from visual render Hz.
inline FramePace frame_pace_for_state(ExecutionState state, int max_fps, int background_fps, int bg_tick_hz,
                                      bool focused, std::chrono::microseconds since_drawn,
                                      bool light_screen = false) {
    if (state == ExecutionState::Transitioning) {
        // Immediate unthrottled transition frame to hit < 250 ms switch latency.
        return FramePace{0, true, false};
    }
    if (state == ExecutionState::BackgroundHeadless) {
        const int sim_hz = std::clamp(bg_tick_hz, 10, 60);
        const int cap = max_fps > 0 ? std::min(max_fps, sim_hz) : sim_hz;
        return FramePace{cap, false, true};
    }
    if (state == ExecutionState::Disconnected && !focused) {
        return FramePace{10, false, true};
    }
    return frame_pace(max_fps, background_fps, focused, since_drawn, light_screen);
}

} // namespace hailstone::app
