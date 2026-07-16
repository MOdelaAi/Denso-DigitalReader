#include "camera/area_validation.h"

#include <cmath>
#include <set>

namespace denso::camera {

double polygon_area(const std::vector<Point>& poly) {
    if (poly.size() < 3) {
        return 0.0;
    }
    double twice = 0.0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Point& a = poly[i];
        const Point& b = poly[(i + 1) % poly.size()];  // closing edge included
        twice += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
    }
    return std::abs(twice) / 2.0;
}

namespace {

/// Sign of the cross product (b-a)×(c-a): >0 left turn, <0 right turn, 0
/// collinear.
int orientation(const Point& a, const Point& b, const Point& c) {
    const double v = (static_cast<double>(b.x) - a.x) * (static_cast<double>(c.y) - a.y) -
                     (static_cast<double>(b.y) - a.y) * (static_cast<double>(c.x) - a.x);
    if (v > 0.0) return 1;
    if (v < 0.0) return -1;
    return 0;
}

/// True when segments a1→a2 and b1→b2 properly cross (each straddles the
/// other's line). Collinear overlap is deliberately NOT reported: it can't
/// arise from dragging without also collapsing the area, which the area floor
/// already rejects, and treating it as a crossing would flag legal shapes.
bool segments_cross(const Point& a1, const Point& a2, const Point& b1,
                    const Point& b2) {
    const int d1 = orientation(a1, a2, b1);
    const int d2 = orientation(a1, a2, b2);
    const int d3 = orientation(b1, b2, a1);
    const int d4 = orientation(b1, b2, a2);
    return d1 * d2 < 0 && d3 * d4 < 0;
}

}  // namespace

bool polygon_self_intersects(const std::vector<Point>& poly) {
    const size_t n = poly.size();
    if (n < 4) {
        return false;  // a triangle's edges all share vertices
    }
    for (size_t i = 0; i < n; ++i) {
        const Point& a1 = poly[i];
        const Point& a2 = poly[(i + 1) % n];
        // Start at i+2: edge i+1 shares a vertex with edge i. Stop before the
        // edge that wraps onto i for the same reason.
        for (size_t j = i + 2; j < n; ++j) {
            if (i == 0 && j == n - 1) {
                continue;  // the closing edge is adjacent to edge 0
            }
            if (segments_cross(a1, a2, poly[j], poly[(j + 1) % n])) {
                return true;
            }
        }
    }
    return false;
}

bool polygon_is_degenerate(const std::vector<Point>& poly) {
    return poly.size() < 3 || polygon_area(poly) < kMinPolygonArea ||
           polygon_self_intersects(poly);
}

std::optional<ZoneConflict> find_zone_conflict(
    const std::vector<CameraArea>& areas,
    const std::map<int, std::string>& zones_owned_elsewhere) {
    std::set<int> claimed_here;
    for (const CameraArea& a : areas) {
        if (!a.zone || *a.zone == 0) {
            continue;  // ROI-only: not reported, so never unique
        }
        const auto elsewhere = zones_owned_elsewhere.find(*a.zone);
        if (elsewhere != zones_owned_elsewhere.end()) {
            return ZoneConflict{*a.zone, a.name, elsewhere->second};
        }
        if (!claimed_here.insert(*a.zone).second) {
            return ZoneConflict{*a.zone, a.name, {}};
        }
    }
    return std::nullopt;
}

bool areas_equal(const std::vector<CameraArea>& a,
                 const std::vector<CameraArea>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name || a[i].zone != b[i].zone ||
            a[i].points.size() != b[i].points.size()) {
            return false;
        }
        for (size_t p = 0; p < a[i].points.size(); ++p) {
            if (a[i].points[p].x != b[i].points[p].x ||
                a[i].points[p].y != b[i].points[p].y) {
                return false;
            }
        }
    }
    return true;
}

} // namespace denso::camera
