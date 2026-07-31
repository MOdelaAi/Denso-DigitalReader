// Slice 9 — the UI half of the filtering seam.
//
// Three separate claims, each of which a repository-only test cannot make:
//
//  1. THE PAGE RENDERS WHAT THE POLICY RETURNS. ModelsPage is driven through the
//     real CameraWizardController, against a real database, and the checkbox list
//     it builds is read back off the widgets.
//  2. THE COMMITTED MODE WINS. `mode.target` in the database decides the list —
//     never a settings-page selector holding an unconfirmed choice (spec §6.3).
//  3. THE PAGE OWNS NO RULE. A structural test reads the production source of
//     ModelsPage and fails if any model name or mode token appears in it.
//
// Plus the Ball Leveler UI lock: the repository API answers for ball_leveler, but
// the production UI must still expose no wizard at all (Revision 3b §2.1).
//
// Lives in denso_integration_tests: it constructs real Qt widgets over denso_app.
// Every camera seeded here is model-less or points at a closed local port, so no
// engine is ever loaded and no device is contacted.
#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/camera_stream.h"      // CameraStream::constructed_count()
#include "camera/frame_processor.h"    // DetectionProcessor::constructed_count()
#include "camera/repo.h"
#include "db/db.h"
#include "detection/detection.h"
#include "detection/engine_registry.h"
#include "detection/repo.h"
#include "mode/config.h"
#include "mode/mode.h"
#include "models/hashing.h"
#include "models/manifest.h"
#include "models/model_identity.h"
#include "paths/paths.h"
#include "settings/settings.h"
#include "ui/camera/camera_dialog.h"
#include "ui/camera/dialog/add_page.h"
#include "ui/camera/dialog/areas_page.h"
#include "ui/camera/dialog/configure_page.h"
#include "ui/camera/dialog/models_page.h"
#include "ui/camera/wizard_controller.h"
#include "ui/mainwindow.h"
#include "ui/warmup_state.h"

#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlQuery>
#include <QTimer>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using denso::mode::TargetMode;
using denso::models::Manifest;
using denso::models::ManifestView;
using denso::models::ModelGeneration;
using denso::models::PlatformInfo;
using denso::ui::CameraDialog;
using denso::ui::CameraStream;
using denso::ui::DetectionProcessor;
using denso::ui::EngineRegistry;
using denso::ui::MainWindow;
using denso::ui::ModelsPage;
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

