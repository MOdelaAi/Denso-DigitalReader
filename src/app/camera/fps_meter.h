// Smoothed frames-per-second estimate from frame-arrival timestamps. A tile
// ticks it once per displayed frame and reads back an EMA-smoothed rate to show
// the camera's real live fps. Pure (std::chrono only), so it's unit-tested off
// the GUI thread; it holds no Qt or capture state.
#pragma once

#include <chrono>

namespace denso::ui {

class FpsMeter {
public:
    using clock = std::chrono::steady_clock;

    /// Record a displayed frame at `now`. The interval since the previous frame
    /// updates the smoothed rate. The first tick only sets the baseline; a zero
    /// or negative interval is ignored (no division by zero, no time going back).
    void tick(clock::time_point now);

    /// Smoothed frames per second — 0 until there have been two ticks.
    double fps() const { return fps_; }

    /// Forget all history (e.g. when the stream drops), so a later restart
    /// re-seeds instead of blending across the gap.
    void reset();

private:
    static constexpr double kAlpha = 0.2;  // EMA weight of the newest sample

    bool have_last_ = false;
    clock::time_point last_{};
    double fps_ = 0.0;
};

} // namespace denso::ui
