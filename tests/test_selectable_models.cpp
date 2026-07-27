// Slice 8 — ATTACHMENT REFUSAL ONLY.
//
// `detection::set_camera_models` is the domain chokepoint for every write that
// binds a model to a camera (spec §7.1): the wizard, any future Leveler UI, and
// any future CLI all pass through it. This file proves it refuses the WHOLE
// requested set when any member is not `Allowed`, and that the refusal is a
// rolled-back transaction — never a partial attachment.
//
// SCOPE NOTE: the file name is the one the plan dictates, but Slice 8 owns only
// the refusal half. The Slice-9 selectable-list API (`selectable_models` /
// `SelectableModel`) is deliberately NOT implemented or tested here.
#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/repo.h"
#include "db/db.h"
#include "detection/detection.h"
#include "detection/repo.h"
#include "mode/mode.h"
#include "models/hashing.h"
#include "models/manifest.h"
#include "models/model_identity.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>

#include <string>
#include <vector>

using denso::detection::CameraModel;
using denso::detection::DetectionModel;
using denso::detection::ModelClassSelection;
using denso::mode::TargetMode;
using denso::models::Manifest;
using denso::models::ManifestView;
using denso::models::ModelGeneration;
using denso::models::PlatformInfo;

namespace {

#ifdef _WIN32
constexpr const char* kExt = ".onnx";
#else
constexpr const char* kExt = ".engine";
#endif

// The qualified Jetson triple the declarations below assert. Read only on the
// TensorRt backend; ignored under ONNX Runtime. Plain const, not constexpr:
// PlatformInfo holds std::strings and constexpr std::string needs GCC 12+ while
// the Jetson gate builds gcc11.
const PlatformInfo kPlatform{"10.3", "12.6", "87"};

std::string write_and_hash(const QString& dir, const std::string& name,
                           const QByteArray& bytes) {
    const QString path = QDir(dir).filePath(QString::fromStdString(name));
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write(bytes) == bytes.size());
    f.close();
    const auto h = denso::models::file_sha256(path);
    REQUIRE(h.has_value());
    return *h;
}

// A DECLARED, valid, provenance-clean schema-2 generation for the ACTIVE backend,
// with its artifact written into `dir` so hashes corroborate.
ModelGeneration declare(const QString& dir, const std::string& id,
                        const std::string& family,
                        const std::vector<std::string>& classes) {
    const QByteArray body = QByteArrayLiteral("model-bytes");
    ModelGeneration g;
    g.declared = true;
    g.canonical_id = id;
    g.family = family;
    g.task = "detect";
    g.input_size = 640;
    g.class_count = static_cast<int>(classes.size());
    g.class_names = classes;
#ifdef _WIN32
    denso::models::OnnxRuntimeArtifact ort;
    ort.model = id + ".onnx";
    ort.model_sha256 = write_and_hash(dir, ort.model, body);
    ort.class_metadata_source = denso::models::kSourceOnnxMetadataNames;
    g.runtime.onnxruntime = ort;
#else
    denso::models::TensorRtArtifact trt;
    trt.engine = id + ".engine";
    trt.engine_sha256 = write_and_hash(dir, trt.engine, body);
    trt.sidecar = id + ".names.json";
    QByteArray sidecar = "[";
    for (size_t i = 0; i < classes.size(); ++i)
        sidecar += (i ? ",\"" : "\"") + QByteArray::fromStdString(classes[i]) + "\"";
    sidecar += "]";
    trt.sidecar_sha256 = write_and_hash(dir, trt.sidecar, sidecar);
    trt.class_metadata_source = denso::models::kSourceNamesSidecar;
    trt.built_for = {"10.3", "12.6", "87"};
    g.runtime.tensorrt = trt;
#endif
    return g;
}

DetectionModel catalog_row(const std::string& id,
                           const std::vector<std::string>& classes) {
    DetectionModel m;
    m.name = id;
    m.filename = id + kExt;
    m.class_names = classes;
    return m;
}

int row_count(const QSqlDatabase& db, const QString& table) {
    QSqlQuery q(db);
    REQUIRE(q.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table));
    REQUIRE(q.next());
    return q.value(0).toInt();
}

int64_t seed_camera(const QSqlDatabase& db, const std::string& name) {
    denso::camera::Camera c;
    c.name = name;
    c.camera_type = "usb";
    c.active = true;
    c.index = 0;
    c.width = 640;
    c.height = 480;
    c.fps = 30;
    const auto id = denso::camera::insert(db, c);
    REQUIRE(id.has_value());
    return *id;
}