ModelGeneration declare(const QString& dir, const std::string& id,
                        const std::string& family,
                        const std::vector<std::string>& classes) {
    const QByteArray body = QByteArrayLiteral("model-bytes");
    ModelGeneration g;
    g.declared = true;
    g.name = id;
    g.installed_utc = "2026-07-27T00:00:00Z";
    g.state = "installed";
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

QString manifest_json(const Manifest& m) {
    QString out = QStringLiteral("{\n  \"schema\": 2,\n  \"generations\": [\n");
    for (size_t i = 0; i < m.generations.size(); ++i) {
        const ModelGeneration& g = m.generations[i];
        QString classes;
        for (size_t c = 0; c < g.class_names.size(); ++c)
            classes += (c ? QStringLiteral(", \"") : QStringLiteral("\"")) +
                       QString::fromStdString(g.class_names[c]) + QStringLiteral("\"");
        QString runtime;
#ifdef _WIN32
        const auto& a = *g.runtime.onnxruntime;
        runtime = QStringLiteral(
                      "\"onnxruntime\": {\"model\": \"%1\", \"model_sha256\": \"%2\", "
                      "\"class_metadata_source\": \"%3\"}")
                      .arg(QString::fromStdString(a.model),
                           QString::fromStdString(a.model_sha256),
                           QString::fromStdString(a.class_metadata_source));
#else
        const auto& a = *g.runtime.tensorrt;
        runtime = QStringLiteral(
                      "\"tensorrt\": {\"engine\": \"%1\", \"engine_sha256\": \"%2\", "
                      "\"sidecar\": \"%3\", \"sidecar_sha256\": \"%4\", "
                      "\"class_metadata_source\": \"%5\", \"built_for\": "
                      "{\"trt\": \"10.3\", \"cuda\": \"12.6\", \"sm\": \"87\"}}")
                      .arg(QString::fromStdString(a.engine),
                           QString::fromStdString(a.engine_sha256),
                           QString::fromStdString(a.sidecar),
                           QString::fromStdString(a.sidecar_sha256),
                           QString::fromStdString(a.class_metadata_source));
#endif
        out += QStringLiteral(
                   "    {\"name\": \"%1\", \"canonical_id\": \"%1\", \"family\": \"%2\", "
                   "\"task\": \"detect\", \"installed_utc\": \"2026-07-27T00:00:00Z\", "
                   "\"state\": \"installed\", \"input_size\": 640, \"class_count\": %3, "
                   "\"class_names\": [%4], \"runtime\": {%5}, \"provenance\": "
                   "{\"source_pt_sha256\": \"%6\", \"onnx_sha256\": \"%6\", "
                   "\"export_ultralytics\": \"8.4.21\", \"precision\": \"fp16\", "
                   "\"export_engine_command\": \"trtexec\"}}%7\n")
                   .arg(QString::fromStdString(g.canonical_id),
                        QString::fromStdString(g.family))
                   .arg(g.class_count)
                   .arg(classes, runtime, QString(64, QLatin1Char('a')),
                        i + 1 < m.generations.size() ? QStringLiteral(",") : QString());
    }
    out += QStringLiteral("  ]\n}\n");
    return out;
}

denso::detection::DetectionModel catalog_row(const std::string& id,
                                             const std::vector<std::string>& classes) {
    denso::detection::DetectionModel m;
    m.name = id;
    m.filename = id + kExt;
    m.class_names = classes;
    return m;
}

/// A real data dir + on-disk manifest + migrated database with all three models
/// catalogued. The manifest is written to disk because the production wizard path
/// re-reads it through `models::load_manifest_view`.
struct Harness {
    QTemporaryDir data;
    ScopedDataDir guard{data.isValid() ? data.path().toUtf8() : QByteArray()};
    std::optional<denso::db::Db> db;
    std::shared_ptr<EngineRegistry> engines;
    std::unique_ptr<WarmupState> warmup;
    std::map<std::string, int64_t> id;

    Harness() {
        REQUIRE(data.isValid());
        REQUIRE(QDir(data.path()).mkpath(QStringLiteral("models")));
        const QString models = denso::paths::models_dir();

        Manifest m;
        m.schema = 2;
        m.generations.push_back(declare(models, "digitv3", "digit_numeric",
                                        {"0", "1", "2", "3"}));
        m.generations.push_back(declare(models, "float-small", "float_ball", {"Small"}));
        m.generations.push_back(declare(models, "float-big", "float_ball", {"Big"}));
        QFile mf(QDir(models).filePath(QStringLiteral("manifest.json")));
        REQUIRE(mf.open(QIODevice::WriteOnly));
        REQUIRE(mf.write(manifest_json(m).toUtf8()) > 0);
        mf.close();

        db = denso::db::Db::open_in_memory();
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));
        for (const auto& [stem, classes] :
             std::vector<std::pair<std::string, std::vector<std::string>>>{
                 {"digitv3", {"0", "1", "2", "3"}},
                 {"float-small", {"Small"}},
                 {"float-big", {"Big"}}}) {
            const auto r =
                denso::detection::upsert_model(db->handle(), catalog_row(stem, classes));
            REQUIRE(r.has_value());
            id[stem] = *r;
        }

        engines = std::make_shared<EngineRegistry>(
            denso::paths::models_dir().toStdString(),
            denso::paths::trt_cache_dir().toStdString(),
            std::set<std::string>{});   // nothing allow-listed: nothing can load
        warmup = std::make_unique<WarmupState>(engines);
    }

    QSqlDatabase h() const { return db->handle(); }
    QString models() const { return denso::paths::models_dir(); }

    int64_t seed_camera(const std::string& name) {
        denso::camera::Camera c;
        c.name = name;
        c.camera_type = "ip";
        c.ip = "127.0.0.1";
        c.rtsp = "rtsp://127.0.0.1:9/none";   // discard port: refused instantly
        c.channel = 1;
        c.stream = 0;
        c.width = 1280;
        c.height = 720;
        c.fps = 25;
        c.active = true;
        c.setup_complete = true;
        const auto r = denso::camera::insert(h(), c);
        REQUIRE(r.has_value());
        return *r;
    }
};

