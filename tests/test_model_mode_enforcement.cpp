// Slice 8 — camera-scoped model/mode enforcement (spec §7.2–§7.4).
//
// The property under test: an incompatible attachment — HOWEVER it entered the
// database — inhibits exactly its own camera. It never blocks the appliance, it
// never terminates it, no DetectionProcessor is built for that camera, no engine
// is requested for it, and every healthy sibling keeps running.
//
// The case this exists for is the RESTORED / HAND-EDITED database (§7.4): the
// attachment row is written DIRECTLY here, bypassing set_camera_models, because
// that is precisely the path a `sqlite3` session or a restored backup takes.
//
// Lives in denso_integration_tests: the CameraGrid cases drive the REAL grid over
// the app objects. No real engine is ever loaded — the EngineRegistry is given an
// injected counting factory, so "no engine requested" is observed, not assumed.
#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/camera_stream.h"
#include "camera/frame_processor.h"   // DetectionProcessor::constructed_count()
#include "camera/repo.h"
#include "cli/args.h"
#include "cli/run_headless.h"
#include "db/db.h"
#include "detection/detection.h"
#include "detection/engine_registry.h"
#include "detection/inference_engine.h"
#include "detection/repo.h"
#include "health/integrity.h"
#include "health/status_file.h"
#include "mode/config.h"
#include "mode/mode.h"
#include "models/compatibility.h"
#include "models/hashing.h"
#include "models/manifest.h"
#include "models/model_identity.h"
#include "paths/paths.h"
#include "ui/camera/grid/camera_grid.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using denso::detection::DetectionModel;
using denso::health::Readiness;
using denso::health::ZoneIssue;
using denso::mode::TargetMode;
using denso::models::Manifest;
using denso::models::ManifestView;
using denso::models::ModelGeneration;
using denso::models::PlatformInfo;
using denso::ui::CameraGrid;
using denso::ui::CameraStream;
using denso::ui::DetectionProcessor;
using denso::ui::EngineRegistry;

namespace {

#ifdef _WIN32
constexpr const char* kExt = ".onnx";
#else
constexpr const char* kExt = ".engine";
#endif

// The qualified Jetson triple the declarations below assert. Read only on the
// TensorRt backend; ignored under ONNX Runtime. Plain const (not constexpr):
// PlatformInfo holds std::strings and the Jetson gate builds gcc11.
const PlatformInfo kPlatform{"10.3", "12.6", "87"};

struct ScopedDataDir {
    QByteArray prev_ = qgetenv("DENSO_DATA_DIR");
    bool had_ = qEnvironmentVariableIsSet("DENSO_DATA_DIR");
    explicit ScopedDataDir(const QByteArray& path) { qputenv("DENSO_DATA_DIR", path); }
    ~ScopedDataDir() {
        if (had_) qputenv("DENSO_DATA_DIR", prev_);
        else qunsetenv("DENSO_DATA_DIR");
    }
};

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

/// A schema-2 generation for the ACTIVE backend, artifact written into `dir` so
/// its hashes corroborate. Every knob the seven policy reasons need is a
/// parameter, so each rejection cause is produced by DECLARATION, never by a
/// filename convention.
struct Decl {
    std::string stem;                     // the on-disk artifact stem
    std::string canonical_id;
    std::string family;
    std::string task = "detect";
    int         input_size = 640;
    std::vector<std::string> class_names{"0", "1", "2", "3"};
    denso::models::BuiltFor built_for{"10.3", "12.6", "87"};
};

ModelGeneration declare(const QString& dir, const Decl& d) {
    const QByteArray body = QByteArrayLiteral("model-bytes");
    ModelGeneration g;
    g.declared = true;
    g.name = d.canonical_id;          // display name; also the serializer's "name"
    g.installed_utc = "2026-07-27T00:00:00Z";
    g.state = "installed";
    g.canonical_id = d.canonical_id;
    g.family = d.family;
    g.task = d.task;
    g.input_size = d.input_size;
    g.class_count = static_cast<int>(d.class_names.size());
    g.class_names = d.class_names;
#ifdef _WIN32
    denso::models::OnnxRuntimeArtifact ort;
    ort.model = d.stem + ".onnx";
    ort.model_sha256 = write_and_hash(dir, ort.model, body);
    ort.class_metadata_source = denso::models::kSourceOnnxMetadataNames;
    g.runtime.onnxruntime = ort;
#else
    denso::models::TensorRtArtifact trt;
    trt.engine = d.stem + ".engine";
    trt.engine_sha256 = write_and_hash(dir, trt.engine, body);
    trt.sidecar = d.stem + ".names.json";
    QByteArray sidecar = "[";
    for (size_t i = 0; i < d.class_names.size(); ++i)
        sidecar += (i ? ",\"" : "\"") + QByteArray::fromStdString(d.class_names[i]) + "\"";
    sidecar += "]";
    trt.sidecar_sha256 = write_and_hash(dir, trt.sidecar, sidecar);
    trt.class_metadata_source = denso::models::kSourceNamesSidecar;
    trt.built_for = d.built_for;
    g.runtime.tensorrt = trt;
#endif
    return g;
}

// Corrupt an already-declared artifact so its recorded hash no longer matches —
// the provenance fault, produced physically rather than by editing a field.
void tamper(const QString& dir, const std::string& stem) {
    const QString path =
        QDir(dir).filePath(QString::fromStdString(stem + kExt));
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write("tampered-bytes-different-length") > 0);
    f.close();
}

