#include "ui/camera/grid/frame_processor.h"

#include "camera/area_geometry.h"             // inside_any_area
#include "ui/camera/shared/frame_convert.h"  // qimage_to_mat, mat_to_qimage
#include "ui/camera/shared/snapshot.h"       // apply_orientation

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdio>
#include <optional>

namespace denso::ui {

OrientationProcessor::OrientationProcessor(int degrees, double pitch, double roll)
    : degrees_(degrees), pitch_(pitch), roll_(roll) {}

QImage OrientationProcessor::process(const QImage& frame) {
    return apply_orientation(frame, degrees_, pitch_, roll_);
}

DetectionProcessor::DetectionProcessor(int degrees, double pitch, double roll,
                                       std::vector<ModelRun> models,
                                       std::vector<denso::camera::CameraArea> areas)
    : degrees_(degrees), pitch_(pitch), roll_(roll),
      models_(std::move(models)), areas_(std::move(areas)) {}

namespace {
// Min confidence a camera keeps for class_id, or nullopt if not selected.
std::optional<float> selected_conf(
    const std::vector<denso::detection::ModelClassSelection>& sel, int class_id) {
    for (const auto& s : sel) {
        if (s.class_id == class_id) return s.conf;
    }
    return std::nullopt;
}
} // namespace

QImage DetectionProcessor::process(const QImage& frame) {
    const QImage oriented = apply_orientation(frame, degrees_, pitch_, roll_);
    cv::Mat bgr = qimage_to_mat(oriented);
    if (bgr.empty()) {
        return oriented;
    }
    const float w = static_cast<float>(bgr.cols);
    const float h = static_cast<float>(bgr.rows);
    for (const ModelRun& run : models_) {
        if (!run.engine) continue;
        for (const Detection& d : run.engine->infer(bgr)) {
            const auto conf = selected_conf(run.classes, d.class_id);
            if (!conf || d.conf < *conf) continue;  // not selected / below thr
            // Confine to ROI: keep only boxes whose center is inside an area.
            // Areas are normalized [0,1] to this oriented frame, so normalize
            // the box center the same way. Empty areas → no confinement.
            if (!areas_.empty() && w > 0.0f && h > 0.0f) {
                const denso::camera::Point center{
                    (d.box.x + d.box.width * 0.5f) / w,
                    (d.box.y + d.box.height * 0.5f) / h};
                if (!denso::camera::inside_any_area(areas_, center)) continue;
            }
            cv::rectangle(bgr, d.box, cv::Scalar(0, 215, 255), 2);
            std::string label =
                (d.class_id < static_cast<int>(run.class_names.size())
                     ? run.class_names[d.class_id]
                     : std::to_string(d.class_id));
            char buf[16];
            std::snprintf(buf, sizeof(buf), " %.0f%%", d.conf * 100.0f);
            label += buf;
            cv::putText(bgr, label, cv::Point(d.box.x, std::max(0, d.box.y - 4)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 215, 255), 1);
        }
    }
    return mat_to_qimage(bgr);
}

} // namespace denso::ui
