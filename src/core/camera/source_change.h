// Pure predicates that decide whether editing a camera's source/geometry should
// quarantine its ROI areas for re-verification (see Camera::areas_need_review).
// Qt-free and unit-tested; the wizard controller calls requires_area_review().
#pragma once

#include "camera/camera.h"

#include <string>

namespace denso::camera {

/// True if the width/height RATIO differs (cross-multiplied — no float division,
/// no divide-by-zero). Unknown (0×0, e.g. a legacy camera) ↔ known counts as
/// changed, because Configure resolves 0×0 into a preset; both-unknown does not.
/// Exposed because the wizard needs aspect ALONE: rotation/pitch/roll are
/// re-applied to the retained raw frame, so they don't stale a captured preview,
/// but an aspect change makes that frame the wrong SHAPE to verify ROIs against.
bool aspect_changed(const Camera& a, const Camera& b);

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

/// An opaque fingerprint of exactly the fields the predicates above call
/// view-significant: two cameras share a revision precisely when
/// `requires_area_review` between them is false. That agreement IS the contract —
/// the digest itself is an implementation detail and nothing may parse it.
///
/// It exists because Ball Leveler calibration geometry is expressed in
/// oriented-frame coordinates (`ball_level_calibration.view_revision`), so a
/// change to the source, rotation, pitch, roll or aspect makes a stored
/// calibration refer to a DIFFERENT physical view. Storing the fingerprint lets
/// that be detected later without re-deriving "what makes a view different" in a
/// second place — the same reason the ROI quarantine reuses these predicates.
///
/// Hashed rather than composed in the clear, deliberately: `rtsp` is an
/// operator-editable column and an operator may paste a credential-bearing URL
/// into it. A fixed-width digest cannot carry one into the database, a backup or
/// a diagnostic. Credentials are excluded from the input regardless (they do not
/// change the view), which is belt AND braces.
///
/// Stable across processes and runs: composed only from persisted field values,
/// never from a clock, a pointer or an iteration order.
std::string view_revision(const Camera& c);

} // namespace denso::camera