// One fixture: a digit_reader appliance whose manifest declares digitv3 (allowed
// here) and float-small (declared, valid, provenance-clean — rejected SOLELY on
// mode). Both are catalogued.
struct Fixture {
    QTemporaryDir tmp;
    std::optional<denso::db::Db> db;
    std::optional<ManifestView> view;
    int64_t digit_id = 0;
    int64_t float_id = 0;
    int64_t cam = 0;

    Fixture() {
        REQUIRE(tmp.isValid());
        Manifest m;
        m.schema = 2;
        m.generations.push_back(declare(tmp.path(), "digitv3", "digit_numeric",
                                        {"0", "1", "2", "3"}));
        m.generations.push_back(declare(tmp.path(), "float-small", "float_ball",
                                        {"Small"}));
        view.emplace(std::move(m), tmp.path());  // production ctor → active backend

        db = denso::db::Db::open_in_memory();
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));

        const auto d = denso::detection::upsert_model(
            db->handle(), catalog_row("digitv3", {"0", "1", "2", "3"}));
        const auto f = denso::detection::upsert_model(
            db->handle(), catalog_row("float-small", {"Small"}));
        REQUIRE(d.has_value());
        REQUIRE(f.has_value());
        digit_id = *d;
        float_id = *f;
        cam = seed_camera(db->handle(), "Cam A");
    }

    QSqlDatabase h() const { return db->handle(); }

    CameraModel attach(int64_t model_id, std::vector<ModelClassSelection> cls = {}) const {
        CameraModel cm;
        cm.camera_id = cam;
        cm.model_id = model_id;
        cm.classes = std::move(cls);
        return cm;
    }
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The allowed path is UNCHANGED: digitv3 on a digit_reader appliance attaches.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("set_camera_models attaches an allowed model", "[model_enforcement]") {
    Fixture f;
    REQUIRE(denso::detection::set_camera_models(
        f.h(), f.cam, {f.attach(f.digit_id, {{0, 0.5f}, {1, 0.4f}})},
        TargetMode::DigitReader, *f.view, kPlatform));

    CHECK(row_count(f.h(), "camera_model") == 1);
    CHECK(row_count(f.h(), "camera_model_class") == 2);
    const auto det = denso::detection::detection_for(
        f.h(), f.cam, TargetMode::DigitReader, *f.view, kPlatform);
    CHECK_FALSE(det.compatibility_rejected);
    REQUIRE(det.models.size() == 1);
    CHECK(det.models.at(0).filename == std::string("digitv3") + kExt);
}

// ─────────────────────────────────────────────────────────────────────────────
// A rejected model is refused, and NOTHING is written — not even the delete of
// the previous set. The pre-existing attachment must survive byte-for-byte.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("set_camera_models refuses a rejected model and rolls back",
          "[model_enforcement]") {
    Fixture f;
    // Establish a good attachment first: the refusal must not destroy it.
    REQUIRE(denso::detection::set_camera_models(
        f.h(), f.cam, {f.attach(f.digit_id, {{0, 0.5f}, {1, 0.4f}})},
        TargetMode::DigitReader, *f.view, kPlatform));
    const int cm_before = row_count(f.h(), "camera_model");
    const int cc_before = row_count(f.h(), "camera_model_class");

    denso::detection::AttachRefusal refusal;
    CHECK_FALSE(denso::detection::set_camera_models(
        f.h(), f.cam, {f.attach(f.float_id, {{0, 0.5f}})},
        TargetMode::DigitReader, *f.view, kPlatform, &refusal));

    // Row counts identical before and after — no partial model attachment and no
    // partial class attachment.
    CHECK(row_count(f.h(), "camera_model") == cm_before);
    CHECK(row_count(f.h(), "camera_model_class") == cc_before);

    // The ORIGINAL attachment is still exactly what it was.
    const auto det = denso::detection::detection_for(
        f.h(), f.cam, TargetMode::DigitReader, *f.view, kPlatform);
    REQUIRE(det.models.size() == 1);
    CHECK(det.models.at(0).filename == std::string("digitv3") + kExt);
    REQUIRE(det.models.at(0).classes.size() == 2);

    // The refusal is DIAGNOSABLE and names the real policy reason (spec §7.1) —
    // camera id, canonical id, filename, reason. Nothing else (§12).
    CHECK(refusal.camera_id == f.cam);
    CHECK(refusal.canonical_id == "float-small");
    CHECK(refusal.filename == std::string("float-small") + kExt);
    CHECK(refusal.policy_reason == "model_mode_incompatible");
}

