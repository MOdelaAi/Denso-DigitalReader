// One assembled zone value: which reporting zone, its current number, and the
// aggregate confidence. Shared by BOTH modes' assembly steps and the debounce
// aggregator — a tiny header so the aggregator stays free of OpenCV.
#pragma once

namespace denso::ui {

/// How much of a zone's value the detector actually produced this frame.
/// `Complete` is the ONLY kind carrying a usable `value` — see spec §5.1.
///
/// The names are mode-NEUTRAL because both zone producers emit these. For the
/// digit reader: Complete is a plausible contiguous number, Incomplete is an
/// obvious internal missing position, NoValue is no digits at all. For the Ball
/// Leveler: Complete is a ball selected inside the zone's rectangle and mapped
/// to a percentage, NoValue is no qualifying ball this frame. Ball never emits
/// Incomplete — a percentage is whole or absent, there is no partial one.
///
/// The aggregator treats every non-Complete kind identically (they break the
/// stable run and carry no value); the distinction exists for diagnostics.
enum class ReadingKind { Complete, Incomplete, NoValue };

/// One zone's reading, as handed to ZoneSink::on_zones().
///
/// `value` is an INT for both modes. The digit reader's assembled number is
/// naturally integral; the Ball Leveler's percentage is computed as a double and
/// quantized to the nearest whole percent at THIS seam, because the whole
/// delivery stack below — the aggregator snapshot and build_brazing_payload — is
/// integer-valued and the backend contract is `{"zoneN": <int>}` (amendment
/// §10.3). Quantizing here rather than deeper means the value that is debounced
/// is exactly the value that is sent.
struct ZoneReading {
    int         zone_no = 0;
    int         value   = 0;   // meaningful ONLY when kind == Complete
    float       conf    = 0.0f;
    ReadingKind kind    = ReadingKind::Complete;
};

} // namespace denso::ui
