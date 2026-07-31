// Phase B — the ball_leveler CameraGrid runtime branch.
//
// The properties under test: a ball_leveler grid builds Ball machinery and ONLY
// Ball machinery; it asks for exactly the one Float engine its calibration binds
// and never for digitv3; an unconfigured, invalidated or unbindable camera builds
// no measuring pipeline while its siblings keep running; and switching modes
// tears the old pipeline down before the new one starts.
//
// Drives the REAL CameraGrid over the app objects. No real engine is ever loaded
// — the EngineRegistry is given an injected counting factory, so "no engine
// requested" is OBSERVED, not assumed. No camera is contacted: every fixture
// camera points at a dead local port, so the capture thread simply reports
// Offline, which is all these assertions need.
#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/camera_stream.h"
#include "camera/frame_processor.h"    // DetectionProcessor::constructed_count()
#include "camera/level_processor.h"    // BallLevelProcessor::constructed_count()
#include "camera/repo.h"
#include "camera/source_change.h"      // view_revision
#include "db/db.h"
#include "detection/detection.h"
#include "detection/engine_registry.h"
#include "detection/inference_engine.h"
#include "detection/repo.h"
#include "level/calibration.h"
#include "level/repo.h"
#include "mode/config.h"
#include "mode/mode.h"
#include "mode/reset.h"
#include "models/hashing.h"
#include "models/manifest.h"
#include "models/model_identity.h"
#include "paths/paths.h"
#include "platform/platform_info.h"
#include "ui/camera/grid/camera_grid.h"
#include "ui/warmup_state.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using denso::detection::DetectionModel;
using denso::level::LevelCalibration;
using denso::mode::TargetMode;
using denso::models::Manifest;
using denso::models::ManifestView;
using denso::models::ModelGeneration;
using denso::models::PlatformInfo;
using denso::ui::BallLevelProcessor;
using denso::ui::CameraGrid;
using denso::ui::CameraStream;
using denso::ui::DetectionProcessor;
using denso::ui::EngineRegistry;
using denso::ui::WarmupState;

namespace {

#ifdef _WIN32
constexpr const char* kExt = ".onnx";
#else
constexpr const char* kExt = ".engine";
#endif

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

struct Decl {
    std::string stem;
    std::string canonical_id;
    std::string family;
    std::string task = "detect";
    int input_size = 640;
    std::vector<std::string> class_names{"0", "1", "2", "3"};
    denso::models::BuiltFor built_for{"10.3", "12.6", "87"};
};

ModelGeneration declare(const QString& dir, const Decl& d) {
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

QString json_escape(const std::string& s) { return QString::fromStdString(s); }

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
                        i + 1 < m.generations.size() ? QStringLiteral(",") : QString(),
                        QString(64, QLatin1Char('a')));
    }
    out += QStringLiteral("  ]\n}\n");
    QFile f(QDir(models).filePath(QStringLiteral("manifest.json")));
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write(out.toUtf8()) > 0);
}

DetectionModel catalog_row(const std::string& stem,
                           const std::vector<std::string>& classes) {
    DetectionModel m;
    m.name = stem;
    m.filename = stem + kExt;
    m.class_names = classes;
    return m;
}

// ── the counting stub engine + factory (no ORT/TensorRT load) ───────────────
struct BuildLog {
    std::vector<std::string> built;
};

