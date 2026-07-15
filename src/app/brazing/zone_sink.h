// The zone-reporting CONTRACT between the camera pipeline and the brazing
// reporter. A DetectionProcessor (producer) publishes each camera's assembled
// zone readings to a ZoneSink (the ZoneReporter/brazing side, consumer). Kept in
// its own header — not in frame_processor.h — so the reporting side depends only
// on this contract, never on the capture/frame-processing code. Pairs with
// zone_reading.h. (Both move to app/brazing/ when the reporting subsystem does.)
#pragma once

#include "brazing/zone_reading.h"  // ZoneReading

#include <cstdint>
#include <vector>

namespace denso::ui {

/// Optional per-frame hook that receives a camera's assembled zone values.
/// CONTRACT: on_zones() is invoked on the INFERENCE WORKER THREAD — an
/// implementation must NOT block or do I/O inline; hand off to a worker (queue /
/// common::post_to_gui) and return immediately.
struct ZoneSink {
    virtual ~ZoneSink() = default;
    virtual void on_zones(int64_t camera_id,
                          const std::vector<ZoneReading>& zones) = 0;
};

} // namespace denso::ui