DetectionModel catalog_row(const std::string& stem,
                           const std::vector<std::string>& classes) {
    DetectionModel m;
    m.name = stem;
    m.filename = stem + kExt;
    m.class_names = classes;
    return m;
}

// ── the counting stub engine + factory (no ORT/TensorRT load) ────────────────
struct BuildLog {
    std::vector<std::string> built;
};

struct StubEngine : denso::ui::InferenceEngine {
    std::vector<std::string> names{"0", "1", "2", "3"};
    std::vector<denso::ui::Detection> infer(const cv::Mat&) override { return {}; }
    const std::vector<std::string>& class_names() const override { return names; }
};

EngineRegistry::EngineFactory make_factory(BuildLog& log) {
    return [&log](const std::string& path, const std::string&)
               -> std::unique_ptr<denso::ui::InferenceEngine> {
        log.built.push_back(path);
        return std::make_unique<StubEngine>();
    };
}

int count_built(const BuildLog& log, const std::string& needle) {
    int n = 0;
    for (const auto& p : log.built)
        if (p.find(needle) != std::string::npos) ++n;
    return n;
}

/// Write a camera_model attachment DIRECTLY, bypassing set_camera_models — the
/// restored-backup / hand-edited-database path (§7.4). This is the whole point:
/// the write-path guard (§7.1) never ran, so read-time enforcement must catch it.
void attach_directly(const QSqlDatabase& db, int64_t camera_id, int64_t model_id) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO camera_model (camera_id, model_id) VALUES (?, ?)"));
    q.addBindValue(static_cast<qlonglong>(camera_id));
    q.addBindValue(static_cast<qlonglong>(model_id));
    REQUIRE(q.exec());
}

/// A camera whose RTSP source carries credentials — so every status/diagnostic
/// assertion below is scanning a fixture that HAS something to leak.
denso::camera::Camera cred_cam(const std::string& name) {
    denso::camera::Camera c;
    c.name = name;
    c.camera_type = "ip";
    c.ip = "127.0.0.1";
    c.rtsp = "rtsp://admin:hunter2-SECRET@127.0.0.1:9/stream";
    c.channel = 1;
    c.stream = 0;
    c.username = "admin-USERNAME";
    c.password = "hunter2-SECRET";
    c.width = 1280;
    c.height = 720;
    c.fps = 25;
    c.active = true;
    c.setup_complete = true;
    c.areas_need_review = false;
    return c;
}

/// A scratch data dir with models/, an in-memory migrated DB, and the production
/// ManifestView over the written manifest.
struct Fixture {
    QTemporaryDir data;
    ScopedDataDir guard{data.isValid() ? data.path().toUtf8() : QByteArray()};
    std::optional<denso::db::Db> db;

    Fixture() {
        REQUIRE(data.isValid());
        REQUIRE(QDir(data.path()).mkpath(QStringLiteral("models")));
        db = denso::db::Db::open_in_memory();
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));
    }

    QString models() const { return denso::paths::models_dir(); }
    QSqlDatabase h() const { return db->handle(); }

    int64_t add_model(const std::string& stem, const std::vector<std::string>& cls) {
        const auto id = denso::detection::upsert_model(h(), catalog_row(stem, cls));
        REQUIRE(id.has_value());
        return *id;
    }
    int64_t add_camera(const denso::camera::Camera& c) {
        const auto id = denso::camera::insert(h(), c);
        REQUIRE(id.has_value());
        return *id;
    }
};

// Minimal schema-2 manifest serializer — only what these fixtures declare. Keeps
// the ON-DISK file and the in-memory view describing the SAME generations, which
// is what lets the paths that re-read models/manifest.json for themselves
// (CameraGrid, --check) agree with the directly-constructed ManifestView.
QString json_escape(const std::string& s) {
    return QString::fromStdString(s);
}