struct StubEngine : denso::ui::InferenceEngine {
    std::vector<std::string> names{"Small"};
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

/// A factory whose every load FAILS (returns null), which is how warm_up()
/// reaches its fail-loud path: the model is never "warmed", so the required-set
/// check at the end throws and WarmupWorker emits `failed` — and, critically,
/// never `finished`.
EngineRegistry::EngineFactory failing_factory(BuildLog& log) {
    return [&log](const std::string& path, const std::string&)
               -> std::unique_ptr<denso::ui::InferenceEngine> {
        log.built.push_back(path);
        return nullptr;
    };
}

/// An engine that CONSTRUCTS fine and then throws from infer(). This is the
/// case get() cannot detect: EngineRegistry caches the engine before warm_up()
/// runs its blank inference, so after that inference throws the cached engine is
/// still there and still non-null.
struct ThrowingEngine : denso::ui::InferenceEngine {
    std::vector<std::string> names{"Small"};
    std::vector<denso::ui::Detection> infer(const cv::Mat&) override {
        throw std::runtime_error("warm-up inference failure");
    }
    const std::vector<std::string>& class_names() const override { return names; }
};

EngineRegistry::EngineFactory throwing_factory(BuildLog& log) {
    return [&log](const std::string& path, const std::string&)
               -> std::unique_ptr<denso::ui::InferenceEngine> {
        log.built.push_back(path);
        return std::make_unique<ThrowingEngine>();
    };
}

/// Run the event loop until `pred` holds or the budget expires. The warm-up
/// worker signals across a thread boundary, so its delivery needs a running
/// loop — sleeping would prove nothing.
template <typename Pred>
bool pump_events_until(Pred pred, int budget_ms = 5000) {
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < budget_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return pred();
}

int count_built(const BuildLog& log, const std::string& needle) {
    int n = 0;
    for (const auto& p : log.built)
        if (p.find(needle) != std::string::npos) ++n;
    return n;
}

/// A camera pointed at a dead local port. Never reachable, so the capture thread
/// reports Offline and no device is ever opened.
denso::camera::Camera dead_cam(const std::string& name) {
    denso::camera::Camera c;
    c.name = name;
    c.camera_type = "ip";
    c.ip = "127.0.0.1";
    c.rtsp = "rtsp://127.0.0.1:9/stream";
    c.channel = 1;
    c.stream = 0;
    c.width = 1280;
    c.height = 720;
    c.fps = 25;
    c.active = true;
    c.setup_complete = true;
    c.areas_need_review = false;
    return c;
}

LevelCalibration good_calibration() {
    LevelCalibration c;
    c.rect_x = 0.3;
    c.rect_y = 0.1;
    c.rect_w = 0.4;
    c.rect_h = 0.8;
    c.y_100 = 0.2;
    c.y_0 = 0.8;
    c.conf = 0.5;
    c.hold_ms = 2000;
    return c;
}

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

    /// Declare digitv3 + float-small, both valid for their own mode.
    void declare_both() {
        Manifest m;
        m.schema = 2;
        m.generations.push_back(
            declare(models(), {"digitv3", "digitv3", "digit_numeric"}));
        m.generations.push_back(declare(
            models(),
            {"float-small", "float-small", "float_ball", "detect", 640, {"Small"}}));
        write_manifest(models(), m);
    }

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
    /// The view fingerprint the runtime will compare against, read back from the
    /// stored row so it is exactly what the grid will compute.
    std::string revision_of(int64_t camera_id) {
        const auto cam = denso::camera::get(h(), camera_id);
        REQUIRE(cam.has_value());
        return denso::camera::view_revision(*cam);
    }
    /// Save through the ONE Ball write chokepoint.
    void calibrate(int64_t camera_id, int64_t model_id, int class_id,
                   const LevelCalibration& c) {
        const ManifestView view = denso::models::load_manifest_view(models());
        denso::level::SaveRefusal refusal;
        const bool ok = denso::level::save_level_configuration(
            h(), camera_id, {{model_id, {class_id}}}, c, revision_of(camera_id),
            view, kPlatform, &refusal);
        INFO("refusal reason: " << refusal.reason_code);
        REQUIRE(ok);
    }
};

