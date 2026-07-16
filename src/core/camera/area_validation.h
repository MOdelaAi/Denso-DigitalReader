// Validation for a camera's ROI area set, checked BEFORE it reaches the repo.
// `replace_areas` already refuses a bad save transactionally, but it can only
// answer yes/no — the operator then sees a generic "failed to save". These pure
// predicates let the UI catch the same problems up front and name them ("Zone 4
// is already used by Line 2 Cam"), so a deterministic validation error never
// masquerades as a storage fault. Qt/OpenCV-free, unit-tested in the core suite.
#pragma once

#include "camera/camera.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace denso::camera {

/// Absolute area enclosed by `poly`, in normalized frame units (1.0 = the whole
/// frame). Shoelace formula over the closed polygon; winding-independent.
/// Fewer than 3 vertices enclose nothing, so it returns 0.
double polygon_area(const std::vector<Point>& poly);

/// Smallest area (normalized, so 1e-4 = 0.01% of the frame) that still counts
/// as a real ROI. Anything under this is a sliver the operator can't have meant.
inline constexpr double kMinPolygonArea = 1e-4;

/// True when two non-adjacent edges of the closed polygon cross. Neighbouring
/// edges share a vertex by construction and don't count. Concave shapes are
/// fine — only crossings are the problem: point_in_polygon fills by even-odd,
/// so a bow-tie's lobes read as HOLES, and the ROI silently stops detecting
/// where the operator can plainly see coverage. Dragging one corner across the
/// shape produces this in a single gesture. Fewer than 4 vertices can't cross.
bool polygon_self_intersects(const std::vector<Point>& poly);

/// True when `poly` can't work as an ROI: fewer than 3 vertices, an enclosed
/// area under `kMinPolygonArea` (which covers collinear runs, duplicated
/// vertices, and the slivers that clicks pinned to the image edge produce), or
/// edges that cross.
bool polygon_is_degenerate(const std::vector<Point>& poly);

/// A zone number claimed twice. `owner` is the name of the OTHER camera holding
/// it, or empty when the clash is between two areas of the camera being edited.
struct ZoneConflict {
    int zone = 0;
    std::string area_name;  // the area that lost the race, in area order
    std::string owner;      // "" ⇒ duplicated within this same camera
};

/// The first zone clash in `areas`, or nullopt when the set is clean. Mirrors
/// the machine-wide uniqueness rule `replace_areas` enforces: a zone may be
/// claimed by exactly one area across all cameras, because the brazing payload
/// keys by zone number. `zones_owned_elsewhere` maps zone → owning camera name
/// and must EXCLUDE the camera being edited (its rows are replaced wholesale).
/// Areas with no zone are ROI-only and never conflict.
std::optional<ZoneConflict> find_zone_conflict(
    const std::vector<CameraArea>& areas,
    const std::map<int, std::string>& zones_owned_elsewhere);

/// True when two area sets are the same in every operator-visible respect:
/// order, name, zone, and vertices. DB-assigned `id`/`camera_id` are ignored —
/// a freshly redrawn area carries no id, and that must not read as a change.
/// Used to decide whether leaving the page would silently discard work.
bool areas_equal(const std::vector<CameraArea>& a,
                 const std::vector<CameraArea>& b);

} // namespace denso::camera
