// Pure flow-control helpers for the capture loop: the reconnect backoff schedule
// and the display-queue backpressure gate. No Qt/OpenCV — just int policy, so it
// unit-tests without a camera or the GUI. Used by CameraStream.
#pragma once

namespace denso::ui {

/// Next reconnect delay given the previous one. 0 or negative (the first
/// attempt) returns 1000 ms; otherwise doubles, capped at 10000 ms.
int next_backoff_ms(int current_ms);

/// Whether the capture thread should emit another frame, or drop it: true iff
/// fewer than `max_in_flight` frames are already queued for the GUI (drop-oldest
/// backpressure).
bool should_emit(int in_flight, int max_in_flight);

} // namespace denso::ui