// ─────────────────────────────────────────────────────────────────────────────
// A MIXED set — one allowed, one rejected — is refused AS A WHOLE. Running or
// storing the allowed subset would silently change what the camera reports.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("set_camera_models refuses a mixed allowed/rejected set as a whole",
          "[model_enforcement]") {
    Fixture f;
    const int cm_before = row_count(f.h(), "camera_model");
    const int cc_before = row_count(f.h(), "camera_model_class");
    REQUIRE(cm_before == 0);

    denso::detection::AttachRefusal refusal;
    CHECK_FALSE(denso::detection::set_camera_models(
        f.h(), f.cam,
        {f.attach(f.digit_id, {{0, 0.5f}}), f.attach(f.float_id, {{0, 0.5f}})},
        TargetMode::DigitReader, *f.view, kPlatform, &refusal));

    // The ALLOWED member must not have been written either.
    CHECK(row_count(f.h(), "camera_model") == cm_before);
    CHECK(row_count(f.h(), "camera_model_class") == cc_before);
    CHECK(denso::detection::detection_for(f.h(), f.cam, TargetMode::DigitReader,
                                          *f.view, kPlatform)
              .models.empty());
    CHECK(refusal.policy_reason == "model_mode_incompatible");
    CHECK(refusal.canonical_id == "float-small");  // the rejected member is named
}

// ─────────────────────────────────────────────────────────────────────────────
// An UNDECLARED model (no manifest entry at all) is refused too, and reports its
// own reason — not a mode problem. Its canonical id is "<undeclared>" because it
// has none; identity is never inferred from the filename.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("set_camera_models refuses an undeclared model with its own reason",
          "[model_enforcement]") {
    Fixture f;
    const auto stray = denso::detection::upsert_model(
        f.h(), catalog_row("mystery", {"0"}));
    REQUIRE(stray.has_value());

    denso::detection::AttachRefusal refusal;
    CHECK_FALSE(denso::detection::set_camera_models(
        f.h(), f.cam, {f.attach(*stray)}, TargetMode::DigitReader, *f.view,
        kPlatform, &refusal));

    CHECK(row_count(f.h(), "camera_model") == 0);
    CHECK(refusal.policy_reason == "model_undeclared");
    CHECK(refusal.canonical_id == "<undeclared>");
    CHECK(refusal.filename == std::string("mystery") + kExt);
}

// ─────────────────────────────────────────────────────────────────────────────
// REPLACEMENT of one valid non-empty set with another still works. The old
// "set_camera_models replaces the previous set" case moved here when the write
// path gained a policy gate: this is the same regression, now exercised against a
// declared, provenance-clean model so it goes through the REAL gate end to end.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("set_camera_models replaces one allowed set with another",
          "[model_enforcement]") {
    Fixture f;
    REQUIRE(denso::detection::set_camera_models(
        f.h(), f.cam, {f.attach(f.digit_id, {{0, 0.5f}, {1, 0.4f}, {2, 0.3f}})},
        TargetMode::DigitReader, *f.view, kPlatform));
    REQUIRE(row_count(f.h(), "camera_model") == 1);
    REQUIRE(row_count(f.h(), "camera_model_class") == 3);

    // Replace with a DIFFERENT selection of the same allowed model: the previous
    // attachment and all of its class rows must be gone, not merged.
    REQUIRE(denso::detection::set_camera_models(
        f.h(), f.cam, {f.attach(f.digit_id, {{3, 0.9f}})}, TargetMode::DigitReader,
        *f.view, kPlatform));
    CHECK(row_count(f.h(), "camera_model") == 1);
    CHECK(row_count(f.h(), "camera_model_class") == 1);

    const auto det = denso::detection::detection_for(
        f.h(), f.cam, TargetMode::DigitReader, *f.view, kPlatform);
    REQUIRE(det.models.size() == 1);
    REQUIRE(det.models.at(0).classes.size() == 1);
    CHECK(det.models.at(0).classes.at(0).class_id == 3);
    CHECK(det.models.at(0).classes.at(0).conf == 0.9f);
}

