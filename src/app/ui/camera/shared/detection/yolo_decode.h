// Decode a YOLOv8-detect ONNX output tensor ([1, 4+nc, na], transposed layout)
// into Detections in original-frame pixels: per-anchor argmax over the class
// scores, confidence-floor filter, class-agnostic NMS, and letterbox inverse
// mapping. Pure (OpenCV only) — unit-tested with a synthetic buffer.
#pragma once

#include "ui/camera/shared/detection/inference_engine.h"
#include "ui/camera/shared/detection/letterbox.h"

#include <vector>

namespace denso::ui {

std::vector<Detection> decode_yolo(const float* out, int num_classes,
                                   int num_anchors, const LetterboxInfo& lb,
                                   int orig_w, int orig_h, float conf_floor,
                                   float nms_iou);

// Decode an end-to-end (NMS-free) YOLO ONNX output ([1, num_dets, 6], each row
// x1,y1,x2,y2,score,class_id in letterboxed input pixels) into Detections in
// original-frame pixels. NMS is already applied by the model, so this only
// filters by confidence and maps boxes back via undo_letterbox. Pure — tested.
std::vector<Detection> decode_yolo_end2end(const float* out, int num_dets,
                                           const LetterboxInfo& lb, int orig_w,
                                           int orig_h, float conf_floor);

} // namespace denso::ui
