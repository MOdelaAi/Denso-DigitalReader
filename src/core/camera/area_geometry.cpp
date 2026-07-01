#include "camera/area_geometry.h"

namespace denso::camera {

bool point_in_polygon(const std::vector<Point>& poly, Point p) {
    const size_t n = poly.size();
    if (n < 3) return false;
    // Standard even-odd ray casting: count edges the rightward ray from p
    // crosses. j trails i by one so each iteration tests edge (poly[j], poly[i]),
    // including the closing edge (poly[n-1], poly[0]).
    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point& a = poly[i];
        const Point& b = poly[j];
        const bool straddles = (a.y > p.y) != (b.y > p.y);
        if (straddles) {
            const float x_cross =
                (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
            if (p.x < x_cross) inside = !inside;
        }
    }
    return inside;
}

bool inside_any_area(const std::vector<CameraArea>& areas, Point p) {
    for (const CameraArea& area : areas) {
        if (point_in_polygon(area.points, p)) return true;
    }
    return false;
}

} // namespace denso::camera
