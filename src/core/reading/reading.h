// A single recorded detection reading — one row in the `reading` table. Written
// by the (future) reading sink on the app side, read back by the logging/export
// UI. Qt/OpenCV-free, like the other core domain structs.
#pragma once

#include <cstdint>
#include <string>

namespace denso::reading {

/// One reading captured from a camera at a point in time. `value` is the
/// assembled reading text (assembly logic lives in the app-side sink); `conf`
/// is its aggregate confidence in [0,1]. Row in `reading`.
struct Reading {
    int64_t     id        = 0;
    int64_t     camera_id = 0;  // FK → camera.id
    int64_t     ts_ms     = 0;  // capture time, epoch milliseconds
    std::string value;          // the reading text
    float       conf      = 0.0f;
};

} // namespace denso::reading
