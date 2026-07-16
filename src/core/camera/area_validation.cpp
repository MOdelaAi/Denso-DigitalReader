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

bool polygon_is_degenerate(const std::vector<Point>& poly) {
    return poly.size() < 3 || polygon_area(poly) < kMinPolygonArea;
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
