// Slice 7 — the warm-up firewall. Proves that a model the central compatibility
// policy rejects is NEVER deserialized (skipped by the directory scan, never
// handed to EngineRegistry::get(), never inferred) and NEVER able to abort
// startup (excluded from the fail-loud required set), while an ALLOWED model
// keeps its existing fail-loud behaviour. Two seams make this observable without
// a GPU or a real engine:
//   * loadable_model_files / try_attached_model_filenames — the pure policy chain
//     reducing (catalog × manifest) to the allow-list and the required set;
//   * an injected counting EngineFactory — so warm_up()/get() are exercised with a
//     stub that records which files were built and inferred (no ORT/TensorRT load).
//
// Lives in denso_integration_tests because it constructs EngineRegistry, which is
// backend-coupled (denso_tests is deliberately backend-free). The stub factory
// means no real engine is ever loaded here.
#include <catch2/catch_test_macros.hpp>

#include "db/db.h"
#include "camera/camera.h"
#include "camera/repo.h"
#include "detection/detection.h"
#include "detection/engine_registry.h"
#include "detection/inference_engine.h"
#include "detection/repo.h"
#include "models/compatibility.h"
#include "models/hashing.h"
#include "models/manifest.h"
#include "models/model_identity.h"
#include "mode/mode.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>
#include <QtGlobal>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using denso::mode::TargetMode;
using denso::models::Backend;
using denso::models::Manifest;
using denso::models::ManifestView;
using denso::models::ModelGeneration;
using denso::models::ModelMetadata;
using denso::models::PlatformInfo;
using denso::detection::DetectionModel;

namespace {

// The active backend's model extension — MUST match EngineRegistry::warm_up's
// scan (#ifdef _WIN32) so the files we write are the ones it iterates.
#ifdef _WIN32
constexpr const char* kExt = ".onnx";
#else
constexpr const char* kExt = ".engine";
#endif

// The qualified-platform triple startup.cpp supplies. Read only on the TensorRt
// backend; ignored under ONNX Runtime. Matches the declared built_for below.
// Plain const (NOT constexpr): PlatformInfo holds std::strings and constexpr
// std::string needs GCC 12+, but the Jetson gate builds with gcc11.
const PlatformInfo kPlatform{"10.3", "12.6", "87"};

std::string file_at(const QString& dir, const std::string& name) {
    return QDir(dir).filePath(QString::fromStdString(name)).toStdString();
}

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

// Build a DECLARED schema-2 generation for the ACTIVE backend, writing its
// artifact file(s) into `dir` so provenance corroboration passes. `body` is the
// engine/onnx bytes (content is irrelevant to the stub — only its hash matters).
ModelGeneration declare(const QString& dir, const std::string& id,
                        const std::string& family,
                        const std::vector<std::string>& classes,
                        const QByteArray& body = QByteArrayLiteral("model-bytes")) {
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
    m.filename = id + kExt;  // the active-backend on-disk artifact
    m.class_names = classes;
    return m;
}

// ── the counting stub engine + factory ───────────────────────────────────────
struct BuildLog {
    std::vector<std::string> built;         // paths the factory was asked to build
    std::map<std::string, int> infer_calls; // by path
};

struct StubEngine : denso::ui::InferenceEngine {
    std::string path;
    BuildLog* log = nullptr;
    std::vector<std::string> names{"0"};
    std::vector<denso::ui::Detection> infer(const cv::Mat&) override {
        if (log) log->infer_calls[path]++;
        return {};
    }
    const std::vector<std::string>& class_names() const override { return names; }
};

denso::ui::EngineRegistry::EngineFactory make_factory(BuildLog& log) {
    return [&log](const std::string& path, const std::string&)
               -> std::unique_ptr<denso::ui::InferenceEngine> {
        log.built.push_back(path);
        auto e = std::make_unique<StubEngine>();
        e->path = path;
        e->log = &log;
        return e;
    };
}

// Write an attachment DIRECTLY, bypassing set_camera_models. Slice 8 made that
// function refuse a model the policy rejects, so a REJECTED attachment can no
// longer be seeded through it — which is exactly right: the only way such a row
// exists in the field is a restored backup or a hand-edited database (spec §7.4),
// and that is the state these warm-up cases must exercise.
void attach_directly(const QSqlDatabase& db, int64_t cam, int64_t model_id) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO camera_model (camera_id, model_id) VALUES (?, ?)"));
    q.addBindValue(static_cast<qlonglong>(cam));
    q.addBindValue(static_cast<qlonglong>(model_id));
    REQUIRE(q.exec());
}

