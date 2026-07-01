// Detection-model domain types. Mirrors camera/camera.h: persisted rows above,
// transient runtime bundles below. Qt/OpenCV-free — the inference engine that
// consumes these lives in the app layer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace denso::detection {

// ─── DB (persisted) ───────────────────────────────────────────────────────────

/// One detection model available to the app — a row per .onnx under models/.
/// `class_names` is cached from the ONNX metadata (index == class id) so the UI
/// can list a model's classes without loading it. Row in `model`.
struct DetectionModel {
    int64_t id = 0;
    std::string name;                      // display name (default: filename stem)
    std::string filename;                  // "denso.onnx", resolved under models/
    std::vector<std::string> class_names;  // index == class_id
};

/// One class kept from a camera's attached model, with its own confidence
/// threshold. Row in `camera_model_class`.
struct ModelClassSelection {
    int   class_id = 0;
    float conf     = 0.5f;
};

/// A model attached to a camera + the classes kept from it. Row in
/// `camera_model` with its `camera_model_class` children. A camera attaches
/// 1..N; detection runs for a camera iff it has ≥1.
struct CameraModel {
    int64_t id        = 0;
    int64_t camera_id = 0;  // FK → camera.id
    int64_t model_id  = 0;  // FK → model.id
    std::vector<ModelClassSelection> classes;
};

// ─── Runtime (transient — never stored) ──────────────────────────────────────

/// One attached model resolved for inference: the model file + its class names
/// joined with this camera's per-class selections. Built from a DetectionModel
/// + CameraModel; handed to the DetectionProcessor. Never stored.
struct ResolvedModel {
    std::string filename;                       // selects which shared engine to run
    std::vector<std::string> class_names;       // for drawing labels
    std::vector<ModelClassSelection> classes;   // selected class_ids + per-class conf
};

/// A camera's full detection config resolved for the runtime — the analog of
/// camera::CameraWithAreas. Empty `models` means the camera runs no detection.
struct CameraDetection {
    int64_t camera_id = 0;
    std::vector<ResolvedModel> models;
};

} // namespace denso::detection