// Slice 8 — ATTACHMENT REFUSAL, and Slice 9 — THE SELECTABLE LIST.
//
// Slice 8 half: `detection::set_camera_models` is the domain chokepoint for every
// write that binds a model to a camera (spec §7.1): the wizard, any future Leveler
// UI, and any future CLI all pass through it. It refuses the WHOLE requested set
// when any member is not `Allowed`, and the refusal is a rolled-back transaction —
// never a partial attachment.
//
// Slice 9 half: `detection::selectable_models` is the ONLY list a selection UI may
// render (spec §6.1). It is the READ counterpart of the same policy: what the
// wizard offers and what the wizard accepts are two readings of one rule, so a
// model can never be offered and then refused. The two halves are deliberately in
// one file for exactly that reason.
#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/repo.h"
#include "db/db.h"
#include "detection/detection.h"
#include "detection/repo.h"
#include "health/integrity.h"   // Slice-8 agreement: the list and the verdict agree
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

#include <map>
#include <optional>
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

// ═════════════════════════════════════════════════════════════════════════════
// SLICE 9 — the selectable list.
//
// Every case below drives `detection::selectable_models`, which must be a pure
// consumer of the central policy: it loads the catalog in id order, resolves each
// row for the view's ACTIVE backend, asks `models::model_compatibility`, and keeps
// only what is Allowed. It owns no rule of its own — which is why the rejection
// cases are produced by DECLARATION (or by tampering with the artifact), never by
// naming a file something suggestive.
// ═════════════════════════════════════════════════════════════════════════════

namespace {

/// A schema-2 generation with every knob the rejection cases need. The defaults
/// are the ALLOWED shape; each test perturbs exactly one field, so a case name
/// describes the single thing that differs.
struct Decl {
    std::string stem;                       // on-disk artifact stem
    std::string canonical_id;
    std::string family;
    std::string task = "detect";
    int         input_size = 640;
    std::vector<std::string> class_names{"0", "1", "2", "3"};
    denso::models::BuiltFor built_for{"10.3", "12.6", "87"};
    bool        active_backend_block = true;  // false = declare the OTHER backend
};

ModelGeneration declare_ex(const QString& dir, const Decl& d) {
    const QByteArray body = QByteArrayLiteral("model-bytes");
    ModelGeneration g;
    g.declared = true;
    g.name = d.canonical_id;
    g.installed_utc = "2026-07-27T00:00:00Z";
    g.state = "installed";
    g.canonical_id = d.canonical_id;
    g.family = d.family;
    g.task = d.task;
    g.input_size = d.input_size;
    g.class_count = static_cast<int>(d.class_names.size());
    g.class_names = d.class_names;

    // Build the block for the ACTIVE backend, unless the case is specifically
    // about a generation that declares only the OTHER one (spec 3.2.1 rule 5).
    const bool want_ort =
#ifdef _WIN32
        d.active_backend_block;
#else
        !d.active_backend_block;
#endif
    if (want_ort) {
        denso::models::OnnxRuntimeArtifact ort;
        ort.model = d.stem + ".onnx";
        ort.model_sha256 = write_and_hash(dir, ort.model, body);
        ort.class_metadata_source = denso::models::kSourceOnnxMetadataNames;
        g.runtime.onnxruntime = ort;
    } else {
        denso::models::TensorRtArtifact trt;
        trt.engine = d.stem + ".engine";
        trt.engine_sha256 = write_and_hash(dir, trt.engine, body);
        trt.sidecar = d.stem + ".names.json";
        QByteArray sidecar = "[";
        for (size_t i = 0; i < d.class_names.size(); ++i)
            sidecar += (i ? ",\"" : "\"") +
                       QByteArray::fromStdString(d.class_names[i]) + "\"";
        sidecar += "]";
        trt.sidecar_sha256 = write_and_hash(dir, trt.sidecar, sidecar);
        trt.class_metadata_source = denso::models::kSourceNamesSidecar;
        trt.built_for = d.built_for;
        g.runtime.tensorrt = trt;
    }
    return g;
}

/// Overwrite an artifact after its hash was recorded — a provenance fault made
/// physically, not by editing a manifest field.
void tamper(const QString& dir, const std::string& stem) {
    QFile f(QDir(dir).filePath(QString::fromStdString(stem + kExt)));
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write("tampered-bytes-of-a-different-length") > 0);
    f.close();
}

