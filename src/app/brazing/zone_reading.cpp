#include "brazing/zone_reading.h"

#include <algorithm>
#include <cstdlib>

namespace denso::ui {

namespace {

// Render a fixed-point value as text. `min_digits` is the minimum number of
// DIGIT positions before the point is inserted — the fixed instrument face for
// the annotation, and 0 for the payload, where a leading zero would be noise.
//
// The decimal count is clamped rather than asserted: this is the last stop
// before a value is shown to an operator or sent to a backend, and a corrupt dp
// (a hand-edited database row, say) must degrade to a plainer rendering of the
// SAME number rather than produce malformed JSON or read out of bounds.
std::string render(int raw, int decimal_places, int min_digits) {
    const int dp = std::clamp(decimal_places, 0, 9);
    const bool negative = raw < 0;
    // Via unsigned, so INT_MIN does not overflow on negation. No reading is ever
    // negative today, but the formatter is the wrong place to assume that.
    unsigned long long mag =
        negative ? -static_cast<unsigned long long>(raw)
                 : static_cast<unsigned long long>(raw);
    std::string digits = std::to_string(mag);

    // At least one digit must survive to the LEFT of the point, so the width
    // floor is dp + 1 even when the caller asked for fewer positions.
    const std::size_t width =
        std::max(static_cast<std::size_t>(std::max(min_digits, 0)),
                 static_cast<std::size_t>(dp) + 1);
    if (digits.size() < width) {
        digits.insert(0, width - digits.size(), '0');
    }
    if (dp > 0) {
        digits.insert(digits.size() - static_cast<std::size_t>(dp), 1, '.');
    }
    if (negative) {
        digits.insert(0, 1, '-');
    }
    return digits;
}

} // namespace

std::string zone_value_json(const ZoneValue& v) {
    // min_digits 0: the payload carries the NUMBER, so 0.12 is right and "00.12"
    // would not even be legal JSON. The fixed face is an annotation concern.
    return render(v.raw, v.decimal_places, 0);
}

std::string zone_value_display(const ZoneValue& v) {
    return render(v.raw, v.decimal_places, v.display_digits);
}

} // namespace denso::ui
