#include "ui/camera/shared/detection/trt_engine.h"

namespace denso::ui {

// Phase A stub — builds and links against nothing TensorRT yet, returns no
// detections. Phase B: deserialize the engine here and run inference.
TrtEngine::TrtEngine(const std::string& /*engine_path*/,
                     const std::string& /*cache_dir*/) {}

std::vector<Detection> TrtEngine::infer(const cv::Mat& /*bgr*/) { return {}; }

} // namespace denso::ui
