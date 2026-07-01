// Serialize a polygon's normalized vertices to / from the single TEXT column
// `camera_area.points`. Format: "x,y;x,y;..." with locale-independent decimal
// points. The vertex count is implicit in the number of pairs, so triangles,
// rectangles and arbitrary N-gons all share one column and code path. Parsing
// is tolerant: blank input yields no vertices and malformed pairs are skipped
// rather than throwing, so a hand-edited or partial DB value can't crash a read.
#pragma once

#include "camera/camera.h"

#include <string>
#include <vector>

namespace denso::camera {

std::string serialize_points(const std::vector<Point>& points);
std::vector<Point> parse_points(const std::string& text);

} // namespace denso::camera
