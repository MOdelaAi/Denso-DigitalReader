// Pure geometry for the ROI canvas: where the (aspect-preserved) frame lands
// inside the widget, and the mapping between widget pixels and normalized
// [0,1] polygon coordinates. Free functions over QtGui/QtCore value types only
// (no QWidget), so they're unit-tested off-screen alongside the rest of core.
#pragma once

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSizeF>

namespace denso::ui {

/// The rectangle the `image` occupies when scaled to fit `widget` with aspect
/// ratio preserved and centered (letter-/pillar-boxed). Empty if either size
/// is degenerate.
QRectF fitted_image_rect(const QSizeF& image, const QSizeF& widget);

/// Map a widget-space point to normalized [0,1] coordinates within `image_rect`
/// (the fitted rect above), clamped to the unit square.
QPointF to_normalized(const QPointF& widget_pt, const QRectF& image_rect);

/// Map a normalized [0,1] point back to widget space within `image_rect`.
QPointF to_widget(const QPointF& norm, const QRectF& image_rect);

/// True when `widget_pt` lands on the drawn frame (within `tolerance_px` of the
/// fitted rect), rather than in the letter-/pillar-box bars beside it. The
/// clamp in `to_normalized` is a safety net, not a gate — without this check a
/// click in the bars silently becomes a vertex pinned to the image edge.
bool image_contains(const QRectF& image_rect, const QPointF& widget_pt,
                    double tolerance_px);

/// Index of the vertex within `radius_px` of `probe`, or -1 if none is in
/// range. The NEAREST vertex wins when several qualify, so a drag grabs the
/// handle under the cursor rather than whichever was declared first. A
/// non-positive radius hits nothing.
int hit_test_vertex(const QList<QPointF>& widget_pts, const QPointF& probe,
                    double radius_px);

/// Index at which to INSERT a new vertex so it splits the polygon edge nearest
/// `probe` (one past that edge's start vertex, preserving winding), or -1 when
/// no edge is within `max_dist_px`. Distance is measured to the SEGMENT, not
/// its infinite line, and the implicit closing edge is included — it's drawn,
/// so it must be splittable. Fewer than 3 vertices have no edge to split, and a
/// non-positive distance matches nothing.
int nearest_edge_insert_index(const QList<QPointF>& widget_pts,
                              const QPointF& probe, double max_dist_px);

} // namespace denso::ui
