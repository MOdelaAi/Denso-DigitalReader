// Cross-model class-aware NMS: pool detections from several models (each tagged
// with its class *name*) and keep, per name, only the highest-confidence box
// among overlapping ones. Merging by name (not class id) is what lets the same
// class from different models dedup, since ids differ across models. Different
// names never suppress each other. Pure (OpenCV only) — unit-tested.
#pragma once

#include <opencv2/core.hpp>

#include <string>
#include <vector>

namespace denso::ui {

struct NamedDetection {
    cv::Rect box;
    float conf = 0.0f;
    std::string name;  // class name — the merge identity
};

// Greedy NMS within each class name. Input need not be sorted. Output is in
// descending-confidence order. A box is dropped when it overlaps an already-kept
// box of the same name by IoU strictly greater than iou_thresh.
std::vector<NamedDetection> merge_detections(std::vector<NamedDetection> dets,
                                             float iou_thresh);

} // namespace denso::ui
