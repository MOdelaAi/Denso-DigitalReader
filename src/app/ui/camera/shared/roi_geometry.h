// Pure geometry for the ROI canvas: where the (aspect-preserved) frame lands
// inside the widget, and the mapping between widget pixels and normalized
// [0,1] polygon coordinates. Free functions over QtGui/QtCore value types only
// (no QWidget), so they're unit-tested off-screen alongside the rest of core.
#pragma once

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

} // namespace denso::ui
