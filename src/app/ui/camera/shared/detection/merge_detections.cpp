#include "ui/camera/shared/detection/merge_detections.h"

#include <algorithm>

namespace denso::ui {
namespace {

float iou(const cv::Rect& a, const cv::Rect& b) {
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.width, b.x + b.width);
    const int y2 = std::min(a.y + a.height, b.y + b.height);
    const int iw = std::max(0, x2 - x1);
    const int ih = std::max(0, y2 - y1);
    const float inter = static_cast<float>(iw) * static_cast<float>(ih);
    const float uni = static_cast<float>(a.area() + b.area()) - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

} // namespace

std::vector<NamedDetection> merge_detections(std::vector<NamedDetection> dets,
                                             float iou_thresh) {
    std::sort(dets.begin(), dets.end(),
              [](const NamedDetection& a, const NamedDetection& b) {
                  return a.conf > b.conf;
              });
    std::vector<NamedDetection> kept;
    kept.reserve(dets.size());
    for (const NamedDetection& d : dets) {
        bool suppressed = false;
        for (const NamedDetection& k : kept) {
            if (k.name == d.name && iou(k.box, d.box) > iou_thresh) {
                suppressed = true;
                break;
            }
        }
        if (!suppressed) kept.push_back(d);
    }
    return kept;
}

} // namespace denso::ui
