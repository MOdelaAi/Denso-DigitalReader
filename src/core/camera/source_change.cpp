#include "camera/source_change.h"

#include <cstdint>

namespace denso::camera {
namespace {

/// Compare width/height ratios via cross-multiplication (no float division, no
/// divide-by-zero). Unset (0) dimensions carry no aspect info → not a change.
bool aspect_changed(const Camera& a, const Camera& b) {
    if (a.width == 0 || a.height == 0 || b.width == 0 || b.height == 0) {
        return false;
    }
    return static_cast<uint64_t>(a.width) * b.height !=
           static_cast<uint64_t>(b.width) * a.height;
}

} // namespace

bool same_effective_source(const Camera& a, const Camera& b) {
    if (a.camera_type != b.camera_type) {
        return false;
    }
    if (a.camera_type == "usb") {
        return a.index == b.index;
    }
    // IP (or any non-USB): the fields that determine which stream/view is opened.
    // Credentials (username/password) are excluded — they don't change the view.
    return a.ip == b.ip && a.rtsp == b.rtsp && a.manufacturer == b.manufacturer &&
           a.channel == b.channel && a.stream == b.stream;
}

bool view_geometry_changed(const Camera& a, const Camera& b) {
    return a.rotation != b.rotation || a.pitch != b.pitch || a.roll != b.roll ||
           aspect_changed(a, b);
}

bool requires_area_review(const Camera& before, const Camera& after) {
    return !same_effective_source(before, after) ||
           view_geometry_changed(before, after);
}

} // namespace denso::camera