int count_built(const BuildLog& log, const std::string& needle) {
    int n = 0;
    for (const auto& p : log.built)
        if (p.find(needle) != std::string::npos) ++n;
    return n;
}

std::set<std::string> allow_from(TargetMode mode, const ManifestView& view,
                                 const std::vector<DetectionModel>& catalog) {
    std::vector<ModelMetadata> metas;
    for (const auto& row : catalog)
        metas.push_back(denso::models::resolve_model_metadata(view, row, kPlatform));
    return denso::models::loadable_model_files(mode, metas);
}

// ── qInfo capture, for the idle-artifact informational-line assertion ─────────
std::vector<QString>* g_log_sink = nullptr;
void capture_handler(QtMsgType, const QMessageLogContext&, const QString& msg) {
    if (g_log_sink) g_log_sink->push_back(msg);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// THE main Slice-7 regression: an unattached rejected artifact on disk is never
// loaded. digitv3 (allowed in digit_reader) is warmed exactly once; float-small
// (declared+valid but wrong-mode) is skipped — zero get(), zero infer().
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("warm-up skips a rejected on-disk artifact entirely", "[warmup]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();

    Manifest m;
    m.schema = 2;
    m.generations.push_back(declare(dir, "digitv3", "digit_numeric", {"0", "1", "2", "3"}));
    m.generations.push_back(declare(dir, "float-small", "float_ball", {"Small"}));
    const ManifestView view(std::move(m), dir);  // production ctor → active backend

    const std::vector<DetectionModel> catalog{
        catalog_row("digitv3", {"0", "1", "2", "3"}),
        catalog_row("float-small", {"Small"}),
    };
    const std::set<std::string> allow = allow_from(TargetMode::DigitReader, view, catalog);

    // The policy excludes float-small (wrong mode); the active-backend digitv3
    // filename is the only allow-list entry.
    REQUIRE(allow == std::set<std::string>{std::string("digitv3") + kExt});

    BuildLog log;
    denso::ui::EngineRegistry reg(dir.toStdString(), file_at(dir, "cache"),
                                  allow, {std::string("digitv3") + kExt},
                                  make_factory(log));
    REQUIRE_NOTHROW(reg.warm_up());  // digitv3 is present + allowed → no fail-loud

    // digitv3: exactly one build + one warm-up inference. float-small: zero.
    CHECK(count_built(log, "digitv3") == 1);
    CHECK(count_built(log, "float-small") == 0);
    CHECK(log.infer_calls[file_at(dir, std::string("digitv3") + kExt)] == 1);
    CHECK(log.infer_calls.count(file_at(dir, std::string("float-small") + kExt)) == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// get(): a request for a filename OUTSIDE the allow-list is a programming error
// (throws); an allowed filename returns an engine.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("get() fails loud outside the allow-list", "[warmup]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();
    // Files need not exist — get() throws before any factory call for a rejected
    // name, and the stub does not read the file for an allowed one.
    BuildLog log;
    denso::ui::EngineRegistry reg(dir.toStdString(), file_at(dir, "cache"),
                                  {std::string("digitv3") + kExt}, {},
                                  make_factory(log));

    REQUIRE_THROWS_AS(reg.get(std::string("float-small") + kExt), std::logic_error);
    CHECK(count_built(log, "float-small") == 0);  // never even attempted

    REQUIRE(reg.get(std::string("digitv3") + kExt) != nullptr);
    CHECK(count_built(log, "digitv3") == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// The fail-loud contract for ALLOWED models is UNCHANGED: an allowed, required
// model that is missing from disk still aborts warm-up. Filtering must not weaken
// this.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("an allowed but missing model still fails loud", "[warmup]") {
    QTemporaryDir tmp;  // exists but empty — digitv3 is NOT written
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();

    BuildLog log;
    denso::ui::EngineRegistry reg(dir.toStdString(), file_at(dir, "cache"),
                                  {std::string("digitv3") + kExt},
                                  {std::string("digitv3") + kExt},
                                  make_factory(log));
    REQUIRE_THROWS(reg.warm_up());        // required digitv3 never warmed → abort
    CHECK(log.built.empty());             // the throw is the required-set check, not a load
}

// ─────────────────────────────────────────────────────────────────────────────
// The required set (try_attached_model_filenames) is mode-filtered: a rejected
// attachment is excluded, so it never enters the fail-loud set and warm-up does
// not throw because of it. An allowed attachment is retained.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("rejected attachment is excluded from the required set", "[warmup]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();

    Manifest m;
    m.schema = 2;
    m.generations.push_back(declare(dir, "digitv3", "digit_numeric", {"0", "1", "2", "3"}));
    m.generations.push_back(declare(dir, "float-small", "float_ball", {"Small"}));
    const ManifestView view(std::move(m), dir);

    auto d = denso::db::Db::open_in_memory();
    REQUIRE(d.has_value());
    REQUIRE(denso::db::run_migrations(d->handle()));

    DetectionModel dm = catalog_row("digitv3", {"0", "1", "2", "3"});
    DetectionModel fm = catalog_row("float-small", {"Small"});
    const auto digit_id = denso::detection::upsert_model(d->handle(), dm);
    const auto float_id = denso::detection::upsert_model(d->handle(), fm);
    REQUIRE(digit_id.has_value());
    REQUIRE(float_id.has_value());

    denso::camera::Camera c;
    c.name = "Cam"; c.camera_type = "usb"; c.active = true; c.index = 0;
    c.width = 640; c.height = 480; c.fps = 30;
    const auto cam = denso::camera::insert(d->handle(), c);
    REQUIRE(cam.has_value());

    SECTION("only a wrong-mode attachment → empty required set, no fail-loud") {
        attach_directly(d->handle(), *cam, *float_id);

        const auto req = denso::detection::try_attached_model_filenames(
            d->handle(), TargetMode::DigitReader, view, kPlatform);
        REQUIRE(req.has_value());
        CHECK(req->empty());  // float-small rejected → not required

        // float-small.engine IS on disk, but the policy excludes it from BOTH the
        // allow-list and the required set → warm-up loads nothing and does NOT throw.
        const std::vector<DetectionModel> catalog{dm, fm};
        const auto allow = allow_from(TargetMode::DigitReader, view, catalog);
        CHECK(allow == std::set<std::string>{std::string("digitv3") + kExt});

        BuildLog log;
        denso::ui::EngineRegistry reg(dir.toStdString(), file_at(dir, "cache"),
                                      allow, *req, make_factory(log));
        REQUIRE_NOTHROW(reg.warm_up());
        CHECK(count_built(log, "float-small") == 0);
    }

    SECTION("allowed digitv3 is retained; wrong-mode float-small excluded") {
        attach_directly(d->handle(), *cam, *digit_id);
        attach_directly(d->handle(), *cam, *float_id);

        const auto req = denso::detection::try_attached_model_filenames(
            d->handle(), TargetMode::DigitReader, view, kPlatform);
        REQUIRE(req.has_value());
        CHECK(*req == std::vector<std::string>{std::string("digitv3") + kExt});
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// The (mode × model) allow-list matrix, driven through resolve + loadable.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("loadable_model_files honours the mode matrix", "[warmup]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();

    Manifest m;
    m.schema = 2;
    m.generations.push_back(declare(dir, "digitv3", "digit_numeric", {"0", "1", "2", "3"}));
    m.generations.push_back(declare(dir, "float-small", "float_ball", {"Small"}));
    m.generations.push_back(declare(dir, "float-big", "float_ball", {"Big"}));
    const ManifestView view(std::move(m), dir);

    const std::vector<DetectionModel> catalog{
        catalog_row("digitv3", {"0", "1", "2", "3"}),
        catalog_row("float-small", {"Small"}),
        catalog_row("float-big", {"Big"}),
    };

    CHECK(allow_from(TargetMode::DigitReader, view, catalog) ==
          std::set<std::string>{std::string("digitv3") + kExt});
    CHECK(allow_from(TargetMode::BallLeveler, view, catalog) ==
          std::set<std::string>{std::string("float-small") + kExt,
                                std::string("float-big") + kExt});
}

// ─────────────────────────────────────────────────────────────────────────────
// Every rejection branch keeps a rejected model out of the allow-list.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("every rejection branch excludes the model from the allow-list", "[warmup]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();

    SECTION("undeclared (no manifest generation)") {
        const ManifestView view(Manifest{}, dir);  // empty manifest
        const std::vector<DetectionModel> catalog{catalog_row("digitv3", {"0", "1", "2", "3"})};
        CHECK(allow_from(TargetMode::DigitReader, view, catalog).empty());
        CHECK(allow_from(TargetMode::BallLeveler, view, catalog).empty());
    }

    SECTION("unknown canonical_id") {
        Manifest m; m.schema = 2;
        m.generations.push_back(declare(dir, "mystery", "mystery", {"0"}));
        const ManifestView view(std::move(m), dir);
        const std::vector<DetectionModel> catalog{catalog_row("mystery", {"0"})};
        CHECK(allow_from(TargetMode::DigitReader, view, catalog).empty());
        CHECK(allow_from(TargetMode::BallLeveler, view, catalog).empty());
    }

    SECTION("metadata (class) mismatch") {
        Manifest m; m.schema = 2;
        m.generations.push_back(declare(dir, "digitv3", "digit_numeric", {"0", "1", "2", "3"}));
        const ManifestView view(std::move(m), dir);
        // Catalog row disagrees with the declared class names → artifact_matches false.
        const std::vector<DetectionModel> catalog{catalog_row("digitv3", {"0", "1"})};
        CHECK(allow_from(TargetMode::DigitReader, view, catalog).empty());
    }

    SECTION("provenance failure (on-disk hash disagrees with the declaration)") {
        Manifest m; m.schema = 2;
        ModelGeneration g = declare(dir, "digitv3", "digit_numeric", {"0", "1", "2", "3"});
        // Corrupt the declared active-backend hash so the on-disk file cannot match.
#ifdef _WIN32
        g.runtime.onnxruntime->model_sha256 = std::string(64, '0');
#else
        g.runtime.tensorrt->engine_sha256 = std::string(64, '0');
#endif
        m.generations.push_back(g);
        const ManifestView view(std::move(m), dir);
        const std::vector<DetectionModel> catalog{catalog_row("digitv3", {"0", "1", "2", "3"})};
        CHECK(allow_from(TargetMode::DigitReader, view, catalog).empty());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Duplicate-filename dedup + empty catalog/allow-list.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("allow-list dedups and an empty catalog loads nothing", "[warmup]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();

    SECTION("empty catalog → empty allow-list; empty allow-list → warm-up loads nothing") {
        const ManifestView view(Manifest{}, dir);
        CHECK(allow_from(TargetMode::DigitReader, view, {}).empty());

        // Put an unrelated artifact on disk; an empty allow-list must skip it.
        (void)write_and_hash(dir, std::string("digitv3") + kExt, QByteArrayLiteral("x"));
        BuildLog log;
        denso::ui::EngineRegistry reg(dir.toStdString(), file_at(dir, "cache"),
                                      std::set<std::string>{}, {}, make_factory(log));
        REQUIRE_NOTHROW(reg.warm_up());
        CHECK(log.built.empty());
    }

    SECTION("duplicate attachments of the same model dedup in the required set") {
        Manifest m; m.schema = 2;
        m.generations.push_back(declare(dir, "digitv3", "digit_numeric", {"0", "1", "2", "3"}));
        const ManifestView view(std::move(m), dir);

        auto d = denso::db::Db::open_in_memory();
        REQUIRE(d.has_value());
        REQUIRE(denso::db::run_migrations(d->handle()));
        const auto id = denso::detection::upsert_model(
            d->handle(), catalog_row("digitv3", {"0", "1", "2", "3"}));
        REQUIRE(id.has_value());

        denso::camera::Camera c;
        c.name = "A"; c.camera_type = "usb"; c.active = true; c.index = 0;
        c.width = 640; c.height = 480; c.fps = 30;
        const auto cam1 = denso::camera::insert(d->handle(), c);
        c.name = "B"; c.index = 1;
        const auto cam2 = denso::camera::insert(d->handle(), c);
        REQUIRE(cam1.has_value());
        REQUIRE(cam2.has_value());
        // digitv3 is ALLOWED here, so this one still goes through the real
        // write path — proving the attachment API still works for a good model.
        denso::detection::CameraModel a; a.camera_id = *cam1; a.model_id = *id;
        denso::detection::CameraModel b; b.camera_id = *cam2; b.model_id = *id;
        REQUIRE(denso::detection::set_camera_models(
            d->handle(), *cam1, {a}, TargetMode::DigitReader, view, kPlatform));
        REQUIRE(denso::detection::set_camera_models(
            d->handle(), *cam2, {b}, TargetMode::DigitReader, view, kPlatform));

        const auto req = denso::detection::try_attached_model_filenames(
            d->handle(), TargetMode::DigitReader, view, kPlatform);
        REQUIRE(req.has_value());
        CHECK(*req == std::vector<std::string>{std::string("digitv3") + kExt});  // one, not two
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// try_attached_model_filenames preserves its non-throwing "empty vs unreadable"
// contract even with the resolution added.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("try_attached_model_filenames stays non-throwing", "[warmup]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const ManifestView view(Manifest{}, tmp.path());

    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db.has_value());
    // No migrations → the join query fails → nullopt, never an exception.
    std::optional<std::vector<std::string>> got;
    REQUIRE_NOTHROW(got = denso::detection::try_attached_model_filenames(
                        db->handle(), TargetMode::DigitReader, view, kPlatform));
    CHECK_FALSE(got.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// The idle-artifact informational line (spec §5.1, warm-up half): a declared,
// valid, unattached wrong-mode artifact on disk is skipped and produces AT MOST
// ONE redaction-safe informational line. Readiness classification (Ready) is
// Slice 8's to assert, not here.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("an idle wrong-mode artifact yields at most one redaction-safe line",
          "[warmup]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();

    Manifest m;
    m.schema = 2;
    m.generations.push_back(declare(dir, "digitv3", "digit_numeric", {"0", "1", "2", "3"}));
    m.generations.push_back(declare(dir, "float-small", "float_ball", {"Small"}));
    const ManifestView view(std::move(m), dir);
    const std::vector<DetectionModel> catalog{
        catalog_row("digitv3", {"0", "1", "2", "3"}),
        catalog_row("float-small", {"Small"}),
    };
    const auto allow = allow_from(TargetMode::DigitReader, view, catalog);

    std::vector<QString> lines;
    g_log_sink = &lines;
    QtMessageHandler prev = qInstallMessageHandler(capture_handler);

    BuildLog log;
    denso::ui::EngineRegistry reg(dir.toStdString(), file_at(dir, "cache"),
                                  allow, {}, make_factory(log));
    reg.warm_up();

    qInstallMessageHandler(prev);
    g_log_sink = nullptr;

    int mentions = 0;
    for (const QString& l : lines) {
        if (l.contains(QStringLiteral("float-small"))) {
            ++mentions;
            // Redaction-safe: a filename, never a credential/URL.
            CHECK_FALSE(l.contains(QStringLiteral("://")));
            CHECK_FALSE(l.contains(QLatin1Char('@')));
        }
    }
    CHECK(mentions <= 1);
    CHECK(count_built(log, "float-small") == 0);  // and it was genuinely skipped
}
