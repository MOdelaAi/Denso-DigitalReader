// Model identity resolution — Slice 6.
//
// Joins a catalog row to its schema-2 manifest generation FOR THE ACTIVE
// BACKEND, corroborates the loaded artifact against the declaration, and hands
// the result to the pure compatibility policy as a ModelMetadata.
//
// The active backend is an IMMUTABLE property of ManifestView, fixed once at
// construction by the one #ifdef _WIN32 split the tree uses (spec 3.2.1 rule 4).
// It is deliberately NOT a parameter on resolve_model_metadata (nor on the future
// evaluate_integrity / set_camera_models / detection_for / selectable_models):
// denso_core must not infer the application's backend, and threading a Backend
// through each of those would be four more places to get it wrong. This function
// is where the backend is resolved away; model_compatibility never sees one.
//
// This TU may touch Qt Core (QString/QDir), the manifest/catalog abstractions,
// and file hashing (models::file_sha256) — it must NOT link an inference backend
// or Qt Widgets. It performs NO device probing: measured platform information is
// supplied by the caller as PlatformInfo.
#pragma once

#include "detection/detection.h"
#include "models/compatibility.h"
#include "models/manifest.h"

#include <QString>

#include <string>
#include <utility>

namespace denso::models {

/// The inference runtime an appliance actually loads. Fixed by the build.
enum class Backend { OnnxRuntime, TensorRt };

/// The active backend for THIS build — the single platform split. Windows/MSYS2
/// runs ONNX Runtime; the Jetson appliance runs TensorRT.
constexpr Backend active_backend() {
#ifdef _WIN32
    return Backend::OnnxRuntime;
#else
    return Backend::TensorRt;
#endif
}

/// Measured runtime platform, supplied by the caller — denso_core never probes a
/// device, shells out, or reads the filesystem for this. Consulted ONLY on the
/// TensorRt backend, to corroborate runtime.tensorrt.built_for. Ignored entirely
/// on ONNX Runtime.
struct PlatformInfo {
    std::string trt;
    std::string cuda;
    std::string sm;
};

/// A parsed manifest bound to a models directory and an IMMUTABLE backend. The
/// backend is set once at construction and thereafter carried with the view;
/// every consumer reads it from here rather than taking it as a parameter.
class ManifestView {
public:
    // Production construction. The backend is bound to active_backend() — the one
    // #ifdef _WIN32 split — so a caller cannot select the wrong one. There is no
    // Backend parameter here by design (spec 3.2.1 rule 4).
    ManifestView(Manifest manifest, QString models_dir)
        : ManifestView(std::move(manifest), std::move(models_dir), active_backend()) {}

    // Test seam ONLY: construct with an explicit backend so BOTH platforms'
    // resolution can be exercised off-host. Production uses the two-arg form
    // above; every runtime call site resolves the backend through active_backend().
    ManifestView(Manifest manifest, QString models_dir, Backend backend)
        : manifest_(std::move(manifest)),
          models_dir_(std::move(models_dir)),
          backend_(backend) {}

    Backend         backend() const { return backend_; }
    const Manifest& manifest() const { return manifest_; }
    const QString&  models_dir() const { return models_dir_; }

private:
    Manifest manifest_;
    QString  models_dir_;
    Backend  backend_;
};

/// Resolve a catalog row's declared identity FOR THE VIEW'S BACKEND.
///
/// Identity comes from the manifest generation, never from the filename: the row
/// is joined to a generation via the active backend's declared artifact filename,
/// and the returned canonical_id/family/task/etc. are the generation's.
///
///   declared         — a schema-2 generation WITH a block for the active backend
///                      declares this row's filename. A schema-1 generation, or a
///                      generation with no block for this backend, resolves
///                      declared == false.
///   artifact_matches — set SOLELY from ordered class-name and class-count
///                      agreement between the catalog row and the declaration.
///   provenance_ok    — set SOLELY from this backend's artifact hashes (+ the
///                      TensorRT built_for vs `platform` on the TensorRt backend).
///
/// artifact_matches and provenance_ok are kept independent so the policy's
/// evaluation order can name the correct reason for a doubly-faulted model.
ModelMetadata resolve_model_metadata(const ManifestView& view,
                                     const denso::detection::DetectionModel& row,
                                     const PlatformInfo& platform);

}  // namespace denso::models