void write_manifest(const QString& models, const Manifest& m) {
    QString out = QStringLiteral("{\n  \"schema\": %1,\n  \"generations\": [\n")
                      .arg(m.schema);
    for (size_t i = 0; i < m.generations.size(); ++i) {
        const ModelGeneration& g = m.generations[i];
        QString classes;
        for (size_t c = 0; c < g.class_names.size(); ++c)
            classes += (c ? QStringLiteral(", \"") : QStringLiteral("\"")) +
                       json_escape(g.class_names[c]) + QStringLiteral("\"");
        QString runtime;
#ifdef _WIN32
        const auto& a = *g.runtime.onnxruntime;
        runtime = QStringLiteral(
                      "\"onnxruntime\": {\"model\": \"%1\", \"model_sha256\": \"%2\", "
                      "\"class_metadata_source\": \"%3\"}")
                      .arg(json_escape(a.model), json_escape(a.model_sha256),
                           json_escape(a.class_metadata_source));
#else
        const auto& a = *g.runtime.tensorrt;
        runtime = QStringLiteral(
                      "\"tensorrt\": {\"engine\": \"%1\", \"engine_sha256\": \"%2\", "
                      "\"sidecar\": \"%3\", \"sidecar_sha256\": \"%4\", "
                      "\"class_metadata_source\": \"%5\", "
                      "\"built_for\": {\"trt\": \"%6\", \"cuda\": \"%7\", \"sm\": \"%8\"}}")
                      .arg(json_escape(a.engine), json_escape(a.engine_sha256),
                           json_escape(a.sidecar), json_escape(a.sidecar_sha256),
                           json_escape(a.class_metadata_source),
                           json_escape(a.built_for.trt), json_escape(a.built_for.cuda),
                           json_escape(a.built_for.sm));
#endif
        out += QStringLiteral(
                   "    {\"name\": \"%1\", \"canonical_id\": \"%2\", \"family\": \"%3\", "
                   "\"task\": \"%4\", \"installed_utc\": \"%5\", \"state\": \"%6\", "
                   "\"input_size\": %7, \"class_count\": %8, \"class_names\": [%9], "
                   // The five provenance fields validate_manifest enforces — the
                   // file must be VALID, or ManifestCorrupt (78) would mask the
                   // camera-scoped behaviour this case exists to prove.
                   "\"runtime\": {%10}, \"provenance\": {"
                   "\"source_pt_sha256\": \"%12\", \"onnx_sha256\": \"%12\", "
                   "\"export_ultralytics\": \"8.4.21\", \"precision\": \"fp16\", "
                   "\"export_engine_command\": \"trtexec --onnx=model.onnx\"}}%11\n")
                   .arg(json_escape(g.name), json_escape(g.canonical_id),
                        json_escape(g.family), json_escape(g.task),
                        json_escape(g.installed_utc), json_escape(g.state))
                   .arg(g.input_size)
                   .arg(g.class_count)
                   .arg(classes, runtime,
                        i + 1 < m.generations.size() ? QStringLiteral(",")
                                                     : QString(),
                        QString(64, QLatin1Char('a')));  // a well-formed sha256
    }
    out += QStringLiteral("  ]\n}\n");
    QFile f(QDir(models).filePath(QStringLiteral("manifest.json")));
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write(out.toUtf8()) > 0);
}

/// Find the single ModelCompatibilityRejected issue for `camera_id`, if any.
std::optional<ZoneIssue> rejection_for(const denso::health::IntegrityVerdict& v,
                                       int64_t camera_id) {
    for (const auto& i : v.issues)
        if (i.kind == ZoneIssue::Kind::ModelCompatibilityRejected &&
            i.camera_id == camera_id)
            return i;
    return std::nullopt;
}

int count_rejections(const denso::health::IntegrityVerdict& v) {
    int n = 0;
    for (const auto& i : v.issues)
        if (i.kind == ZoneIssue::Kind::ModelCompatibilityRejected) ++n;
    return n;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// §7.4 — the restored / hand-edited database. The attachment is written DIRECTLY,
// so no write-path guard ever saw it. Read-time enforcement must catch it, name
// the RIGHT camera and the RIGHT reason, and classify it camera-scoped.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("a directly-written wrong-mode attachment inhibits only its camera",
          "[model_enforcement][integrity]") {
    Fixture f;
    Manifest m;
    m.schema = 2;
    m.generations.push_back(declare(f.models(), {"digitv3", "digitv3", "digit_numeric"}));
    m.generations.push_back(
        declare(f.models(), {"float-small", "float-small", "float_ball", "detect", 640,
                             {"Small"}}));
    const ManifestView view(m, f.models());

    const int64_t digit = f.add_model("digitv3", {"0", "1", "2", "3"});
    const int64_t flt = f.add_model("float-small", {"Small"});
    const int64_t cam = f.add_camera(cred_cam("Line 1"));
    attach_directly(f.h(), cam, flt);

    const auto v = denso::health::evaluate_integrity(
        f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);

    const auto issue = rejection_for(v, cam);
    REQUIRE(issue.has_value());
    CHECK(issue->camera_id == cam);
    // ONLY the genuine wrong-mode case may use this reason.
    CHECK(issue->policy_reason == QStringLiteral("model_mode_incompatible"));
    CHECK(denso::health::reason_code(issue->kind) ==
          QStringLiteral("model_compatibility_rejected"));

    // Camera-scoped: Degraded (10), NEVER Blocked (78), and no global blocker.
    CHECK(v.status == Readiness::Degraded);
    CHECK(denso::health::exit_code_for(v.status) == 10);
    CHECK(v.blockers.empty());

    // The detail is redaction-safe: identity only, never a credential (§12).
    CHECK_FALSE(issue->detail.contains(QStringLiteral("hunter2-SECRET")));
    CHECK_FALSE(issue->detail.contains(QStringLiteral("rtsp://")));
    CHECK_FALSE(issue->detail.contains(QStringLiteral("@")));

    (void)digit;
}

