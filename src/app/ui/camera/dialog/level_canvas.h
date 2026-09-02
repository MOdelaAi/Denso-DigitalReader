// The Ball Leveler calibration canvas: ONE axis-aligned measurement rectangle
// and TWO horizontal reference lines over a camera snapshot.
//
// Two modes, mirroring the ROI canvas it is modelled on. Drawing rubber-bands a
// new rectangle; Editing drags either reference line. The page owns the mode —
// the canvas never changes it by itself, so "the operator pressed Redraw" and
// "the page reloaded a saved calibration" are the only two ways the shape of the
// interaction can change.
//
// The canvas owns NO rule and NO data. It emits the requested geometry in
// normalized coordinates and paints back whatever the page gives it; every
// constraint (lines inside the rectangle, 100% above 0%, the minimum span) lives
// in level::CalibrationDraft. That separation is deliberate: a constraint
// enforced in a painter is a constraint the keyboard path, the reload path and
// the write path do not have.
//
// Coordinates are normalized (0..1) against the FITTED image rect, so a
// calibration survives a resolution or window change. The widget<->normalized
// mapping and the aspect-fit rect come from roi_geometry (pure, unit-tested) —
// the same primitives the ROI canvas uses. A press outside the drawn frame is
// REJECTED rather than clamped onto its edge; a drag already in progress clamps,
// because by then the operator is holding the handle.
#pragma once

#include "level/calibration.h"

#include <QImage>
#include <QPointF>
#include <QWidget>

#include <vector>

class QString;

namespace denso::ui {

class LevelCanvas : public QWidget {
    Q_OBJECT

public:
    enum class Mode {
        Drawing,  ///< rubber-banding a new measurement rectangle
        Editing,  ///< dragging the 100% / 0% reference lines
    };

    explicit LevelCanvas(QWidget* parent = nullptr);

    void set_frame(const QImage& oriented);  // background; repaints
    bool has_frame() const { return !frame_.isNull(); }

    /// The values to PAINT. `has_rect` false means no rectangle has been drawn
    /// yet, so neither it nor the reference lines are drawn.
    void set_calibration(const denso::level::LevelCalibration& c, bool has_rect);

    /// The camera's OTHER level zones, drawn ghosted behind the selected one so
    /// the operator can see overlap and relative placement while editing — the
    /// same job RoiCanvas::set_context_areas does on the digit Areas page, and
    /// deliberately the same visual language.
    ///
    /// Context zones are painted only. They are never hit-tested: line_at() and
    /// every mouse handler read `c_` alone, so "not interactive" is structural
    /// here rather than a flag someone can forget to check.
    void set_context_zones(const std::vector<denso::level::LevelZone>& others);

    /// What is currently ghosted. A test seam: asserting on the render MODEL is
    /// stable, where asserting on pixels would break on any styling change.
    const std::vector<denso::level::LevelZone>& context_zones() const {
        return context_;
    }

    void begin_draw();  // → Drawing (the operator asked to (re)draw)
    void begin_edit();  // → Editing (a rectangle exists)

    Mode mode() const { return mode_; }

signals:
    /// A rectangle was rubber-banded out, in normalized coordinates. The page
    /// forwards it to the draft, which owns clamping and line re-seating.
    void rect_drawn(double x, double y, double w, double h);
    /// A reference line was dragged. Two signals rather than one with a flag, so a
    /// mis-wired connection is a compile error rather than a swapped line.
    void y_100_moved(double y);
    void y_0_moved(double y);
    /// A rejected interaction worth explaining (a press off the frame, a
    /// rectangle with no area). The page shows it.
    void rejected(const QString& why);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    QRectF image_rect() const;  // where the frame is drawn inside the widget
    /// Which reference line is within grab range of `widget_pt`, or 0 for none.
    /// The NEARER line wins so a drag grabs the handle under the cursor rather
    /// than whichever was tested first.
    int line_at(const QPointF& widget_pt) const;
    void draw_calibration(QPainter& p, const QRectF& img) const;
    void draw_context(QPainter& p, const QRectF& img) const;
    void draw_rubber_band(QPainter& p, const QRectF& img) const;

    QImage frame_;
    denso::level::LevelCalibration c_;
    std::vector<denso::level::LevelZone> context_;  // the camera's other zones
    bool has_rect_ = false;
    Mode mode_ = Mode::Drawing;

    bool banding_ = false;   // a rectangle drag is in progress
    QPointF band_from_;      // widget space
    QPointF band_to_;
    int grabbed_ = 0;        // 0 none, 1 the 100% line, 2 the 0% line
};

} // namespace denso::ui