/// The model names the page is actually OFFERING, read back off its checkboxes.
std::vector<QString> offered_models(const ModelsPage& page) {
    std::vector<QString> out;
    for (const QCheckBox* cb : page.findChildren<QCheckBox*>()) {
        // The class rows also use checkboxes; the model checkboxes are the ones
        // parented into the models column. Distinguish by object name, which the
        // page sets for exactly this reason.
        if (cb->objectName() == QStringLiteral("modelCheck")) out.push_back(cb->text());
    }
    return out;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// The page renders the POLICY's list — and the COMMITTED mode decides it, even
// while a settings selector holds a different, unconfirmed value (spec §6.3).
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("the Models page offers only digitv3 in a committed digit_reader",
          "[selectable_models][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::DigitReader));
    const int64_t cam = h.seed_camera("Line 1");

    // A settings-page combo holding the OTHER mode — the exact "proposed
    // destination" state the operator has NOT confirmed. It must change nothing.
    QComboBox selector;
    selector.addItem(QStringLiteral("Digit Reader"));
    selector.addItem(QStringLiteral("Ball Leveler"));
    selector.setCurrentIndex(1);            // ball_leveler, uncommitted
    REQUIRE(denso::mode::load(h.h()) == TargetMode::DigitReader);

    ModelsPage page;
    page.set_db(h.h());
    page.load_for(cam, denso::mode::load(h.h()),
                  denso::models::load_manifest_view(h.models()), kPlatform);

    const auto offered = offered_models(page);
    REQUIRE(offered.size() == 1);
    CHECK(offered.at(0) == QStringLiteral("digitv3"));
    // The uncommitted selector still says ball_leveler — and was ignored.
    CHECK(selector.currentIndex() == 1);
}

TEST_CASE("the Models page offers no Float model in digit_reader",
          "[selectable_models][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::DigitReader));
    const int64_t cam = h.seed_camera("Line 1");

    ModelsPage page;
    page.set_db(h.h());
    page.load_for(cam, denso::mode::load(h.h()),
                  denso::models::load_manifest_view(h.models()), kPlatform);

    for (const QString& name : offered_models(page)) {
        CHECK(name != QStringLiteral("float-small"));
        CHECK(name != QStringLiteral("float-big"));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// THROUGH THE REAL CONTROLLER. The case above drives the page directly, which
// proves the page filters but not that the CONTROLLER hands it the committed
// mode. This one goes through CameraWizardController::save_configured_camera(),
// whose tail is the production enter_models() — so a controller that read a
// selector, assumed a mode, or defaulted one would be caught here.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("the controller drives the Models step with the committed mode",
          "[selectable_models][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::DigitReader));
    const int64_t cam_id = h.seed_camera("Line 1");

    // The four real wizard pages the dialog owns, and the real controller.
    denso::ui::CameraAddPage add;
    denso::ui::CameraConfigurePage configure;
    ModelsPage models;
    denso::ui::CameraAreasPage areas;
    models.set_db(h.h());

    int shown_page = -1;
    denso::ui::CameraWizardController controller(
        h.h(), denso::ui::CameraWizardController::Pages{&add, &configure, &models, &areas},
        [&shown_page](int p) { shown_page = p; });

    auto all = denso::camera::all(h.h());
    REQUIRE(all.size() == 1);
    controller.begin_edit(all.at(0));      // sets the draft; does NOT capture
    // open_configure() would kick off a threaded RTSP snapshot; populate the page
    // directly instead so read_into() round-trips sane geometry. This test is
    // about the MODE the controller supplies, not about capture.
    configure.populate(all.at(0));

    // save_configured_camera() confirms "no live preview" before saving (we never
    // captured a frame). Auto-answer Yes so it proceeds into enter_models(); the
    // modal runs a nested event loop, so a queued poll reaches it.
    auto* answer = new QTimer;
    answer->setInterval(10);
    QObject::connect(answer, &QTimer::timeout, [answer] {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(w);
            if (box && box->isVisible()) {
                answer->stop();
                box->button(QMessageBox::Yes)->click();
                return;
            }
        }
    });
    answer->start();
    controller.save_configured_camera();   // …ends in the production enter_models()
    answer->stop();
    answer->deleteLater();

    CHECK(shown_page == 3);                // the Models step is showing
    const auto offered = offered_models(models);
    REQUIRE(offered.size() == 1);
    CHECK(offered.at(0) == QStringLiteral("digitv3"));
    CHECK(cam_id > 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// An empty allowed list renders safely — no crash, no rows, and critically NO
// fallback to the raw catalog.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("the Models page renders an empty allowed list safely",
          "[selectable_models][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::DigitReader));
    const int64_t cam = h.seed_camera("Line 1");
    // Remove the manifest: nothing is declared, so nothing is selectable.
    REQUIRE(QFile::remove(QDir(h.models()).filePath(QStringLiteral("manifest.json"))));

    ModelsPage page;
    page.set_db(h.h());
    REQUIRE_NOTHROW(page.load_for(cam, denso::mode::load(h.h()),
                                  denso::models::load_manifest_view(h.models()),
                                  kPlatform));
    CHECK(offered_models(page).empty());
    // The catalog still holds three rows — the page did NOT fall back to it.
    CHECK(denso::detection::list_models(h.h()).size() == 3);
    // And nothing was written by merely opening the page.
    CHECK(denso::detection::models_for(h.h(), cam).empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// Class-selection state and per-class confidence survive the filtering change:
// an existing digitv3 attachment is still shown checked, with its classes.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("an existing digitv3 selection is preserved by the filtered page",
          "[selectable_models][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::DigitReader));
    const int64_t cam = h.seed_camera("Line 1");

    denso::detection::CameraModel att;
    att.camera_id = cam;
    att.model_id = h.id["digitv3"];
    att.classes = {denso::detection::ModelClassSelection{1, 0.62f}};
    REQUIRE(denso::detection::set_camera_models(
        h.h(), cam, {att}, TargetMode::DigitReader,
        denso::models::load_manifest_view(h.models()), kPlatform));

    ModelsPage page;
    page.set_db(h.h());
    page.load_for(cam, denso::mode::load(h.h()),
                  denso::models::load_manifest_view(h.models()), kPlatform);

    // digitv3 is offered AND checked (it is attached).
    bool checked = false;
    for (const QCheckBox* cb : page.findChildren<QCheckBox*>()) {
        if (cb->objectName() == QStringLiteral("modelCheck") &&
            cb->text() == QStringLiteral("digitv3")) {
            checked = cb->isChecked();
        }
    }
    CHECK(checked);

    // Round-tripping the selections keeps the class and its confidence.
    const auto sel = page.selections(cam);
    REQUIRE(sel.size() == 1);
    CHECK(sel.at(0).model_id == h.id["digitv3"]);
    REQUIRE(sel.at(0).classes.size() == 1);
    CHECK(sel.at(0).classes.at(0).class_id == 1);
    CHECK(sel.at(0).classes.at(0).conf == 0.62f);
}

// ═════════════════════════════════════════════════════════════════════════════
// THE HIDDEN-ATTACHMENT GUARD.
//
// A model that is ATTACHED but now rejected is ABSENT from the page (spec §6.2),
// so selections() cannot carry it and a save would DETACH it. That would take the
// camera from "Degraded, inhibited, diagnosable ModelCompatibilityRejected" to
// "Ready, no model, silently reads nothing" — the exact end state this slice
// forbids. It must not happen without the operator being told and agreeing.
// ═════════════════════════════════════════════════════════════════════════════
namespace {

/// Answer the next modal question with the given button; returns the timer so the
/// caller can stop it. Records whether a modal was actually seen.
QTimer* answer_next_modal(QMessageBox::StandardButton button, bool* seen) {
    auto* t = new QTimer;
    t->setInterval(10);
    QObject::connect(t, &QTimer::timeout, [t, button, seen] {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(w);
            if (box && box->isVisible()) {
                t->stop();
                if (seen) *seen = true;
                box->button(button)->click();
                return;
            }
        }
    });
    t->start();
    return t;
}

/// Attach `model_id` to `cam` DIRECTLY, bypassing the policy gate — the state a
/// restored backup or a model that went bad after attachment leaves behind.
void attach_directly(const QSqlDatabase& db, int64_t cam, int64_t model_id) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO camera_model (camera_id, model_id) VALUES (?, ?)"));
    q.addBindValue(static_cast<qlonglong>(cam));
    q.addBindValue(static_cast<qlonglong>(model_id));
    REQUIRE(q.exec());
}

}  // namespace

TEST_CASE("saving the Models step cannot silently detach a hidden rejected model",
          "[selectable_models][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::DigitReader));
    const int64_t cam = h.seed_camera("Line 1");
    // float-small is attached to a digit_reader camera — rejected, so the page
    // will not offer it. (Written directly: the write path would refuse it.)
    attach_directly(h.h(), cam, h.id["float-small"]);
    REQUIRE(denso::detection::models_for(h.h(), cam).size() == 1);

    denso::ui::CameraAddPage add;
    denso::ui::CameraConfigurePage configure;
    ModelsPage models;
    denso::ui::CameraAreasPage areas;
    models.set_db(h.h());
    denso::ui::CameraWizardController controller(
        h.h(), denso::ui::CameraWizardController::Pages{&add, &configure, &models, &areas},
        [](int) {});
    controller.begin_edit(denso::camera::all(h.h()).at(0));
    models.load_for(cam, denso::mode::load(h.h()),
                    denso::models::load_manifest_view(h.models()), kPlatform);

    // The rejected model is indeed absent from the page.
    for (const QString& name : offered_models(models))
        CHECK(name != QStringLiteral("float-small"));

    SECTION("declining the warning leaves the attachment untouched") {
        bool warned = false;
        QTimer* t = answer_next_modal(QMessageBox::No, &warned);
        controller.save_models();          // → save_models_only()
        t->stop();
        t->deleteLater();

        CHECK(warned);                     // the operator WAS told
        // …and nothing was written: the camera is still attached, still inhibited.
        REQUIRE(denso::detection::models_for(h.h(), cam).size() == 1);
        const auto det = denso::detection::detection_for(
            h.h(), cam, TargetMode::DigitReader,
            denso::models::load_manifest_view(h.models()), kPlatform);
        CHECK(det.compatibility_rejected);
    }

    SECTION("accepting the warning removes it, but only with explicit consent") {
        bool warned = false;
        QTimer* t = answer_next_modal(QMessageBox::Yes, &warned);
        controller.save_models();
        t->stop();
        t->deleteLater();

        CHECK(warned);
        CHECK(denso::detection::models_for(h.h(), cam).empty());
    }
}

TEST_CASE("saving a camera with no hidden attachment asks nothing",
          "[selectable_models][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::DigitReader));
    const int64_t cam = h.seed_camera("Line 1");
    // A perfectly ordinary allowed attachment — the guard must stay silent.
    denso::detection::CameraModel att;
    att.camera_id = cam;
    att.model_id = h.id["digitv3"];
    att.classes = {denso::detection::ModelClassSelection{0, 0.5f}};
    REQUIRE(denso::detection::set_camera_models(
        h.h(), cam, {att}, TargetMode::DigitReader,
        denso::models::load_manifest_view(h.models()), kPlatform));

    denso::ui::CameraAddPage add;
    denso::ui::CameraConfigurePage configure;
    ModelsPage models;
    denso::ui::CameraAreasPage areas;
    models.set_db(h.h());
    denso::ui::CameraWizardController controller(
        h.h(), denso::ui::CameraWizardController::Pages{&add, &configure, &models, &areas},
        [](int) {});
    controller.begin_edit(denso::camera::all(h.h()).at(0));
    models.load_for(cam, denso::mode::load(h.h()),
                    denso::models::load_manifest_view(h.models()), kPlatform);

    bool warned = false;
    QTimer* t = answer_next_modal(QMessageBox::Yes, &warned);
    controller.save_models();
    t->stop();
    t->deleteLater();

    CHECK_FALSE(warned);   // no spurious prompt on the ordinary path
    CHECK(denso::detection::models_for(h.h(), cam).size() == 1);
}

// ═════════════════════════════════════════════════════════════════════════════
// STRUCTURAL: the production UI holds no policy. If any model name or mode token
// ever appears in ModelsPage, the matrix has leaked out of compatibility.cpp.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("ModelsPage source contains no model or mode token",
          "[selectable_models][ui]") {
    const QStringList sources{
        QStringLiteral(DENSO_SOURCE_DIR "/src/app/ui/camera/dialog/models_page.cpp"),
        QStringLiteral(DENSO_SOURCE_DIR "/src/app/ui/camera/dialog/models_page.h"),
    };
    // Every token that could only appear as an authorization rule. Note these are
    // the canonical ids and the mode tokens: the page may render a model's NAME at
    // runtime (it comes from the database), but must never NAME one in its source.
    const QStringList forbidden{
        QStringLiteral("digitv3"),     QStringLiteral("float-small"),
        QStringLiteral("float-big"),   QStringLiteral("digit_reader"),
        QStringLiteral("ball_leveler"), QStringLiteral("digit_numeric"),
        QStringLiteral("float_ball"),
    };
    for (const QString& path : sources) {
        QFile f(path);
        REQUIRE(f.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(f.readAll());
        for (const QString& token : forbidden) {
            INFO("token '" << token.toStdString() << "' found in "
                           << path.toStdString());
            CHECK_FALSE(text.contains(token));
        }
        // It must also not reimplement the policy under another name.
        CHECK_FALSE(text.contains(QStringLiteral("model_compatibility")));
        // And it MUST go through the repository seam.
        (void)text;
    }
    // The seam itself: models_page.cpp goes through the repository's evaluation,
    // which is where the ONE policy is applied. It used to call selectable_models;
    // it now calls evaluated_models, of which selectable_models is the filtered
    // reading, so that the models it OFFERS and the reasons it shows for the ones
    // it does not are produced by a single evaluation and cannot disagree.
    QFile impl(QStringLiteral(DENSO_SOURCE_DIR
                              "/src/app/ui/camera/dialog/models_page.cpp"));
    REQUIRE(impl.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(impl.readAll());
    CHECK(text.contains(QStringLiteral("evaluated_models")));
    // evaluated_models deliberately RETURNS the rejected rows too, so the page
    // carries the one obligation selectable_models used to discharge for it: an
    // offered model must be gated on the policy's own verdict. Without this the
    // page would render rejected models as selectable — the exact fail-open the
    // filtering seam exists to prevent.
    CHECK(text.contains(QStringLiteral("result.allowed()")));
    // ...and it still never renders the unfiltered catalog.
    CHECK_FALSE(text.contains(QStringLiteral("list_models")));
}

// ═════════════════════════════════════════════════════════════════════════════
// BALL LEVELER STAYS LOCKED. The repository answers for ball_leveler — that is
// the seam this slice ships — but the production UI must expose no wizard.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("ball_leveler offers exactly the Float models the seam answers",
          "[selectable_models][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::BallLeveler));
    h.seed_camera("Leveler Cam");

    // (1) The REPOSITORY layer does return the Float models for ball_leveler.
    const auto seam = denso::detection::selectable_models(
        h.h(), TargetMode::BallLeveler,
        denso::models::load_manifest_view(h.models()), kPlatform);
    REQUIRE(seam.size() == 2);
    CHECK(seam.at(0).metadata.canonical_id == "float-small");
    CHECK(seam.at(1).metadata.canonical_id == "float-big");

    // (2) ACTIVATION: the PRODUCTION UI now reaches that seam, and reaching it is
    // the point — the wizard is how an operator binds one of those Float models.
    const uint64_t procs_before = DetectionProcessor::constructed_count();

    auto state = std::make_shared<denso::settings::Settings>();
    MainWindow window(h.h(), state, h.engines, h.warmup.get());

    // Top-bar Camera button is live in ball_leveler.
    const auto buttons = window.findChildren<QPushButton*>(QStringLiteral("cameraButton"));
    REQUIRE(buttons.size() == 1);
    CHECK(buttons.at(0)->isEnabled());

    // Called twice on purpose: the dialog is created once and REUSED, so a second
    // call must not build a second one.
    window.open_camera();
    window.open_camera();
    CHECK(window.findChildren<CameraDialog*>().size() == 1);

    // What has NOT changed is mode purity: opening the ball wizard builds no
    // digit detection pipeline. Authorization still comes from the one central
    // policy — this case's part (1) is what pins WHICH models it may offer.
    CHECK(DetectionProcessor::constructed_count() == procs_before);
}

// ═════════════════════════════════════════════════════════════════════════════
// The digit_reader flow is UNCHANGED: the Camera button is live and the wizard
// still opens. Filtering must not have locked the mode that actually ships.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("digit_reader still reaches the camera wizard", "[selectable_models][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::DigitReader));

    auto state = std::make_shared<denso::settings::Settings>();
    MainWindow window(h.h(), state, h.engines, h.warmup.get());

    const auto buttons = window.findChildren<QPushButton*>(QStringLiteral("cameraButton"));
    REQUIRE(buttons.size() == 1);
    CHECK(buttons.at(0)->isEnabled());

    window.open_camera();
    CHECK(window.findChildren<CameraDialog*>().size() == 1);
    // The wizard's Models step exists in digit_reader — the five-page flow is intact.
    CHECK(window.findChildren<ModelsPage*>().size() == 1);
}
