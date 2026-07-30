// One cell of the live grid: paints the latest frame for a camera STRETCHED TO
// FILL the whole tile (the CCTV/NVR full-bleed look — no letterbox bands, nothing
// cropped, aspect may distort slightly), with the camera name and a status dot
// overlaid. When connecting or offline it shows a placeholder instead of a frame.
// Purely a view — frames and status arrive via slots wired to a CameraStream; it
// owns no capture logic.
//
// The fill rect is the ONE coordinate contract every overlay shares: ROI polygons
// and the zone-runtime overlay map onto the same rect() the frame is drawn into,
// so they track the displayed image exactly.
#pragma once

#include "brazing/zone_runtime.h"  // ZoneRuntimeEntry (pure types, no policy)
#include "camera/camera.h"
#include "camera/fps_meter.h"

#include <QImage>
#include <QString>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class QPainter;
class QRectF;

namespace denso::ui {

/// The exact one-line text of a zone overlay row, e.g. `Z1   128 OK` or
/// `Z3    -- INHIBITED`. Pure and free-standing so the rendered CONTENT can be
/// asserted directly instead of being inferred from pixels. A number appears
/// only where the projection carries one, so every state that must not show a
/// reading renders `--` by construction rather than by a rule repeated here.
QString zone_row_text(const ZoneRuntimeEntry& z);

class CameraTile : public QWidget {
    Q_OBJECT

public:
    explicit CameraTile(const QString& name, QWidget* parent = nullptr);

    /// The camera's saved ROI polygons (normalized to the oriented frame),
    /// drawn as outlines over the live feed. Empty = no overlay.
    void set_areas(std::vector<camera::CameraArea> areas);

    /// The paired stream's backpressure counter; decremented as each frame is
    /// consumed. Optional — a tile without one just never decrements.
    void set_frame_counter(std::shared_ptr<std::atomic<int>> counter);

    /// Show a "Preparing model…" placeholder while this camera's detection model
    /// warms in the background. Cleared once the stream is attached (set_status).
    void set_preparing(bool on);

    /// Show a persistent inhibit banner over the live feed describing why this
    /// camera's zones are being suppressed. `causes` is a bitmask of
    /// health::ZoneCause; 0 clears the banner. Supersedes the old single-purpose
    /// review-paused banner (which only knew about one cause).
    void set_inhibited(uint32_t causes);

    /// This camera's zone runtime rows, already filtered and ordered by the grid.
    /// PURE RENDERING STATE: the tile never computes debounce, hold, expiry,
    /// inhibition or pause, never reads the aggregator and never talks to the
    /// backend — it draws exactly what it is handed.
    void set_zone_runtime_view(std::vector<ZoneRuntimeEntry> zones);

    /// Drop the overlay (camera removed, or it no longer has any zone).
    void clear_zone_runtime_view();

public slots:
    void set_frame(const QImage& frame);
    void set_status(int status);  // CameraStream::Status as int

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void draw_areas(QPainter& p, const QRectF& image_rect) const;
    void draw_zone_panel(QPainter& p) const;

    QString name_;
    QImage frame_;
    int status_ = 0;  // Connecting
    bool preparing_ = false;  // true = model still warming, no stream yet
    uint32_t causes_ = 0;     // health::ZoneCause bitmask; 0 = not inhibited
    std::vector<camera::CameraArea> areas_;
    std::vector<ZoneRuntimeEntry> zones_;  // overlay rows; empty = no panel
    FpsMeter meter_;  // real live fps from frame arrivals
    std::shared_ptr<std::atomic<int>> frame_counter_;  // null = no backpressure
};

} // namespace denso::ui
