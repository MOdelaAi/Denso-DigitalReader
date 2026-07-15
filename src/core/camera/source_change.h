// Pure predicates that decide whether editing a camera's source/geometry should
// quarantine its ROI areas for re-verification (see Camera::areas_need_review).
// Qt-free and unit-tested; the wizard controller calls requires_area_review().
#pragma once

#include "camera/camera.h"

namespace denso::camera {

/// True if `a` and `b` resolve to the SAME connection + view source — i.e. only
/// non-view fields (name, username, password) differ. `camera_type` and the
/// per-type view-determining fields must match: USB → index; IP → ip / rtsp /
/// manufacturer / channel / stream.
bool same_effective_source(const Camera& a, const Camera& b);

/// True if the capture geometry the ROI polygons depend on changed: rotation,
/// pitch, roll, or the aspect ratio. A pure resolution change at the same aspect
/// ratio does not move a normalized polygon, so it is not a geometry change.
bool view_geometry_changed(const Camera& a, const Camera& b);

/// True if saving `after` over `before` must quarantine the camera's ROI areas
/// for re-verification — either the effective source or the view geometry
/// changed. Credential/name-only edits return false.
bool requires_area_review(const Camera& before, const Camera& after);

} // namespace denso::camera
