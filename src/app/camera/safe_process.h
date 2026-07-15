// Exception firewall for the capture thread. process() runs OpenCV pre/post work
// and model decode outside any try block, in a raw std::thread body — a single
// malformed frame throwing there would escape the thread function and call
// std::terminate(), killing the whole process. safe_process() swallows it: log,
// return the frame unprocessed, keep the capture loop alive.
#pragma once

#include "camera/frame_processor.h"

#include <QImage>

#include <exception>

namespace denso::ui {

inline QImage safe_process(FrameProcessor* p, const QImage& frame) noexcept {
    if (!p) {
        return frame;
    }
    try {
        return p->process(frame);
    } catch (const std::exception&) {
        return frame;  // caller logs (throttled); never let it escape the thread
    } catch (...) {
        return frame;
    }
}

} // namespace denso::ui