/// A registry whose allow-list DELIBERATELY holds BOTH artifacts, so a get() for
/// either would succeed and be counted. Zero builds therefore proves the grid
/// never asked, rather than that the registry refused.
std::shared_ptr<EngineRegistry> permissive_registry(BuildLog& log) {
    return std::make_shared<EngineRegistry>(
        denso::paths::models_dir().toStdString(),
        denso::paths::trt_cache_dir().toStdString(),
        std::set<std::string>{std::string("digitv3") + kExt,
                              std::string("float-small") + kExt},
        std::vector<std::string>{}, make_factory(log));
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// A configured Ball camera measures, and asks for exactly ONE Float engine.
// ═══════════════════════════════════════════════════════════════════════════
TEST_CASE("a configured ball camera builds one BallLevelProcessor and requests "
          "only its Float engine", "[ball_runtime]") {
    Fixture f;
    f.declare_both();
    const int64_t flt = f.add_model("float-small", {"Small"});
    f.add_model("digitv3", {"0", "1", "2", "3"});
    const int64_t cam = f.add_camera(dead_cam("Tank A"));
    f.calibrate(cam, flt, 0, good_calibration());
    REQUIRE(denso::mode::switch_mode(f.h(), TargetMode::BallLeveler).ok);

    BuildLog log;
    auto engines = permissive_registry(log);
    const uint64_t balls_before = BallLevelProcessor::constructed_count();
    const uint64_t dets_before = DetectionProcessor::constructed_count();

    CameraGrid grid(f.h(), engines, /*warmup*/ nullptr);
    grid.reload();

    CHECK(BallLevelProcessor::constructed_count() == balls_before + 1);
    // The Float engine IS requested — exactly once, for the one camera.
    CHECK(count_built(log, "float-small") == 1);
    // digitv3 is NEVER requested in ball_leveler, even though the registry would
    // happily have built it.
    CHECK(count_built(log, "digitv3") == 0);
    // And no digit machinery exists at all.
    CHECK(DetectionProcessor::constructed_count() == dets_before);

    grid.teardown();
}

// ═══════════════════════════════════════════════════════════════════════════
// The three not-measuring states each build NO measuring pipeline.
// ═══════════════════════════════════════════════════════════════════════════
TEST_CASE("an unconfigured ball camera builds no measuring pipeline",
          "[ball_runtime]") {
    Fixture f;
    f.declare_both();
    f.add_model("float-small", {"Small"});
    const int64_t cam = f.add_camera(dead_cam("Never calibrated"));
    (void)cam;
    REQUIRE(denso::mode::switch_mode(f.h(), TargetMode::BallLeveler).ok);

    BuildLog log;
    auto engines = permissive_registry(log);
    const uint64_t before = BallLevelProcessor::constructed_count();
    const uint64_t streams_before = CameraStream::constructed_count();

    CameraGrid grid(f.h(), engines, nullptr);
    grid.reload();

    CHECK(BallLevelProcessor::constructed_count() == before);
    CHECK(log.built.empty());   // no engine requested at all
    // It IS shown, though — an uncalibrated camera must appear on the wall
    // reporting Unconfigured, not silently vanish from the grid.
    CHECK(CameraStream::constructed_count() == streams_before + 1);

    grid.teardown();
}

TEST_CASE("a calibration drawn against a different view builds no measuring "
          "pipeline", "[ball_runtime]") {
    Fixture f;
    f.declare_both();
    const int64_t flt = f.add_model("float-small", {"Small"});
    const int64_t cam = f.add_camera(dead_cam("Rotated since"));
    f.calibrate(cam, flt, 0, good_calibration());

    // A view-significant edit: the stored geometry now refers to a DIFFERENT
    // physical view, so it must not be measured against.
    auto stored = denso::camera::get(f.h(), cam);
    REQUIRE(stored.has_value());
    stored->rotation = 90;
    REQUIRE(denso::camera::update(f.h(), *stored));
    REQUIRE(denso::mode::switch_mode(f.h(), TargetMode::BallLeveler).ok);

    BuildLog log;
    auto engines = permissive_registry(log);
    const uint64_t before = BallLevelProcessor::constructed_count();

    CameraGrid grid(f.h(), engines, nullptr);
    grid.reload();

    CHECK(BallLevelProcessor::constructed_count() == before);
    CHECK(log.built.empty());
    // The operator's work is NOT deleted — only paused.
    CHECK(denso::level::level_config_for(f.h(), cam).has_value());

    grid.teardown();
}

TEST_CASE("a ball camera bound to a wrong-mode model builds no measuring "
          "pipeline", "[ball_runtime]") {
    Fixture f;
    f.declare_both();
    const int64_t flt = f.add_model("float-small", {"Small"});
    const int64_t digit = f.add_model("digitv3", {"0", "1", "2", "3"});
    const int64_t cam = f.add_camera(dead_cam("Hand-edited"));
    f.calibrate(cam, flt, 0, good_calibration());

    // Repoint the binding at digitv3 DIRECTLY, bypassing the write chokepoint —
    // the restored-backup / hand-edited-database path. The chokepoint would have
    // refused this, which is exactly why read-time enforcement must catch it.
    {
        QSqlQuery q(f.h());
        q.prepare(QStringLiteral(
            "UPDATE ball_level_calibration SET model_id = ? WHERE camera_id = ?"));
        q.addBindValue(static_cast<qlonglong>(digit));
        q.addBindValue(static_cast<qlonglong>(cam));
        REQUIRE(q.exec());
    }
    REQUIRE(denso::mode::switch_mode(f.h(), TargetMode::BallLeveler).ok);

    BuildLog log;
    auto engines = permissive_registry(log);
    const uint64_t before = BallLevelProcessor::constructed_count();

    CameraGrid grid(f.h(), engines, nullptr);
    grid.reload();

    CHECK(BallLevelProcessor::constructed_count() == before);
    // digitv3 was never requested, even though the registry would have built it.
    CHECK(count_built(log, "digitv3") == 0);

    grid.teardown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Isolation: one camera's problem is one camera's problem.
// ═══════════════════════════════════════════════════════════════════════════
TEST_CASE("one unconfigured ball camera does not stop its calibrated sibling",
          "[ball_runtime]") {
    Fixture f;
    f.declare_both();
    const int64_t flt = f.add_model("float-small", {"Small"});
    const int64_t good = f.add_camera(dead_cam("Calibrated"));
    const int64_t bad = f.add_camera(dead_cam("Not calibrated"));
    (void)bad;
    f.calibrate(good, flt, 0, good_calibration());
    REQUIRE(denso::mode::switch_mode(f.h(), TargetMode::BallLeveler).ok);

    BuildLog log;
    auto engines = permissive_registry(log);
    const uint64_t balls_before = BallLevelProcessor::constructed_count();
    const uint64_t streams_before = CameraStream::constructed_count();

    CameraGrid grid(f.h(), engines, nullptr);
    grid.reload();

    // The healthy sibling measures...
    CHECK(BallLevelProcessor::constructed_count() == balls_before + 1);
    CHECK(count_built(log, "float-small") == 1);
    // ...and BOTH are on the wall.
    CHECK(CameraStream::constructed_count() == streams_before + 2);

    grid.teardown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Mode purity in the other direction, and across a switch.
// ═══════════════════════════════════════════════════════════════════════════
TEST_CASE("digit_reader never builds ball machinery even with a stored "
          "calibration", "[ball_runtime]") {
    Fixture f;
    f.declare_both();
    const int64_t flt = f.add_model("float-small", {"Small"});
    const int64_t cam = f.add_camera(dead_cam("Dual-configured"));
    f.calibrate(cam, flt, 0, good_calibration());
    // Mode stays digit_reader (the default) — the Ball row is dormant.
    REQUIRE(denso::mode::load(f.h()) == TargetMode::DigitReader);

    BuildLog log;
    auto engines = permissive_registry(log);
    const uint64_t balls_before = BallLevelProcessor::constructed_count();

    CameraGrid grid(f.h(), engines, nullptr);
    grid.reload();

    CHECK(BallLevelProcessor::constructed_count() == balls_before);
    // No Float engine is requested in digit_reader, dormant calibration or not.
    CHECK(count_built(log, "float-small") == 0);

    grid.teardown();
}

TEST_CASE("switching modes tears the old pipeline down before the new one starts",
          "[ball_runtime]") {
    Fixture f;
    f.declare_both();
    const int64_t flt = f.add_model("float-small", {"Small"});
    const int64_t digit = f.add_model("digitv3", {"0", "1", "2", "3"});
    const int64_t cam = f.add_camera(dead_cam("Both ways"));
    f.calibrate(cam, flt, 0, good_calibration());
    // Also give it a digit attachment, so the digit path has real work to do.
    {
        QSqlQuery q(f.h());
        q.prepare(QStringLiteral(
            "INSERT INTO camera_model (camera_id, model_id) VALUES (?, ?)"));
        q.addBindValue(static_cast<qlonglong>(cam));
        q.addBindValue(static_cast<qlonglong>(digit));
        REQUIRE(q.exec());
    }

    BuildLog log;
    auto engines = permissive_registry(log);
    CameraGrid grid(f.h(), engines, nullptr);

    // ── digit_reader first ──────────────────────────────────────────────────
    grid.reload();
    CHECK(count_built(log, "digitv3") == 1);
    CHECK(count_built(log, "float-small") == 0);
    CHECK(grid.has_live_streams());
    const uint64_t gen_before = grid.generation();

    // ── the switch: teardown FIRST, exactly as MainWindow::perform_switch does ─
    grid.teardown();
    CHECK_FALSE(grid.has_live_streams());
    // Every authoritative teardown advances the generation, which is what makes
    // a callback captured by the old pipeline droppable.
    CHECK(grid.generation() > gen_before);

    // set_engines is legal now, and only now.
    BuildLog ball_log;
    auto ball_engines = permissive_registry(ball_log);
    grid.set_engines(ball_engines, nullptr);

    REQUIRE(denso::mode::switch_mode(f.h(), TargetMode::BallLeveler).ok);
    const uint64_t balls_before = BallLevelProcessor::constructed_count();
    const uint64_t dets_before = DetectionProcessor::constructed_count();

    grid.reload();

    // The destination mode's pipeline, and only it.
    CHECK(BallLevelProcessor::constructed_count() == balls_before + 1);
    CHECK(DetectionProcessor::constructed_count() == dets_before);
    CHECK(count_built(ball_log, "float-small") == 1);
    CHECK(count_built(ball_log, "digitv3") == 0);

    grid.teardown();
}

TEST_CASE("the engine registry cannot be replaced under a live pipeline",
          "[ball_runtime]") {
    Fixture f;
    f.declare_both();
    const int64_t digit = f.add_model("digitv3", {"0", "1", "2", "3"});
    const int64_t cam = f.add_camera(dead_cam("Streaming"));
    {
        QSqlQuery q(f.h());
        q.prepare(QStringLiteral(
            "INSERT INTO camera_model (camera_id, model_id) VALUES (?, ?)"));
        q.addBindValue(static_cast<qlonglong>(cam));
        q.addBindValue(static_cast<qlonglong>(digit));
        REQUIRE(q.exec());
    }

    BuildLog log;
    auto engines = permissive_registry(log);
    CameraGrid grid(f.h(), engines, nullptr);
    grid.reload();
    REQUIRE(grid.has_live_streams());

    // A live inference worker holds a raw engine pointer owned by THIS registry.
    // Swapping underneath it would dangle that pointer, so the swap must refuse.
    BuildLog other_log;
    auto other = permissive_registry(other_log);
    grid.set_engines(other, nullptr);

    REQUIRE(denso::mode::switch_mode(f.h(), TargetMode::BallLeveler).ok);
    grid.reload();
    // The refused registry was never adopted: the reload used the ORIGINAL one,
    // so the replacement's log stayed empty.
    CHECK(other_log.built.empty());

    grid.teardown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase B review, blocking finding: a FAILED warm-up is terminal.
//
// WarmupWorker emits `failed` and returns WITHOUT `finished`, so the pending
// gate had no terminating edge. That was invisible while a warm-up failure meant
// app.exit(1) at boot — but a post-switch failure is deliberately NOT fatal, so
// every camera waiting on that model sat on "Preparing model..." for the life of
// the process instead of reporting Unavailable.
// ═══════════════════════════════════════════════════════════════════════════
TEST_CASE("a failed warm-up releases the cameras waiting on it", "[ball_runtime]") {
    Fixture f;
    f.declare_both();
    const int64_t flt = f.add_model("float-small", {"Small"});
    const int64_t cam = f.add_camera(dead_cam("Tank"));
    f.calibrate(cam, flt, 0, good_calibration());
    REQUIRE(denso::mode::switch_mode(f.h(), TargetMode::BallLeveler).ok);

    // Every load fails, and the Float engine is REQUIRED — so warm_up() throws.
    BuildLog log;
    auto engines = std::make_shared<EngineRegistry>(
        denso::paths::models_dir().toStdString(),
        denso::paths::trt_cache_dir().toStdString(),
        std::set<std::string>{std::string("float-small") + kExt},
        std::vector<std::string>{std::string("float-small") + kExt},
        failing_factory(log));

    WarmupState warmup(engines);
    CameraGrid grid(f.h(), engines, &warmup);

    const uint64_t balls_before = BallLevelProcessor::constructed_count();
    grid.reload();
    // The camera is WAITING: enrolled in the pending gate, so nothing streams yet
    // and no measuring pipeline exists.
    CHECK_FALSE(grid.has_live_streams());
    CHECK(BallLevelProcessor::constructed_count() == balls_before);

    warmup.start();
    // The terminal property itself: a failure completes warm-up. Without it the
    // camera below re-enrols instead of falling through, and waits forever.
    REQUIRE(pump_events_until([&warmup] { return warmup.is_complete(); }));
    CHECK_FALSE(warmup.is_ready(std::string("float-small") + kExt));

    // MainWindow's post-switch failure handler calls exactly this.
    grid.settle_pending_after_warmup();

    // The camera resolved instead of waiting: it is on the wall reporting a state
    // (a display-only stream), and it did NOT get a measuring pipeline, because
    // its engine never loaded.
    CHECK(grid.has_live_streams());
    CHECK(BallLevelProcessor::constructed_count() == balls_before);

    grid.teardown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase B review, blocking finding: the OUTGOING coordinator must be retired.
//
// The boot WarmupState is owned by ui::launch, not by the window, so a switch
// cannot destroy it — and boot wires its `failed` to app.exit(1). Left connected,
// an outgoing-mode warm-up failure arriving after a committed switch takes a
// working appliance dark, contrary to spec §7.3/§7.5.
// ═══════════════════════════════════════════════════════════════════════════
TEST_CASE("a retired warm-up coordinator drives nothing", "[ball_runtime]") {
    Fixture f;
    f.declare_both();

    BuildLog log;
    auto engines = std::make_shared<EngineRegistry>(
        denso::paths::models_dir().toStdString(),
        denso::paths::trt_cache_dir().toStdString(),
        std::set<std::string>{std::string("float-small") + kExt},
        std::vector<std::string>{std::string("float-small") + kExt},
        failing_factory(log));

    WarmupState warmup(engines);
    int failures_seen = 0;
    QObject::connect(&warmup, &WarmupState::failed,
                     [&failures_seen](const QString&) { ++failures_seen; });

    // Baseline: the subscription really is live, so the check after retirement
    // is testing the disconnect and not a wiring mistake.
    emit warmup.failed(QStringLiteral("boom"));
    REQUIRE(failures_seen == 1);

    warmup.retire();

    // Nothing this coordinator reports can reach its subscribers any more — which
    // is what keeps a stale `failed` away from app.exit(1).
    emit warmup.failed(QStringLiteral("boom again"));
    CHECK(failures_seen == 1);

    // ...and a retired coordinator never starts a worker, so it cannot go on
    // deserializing the outgoing mode's engines after the switch.
    warmup.start();
    CHECK(pump_events_until([] { return false; }, 200) == false);
    CHECK(log.built.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase B review round 2, blocking finding: a CACHED engine is not a warm one.
//
// EngineRegistry::get() caches the engine it constructs BEFORE warm_up() runs
// the blank inference on it. So when that inference throws, a later get() hands
// back a perfectly non-null engine for a plan that never warmed — and the grid
// would build a measuring pipeline on it. Warm-up completing without marking the
// model ready is the authoritative answer, and the grid must take it.
// ═══════════════════════════════════════════════════════════════════════════
TEST_CASE("a model whose warm-up inference threw does not measure",
          "[ball_runtime]") {
    Fixture f;
    f.declare_both();
    const int64_t flt = f.add_model("float-small", {"Small"});
    const int64_t cam = f.add_camera(dead_cam("Tank"));
    f.calibrate(cam, flt, 0, good_calibration());
    REQUIRE(denso::mode::switch_mode(f.h(), TargetMode::BallLeveler).ok);

    BuildLog log;
    auto engines = std::make_shared<EngineRegistry>(
        denso::paths::models_dir().toStdString(),
        denso::paths::trt_cache_dir().toStdString(),
        std::set<std::string>{std::string("float-small") + kExt},
        std::vector<std::string>{std::string("float-small") + kExt},
        throwing_factory(log));

    WarmupState warmup(engines);
    CameraGrid grid(f.h(), engines, &warmup);

    const uint64_t balls_before = BallLevelProcessor::constructed_count();
    grid.reload();
    CHECK_FALSE(grid.has_live_streams());   // waiting on the model

    warmup.start();
    REQUIRE(pump_events_until([&warmup] { return warmup.is_complete(); }));
    // The engine was CONSTRUCTED — this is exactly the state in which get() lies.
    REQUIRE_FALSE(log.built.empty());
    CHECK_FALSE(warmup.is_ready(std::string("float-small") + kExt));

    grid.settle_pending_after_warmup();

    // Resolved to a display-only stream, NOT to a measuring pipeline built on a
    // plan whose warm-up inference threw.
    CHECK(grid.has_live_streams());
    CHECK(BallLevelProcessor::constructed_count() == balls_before);

    grid.teardown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase B review round 3: the DIGIT path needs the same gate.
//
// settle_pending_after_warmup() drains BOTH build paths, so a terminal warm-up
// failure now reaches start_one() too — where the cached-but-unwarmed engine
// would either back a real DetectionProcessor or, on a null load, be demoted to
// a silent orientation-only stream that hides the missing detection.
// ═══════════════════════════════════════════════════════════════════════════
TEST_CASE("a digit camera whose model did not warm up is not started",
          "[ball_runtime]") {
    Fixture f;
    f.declare_both();
    const int64_t digit = f.add_model("digitv3", {"0", "1", "2", "3"});
    const int64_t cam = f.add_camera(dead_cam("Line 1"));
    {
        QSqlQuery q(f.h());
        q.prepare(QStringLiteral(
            "INSERT INTO camera_model (camera_id, model_id) VALUES (?, ?)"));
        q.addBindValue(static_cast<qlonglong>(cam));
        q.addBindValue(static_cast<qlonglong>(digit));
        REQUIRE(q.exec());
    }
    // digit_reader is the committed mode here — this is the unchanged pipeline.

    BuildLog log;
    auto engines = std::make_shared<EngineRegistry>(
        denso::paths::models_dir().toStdString(),
        denso::paths::trt_cache_dir().toStdString(),
        std::set<std::string>{std::string("digitv3") + kExt},
        std::vector<std::string>{std::string("digitv3") + kExt},
        throwing_factory(log));

    WarmupState warmup(engines);
    CameraGrid grid(f.h(), engines, &warmup);

    const uint64_t dets_before = DetectionProcessor::constructed_count();
    grid.reload();
    CHECK_FALSE(grid.has_live_streams());   // waiting on its model

    warmup.start();
    REQUIRE(pump_events_until([&warmup] { return warmup.is_complete(); }));
    REQUIRE_FALSE(log.built.empty());       // constructed, so get() would lie
    CHECK_FALSE(warmup.is_ready(std::string("digitv3") + kExt));

    grid.settle_pending_after_warmup();

    // Not started at all: no DetectionProcessor on an unwarmed plan, and no
    // orientation-only stream quietly standing in for the missing detection.
    CHECK(DetectionProcessor::constructed_count() == dets_before);
    CHECK_FALSE(grid.has_live_streams());

    grid.teardown();
}
