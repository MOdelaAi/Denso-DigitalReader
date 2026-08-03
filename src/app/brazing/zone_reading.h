// One assembled zone value: which reporting zone, its current number, and the
// aggregate confidence. Shared by BOTH modes' assembly steps and the debounce
// aggregator — a tiny header so the aggregator stays free of OpenCV.
#pragma once

#include <string>

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

/// The numeric value of one zone reading, carried UNCHANGED from assembly all
/// the way to the backend payload.
///
/// It is a FIXED-POINT number, not a float: `raw` is the accepted reading as an
/// integer and `decimal_places` says how far from the right the point sits, so
/// raw 1234 with dp 2 is 12.34. Two independent reasons it is not a double:
///
///   - the serializer has to be told the decimal count anyway. Printing a double
///     that "is" 12.34 can yield 12.339999999999999, so the count must travel
///     with the value regardless — and once it does, the double adds nothing but
///     a rounding step; and
///   - ZoneAggregator debounces by comparing successive values for EQUALITY, and
///     BrazingRetryPolicy compares whole snapshots to decide whether the server
///     is behind. Integers compare exactly. That exactness is the property the
///     whole debounce and single-flight machinery is built on.
///
/// `display_digits` is a DISPLAY fact and never reaches the payload. The digit
/// reader shows a fixed four-position instrument face, so raw 12 with dp 2 must
/// be annotated "00.12" — the leading zeros are recoverable from the fixed width
/// alone, which is why no separate digit TEXT is stored anywhere. The Ball
/// Leveler leaves it 0 and renders a plain integer percent, exactly as before.
struct ZoneValue {
    int raw            = 0;
    int decimal_places = 0;  // 0..3
    int display_digits = 0;  // 0 = plain integer; 4 = fixed digital face

    friend bool operator==(const ZoneValue& a, const ZoneValue& b) {
        return a.raw == b.raw && a.decimal_places == b.decimal_places &&
               a.display_digits == b.display_digits;
    }
    friend bool operator!=(const ZoneValue& a, const ZoneValue& b) {
        return !(a == b);
    }
};

/// The value as a JSON NUMBER: `raw` with the point inserted `decimal_places`
/// from the right. dp 0 renders the bare integer, so a Ball Leveler percent
/// serializes byte-identically to how it did before decimals existed — which is
/// what keeps the existing backend contract intact.
std::string zone_value_json(const ZoneValue& v);

/// The value as the operator sees it burned into the frame: zero-padded to
/// `display_digits` positions BEFORE the point is inserted, so a four-position
/// face keeps its leading zeros. `display_digits` 0 means "no fixed face" and
/// renders exactly what zone_value_json does.
std::string zone_value_display(const ZoneValue& v);

/// One zone's reading, as handed to ZoneSink::on_zones().
struct ZoneReading {
    int         zone_no = 0;
    ZoneValue   value;         // meaningful ONLY when kind == Complete
    float       conf    = 0.0f;
    ReadingKind kind    = ReadingKind::Complete;
};

} // namespace denso::ui
