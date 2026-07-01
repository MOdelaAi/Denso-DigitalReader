// Point-in-polygon tests over a camera's ROI areas. Used to confine detection
// to the drawn ROI: a detection is kept only when its (normalized) box center
// lands inside some area polygon. Pure, Qt/OpenCV-free math over `camera::Point`
// (normalized [0,1] coords), so it's unit-tested in the core suite and shared by
// the app-side DetectionProcessor.
#pragma once

#include "camera/camera.h"

#include <vector>

namespace denso::camera {

/// True if `p` lies inside the closed polygon `poly` (ray-casting, the implicit
/// closing edge from last vertex to first included). A polygon with fewer than
/// 3 vertices can't contain anything, so it returns false. Boundary points are
/// reported consistently but not guaranteed either way — callers use box
/// centers, which don't sit exactly on edges in practice.
bool point_in_polygon(const std::vector<Point>& poly, Point p);

/// True if `p` is inside any of `areas`' polygons. Empty `areas` → false (there
/// is no region to be inside). The "no areas means detect the whole frame" rule
/// lives in the caller, not here.
bool inside_any_area(const std::vector<CameraArea>& areas, Point p);

} // namespace denso::camera