/// The canonical ids the list returned, in the order it returned them.
std::vector<std::string> ids_of(
    const std::vector<denso::detection::SelectableModel>& v) {
    std::vector<std::string> out;
    for (const auto& s : v) out.push_back(s.metadata.canonical_id);
    return out;
}

/// The catalog filenames the list returned, in order.
std::vector<std::string> files_of(
    const std::vector<denso::detection::SelectableModel>& v) {
    std::vector<std::string> out;
    for (const auto& s : v) out.push_back(s.row.filename);
    return out;
}

std::vector<std::string> classes_for_stem(const std::string& stem) {
    if (stem == "digitv3") return {"0", "1", "2", "3"};
    if (stem == "float-small") return {"Small"};
    return {"Big"};
}

/// A fully-provisioned appliance: all three real models declared, valid and
/// provenance-clean, catalogued in the given order (insertion order IS catalog-id
/// order, which is the property the list must preserve).
struct ThreeModels {
    QTemporaryDir tmp;
    std::optional<denso::db::Db> db;
    std::optional<ManifestView> view;
    std::map<std::string, int64_t> id;

    explicit ThreeModels(const std::vector<std::string>& catalog_order = {
                             "digitv3", "float-small", "float-big"}) {
        REQUIRE(tmp.isValid());
        Manifest m;
        m.schema = 2;
        m.generations.push_back(
            declare_ex(tmp.path(), {"digitv3", "digitv3", "digit_numeric"}));
        m.generations.push_back(declare_ex(
            tmp.path(),
            {"float-small", "float-small", "float_ball", "detect", 640, {"Small"}}));
        m.generations.push_back(declare_ex(
            tmp.path(),
            {"float-big", "float-big", "float_ball", "detect", 640, {"Big"}}));
        view.emplace(std::move(m), tmp.path());

        db = denso::db::Db::open_in_memory();
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));

        for (const std::string& stem : catalog_order) {
            const auto r = denso::detection::upsert_model(
                db->handle(), catalog_row(stem, classes_for_stem(stem)));
            REQUIRE(r.has_value());
            id[stem] = *r;
        }
    }
    QSqlDatabase h() const { return db->handle(); }
};

/// A one-model appliance built from an explicit manifest, for the rejection cases.
struct OneModel {
    QTemporaryDir tmp;
    std::optional<denso::db::Db> db;

    OneModel() { REQUIRE(tmp.isValid()); }

    void seed(const std::vector<std::string>& catalog_classes,
              const std::string& stem = "digitv3") {
        db = denso::db::Db::open_in_memory();
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));
        REQUIRE(denso::detection::upsert_model(db->handle(),
                                               catalog_row(stem, catalog_classes)));
    }
    QSqlDatabase h() const { return db->handle(); }

    std::vector<std::string> ids_in(const ManifestView& view, TargetMode mode) const {
        return ids_of(denso::detection::selectable_models(h(), mode, view, kPlatform));
    }
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// THE headline behaviour: one catalog, two modes, two disjoint lists.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("selectable_models returns only digitv3 in digit_reader",
          "[selectable_models]") {
    ThreeModels f;
    const auto got = denso::detection::selectable_models(
        f.h(), TargetMode::DigitReader, *f.view, kPlatform);

    CHECK(ids_of(got) == std::vector<std::string>{"digitv3"});
    CHECK(files_of(got) == std::vector<std::string>{std::string("digitv3") + kExt});
    // The row AND its resolved metadata come back together (spec 6.1).
    REQUIRE(got.size() == 1);
    CHECK(got[0].row.id == f.id["digitv3"]);
    CHECK(got[0].metadata.family == "digit_numeric");
    CHECK(got[0].metadata.declared);
    CHECK(got[0].metadata.artifact_matches);
    CHECK(got[0].metadata.provenance_ok);
}

