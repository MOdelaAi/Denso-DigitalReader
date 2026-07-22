// The appliance operating mode: which processing pipeline the box runs. Persisted
// under the `settings` key `mode.target` (see mode/config.{h,cpp}). Pure — no Qt,
// no widgets — so it lives in denso_core and is fully unit-testable.
//
// digit_reader is the DEFAULT and the migration target for every existing
// installation: an absent, empty, unknown or corrupt token ALWAYS resolves to
// DigitReader, never the newer BallLeveler. The Floating Ball algorithm is not
// implemented in this release.
#pragma once

#include <string>

namespace denso::mode {

enum class TargetMode { DigitReader, BallLeveler };

inline const char* to_string(TargetMode m) {
    switch (m) {
        case TargetMode::DigitReader: return "digit_reader";
        case TargetMode::BallLeveler: return "ball_leveler";
    }
    return "digit_reader";
}

// Any unknown/empty/corrupt token resolves to DigitReader — never the newer mode.
inline TargetMode parse_target_mode(const std::string& s) {
    if (s == "ball_leveler") return TargetMode::BallLeveler;
    return TargetMode::DigitReader;
}

// Validate a UI selector index / any raw int to a real enumerator. An
// out-of-range value is NEVER stored as an invalid enum (which would make
// to_string fall back while current_mode_ held garbage — a DB/UI mismatch).
inline TargetMode from_index(int i) {
    return i == static_cast<int>(TargetMode::BallLeveler) ? TargetMode::BallLeveler
                                                          : TargetMode::DigitReader;
}

} // namespace denso::mode
