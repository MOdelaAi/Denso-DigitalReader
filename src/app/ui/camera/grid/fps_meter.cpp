#include "ui/camera/grid/fps_meter.h"

namespace denso::ui {

void FpsMeter::tick(clock::time_point now) {
    if (!have_last_) {
        have_last_ = true;
        last_ = now;
        return;  // baseline only — need an interval before there's a rate
    }
    const double dt = std::chrono::duration<double>(now - last_).count();
    last_ = now;
    if (dt <= 0.0) {
        return;  // ignore a zero/negative interval
    }
    const double inst = 1.0 / dt;
    // Seed on the first real sample, then exponentially smooth.
    fps_ = (fps_ <= 0.0) ? inst : (kAlpha * inst + (1.0 - kAlpha) * fps_);
}

void FpsMeter::reset() {
    have_last_ = false;
    fps_ = 0.0;
}

} // namespace denso::ui