TEST_CASE("selectable_models returns only the Float models in ball_leveler",
          "[selectable_models]") {
    ThreeModels f;
    const auto got = denso::detection::selectable_models(
        f.h(), TargetMode::BallLeveler, *f.view, kPlatform);

    CHECK(ids_of(got) == std::vector<std::string>{"float-small", "float-big"});
    // digitv3 must never leak into the Leveler list.
    for (const auto& s : got) CHECK(s.metadata.canonical_id != "digitv3");
}

// ─────────────────────────────────────────────────────────────────────────────
// ORDERING is catalog id — not alphabetical, not manifest order. The catalog is
// seeded BACKWARDS here so the two orders disagree: an implementation that sorted
// by name, or walked the manifest, would return float-small first.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("selectable_models preserves catalog-id order, not name order",
          "[selectable_models]") {
    ThreeModels f({"digitv3", "float-big", "float-small"});
    REQUIRE(f.id["float-big"] < f.id["float-small"]);

    const auto got = denso::detection::selectable_models(
        f.h(), TargetMode::BallLeveler, *f.view, kPlatform);
    CHECK(ids_of(got) == std::vector<std::string>{"float-big", "float-small"});
}

// ─────────────────────────────────────────────────────────────────────────────
// FAIL-CLOSED. Every rejection cause keeps the model out of BOTH lists. Each
// section perturbs exactly ONE thing away from the allowed shape.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("selectable_models excludes every rejected model", "[selectable_models]") {
    SECTION("undeclared — the catalog row has no manifest entry at all") {
        OneModel f;
        Manifest m;
        m.schema = 2;                       // valid, declares nothing
        const ManifestView view(std::move(m), f.tmp.path());
        f.seed({"0", "1", "2", "3"});
        CHECK(f.ids_in(view, TargetMode::DigitReader).empty());
        CHECK(f.ids_in(view, TargetMode::BallLeveler).empty());
    }

    SECTION("unknown canonical id — declared, but not in the compiled registry") {
        OneModel f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(
            declare_ex(f.tmp.path(), {"rogue", "rogue-v9", "digit_numeric"}));
        const ManifestView view(std::move(m), f.tmp.path());
        f.seed({"0", "1", "2", "3"}, "rogue");
        CHECK(f.ids_in(view, TargetMode::DigitReader).empty());
        CHECK(f.ids_in(view, TargetMode::BallLeveler).empty());
    }

    SECTION("family mismatch — declared family is not the registry's for that id") {
        OneModel f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(
            declare_ex(f.tmp.path(), {"digitv3", "digitv3", "float_ball"}));
        const ManifestView view(std::move(m), f.tmp.path());
        f.seed({"0", "1", "2", "3"});
        CHECK(f.ids_in(view, TargetMode::DigitReader).empty());
        CHECK(f.ids_in(view, TargetMode::BallLeveler).empty());
    }

    SECTION("wrong task") {
        OneModel f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(declare_ex(
            f.tmp.path(), {"digitv3", "digitv3", "digit_numeric", "segment"}));
        const ManifestView view(std::move(m), f.tmp.path());
        f.seed({"0", "1", "2", "3"});
        CHECK(f.ids_in(view, TargetMode::DigitReader).empty());
    }

    SECTION("unsupported input size") {
        OneModel f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(declare_ex(
            f.tmp.path(), {"digitv3", "digitv3", "digit_numeric", "detect", 320}));
        const ManifestView view(std::move(m), f.tmp.path());
        f.seed({"0", "1", "2", "3"});
        CHECK(f.ids_in(view, TargetMode::DigitReader).empty());
    }

    SECTION("renamed class — the artifact disagrees with the declaration") {
        OneModel f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(
            declare_ex(f.tmp.path(), {"digitv3", "digitv3", "digit_numeric"}));
        const ManifestView view(std::move(m), f.tmp.path());
        f.seed({"0", "X", "2", "3"});       // declaration says class 1 is "1"
        CHECK(f.ids_in(view, TargetMode::DigitReader).empty());
    }

    SECTION("reordered classes — same names, wrong order") {
        OneModel f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(
            declare_ex(f.tmp.path(), {"digitv3", "digitv3", "digit_numeric"}));
        const ManifestView view(std::move(m), f.tmp.path());
        f.seed({"1", "0", "2", "3"});       // class ids are POSITIONAL
        CHECK(f.ids_in(view, TargetMode::DigitReader).empty());
    }

    SECTION("class-count mismatch") {
        OneModel f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(
            declare_ex(f.tmp.path(), {"digitv3", "digitv3", "digit_numeric"}));
        const ManifestView view(std::move(m), f.tmp.path());
        f.seed({"0", "1", "2"});
        CHECK(f.ids_in(view, TargetMode::DigitReader).empty());
    }

    SECTION("provenance failure — the artifact no longer hashes to its declaration") {
        OneModel f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(
            declare_ex(f.tmp.path(), {"digitv3", "digitv3", "digit_numeric"}));
        const ManifestView view(std::move(m), f.tmp.path());
        f.seed({"0", "1", "2", "3"});
        tamper(f.tmp.path(), "digitv3");
        CHECK(f.ids_in(view, TargetMode::DigitReader).empty());
    }

    SECTION("no block for the ACTIVE backend (spec 3.2.1 rule 5)") {
        OneModel f;
        Manifest m;
        m.schema = 2;
        Decl d{"digitv3", "digitv3", "digit_numeric"};
        d.active_backend_block = false;     // declares the OTHER backend only
        m.generations.push_back(declare_ex(f.tmp.path(), d));
        const ManifestView view(std::move(m), f.tmp.path());
        f.seed({"0", "1", "2", "3"});
        CHECK(f.ids_in(view, TargetMode::DigitReader).empty());
    }

    SECTION("schema-1 identity is not a declaration") {
        OneModel f;
        Manifest m;
        m.schema = 1;
        ModelGeneration g;                  // root fields only; declared == false
        g.name = "digitv3";
        g.engine = std::string("digitv3") + kExt;
        g.class_names = {"0", "1", "2", "3"};
        m.generations.push_back(g);
        const ManifestView view(std::move(m), f.tmp.path());
        f.seed({"0", "1", "2", "3"});
        CHECK(f.ids_in(view, TargetMode::DigitReader).empty());
        CHECK(f.ids_in(view, TargetMode::BallLeveler).empty());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// An ABSENT manifest yields an EMPTY list — never a fallback to the raw catalog.
// That is the state of a box before `denso-setup seed-manifest` has run, and
// listing the catalog there would offer models the appliance refuses to load.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("an absent manifest makes every model unselectable", "[selectable_models]") {
    ThreeModels f;                                    // catalog full, artifacts on disk
    const ManifestView empty(Manifest{}, f.tmp.path());  // schema 0, no generations

    CHECK(denso::detection::selectable_models(f.h(), TargetMode::DigitReader, empty,
                                              kPlatform)
              .empty());
    CHECK(denso::detection::selectable_models(f.h(), TargetMode::BallLeveler, empty,
                                              kPlatform)
              .empty());
    // The catalog itself is NOT empty — proving this is a filter, not an empty DB.
    CHECK(denso::detection::list_models(f.h()).size() == 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// A failed platform probe (Slice 7 fails closed to an EMPTY PlatformInfo) must
// not authorize anything on the TensorRT backend. Under ONNX Runtime built_for is
// deliberately never read, so the list is unaffected there — assert the ACTUAL
// backend's contract rather than a platform-independent fiction.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("an empty measured platform cannot widen the list", "[selectable_models]") {
    ThreeModels f;
    const PlatformInfo none{};
    const auto got = denso::detection::selectable_models(
        f.h(), TargetMode::DigitReader, *f.view, none);
#ifdef _WIN32
    CHECK(ids_of(got) == std::vector<std::string>{"digitv3"});  // ORT ignores built_for
#else
    CHECK(got.empty());   // TensorRT: nothing corroborates, nothing selectable
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// SLICE-9 / SLICE-8 AGREEMENT. A model that becomes rejected must vanish from the
// list AND keep its camera inhibited. The dangerous outcome is the model quietly
// leaving the list while the camera looks healthy — a camera that has silently
// stopped reading.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("a newly-rejected attached model leaves the list but keeps its camera inhibited",
          "[selectable_models][model_enforcement]") {
    ThreeModels f;
    const int64_t cam = seed_camera(f.h(), "Line 1");

    // Attach digitv3 through the REAL write path while it is still allowed.
    CameraModel att;
    att.camera_id = cam;
    att.model_id = f.id["digitv3"];
    att.classes = {ModelClassSelection{0, 0.5f}};
    REQUIRE(denso::detection::set_camera_models(f.h(), cam, {att},
                                                TargetMode::DigitReader, *f.view,
                                                kPlatform));
    REQUIRE(ids_of(denso::detection::selectable_models(f.h(), TargetMode::DigitReader,
                                                       *f.view, kPlatform)) ==
            std::vector<std::string>{"digitv3"});

    // The artifact is replaced on disk — the declaration no longer describes what
    // is there. NOTHING about the database changed.
    tamper(f.tmp.path(), "digitv3");

    // (a) It disappears from the selectable list.
    CHECK(denso::detection::selectable_models(f.h(), TargetMode::DigitReader, *f.view,
                                              kPlatform)
              .empty());

    // (b) Slice 8 still reports the camera, with the REAL reason — the camera does
    //     NOT become healthy just because the model left the list.
    const auto verdict = denso::health::evaluate_integrity(
        f.h(), f.tmp.path(), TargetMode::DigitReader, *f.view, kPlatform);
    bool found = false;
    for (const auto& i : verdict.issues) {
        if (i.kind == denso::health::ZoneIssue::Kind::ModelCompatibilityRejected &&
            i.camera_id == cam) {
            found = true;
            CHECK(i.policy_reason == QStringLiteral("model_provenance_failed"));
        }
    }
    CHECK(found);
    CHECK(verdict.status == denso::health::Readiness::Degraded);
    CHECK(denso::health::exit_code_for(verdict.status) == 10);

    // (c) The runtime resolution keeps the camera inhibited as a whole.
    const auto det = denso::detection::detection_for(f.h(), cam, TargetMode::DigitReader,
                                                     *f.view, kPlatform);
    CHECK(det.compatibility_rejected);
    CHECK(det.models.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Reading the list must not WRITE anything — opening the Models step is a
// read-only act, and it happens on every visit to the wizard.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("selectable_models mutates no row", "[selectable_models]") {
    ThreeModels f;
    const int64_t cam = seed_camera(f.h(), "Line 1");
    CameraModel att;
    att.camera_id = cam;
    att.model_id = f.id["digitv3"];
    att.classes = {ModelClassSelection{0, 0.5f}, ModelClassSelection{1, 0.4f}};
    REQUIRE(denso::detection::set_camera_models(f.h(), cam, {att},
                                                TargetMode::DigitReader, *f.view,
                                                kPlatform));

    const int models_before = row_count(f.h(), "model");
    const int cm_before = row_count(f.h(), "camera_model");
    const int cc_before = row_count(f.h(), "camera_model_class");

    for (int i = 0; i < 3; ++i) {
        (void)denso::detection::selectable_models(f.h(), TargetMode::DigitReader,
                                                  *f.view, kPlatform);
        (void)denso::detection::selectable_models(f.h(), TargetMode::BallLeveler,
                                                  *f.view, kPlatform);
    }

    CHECK(row_count(f.h(), "model") == models_before);
    CHECK(row_count(f.h(), "camera_model") == cm_before);
    CHECK(row_count(f.h(), "camera_model_class") == cc_before);
    // The existing attachment is untouched, classes included.
    const auto det = denso::detection::detection_for(f.h(), cam, TargetMode::DigitReader,
                                                     *f.view, kPlatform);
    REQUIRE(det.models.size() == 1);
    CHECK(det.models.at(0).classes.size() == 2);
    // Schema is still v13 — this slice adds no migration.
    const auto ver = denso::db::read_user_version(f.h());
    REQUIRE(ver.has_value());
    CHECK(*ver == 18);
}
