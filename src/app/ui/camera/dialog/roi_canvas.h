// The ROI editing canvas: one editable polygon over a camera snapshot, with the
// camera's other areas drawn behind it for context.
//
// Three modes. Idle shows the areas but takes no edits. Drawing builds a new
// polygon — click to drop a vertex, click the first vertex (or Enter, or the
// page's Done button) to close it. Editing works an existing closed polygon —
// drag a vertex to move it, click an edge to insert one, right-click a vertex
// to remove it. Points stay normalized (0..1) so an area survives a resolution
// change; the widget↔normalized mapping and all hit-testing live in roi_geometry
// (pure, unit-tested). Clicks outside the drawn frame are rejected rather than
// clamped onto its edge. The canvas owns no data policy — the page loads and
// persists areas around it.
#pragma once

#include "camera/camera.h"

#include <QImage>
#include <QWidget>

#include <vector>

namespace denso::ui {

class RoiCanvas : public QWidget {
    Q_OBJECT

public:
    enum class Mode {
        Idle,     // showing areas; no active polygon to edit
        Drawing,  // building a new polygon, not yet closed
        Editing,  // reshaping an existing closed polygon
    };

    explicit RoiCanvas(QWidget* parent = nullptr);

    void set_frame(const QImage& oriented);  // background; repaints
    bool has_frame() const { return !frame_.isNull(); }

    /// The camera's other areas, drawn dimmed behind the active polygon so the
    /// operator can see overall coverage and spot overlaps while editing.
    void set_context_areas(const std::vector<camera::CameraArea>& others);

    void begin_draw();                                     // → Drawing, empty
    void edit_polygon(const std::vector<camera::Point>&);  // → Editing
    void go_idle();                                        // → Idle, no active polygon

    // Actions the page exposes as on-screen buttons (keyboard and right-click
    // are accelerators, never the only way in — the panel may have no keyboard,
    // and a touchscreen has no right button).
    void undo_point();              // Drawing: drop the last vertex
    void close_polygon();           // Drawing: close it if 3+ vertices
    void clear();                   // wipe the active polygon
    void remove_selected_vertex();  // Editing: drop the tapped corner

    Mode mode() const { return mode_; }
    const std::vector<camera::Point>& polygon() const { return points_; }
    int point_count() const { return static_cast<int>(points_.size()); }
    bool is_closed() const { return closed_; }
    bool is_valid() const { return points_.size() >= 3; }
    /// Index of the corner the operator last tapped while Editing, or -1. This
    /// is what makes removal reachable without a right button.
    int selected_vertex() const { return selected_vertex_; }
    bool can_remove_selected_vertex() const {
        return mode_ == Mode::Editing && selected_vertex_ >= 0 &&
               points_.size() > 3;
    }
    /// True while a polygon is part-drawn — the state that used to vanish
    /// silently when the operator pressed Finish.
    bool has_unfinished_draw() const {
        return mode_ == Mode::Drawing && !points_.empty();
    }

signals:
    void changed();  // any edit: vertex added/moved/removed/cleared
    void closed();   // a Drawing polygon was just closed (3+ vertices)
    /// A vertex of the Editing polygon was moved/added/removed. Carries the
    /// live polygon so the page can write it straight back to its working set.
    void polygon_edited(const std::vector<camera::Point>& pts);
    /// A rejected interaction worth explaining (e.g. a click off the frame, or
    /// a delete that would leave fewer than 3 vertices). The page shows it.
    void rejected(const QString& why);
    /// The tapped corner changed (including to none) — the page uses this to
    /// enable its "Remove corner" button.
    void vertex_selection_changed();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    QRectF image_rect() const;  // where the frame is drawn inside the widget
    QList<QPointF> widget_points() const;  // active polygon in widget space
    void try_close();
    void reset_interaction();   // drop drag/hover/selection on a mode change
    void select_vertex(int index);  // -1 clears; emits only on a real change
    void draw_context(QPainter& p, const QRectF& img) const;
    void draw_active(QPainter& p, const QRectF& img) const;

    QImage frame_;
    std::vector<camera::Point> points_;  // active polygon, normalized 0..1
    std::vector<camera::CameraArea> context_;  // the camera's other areas
    Mode mode_ = Mode::Idle;
    bool closed_ = false;
    int drag_vertex_ = -1;      // index being dragged, or -1
    int hover_vertex_ = -1;     // index under the cursor, or -1 (drawn larger)
    int selected_vertex_ = -1;  // index last tapped while Editing, or -1
};

} // namespace denso::ui
