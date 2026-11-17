#pragma once

// Explicit execution-state machine and background telemetry for Hailstone 24.
// Decouples network/simulation/MQ-Lua execution from Vulkan rendering so 23
// backgrounded characters run full game + RG Mercs automation with zero 3D/UI
// rendering overhead, while switching any character to FOREGROUND_FULL in <250ms.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace hailstone::app {

enum class ExecutionState : std::uint8_t {
    Disconnected = 0,
    BackgroundHeadless = 1,
    Transitioning = 2,
    ForegroundFull = 3,
};

enum class GpuRetentionMode : std::uint8_t {
    Deferred = 0,     // Release scene renderers & transient targets in BACKGROUND_HEADLESS
    WarmCache = 1,    // Keep zone/actor GPU buffers resident; suspend swapchain/targets only
    SharedBroker = 2, // Use Linux VK_KHR_external_memory_fd + hailstone_gpu_cache_broker
};

inline const char* execution_state_name(ExecutionState state) noexcept {
    switch (state) {
    case ExecutionState::Disconnected: return "DISCONNECTED";
    case ExecutionState::BackgroundHeadless: return "BACKGROUND_HEADLESS";
    case ExecutionState::Transitioning: return "TRANSITIONING";
    case ExecutionState::ForegroundFull: return "FOREGROUND_FULL";
    }
    return "DISCONNECTED";
}

inline GpuRetentionMode parse_gpu_retention_mode(std::string_view mode) noexcept {
    if (mode == "cached" || mode == "warm" || mode == "warm-cache") return GpuRetentionMode::WarmCache;
    if (mode == "shared" || mode == "shared-broker" || mode == "broker") return GpuRetentionMode::SharedBroker;
    return GpuRetentionMode::Deferred;
}

inline const char* gpu_retention_mode_name(GpuRetentionMode mode) noexcept {
    switch (mode) {
    case GpuRetentionMode::Deferred: return "deferred";
    case GpuRetentionMode::WarmCache: return "cached";
    case GpuRetentionMode::SharedBroker: return "shared-broker";
    }
    return "deferred";
}

struct ExecutionMetrics {
    ExecutionState state = ExecutionState::Disconnected;
    std::uint64_t sim_ticks = 0;
    std::uint64_t rendered_frames = 0;
    std::uint64_t skipped_render_frames = 0;
    std::uint64_t lua_pulses = 0;
    std::uint64_t last_sim_tick_us = 0;
    std::uint64_t last_lua_pulse_us = 0;
    std::uint64_t max_lua_pulse_us = 0;
    std::uint64_t last_switch_ms = 0;
    std::uint64_t last_network_rx_ms = 0;
    std::uint64_t rss_bytes = 0;
    std::uint64_t vram_bytes = 0;
    int bg_tick_hz = 25;
};

class ExecutionController {
public:
    using Clock = std::chrono::steady_clock;

    void configure(bool start_headless, int bg_tick_hz, GpuRetentionMode gpu_mode) noexcept {
        start_headless_ = start_headless;
        bg_tick_hz_ = std::clamp(bg_tick_hz, 10, 60);
        gpu_mode_ = gpu_mode;
        metrics_.bg_tick_hz = bg_tick_hz_;
        if (start_headless) {
            state_ = ExecutionState::BackgroundHeadless;
        } else {
            state_ = ExecutionState::ForegroundFull;
        }
        metrics_.state = state_;
    }

    ExecutionState state() const noexcept { return state_; }
    GpuRetentionMode gpu_mode() const noexcept { return gpu_mode_; }
    int background_tick_hz() const noexcept { return bg_tick_hz_; }
    void set_background_tick_hz(int hz) noexcept {
        bg_tick_hz_ = std::clamp(hz, 10, 60);
        metrics_.bg_tick_hz = bg_tick_hz_;
    }

    bool is_headless() const noexcept { return state_ == ExecutionState::BackgroundHeadless; }
    bool is_foreground() const noexcept { return state_ == ExecutionState::ForegroundFull; }
    bool is_transitioning() const noexcept { return state_ == ExecutionState::Transitioning; }
    bool should_render() const noexcept {
        return state_ == ExecutionState::ForegroundFull || state_ == ExecutionState::Transitioning;
    }
    bool should_suppress_gpu_and_ui() const noexcept {
        return state_ == ExecutionState::BackgroundHeadless;
    }

    // Request transition into FOREGROUND_FULL. Sets state to TRANSITIONING until
    // the first foreground frame is presented (complete_foreground_transition()).
    void request_foreground(Clock::time_point now = Clock::now()) noexcept {
        if (state_ == ExecutionState::ForegroundFull) return;
        transition_started_ = now;
        state_ = ExecutionState::Transitioning;
        metrics_.state = state_;
    }

    void complete_foreground_transition(Clock::time_point now = Clock::now()) noexcept {
        if (state_ != ExecutionState::Transitioning) return;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - transition_started_).count();
        metrics_.last_switch_ms = static_cast<std::uint64_t>(std::max<std::int64_t>(0, ms));
        state_ = ExecutionState::ForegroundFull;
        metrics_.state = state_;
    }

    void enter_background_headless() noexcept {
        if (state_ == ExecutionState::BackgroundHeadless) return;
        state_ = ExecutionState::BackgroundHeadless;
        metrics_.state = state_;
    }

    void mark_disconnected() noexcept {
        if (state_ == ExecutionState::Disconnected) return;
        state_ = ExecutionState::Disconnected;
        metrics_.state = state_;
    }

    void mark_connected() noexcept {
        if (state_ == ExecutionState::Disconnected) {
            state_ = start_headless_ ? ExecutionState::BackgroundHeadless : ExecutionState::ForegroundFull;
            metrics_.state = state_;
        }
    }

    void record_sim_tick(std::uint64_t duration_us) noexcept {
        ++metrics_.sim_ticks;
        metrics_.last_sim_tick_us = duration_us;
    }

    void record_lua_pulse(std::uint64_t duration_us) noexcept {
        ++metrics_.lua_pulses;
        metrics_.last_lua_pulse_us = duration_us;
        metrics_.max_lua_pulse_us = std::max(metrics_.max_lua_pulse_us, duration_us);
    }

    void record_frame_presented() noexcept { ++metrics_.rendered_frames; }
    void record_render_skipped() noexcept { ++metrics_.skipped_render_frames; }
    void update_memory_stats(std::uint64_t rss_bytes, std::uint64_t vram_bytes) noexcept {
        metrics_.rss_bytes = rss_bytes;
        metrics_.vram_bytes = vram_bytes;
    }

    const ExecutionMetrics& metrics() const noexcept { return metrics_; }

private:
    ExecutionState state_ = ExecutionState::Disconnected;
    GpuRetentionMode gpu_mode_ = GpuRetentionMode::Deferred;
    bool start_headless_ = false;
    int bg_tick_hz_ = 25;
    Clock::time_point transition_started_{};
    ExecutionMetrics metrics_{};
};

} // namespace hailstone::app
