// One assembled zone value: which reporting zone, its current number, and the
// aggregate confidence (min digit conf). Shared by the assembly step and the
// debounce aggregator — a tiny header so the aggregator stays free of OpenCV.
#pragma once

namespace denso::ui {

struct ZoneReading {
    int   zone_no = 0;
    int   value   = 0;
    float conf    = 0.0f;
};

} // namespace denso::ui
