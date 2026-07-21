// One assembled zone value: which reporting zone, its current number, and the
// aggregate confidence (min digit conf). Shared by the assembly step and the
// debounce aggregator — a tiny header so the aggregator stays free of OpenCV.
#pragma once

namespace denso::ui {

/// How much of a zone's number the detector actually produced this frame.
/// `Complete` is the ONLY kind carrying a usable `value` — see spec §5.1.
enum class ReadingKind { Complete, Incomplete, NoDigits };

struct ZoneReading {
    int         zone_no = 0;
    int         value   = 0;   // meaningful ONLY when kind == Complete
    float       conf    = 0.0f;
    ReadingKind kind    = ReadingKind::Complete;
};

} // namespace denso::ui
