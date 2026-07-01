#include "ui/camera/shared/detection/yolo_decode.h"

#include <opencv2/dnn.hpp>

namespace denso::ui {

std::vector<Detection> decode_yolo(const float* out, int num_classes,
                                   int num_anchors, const LetterboxInfo& lb,
                                   int orig_w, int orig_h, float conf_floor,
                                   float nms_iou) {
    std::vector<cv::Rect> boxes;
    std::vector<float> scores;
    std::vector<int> class_ids;
    boxes.reserve(64);
    scores.reserve(64);
    class_ids.reserve(64);

    // Transposed layout: value at (row r, anchor a) == out[r * num_anchors + a].
    // Rows 0..3 are cx,cy,w,h; rows 4..4+nc-1 are class scores.
    for (int a = 0; a < num_anchors; ++a) {
        int best = -1;
        float best_score = 0.0f;
        for (int k = 0; k < num_classes; ++k) {
            const float s = out[(4 + k) * num_anchors + a];
            if (s > best_score) {
                best_score = s;
                best = k;
            }
        }
        if (best < 0 || best_score < conf_floor) {
            continue;
        }
        const float cx = out[0 * num_anchors + a];
        const float cy = out[1 * num_anchors + a];
        const float w = out[2 * num_anchors + a];
        const float h = out[3 * num_anchors + a];
        boxes.push_back(undo_letterbox(cx, cy, w, h, lb, orig_w, orig_h));
        scores.push_back(best_score);
        class_ids.push_back(best);
    }

    std::vector<int> keep;
    cv::dnn::NMSBoxes(boxes, scores, conf_floor, nms_iou, keep);

    std::vector<Detection> dets;
    dets.reserve(keep.size());
    for (int i : keep) {
        dets.push_back(Detection{boxes[i], class_ids[i], scores[i]});
    }
    return dets;
}

} // namespace denso::ui
