#include "level/runtime.h"

#include <utility>

namespace denso::level {

LevelRuntimeEntry LevelRuntimeEntry::healthy(int64_t camera_id, double percent,
                                             int64_t ts_ms) {
    LevelRuntimeEntry e;
    e.camera_id = camera_id;
    e.state = LevelState::Healthy;
    e.percent = percent;
    e.ts_ms = ts_ms;
    return e;
}

LevelRuntimeEntry LevelRuntimeEntry::acquiring(int64_t camera_id, int64_t ts_ms) {
    LevelRuntimeEntry e;
    e.camera_id = camera_id;
    e.state = LevelState::Acquiring;
    e.ts_ms = ts_ms;
    return e;
}

LevelRuntimeEntry LevelRuntimeEntry::unavailable(int64_t camera_id,
                                                 std::string reason,
                                                 int64_t ts_ms) {
    LevelRuntimeEntry e;
    e.camera_id = camera_id;
    e.state = LevelState::Unavailable;
    e.reason = std::move(reason);
    e.ts_ms = ts_ms;
    return e;
}

LevelRuntimeEntry LevelRuntimeEntry::unconfigured(int64_t camera_id, int64_t ts_ms) {
    LevelRuntimeEntry e;
    e.camera_id = camera_id;
    e.state = LevelState::Unconfigured;
    e.ts_ms = ts_ms;
    return e;
}

LevelRuntimeEntry LevelRuntimeEntry::calibration_invalid(int64_t camera_id,
                                                         int64_t ts_ms) {
    LevelRuntimeEntry e;
    e.camera_id = camera_id;
    e.state = LevelState::CalibrationInvalid;
    e.ts_ms = ts_ms;
    return e;
}

const char* level_state_label(LevelState s) {
    switch (s) {
        case LevelState::Unconfigured:       return "UNCONFIGURED";
        case LevelState::Acquiring:          return "ACQUIRING";
        case LevelState::Healthy:            return "HEALTHY";
        case LevelState::Unavailable:        return "UNAVAILABLE";
        case LevelState::CalibrationInvalid: return "CALIBRATION INVALID";
    }
    return "UNAVAILABLE";  // fail-closed: never claim health for an unknown state
}

}  // namespace denso::level
