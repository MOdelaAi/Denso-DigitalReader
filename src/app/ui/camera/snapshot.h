// Grab a single frame from a camera for the Configure preview, and rotate a
// frame for display. OpenCV-free on purpose — callers (the dialog) only see
// QImage. Capture is blocking; callers run grab_snapshot off the GUI thread.
#pragma once

#include <QImage>
#include <QString>

#include <optional>

namespace denso::ui {

struct Snapshot {
    QImage  image;   // null on failure
    QString error;   // human-readable reason when image is null
};

/// Open the USB `index` OR the RTSP `url` (exactly one set), apply width/height
/// (when both > 0) + a finite open/read timeout, and grab one frame.
Snapshot grab_snapshot(std::optional<int> index, const QString& url,
                       int width, int height);

/// Rotate a frame for preview. degrees ∈ {0, 90, 180, 270}; multiples of 360
/// are identity.
QImage apply_rotation(const QImage& src, int degrees);

} // namespace denso::ui