// ─────────────────────────────────────────────────────────────────────────────
// A genuine SQL failure is NOT a compatibility refusal. Reporting a broken
// database as "model_undeclared" would send an operator to inspect a manifest
// that was never the problem, so `refusal` must come back untouched.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("a database fault is reported as a write failure, not a policy refusal",
          "[model_enforcement]") {
    Fixture f;
    // Drop the catalog table out from under the write path: the lookup now fails
    // to EXECUTE, rather than returning zero rows.
    QSqlQuery drop(f.h());
    REQUIRE(drop.exec(QStringLiteral("DROP TABLE camera_model_class")));
    REQUIRE(drop.exec(QStringLiteral("DROP TABLE model")));

    denso::detection::AttachRefusal refusal;
    CHECK_FALSE(denso::detection::set_camera_models(
        f.h(), f.cam, {f.attach(f.digit_id)}, TargetMode::DigitReader, *f.view,
        kPlatform, &refusal));

    // Untouched: no invented canonical id, no invented policy reason.
    CHECK(refusal.policy_reason.empty());
    CHECK(refusal.canonical_id.empty());
    CHECK(refusal.camera_id == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// A hand-edited catalog row whose filename is a credential-bearing URL must not
// carry that URL into the operator-visible refusal (spec §12).
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("a refusal never echoes a credential-bearing filename",
          "[model_enforcement]") {
    Fixture f;
    DetectionModel evil;
    evil.name = "evil";
    evil.filename = "rtsp://admin:hunter2-SECRET@10.0.0.9/stream/float-small.onnx";
    evil.class_names = {"Small"};
    const auto id = denso::detection::upsert_model(f.h(), evil);
    REQUIRE(id.has_value());

    denso::detection::AttachRefusal refusal;
    CHECK_FALSE(denso::detection::set_camera_models(
        f.h(), f.cam, {f.attach(*id)}, TargetMode::DigitReader, *f.view, kPlatform,
        &refusal));

    CHECK(refusal.filename == "float-small.onnx");   // basename only
    CHECK(refusal.model_id == *id);                  // identifiable regardless
    CHECK(refusal.filename.find("rtsp://") == std::string::npos);
    CHECK(refusal.filename.find('@') == std::string::npos);
    CHECK(refusal.filename.find("hunter2-SECRET") == std::string::npos);
    CHECK(refusal.filename.find("admin") == std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// diagnostic_filename directly. Dropping the path is NOT sufficient on its own,
// so these are the cases that a strip-the-path-only reduction would have leaked:
// a query string survives the last '/', and a slashless userinfo is not reduced
// at all. The rule is a fail-closed ALLOW-LIST, not a blacklist.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("diagnostic_filename is a fail-closed allow-list", "[model_enforcement]") {
    using denso::models::diagnostic_filename;

    // Ordinary filenames pass through untouched — the common path must not be
    // mangled, or every honest diagnostic becomes useless.
    CHECK(diagnostic_filename("digitv3.engine") == "digitv3.engine");
    CHECK(diagnostic_filename("float-small.onnx") == "float-small.onnx");
    CHECK(diagnostic_filename("digitv3.names.json") == "digitv3.names.json");
    CHECK(diagnostic_filename("model_v2-1.engine") == "model_v2-1.engine");

    // Path reduction, both separators.
    CHECK(diagnostic_filename("/opt/denso/data/models/digitv3.engine") == "digitv3.engine");
    CHECK(diagnostic_filename("C:\\models\\digitv3.engine") == "digitv3.engine");

    // The two bypasses a path-strip alone would have left behind.
    CHECK(diagnostic_filename("https://host/model.engine?token=SECRET") == "<invalid>");
    CHECK(diagnostic_filename("user:password@host") == "<invalid>");

    // Anything else that is not provably a plain filename.
    CHECK(diagnostic_filename("rtsp://admin:pw@10.0.0.9/s.onnx") == "s.onnx");
    CHECK(diagnostic_filename("") == "<invalid>");
    CHECK(diagnostic_filename("/") == "<invalid>");
    CHECK(diagnostic_filename("a b.engine") == "<invalid>");
    CHECK(diagnostic_filename("m.engine;rm -rf /") == "<invalid>");

    // DELIBERATE over-rejection, pinned so it cannot drift silently: the manifest
    // validates runtime filenames with is_basename (separators and ".." only), so
    // `model+v1.engine` is legal upstream yet is still refused here. Over-rejecting
    // costs one diagnostic string; under-rejecting costs a credential — and the
    // callers emit the catalog row id beside this value precisely so an
    // unprintable name remains identifiable.
    CHECK(diagnostic_filename("model+v1.engine") == "<invalid>");
}

// ─────────────────────────────────────────────────────────────────────────────
// Clearing a camera's models (the empty set) is allowed in every mode — there is
// nothing to reject, and the operator must always be able to detach.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("set_camera_models still clears a camera's attachments",
          "[model_enforcement]") {
    Fixture f;
    REQUIRE(denso::detection::set_camera_models(
        f.h(), f.cam, {f.attach(f.digit_id, {{0, 0.5f}})}, TargetMode::DigitReader,
        *f.view, kPlatform));
    REQUIRE(row_count(f.h(), "camera_model") == 1);

    REQUIRE(denso::detection::set_camera_models(
        f.h(), f.cam, {}, TargetMode::DigitReader, *f.view, kPlatform));
    CHECK(row_count(f.h(), "camera_model") == 0);
    CHECK(row_count(f.h(), "camera_model_class") == 0);
}