// ═════════════════════════════════════════════════════════════════════════════
// ONE CASE PER policy_reason (spec §4.2 / §7.3). Each rejection cause must arrive
// with its OWN code: a hash mismatch must never describe itself as a mode problem.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("every policy reason reaches the issue verbatim",
          "[model_enforcement][integrity]") {
    SECTION("model_undeclared — no manifest entry at all") {
        Fixture f;
        Manifest m;
        m.schema = 2;   // valid, but declares nothing for this artifact
        const ManifestView view(m, f.models());
        const int64_t mid = f.add_model("mystery", {"0"});
        const int64_t cam = f.add_camera(cred_cam("C"));
        attach_directly(f.h(), cam, mid);
        const auto v = denso::health::evaluate_integrity(
            f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);
        const auto i = rejection_for(v, cam);
        REQUIRE(i.has_value());
        CHECK(i->policy_reason == QStringLiteral("model_undeclared"));
        CHECK(v.status == Readiness::Degraded);
    }

    SECTION("model_unknown_id — declared, but not in the compiled registry") {
        Fixture f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(
            declare(f.models(), {"rogue", "rogue-v9", "digit_numeric"}));
        const ManifestView view(m, f.models());
        const int64_t mid = f.add_model("rogue", {"0", "1", "2", "3"});
        const int64_t cam = f.add_camera(cred_cam("C"));
        attach_directly(f.h(), cam, mid);
        const auto v = denso::health::evaluate_integrity(
            f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);
        const auto i = rejection_for(v, cam);
        REQUIRE(i.has_value());
        CHECK(i->policy_reason == QStringLiteral("model_unknown_id"));
    }

    SECTION("model_family_mismatch — declared family ≠ the registry's") {
        Fixture f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(
            declare(f.models(), {"digitv3", "digitv3", "float_ball"}));
        const ManifestView view(m, f.models());
        const int64_t mid = f.add_model("digitv3", {"0", "1", "2", "3"});
        const int64_t cam = f.add_camera(cred_cam("C"));
        attach_directly(f.h(), cam, mid);
        const auto v = denso::health::evaluate_integrity(
            f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);
        const auto i = rejection_for(v, cam);
        REQUIRE(i.has_value());
        CHECK(i->policy_reason == QStringLiteral("model_family_mismatch"));
    }

    SECTION("model_shape_unsupported — input_size ≠ 640") {
        Fixture f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(declare(
            f.models(), {"digitv3", "digitv3", "digit_numeric", "detect", 320}));
        const ManifestView view(m, f.models());
        const int64_t mid = f.add_model("digitv3", {"0", "1", "2", "3"});
        const int64_t cam = f.add_camera(cred_cam("C"));
        attach_directly(f.h(), cam, mid);
        const auto v = denso::health::evaluate_integrity(
            f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);
        const auto i = rejection_for(v, cam);
        REQUIRE(i.has_value());
        CHECK(i->policy_reason == QStringLiteral("model_shape_unsupported"));
    }

    SECTION("model_classes_mismatch — the artifact's classes disagree") {
        Fixture f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(declare(f.models(), {"digitv3", "digitv3",
                                                     "digit_numeric", "detect", 640,
                                                     {"0", "1", "2", "3"}}));
        const ManifestView view(m, f.models());
        // Catalog row says class 1 is named "X" — provenance is untouched, so the
        // CLASS check must be the one that fires, not provenance and not mode.
        const int64_t mid = f.add_model("digitv3", {"0", "X", "2", "3"});
        const int64_t cam = f.add_camera(cred_cam("C"));
        attach_directly(f.h(), cam, mid);
        const auto v = denso::health::evaluate_integrity(
            f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);
        const auto i = rejection_for(v, cam);
        REQUIRE(i.has_value());
        CHECK(i->policy_reason == QStringLiteral("model_classes_mismatch"));
        CHECK(i->policy_reason != QStringLiteral("model_mode_incompatible"));
    }

    SECTION("model_provenance_failed — a hash mismatch is NOT a mode problem") {
        Fixture f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(declare(f.models(), {"digitv3", "digitv3",
                                                     "digit_numeric"}));
        const ManifestView view(m, f.models());
        // Classes agree exactly; only the bytes changed after the hash was taken.
        const int64_t mid = f.add_model("digitv3", {"0", "1", "2", "3"});
        tamper(f.models(), "digitv3");
        const int64_t cam = f.add_camera(cred_cam("C"));
        attach_directly(f.h(), cam, mid);
        const auto v = denso::health::evaluate_integrity(
            f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);
        const auto i = rejection_for(v, cam);
        REQUIRE(i.has_value());
        CHECK(i->policy_reason == QStringLiteral("model_provenance_failed"));
        CHECK(i->policy_reason != QStringLiteral("model_mode_incompatible"));
    }

    SECTION("model_mode_incompatible — declared, valid, and simply the wrong mode") {
        Fixture f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(declare(
            f.models(),
            {"float-small", "float-small", "float_ball", "detect", 640, {"Small"}}));
        const ManifestView view(m, f.models());
        const int64_t mid = f.add_model("float-small", {"Small"});
        const int64_t cam = f.add_camera(cred_cam("C"));
        attach_directly(f.h(), cam, mid);
        const auto v = denso::health::evaluate_integrity(
            f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);
        const auto i = rejection_for(v, cam);
        REQUIRE(i.has_value());
        CHECK(i->policy_reason == QStringLiteral("model_mode_incompatible"));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// §5.1 — a declared, valid, provenance-clean, UNATTACHED wrong-mode artifact is a
// NORMAL installation state. Ready, exit 0, and NO camera issue: nothing depends
// on it. (Slice 7 already proved warm-up skips it; this is the readiness half.)
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("an unattached wrong-mode artifact stays Ready",
          "[model_enforcement][integrity]") {
    Fixture f;
    Manifest m;
    m.schema = 2;
    m.generations.push_back(declare(f.models(), {"digitv3", "digitv3", "digit_numeric"}));
    m.generations.push_back(declare(
        f.models(),
        {"float-small", "float-small", "float_ball", "detect", 640, {"Small"}}));
    const ManifestView view(m, f.models());

    // BOTH artifacts are catalogued and BOTH are on disk (so neither can be
    // reported unmanifested), but float-small is attached to NO camera.
    const int64_t digit = f.add_model("digitv3", {"0", "1", "2", "3"});
    f.add_model("float-small", {"Small"});
    const int64_t cam = f.add_camera(cred_cam("Line 1"));
    attach_directly(f.h(), cam, digit);

    const auto v = denso::health::evaluate_integrity(
        f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);

    CHECK(count_rejections(v) == 0);   // no camera depends on float-small
    CHECK(v.issues.empty());           // and nothing else degrades the box
    CHECK(v.blockers.empty());
    CHECK(v.status == Readiness::Ready);
    CHECK(denso::health::exit_code_for(v.status) == 0);

    // The Slice-7 warm-up firewall still excludes it — an unattached wrong-mode
    // artifact is Ready AND idle, not Ready and loaded.
    std::vector<denso::models::ModelMetadata> metas;
    for (const auto& row : denso::detection::list_models(f.h()))
        metas.push_back(denso::models::resolve_model_metadata(view, row, kPlatform));
    const auto allow =
        denso::models::loadable_model_files(TargetMode::DigitReader, metas);
    CHECK(allow == std::set<std::string>{std::string("digitv3") + kExt});
}

// ═════════════════════════════════════════════════════════════════════════════
// Camera-scoped isolation: camera A compatible, camera B not. Only B is affected.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("a rejected camera does not affect its healthy sibling",
          "[model_enforcement][integrity]") {
    Fixture f;
    Manifest m;
    m.schema = 2;
    m.generations.push_back(declare(f.models(), {"digitv3", "digitv3", "digit_numeric"}));
    m.generations.push_back(declare(
        f.models(),
        {"float-small", "float-small", "float_ball", "detect", 640, {"Small"}}));
    const ManifestView view(m, f.models());

    const int64_t digit = f.add_model("digitv3", {"0", "1", "2", "3"});
    const int64_t flt = f.add_model("float-small", {"Small"});
    const int64_t cam_a = f.add_camera(cred_cam("Healthy A"));
    const int64_t cam_b = f.add_camera(cred_cam("Rejected B"));
    attach_directly(f.h(), cam_a, digit);
    attach_directly(f.h(), cam_b, flt);

    const auto v = denso::health::evaluate_integrity(
        f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);

    CHECK_FALSE(rejection_for(v, cam_a).has_value());   // A is untouched
    REQUIRE(rejection_for(v, cam_b).has_value());       // B alone is named
    CHECK(count_rejections(v) == 1);
    CHECK(v.status == Readiness::Degraded);             // not Blocked
    CHECK(v.blockers.empty());

    // A resolves normally for the runtime; B is inhibited as a whole.
    const auto det_a = denso::detection::detection_for(
        f.h(), cam_a, TargetMode::DigitReader, view, kPlatform);
    CHECK_FALSE(det_a.compatibility_rejected);
    CHECK(det_a.models.size() == 1);

    const auto det_b = denso::detection::detection_for(
        f.h(), cam_b, TargetMode::DigitReader, view, kPlatform);
    CHECK(det_b.compatibility_rejected);
    CHECK(det_b.models.empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// A MIXED camera — one allowed model, one rejected — is inhibited AS A WHOLE.
// Running the allowed subset would silently change what the camera reports.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("a mixed attachment inhibits the whole camera",
          "[model_enforcement][detection][repo]") {
    Fixture f;
    Manifest m;
    m.schema = 2;
    m.generations.push_back(declare(f.models(), {"digitv3", "digitv3", "digit_numeric"}));
    m.generations.push_back(declare(
        f.models(),
        {"float-small", "float-small", "float_ball", "detect", 640, {"Small"}}));
    const ManifestView view(m, f.models());

    const int64_t digit = f.add_model("digitv3", {"0", "1", "2", "3"});
    const int64_t flt = f.add_model("float-small", {"Small"});
    const int64_t cam = f.add_camera(cred_cam("Mixed"));
    attach_directly(f.h(), cam, digit);
    attach_directly(f.h(), cam, flt);

    const auto det = denso::detection::detection_for(
        f.h(), cam, TargetMode::DigitReader, view, kPlatform);
    CHECK(det.compatibility_rejected);
    // NOT the allowed subset — the whole set is withheld.
    CHECK(det.models.empty());
    CHECK(det.policy_reason == "model_mode_incompatible");
}

// ═════════════════════════════════════════════════════════════════════════════
// status.json is a FILE FORMAT: it carries the stable kind reason AND the actual
// policy_reason AND the camera id — and no credential.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("status.json carries the reason, the policy_reason and no credential",
          "[model_enforcement][status]") {
    Fixture f;
    Manifest m;
    m.schema = 2;
    m.generations.push_back(declare(
        f.models(),
        {"float-small", "float-small", "float_ball", "detect", 640, {"Small"}}));
    const ManifestView view(m, f.models());
    const int64_t flt = f.add_model("float-small", {"Small"});
    const int64_t cam = f.add_camera(cred_cam("Line 1"));
    attach_directly(f.h(), cam, flt);

    const auto v = denso::health::evaluate_integrity(
        f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);
    REQUIRE(rejection_for(v, cam).has_value());

    const QString path = denso::paths::status_file();
    REQUIRE(denso::health::write_status_file(
        path, v, {{cam, static_cast<uint32_t>(denso::health::ZoneCause::ModelUnavailable)}},
        {}, {}, QStringLiteral("digit_reader"), false));

    QFile sf(path);
    REQUIRE(sf.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(sf.readAll());

    CHECK(text.contains(QStringLiteral("\"model_compatibility_rejected\"")));
    CHECK(text.contains(QStringLiteral("\"policy_reason\"")));
    CHECK(text.contains(QStringLiteral("\"model_mode_incompatible\"")));
    CHECK(text.contains(QStringLiteral("\"camera_id\": \"%1\"").arg(cam)));

    // Redaction (§12): scan the WHOLE emitted file, not just the issue.
    CHECK_FALSE(text.contains(QStringLiteral("rtsp://")));
    CHECK_FALSE(text.contains(QStringLiteral("@")));
    CHECK_FALSE(text.contains(QStringLiteral("hunter2-SECRET")));
    CHECK_FALSE(text.contains(QStringLiteral("admin-USERNAME")));
}

// ═════════════════════════════════════════════════════════════════════════════
// The OTHER credential path into status.json: `model.filename` is a database
// column, and the hand-edited database is exactly the case this slice handles. A
// row whose filename is a credential-bearing URL must not carry it into the
// emitted file (spec §12).
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("a hand-edited model filename cannot leak a URL into status.json",
          "[model_enforcement][status]") {
    Fixture f;
    const ManifestView view(Manifest{}, f.models());  // nothing declared

    DetectionModel evil;
    evil.name = "evil";
    evil.filename = "rtsp://admin:hunter2-SECRET@10.0.0.9/stream/float-small.onnx";
    evil.class_names = {"Small"};
    const auto mid = denso::detection::upsert_model(f.h(), evil);
    REQUIRE(mid.has_value());
    // A second row where the secret sits in the FINAL segment, so stripping the
    // path alone would not remove it — this one must reduce to "<invalid>".
    DetectionModel query;
    query.name = "query";
    query.filename = "https://host/float-big.onnx?token=hunter2-SECRET";
    query.class_names = {"Big"};
    const auto qid = denso::detection::upsert_model(f.h(), query);
    REQUIRE(qid.has_value());
    const int64_t cam = f.add_camera(cred_cam("Line 1"));
    attach_directly(f.h(), cam, *mid);
    attach_directly(f.h(), cam, *qid);

    const auto v = denso::health::evaluate_integrity(
        f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);
    const auto issue = rejection_for(v, cam);
    REQUIRE(issue.has_value());
    CHECK(issue->policy_reason == QStringLiteral("model_undeclared"));

    const QString path = denso::paths::status_file();
    REQUIRE(denso::health::write_status_file(path, v, {}, {}, {},
                                             QStringLiteral("digit_reader"), false));
    QFile sf(path);
    REQUIRE(sf.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(sf.readAll());

    // The plain basename survives; the URL, the userinfo and the host do not.
    CHECK(text.contains(QStringLiteral("float-small.onnx")));
    // Both rows carry their catalog id, so even the fully-reduced one is traceable.
    CHECK(text.contains(QStringLiteral("model #%1").arg(*mid)));
    CHECK(text.contains(QStringLiteral("model #%1").arg(*qid)));
    // The query-string row is not a plain filename, so it reduces entirely.
    CHECK(text.contains(QStringLiteral("<invalid>")));
    CHECK_FALSE(text.contains(QStringLiteral("float-big.onnx")));   // came with a token
    CHECK_FALSE(text.contains(QStringLiteral("rtsp://")));
    CHECK_FALSE(text.contains(QStringLiteral("https://")));
    CHECK_FALSE(text.contains(QStringLiteral("token=")));
    CHECK_FALSE(text.contains(QStringLiteral("@")));
    CHECK_FALSE(text.contains(QStringLiteral("hunter2-SECRET")));
    CHECK_FALSE(text.contains(QStringLiteral("admin")));
    CHECK_FALSE(text.contains(QStringLiteral("10.0.0.9")));
    // The EngineMissing issue on the same row is emitted through the same
    // reduction, so it cannot be the leak either.
    CHECK(v.issues.size() >= 2);
}

// ═════════════════════════════════════════════════════════════════════════════
// Reason-code stability. Both strings are a file format: never renamed, never
// reused, never renumbered.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("issue and policy reason codes are stable strings",
          "[model_enforcement][status]") {
    CHECK(denso::health::reason_code(ZoneIssue::Kind::ModelCompatibilityRejected) ==
          QStringLiteral("model_compatibility_rejected"));
    // The pre-existing kinds are unchanged.
    CHECK(denso::health::reason_code(ZoneIssue::Kind::EngineMissing) ==
          QStringLiteral("engine_missing"));
    CHECK(denso::health::reason_code(ZoneIssue::Kind::EnginesUnmanifested) ==
          QStringLiteral("engines_unmanifested"));

    // Every policy reason the issue can carry, verbatim from the central policy.
    struct Row { denso::models::ModelMetadata md; const char* reason; };
    denso::models::ModelMetadata undeclared;                       // declared=false
    denso::models::ModelMetadata unknown_id;
    unknown_id.declared = true; unknown_id.canonical_id = "nope"; unknown_id.family = "x";
    denso::models::ModelMetadata family_bad;
    family_bad.declared = true; family_bad.canonical_id = "digitv3";
    family_bad.family = "float_ball";
    denso::models::ModelMetadata shape_bad;
    shape_bad.declared = true; shape_bad.canonical_id = "digitv3";
    shape_bad.family = "digit_numeric"; shape_bad.task = "detect"; shape_bad.input_size = 320;
    denso::models::ModelMetadata classes_bad;
    classes_bad.declared = true; classes_bad.canonical_id = "digitv3";
    classes_bad.family = "digit_numeric"; classes_bad.task = "detect";
    classes_bad.input_size = 640; classes_bad.artifact_matches = false;
    denso::models::ModelMetadata prov_bad = classes_bad;
    prov_bad.artifact_matches = true; prov_bad.provenance_ok = false;
    denso::models::ModelMetadata mode_bad;
    mode_bad.declared = true; mode_bad.canonical_id = "float-small";
    mode_bad.family = "float_ball"; mode_bad.task = "detect"; mode_bad.input_size = 640;
    mode_bad.artifact_matches = true; mode_bad.provenance_ok = true;

    const std::vector<Row> rows{
        {undeclared, "model_undeclared"},
        {unknown_id, "model_unknown_id"},
        {family_bad, "model_family_mismatch"},
        {shape_bad, "model_shape_unsupported"},
        {classes_bad, "model_classes_mismatch"},
        {prov_bad, "model_provenance_failed"},
        {mode_bad, "model_mode_incompatible"},
    };
    for (const Row& r : rows) {
        const auto res =
            denso::models::model_compatibility(TargetMode::DigitReader, r.md);
        CHECK_FALSE(res.allowed());
        CHECK(res.reason_code == std::string(r.reason));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// EXISTING behaviour must not move.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("existing integrity behaviour is unchanged by enforcement",
          "[model_enforcement][integrity]") {
    SECTION("a compatible digitv3 camera stays Ready") {
        Fixture f;
        Manifest m;
        m.schema = 2;
        m.generations.push_back(
            declare(f.models(), {"digitv3", "digitv3", "digit_numeric"}));
        const ManifestView view(m, f.models());
        const int64_t mid = f.add_model("digitv3", {"0", "1", "2", "3"});
        const int64_t cam = f.add_camera(cred_cam("Line 1"));
        attach_directly(f.h(), cam, mid);
        const auto v = denso::health::evaluate_integrity(
            f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);
        CHECK(v.status == Readiness::Ready);
        CHECK(v.issues.empty());
    }

    SECTION("EngineMissing still fires for an attached engine absent from disk") {
        Fixture f;
        const ManifestView view(Manifest{}, f.models());  // no manifest → undeclared
        const int64_t mid = f.add_model("gone", {"0"});   // never written to disk
        const int64_t cam = f.add_camera(cred_cam("C"));
        attach_directly(f.h(), cam, mid);
        const auto v = denso::health::evaluate_integrity(
            f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);
        bool missing = false;
        for (const auto& i : v.issues)
            if (i.kind == ZoneIssue::Kind::EngineMissing && i.camera_id == cam)
                missing = true;
        CHECK(missing);
        CHECK(v.status == Readiness::Degraded);
        CHECK(v.blockers.empty());
    }

    SECTION("EnginesUnmanifested stays Degraded and non-blocking") {
        Fixture f;
        // An engine on disk that no manifest describes — today's production Jetson.
        write_and_hash(f.models(), std::string("stray") + kExt,
                       QByteArrayLiteral("bytes"));
        const ManifestView view(Manifest{}, f.models());
        const auto v = denso::health::evaluate_integrity(
            f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);
        bool unmanifested = false;
        for (const auto& i : v.issues)
            if (i.kind == ZoneIssue::Kind::EnginesUnmanifested) unmanifested = true;
        CHECK(unmanifested);
        CHECK(v.status == Readiness::Degraded);
        CHECK(v.blockers.empty());
        CHECK(denso::health::exit_code_for(v.status) == 10);
    }

    SECTION("a malformed manifest is STILL a global blocker") {
        Fixture f;
        QFile bad(QDir(f.models()).filePath(QStringLiteral("manifest.json")));
        REQUIRE(bad.open(QIODevice::WriteOnly));
        REQUIRE(bad.write("{ not json at all") > 0);
        bad.close();
        const ManifestView view(Manifest{}, f.models());
        const auto v = denso::health::evaluate_integrity(
            f.h(), f.models(), TargetMode::DigitReader, view, kPlatform);
        REQUIRE(v.blockers.size() == 1);
        CHECK(v.blockers.at(0).kind ==
              denso::health::GlobalBlocker::Kind::ManifestCorrupt);
        CHECK(v.status == Readiness::Blocked);
        CHECK(denso::health::exit_code_for(v.status) == 78);
    }

    SECTION("the schema is at v14 (Ball Leveler calibration)") {
        Fixture f;
        CHECK(denso::db::supported_schema_version() == 14);
        const auto ver = denso::db::read_user_version(f.h());
        REQUIRE(ver.has_value());
        CHECK(*ver == 14);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// THE RUNTIME PROOF — the real CameraGrid. An inhibited camera constructs NO
// DetectionProcessor and requests NO engine; its healthy sibling does both.
//
// The EngineRegistry allow-list here DELIBERATELY includes the rejected artifact,
// so a get() for it would SUCCEED and be counted. Zero builds therefore proves the
// grid never asked — not merely that the registry refused.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("an inhibited camera builds no processor and requests no engine",
          "[model_enforcement]") {
    Fixture f;
    Manifest m;
    m.schema = 2;
    m.generations.push_back(declare(f.models(), {"digitv3", "digitv3", "digit_numeric"}));
    m.generations.push_back(declare(
        f.models(),
        {"float-small", "float-small", "float_ball", "detect", 640, {"Small"}}));
    write_manifest(f.models(), m);   // CameraGrid re-reads the manifest from models/

    const int64_t digit = f.add_model("digitv3", {"0", "1", "2", "3"});
    const int64_t flt = f.add_model("float-small", {"Small"});
    const int64_t cam_a = f.add_camera(cred_cam("Healthy A"));
    const int64_t cam_b = f.add_camera(cred_cam("Rejected B"));
    attach_directly(f.h(), cam_a, digit);
    attach_directly(f.h(), cam_b, flt);

    BuildLog log;
    auto engines = std::make_shared<EngineRegistry>(
        denso::paths::models_dir().toStdString(),
        denso::paths::trt_cache_dir().toStdString(),
        // BOTH filenames allowed at the registry level on purpose (see above).
        std::set<std::string>{std::string("digitv3") + kExt,
                              std::string("float-small") + kExt},
        // Empty required set: nothing must fail loud here — this case is about
        // what the GRID asks for, not about warm-up.
        std::vector<std::string>{},
        make_factory(log));

    const uint64_t procs_before = DetectionProcessor::constructed_count();
    const uint64_t streams_before = CameraStream::constructed_count();

    CameraGrid grid(f.h(), engines, /*warmup*/ nullptr);
    grid.reload();

    // Camera B: no engine requested for float-small, at all.
    CHECK(count_built(log, "float-small") == 0);
    // Camera A: its engine IS requested — enforcement did not break the good path.
    CHECK(count_built(log, "digitv3") == 1);
    // Exactly ONE DetectionProcessor was built (A's). B produced none.
    CHECK(DetectionProcessor::constructed_count() == procs_before + 1);
    // And exactly ONE CameraStream — B is NOT quietly demoted to an
    // orientation-only stream. A demotion would look like a working camera that
    // has silently stopped reading, which is the failure mode §7.2 forbids by
    // name; without this assertion, dropping the start_one guard would leave B
    // streaming video with no detection and no test would notice.
    CHECK(CameraStream::constructed_count() == streams_before + 1);

    // The cause the grid installed for B is the EXISTING ModelUnavailable bit —
    // no new ZoneCause was spent (spec §7.3), and the bitmask is a file format, so
    // this pins the exact value a status.json consumer will read.
    QFile sf(denso::paths::status_file());
    REQUIRE(sf.open(QIODevice::ReadOnly));
    const QJsonObject status = QJsonDocument::fromJson(sf.readAll()).object();
    const int mask = static_cast<int>(denso::health::ZoneCause::ModelUnavailable);
    CHECK(mask == 2);   // 1u << 1 — unchanged; no new bit was spent
    std::map<QString, int> causes;
    for (const auto c : status.value(QStringLiteral("camera_causes")).toArray())
        causes[c.toObject().value(QStringLiteral("camera_id")).toString()] =
            c.toObject().value(QStringLiteral("causes")).toInt();
    // B carries EXACTLY ModelUnavailable — not a new bit, not a different one.
    REQUIRE(causes.count(QString::number(cam_b)) == 1);
    CHECK(causes.at(QString::number(cam_b)) == mask);
    // A is healthy: no inhibit cause at all.
    CHECK(causes.count(QString::number(cam_a)) == 0);

    grid.teardown();
}

// ═════════════════════════════════════════════════════════════════════════════
// --check END TO END: a directly-attached incompatible model exits 10. Not 1 (a
// warm-up fail-loud), and not 78 (a global blocker).
// ═════════════════════════════════════════════════════════════════════════════
// NOTE the leading word: a Catch2 test name is passed back to the binary as a CLI
// ARGUMENT by catch_discover_tests, so a name starting with "--" is parsed as a
// flag and the case fails with "Unrecognised token" while its logic is fine
// (CLAUDE.md, "Catch2 test names are CLI arguments"). Never start one with a dash.
TEST_CASE("headless check exits 10 for a directly-attached incompatible model",
          "[model_enforcement]") {
    QTemporaryDir data;
    REQUIRE(data.isValid());
    ScopedDataDir guard(data.path().toUtf8());
    REQUIRE(QDir(data.path()).mkpath(QStringLiteral("models")));
    const QString models = denso::paths::models_dir();

    Manifest m;
    m.schema = 2;
    m.generations.push_back(declare(models, {"digitv3", "digitv3", "digit_numeric"}));
    m.generations.push_back(declare(
        models, {"float-small", "float-small", "float_ball", "detect", 640, {"Small"}}));

    // --check opens the REAL database file read-only, so seed one on disk.
    {
        auto db = denso::db::Db::open(denso::paths::db_file());
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));
        const auto flt = denso::detection::upsert_model(
            db->handle(), catalog_row("float-small", {"Small"}));
        REQUIRE(flt.has_value());
        const auto cam = denso::camera::insert(db->handle(), cred_cam("Line 1"));
        REQUIRE(cam.has_value());
        attach_directly(db->handle(), *cam, *flt);
    }

    // The manifest must be VALID on disk — a malformed one would be ManifestCorrupt
    // (78) and would prove nothing about the compatibility path.
    write_manifest(models, m);

    denso::cli::Command cmd;
    cmd.mode = denso::cli::Mode::Check;
    const int code = denso::app::run_headless(cmd);

    CHECK(code == 10);
    CHECK(code != 1);
    CHECK(code != 78);
}
