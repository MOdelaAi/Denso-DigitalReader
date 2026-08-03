// TEST-ONLY comparison shim for the zone pipeline's fixed-point value type.
//
// ZoneValue replaced a bare `int` throughout the reporting pipeline when the
// digit reader gained per-zone decimal formats. The overwhelming majority of the
// existing tests assert WHOLE numbers on zones that carry no decimal format, and
// rewriting several hundred assertions to construct a struct would have churned
// far more coverage than it verified — for no change in what is being tested.
//
// The shim is deliberately STRICT rather than convenient: it requires
// decimal_places == 0, so a value that unexpectedly acquired a format fails the
// comparison instead of silently comparing equal on its raw digits. It ignores
// display_digits alone, which is a rendering width and never part of a numeric
// claim. Tests that care about a FORMAT assert on zone_value_json /
// zone_value_display, or construct a ZoneValue explicitly.
//
// It lives in tests/ and is never compiled into the application: production code
// must always say which format it means.
#pragma once

#include "brazing/zone_reading.h"

namespace denso::ui {

inline bool operator==(const ZoneValue& v, int raw) {
    return v.raw == raw && v.decimal_places == 0;
}
inline bool operator==(int raw, const ZoneValue& v) { return v == raw; }
inline bool operator!=(const ZoneValue& v, int raw) { return !(v == raw); }
inline bool operator!=(int raw, const ZoneValue& v) { return !(v == raw); }

} // namespace denso::ui
