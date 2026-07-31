// Phase A — the Ball Leveler wizard UI.
//
// The measurement core (level/calibration, level/edit, level/measure) and the
// write chokepoint (level/repo) already exist and are covered by their own unit
// suites. This file covers the only thing left in Phase A: the OPERATOR PATH
// through the wizard, driven against real widgets, a real database and a real
// on-disk manifest.
//
// Four claims a core-only test cannot make:
//
//  1. THE MODEL STEP IS SINGLE-SELECT AND POLICY-FED. Ball Leveler binds exactly
//     one model and one class; the offered list is the central policy's answer,
//     so digitv3 can never appear and no model id is named here or in the page.
//  2. THE CANVAS DRIVES THE DRAFT. Real mouse events over the real canvas produce
//     the rectangle and the two ball-centre lines, and every constraint the
//     operator can hit is the CalibrationDraft's — not a second copy in a painter.
//  3. SAVE GOES THROUGH THE ONE CHOKEPOINT. The row that lands is the one
//     level::save_level_configuration writes, and an invalid geometry never
//     reaches it.
//  4. REOPENING IS LOSSLESS. A saved calibration reloads into the editor and can
//     be re-saved without drift.
//
// Plus the guard obligation: the production Camera Wizard stays unreachable in
// ball_leveler. Phase B removes the guards, not this phase.
//
// Lives in denso_integration_tests: it constructs real Qt widgets over denso_app.
// Every camera seeded here points at a closed local port and no model file is
// ever a real engine, so nothing is deserialized and no device is contacted.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/repo.h"
#include "camera/source_change.h"
#include "db/db.h"
#include "detection/detection.h"
#include "detection/repo.h"
#include "level/calibration.h"
#include "level/edit.h"
#include "level/repo.h"
#include "mode/config.h"
#include "mode/mode.h"
#include "models/hashing.h"
#include "models/manifest.h"
#include "models/model_identity.h"
#include "paths/paths.h"
#include "platform/platform_info.h"
#include "settings/settings.h"
#include "camera/camera_stream.h"      // CameraStream::constructed_count()
#include "camera/frame_processor.h"    // DetectionProcessor::constructed_count()
#include "detection/engine_registry.h"
#include "ui/camera/camera_dialog.h"
#include "ui/camera/dialog/add_page.h"
#include "ui/camera/dialog/areas_page.h"
#include "ui/camera/dialog/configure_page.h"
#include "ui/camera/dialog/level_calibration_page.h"
#include "ui/camera/dialog/level_canvas.h"
#include "ui/camera/dialog/models_page.h"
#include "ui/camera/wizard_controller.h"
#include "ui/mainwindow.h"
#include "ui/settings/mode_confirm_text.h"
#include "ui/warmup_state.h"

#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointF>
#include <QPushButton>
#include <QRadioButton>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using denso::level::CalibrationDraft;
using denso::level::LevelCalibration;
using denso::mode::TargetMode;
using denso::models::Manifest;
using denso::models::ManifestView;
using denso::models::ModelGeneration;
using denso::models::PlatformInfo;
using denso::ui::LevelCalibrationPage;
using denso::ui::LevelCanvas;
using denso::ui::ModelsPage;

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

/// A real data dir + on-disk manifest + migrated database holding the same three
/// models the appliance ships: one digit model and two single-class Float models.
/// The manifest is written to disk because the production wizard path re-reads it
/// through models::load_manifest_view.
struct Harness {
    QTemporaryDir data;
    ScopedDataDir guard{data.isValid() ? data.path().toUtf8() : QByteArray()};
    std::optional<denso::db::Db> db;
    std::map<std::string, int64_t> id;

    Harness() {
        REQUIRE(data.isValid());
        REQUIRE(QDir(data.path()).mkpath(QStringLiteral("models")));
        const QString models_dir = denso::paths::models_dir();

        Manifest m;
        m.schema = 2;
        m.generations.push_back(
            declare(models_dir, "digitv3", "digit_numeric", {"0", "1", "2", "3"}));
        m.generations.push_back(
            declare(models_dir, "float-small", "float_ball", {"Small"}));
        m.generations.push_back(declare(models_dir, "float-big", "float_ball", {"Big"}));
        QFile mf(QDir(models_dir).filePath(QStringLiteral("manifest.json")));
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
    }

    QSqlDatabase h() const { return db->handle(); }
    QString models() const { return denso::paths::models_dir(); }
    ManifestView view() const { return denso::models::load_manifest_view(models()); }

    denso::camera::Camera camera_row(const std::string& name) const {
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
        return c;
    }

    int64_t seed_camera(const std::string& name) {
        const auto r = denso::camera::insert(h(), camera_row(name));
        REQUIRE(r.has_value());
        return *r;
    }
};

/// The buttons the page is actually OFFERING, read back off its widgets.
std::vector<QString> button_texts(const QWidget& page, const QString& object_name) {
    std::vector<QString> out;
    for (const QAbstractButton* b : page.findChildren<QAbstractButton*>()) {
        if (b->objectName() == object_name) out.push_back(b->text());
    }
    return out;
}

QAbstractButton* button_named(const QWidget& page, const QString& object_name,
                              const QString& text) {
    for (QAbstractButton* b : page.findChildren<QAbstractButton*>()) {
        if (b->objectName() == object_name && b->text() == text) return b;
    }
    return nullptr;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// 1. THE MODEL STEP. Single-select, and fed by the ONE central policy.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("the Ball model step offers only the compatible Float models",
          "[ball_wizard][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::BallLeveler));
    const int64_t cam = h.seed_camera("Tank 1");

    ModelsPage page;
    page.set_db(h.h());
    page.set_selection_mode(ModelsPage::SelectionMode::Single);
    page.load_for(cam, denso::mode::load(h.h()), h.view(), kPlatform);

    const auto offered = button_texts(page, QStringLiteral("modelChoice"));
    REQUIRE(offered.size() == 2);
    CHECK(offered.at(0) == QStringLiteral("float-small"));
    CHECK(offered.at(1) == QStringLiteral("float-big"));
    // The digit model is absent — not greyed, not annotated. Absent.
    for (const QString& name : offered) CHECK(name != QStringLiteral("digitv3"));
    // And the ensemble checkbox list is not rendered at all in this mode.
    CHECK(button_texts(page, QStringLiteral("modelCheck")).empty());
}

TEST_CASE("the Ball model step binds exactly one model and one class",
          "[ball_wizard][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::BallLeveler));
    const int64_t cam = h.seed_camera("Tank 1");

    ModelsPage page;
    page.set_db(h.h());
    page.set_selection_mode(ModelsPage::SelectionMode::Single);
    page.load_for(cam, denso::mode::load(h.h()), h.view(), kPlatform);

    // Nothing is chosen until the operator chooses: a wizard that silently
    // pre-binds a model would make "which model is this tank using?" unanswerable.
    CHECK(page.selections(cam).empty());

    QAbstractButton* small =
        button_named(page, QStringLiteral("modelChoice"), QStringLiteral("float-small"));
    QAbstractButton* big =
        button_named(page, QStringLiteral("modelChoice"), QStringLiteral("float-big"));
    REQUIRE(small != nullptr);
    REQUIRE(big != nullptr);

    small->setChecked(true);
    CHECK(small->isChecked());
    CHECK_FALSE(big->isChecked());
    {
        const auto sel = page.selections(cam);
        REQUIRE(sel.size() == 1);
        CHECK(sel.at(0).model_id == h.id["float-small"]);
        REQUIRE(sel.at(0).classes.size() == 1);
        CHECK(sel.at(0).classes.at(0).class_id == 0);   // the model's ONE class
    }

    // Choosing the other one REPLACES it — the two are mutually exclusive, so an
    // ensemble of both is not representable in this step at all.
    big->setChecked(true);
    CHECK_FALSE(small->isChecked());
    {
        const auto sel = page.selections(cam);
        REQUIRE(sel.size() == 1);
        CHECK(sel.at(0).model_id == h.id["float-big"]);
    }
}

TEST_CASE("a stored Ball binding comes back selected", "[ball_wizard][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::BallLeveler));
    const int64_t cam = h.seed_camera("Tank 1");

    ModelsPage page;
    page.set_db(h.h());
    page.set_selection_mode(ModelsPage::SelectionMode::Single);
    page.load_for(cam, denso::mode::load(h.h()), h.view(), kPlatform);
    page.select_single(h.id["float-big"], 0);

    const auto sel = page.selections(cam);
    REQUIRE(sel.size() == 1);
    CHECK(sel.at(0).model_id == h.id["float-big"]);
    REQUIRE(sel.at(0).classes.size() == 1);
    CHECK(sel.at(0).classes.at(0).class_id == 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// REGRESSION: the Digital Reader ensemble step is untouched by the new mode.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("the digit_reader model step is still an ensemble checklist",
          "[ball_wizard][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::DigitReader));
    const int64_t cam = h.seed_camera("Line 1");

    ModelsPage page;
    page.set_db(h.h());
    page.load_for(cam, denso::mode::load(h.h()), h.view(), kPlatform);

    // Ensemble is the default: checkboxes, named exactly as they always were.
    const auto offered = button_texts(page, QStringLiteral("modelCheck"));
    REQUIRE(offered.size() == 1);
    CHECK(offered.at(0) == QStringLiteral("digitv3"));
    CHECK(button_texts(page, QStringLiteral("modelChoice")).empty());

    // Several classes at once — the ensemble behaviour Ball Leveler must not
    // inherit and Digital Reader must not lose.
    for (QAbstractButton* b : page.findChildren<QAbstractButton*>()) {
        if (b->objectName() == QStringLiteral("modelCheck")) b->setChecked(true);
    }
    int classes = 0;
    for (QCheckBox* cb : page.findChildren<QCheckBox*>()) {
        if (cb->objectName().isEmpty()) {
            cb->setChecked(true);
            ++classes;
        }
    }
    REQUIRE(classes == 4);
    const auto sel = page.selections(cam);
    REQUIRE(sel.size() == 1);
    CHECK(sel.at(0).classes.size() == 4);
}

// ═════════════════════════════════════════════════════════════════════════════
// 2. THE CANVAS DRIVES THE DRAFT.
//
// Real mouse events over the real widget. The point of driving events rather
// than calling setters is that a constraint enforced only in a setter, or only
// in the painter, would pass a setter-level test and still let the operator
// produce an illegal calibration with the mouse — the exact "clicks gate but
// drags clamp" split the ROI editor grew once.
//
// The frame is 1280×720 in a 640×480 widget, so the fitted image is 640×360 at
// y = 60: there are real letterbox bars to click in, and normalized y maps as
// (widget_y − 60) / 360.
// ═════════════════════════════════════════════════════════════════════════════
namespace {

constexpr int kW = 640;
constexpr int kH = 480;
constexpr double kImgTop = 60.0;     // fitted_image_rect(1280×720 in 640×480).y()
constexpr double kImgH = 360.0;      // …and its height

/// Widget y for a normalized y inside the fitted image.
double wy(double norm) { return kImgTop + norm * kImgH; }
/// Widget x for a normalized x.
double wx(double norm) { return norm * static_cast<double>(kW); }

void send(QWidget* w, QEvent::Type type, const QPointF& at, Qt::MouseButton button,
          Qt::MouseButtons buttons) {
    QMouseEvent e(type, at, w->mapToGlobal(at.toPoint()), button, buttons,
                  Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

/// Press at `from`, drag to `to`, release. One gesture, as the operator makes it.
void drag(QWidget* w, const QPointF& from, const QPointF& to) {
    send(w, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    send(w, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    send(w, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

QImage test_frame() {
    QImage img(1280, 720, QImage::Format_RGB32);
    img.fill(Qt::darkGray);
    return img;
}

/// A calibration page with a frame, sized so the maths above holds.
struct PageFixture {
    LevelCalibrationPage page;
    PageFixture() {
        page.resize(kW + 320, kH + 160);   // room for the side controls
        page.set_background(test_frame());
        page.load(std::nullopt);
        REQUIRE(page.canvas() != nullptr);
        page.canvas()->resize(kW, kH);
    }
    LevelCanvas* canvas() { return page.canvas(); }
};

QAbstractButton* find_button(const QWidget& w, const QString& object_name) {
    for (QAbstractButton* b : w.findChildren<QAbstractButton*>()) {
        if (b->objectName() == object_name) return b;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("dragging out a rectangle writes it into the draft", "[ball_wizard][ui]") {
    PageFixture f;
    // A fresh page has no rectangle, so there is nothing to measure yet.
    CHECK_FALSE(f.page.draft().has_rect());

    drag(f.canvas(), QPointF(wx(0.25), wy(0.25)), QPointF(wx(0.75), wy(0.75)));

    REQUIRE(f.page.draft().has_rect());
    const LevelCalibration& c = f.page.draft().draft();
    CHECK(c.rect_x == Catch::Approx(0.25));
    CHECK(c.rect_y == Catch::Approx(0.25));
    CHECK(c.rect_w == Catch::Approx(0.50));
    CHECK(c.rect_h == Catch::Approx(0.50));
    // The draft seeds both reference lines inside the new band at thirds.
    CHECK(c.y_100 == Catch::Approx(0.25 + 0.50 / 3.0));
    CHECK(c.y_0 == Catch::Approx(0.25 + 0.50 * 2.0 / 3.0));
    CHECK(c.y_100 < c.y_0);
    CHECK(f.page.draft().check().ok);
}

TEST_CASE("a drag that starts off the camera image is rejected, not clamped",
          "[ball_wizard][ui]") {
    PageFixture f;
    QString why;
    QObject::connect(f.canvas(), &LevelCanvas::rejected,
                     [&why](const QString& w) { why = w; });

    // y = 10 is in the letterbox bar above the image. Clamping it would silently
    // pin the rectangle's top edge to the image edge — a rectangle the operator
    // never drew.
    drag(f.canvas(), QPointF(wx(0.25), 10.0), QPointF(wx(0.75), wy(0.75)));

    CHECK_FALSE(f.page.draft().has_rect());
    CHECK_FALSE(why.isEmpty());
}

TEST_CASE("a rectangle with no area is refused", "[ball_wizard][ui]") {
    PageFixture f;
    drag(f.canvas(), QPointF(wx(0.5), wy(0.5)), QPointF(wx(0.5), wy(0.5)));
    CHECK_FALSE(f.page.draft().has_rect());
}

TEST_CASE("a reference line dragged past the rectangle stops at its edge",
          "[ball_wizard][ui]") {
    PageFixture f;
    drag(f.canvas(), QPointF(wx(0.25), wy(0.30)), QPointF(wx(0.75), wy(0.80)));
    REQUIRE(f.page.draft().has_rect());
    const double top = f.page.draft().draft().rect_y;
    const double y100 = f.page.draft().draft().y_100;

    // Grab the 100% line and haul it up past the top of the frame entirely.
    drag(f.canvas(), QPointF(wx(0.5), wy(y100)), QPointF(wx(0.5), wy(0.02)));

    const LevelCalibration& c = f.page.draft().draft();
    CHECK(c.y_100 == Catch::Approx(top));          // stopped at the edge…
    CHECK(c.y_100 >= top);                         // …and never outside it
    CHECK(c.y_0 <= c.rect_y + c.rect_h);
    CHECK(c.y_100 < c.y_0);
    CHECK(f.page.draft().check().ok);
}

TEST_CASE("the 100% line can never be dragged below the 0% line",
          "[ball_wizard][ui]") {
    PageFixture f;
    drag(f.canvas(), QPointF(wx(0.20), wy(0.10)), QPointF(wx(0.80), wy(0.90)));
    REQUIRE(f.page.draft().has_rect());
    const double y100 = f.page.draft().draft().y_100;

    // Drag 100% down to where 0% is and beyond: the invariant is kept by PUSHING
    // the partner, so the handle still follows the pointer.
    drag(f.canvas(), QPointF(wx(0.5), wy(y100)), QPointF(wx(0.5), wy(0.85)));

    const LevelCalibration& c = f.page.draft().draft();
    CHECK(c.y_100 < c.y_0);
    CHECK(c.y_100 == Catch::Approx(0.85).margin(0.01));
    CHECK(c.y_0 >= c.y_100 + denso::level::kMinSpanNorm);
    CHECK(f.page.draft().check().ok);
}

TEST_CASE("the 0% line can never be dragged above the 100% line",
          "[ball_wizard][ui]") {
    PageFixture f;
    drag(f.canvas(), QPointF(wx(0.20), wy(0.10)), QPointF(wx(0.80), wy(0.90)));
    const double y0 = f.page.draft().draft().y_0;

    drag(f.canvas(), QPointF(wx(0.5), wy(y0)), QPointF(wx(0.5), wy(0.15)));

    const LevelCalibration& c = f.page.draft().draft();
    CHECK(c.y_100 < c.y_0);
    CHECK(c.y_0 == Catch::Approx(0.15).margin(0.01));
    CHECK(c.y_0 - c.y_100 >= denso::level::kMinSpanNorm);
}

// ═════════════════════════════════════════════════════════════════════════════
// 3. SAVE IS GATED BY THE SAME VALIDATOR THE WRITE USES.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("Save is unavailable until a measurable rectangle exists",
          "[ball_wizard][ui]") {
    PageFixture f;
    QAbstractButton* save = find_button(f.page, QStringLiteral("levelSave"));
    REQUIRE(save != nullptr);
    CHECK_FALSE(save->isEnabled());

    int emitted = 0;
    QObject::connect(&f.page, &LevelCalibrationPage::save_requested,
                     [&emitted](const LevelCalibration&) { ++emitted; });
    save->click();          // a disabled button must not be a way through
    CHECK(emitted == 0);

    drag(f.canvas(), QPointF(wx(0.25), wy(0.25)), QPointF(wx(0.75), wy(0.75)));
    CHECK(save->isEnabled());
    save->click();
    CHECK(emitted == 1);
}

TEST_CASE("a rectangle too short to measure through blocks Save and says why",
          "[ball_wizard][ui]") {
    PageFixture f;
    // 3 px tall ≈ 0.0083 normalized — below kMinSpanNorm, so the two reference
    // lines cannot be separated enough for the percentage denominator to be safe.
    drag(f.canvas(), QPointF(wx(0.30), wy(0.50)), QPointF(wx(0.70), wy(0.50) + 3.0));

    REQUIRE(f.page.draft().has_rect());
    const auto check = f.page.draft().check();
    CHECK_FALSE(check.ok);
    CHECK(check.reason_code == "calib_span_too_small");

    QAbstractButton* save = find_button(f.page, QStringLiteral("levelSave"));
    REQUIRE(save != nullptr);
    CHECK_FALSE(save->isEnabled());

    // The refusal is NAMED on screen, not just a dead button: an operator staring
    // at a greyed Save with no reason has no way to fix it.
    QLabel* status = f.page.findChild<QLabel*>(QStringLiteral("levelStatus"));
    REQUIRE(status != nullptr);
    CHECK_FALSE(status->text().isEmpty());
    CHECK(status->text().contains(QStringLiteral("tall"), Qt::CaseInsensitive));
}

// ═════════════════════════════════════════════════════════════════════════════
// 4. REOPENING IS LOSSLESS. Merely opening the page must not alter what is
// stored, and re-saving an untouched calibration must return the same numbers.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("a saved calibration reloads into the editor without drift",
          "[ball_wizard][ui]") {
    LevelCalibration saved;
    saved.rect_x = 0.1103;
    saved.rect_y = 0.1307;
    saved.rect_w = 0.6211;
    saved.rect_h = 0.7013;
    saved.y_100 = 0.2917;
    saved.y_0 = 0.6603;
    saved.conf = 0.37;
    saved.hold_ms = 1500;
    REQUIRE(denso::level::validate_calibration(saved).ok);

    LevelCalibrationPage page;
    page.resize(kW + 320, kH + 160);
    page.set_background(test_frame());
    page.load(saved);

    const LevelCalibration& d = page.draft().draft();
    CHECK(d.rect_x == saved.rect_x);      // exact, not approx: opening the page
    CHECK(d.rect_y == saved.rect_y);      // must not nudge a single field
    CHECK(d.rect_w == saved.rect_w);
    CHECK(d.rect_h == saved.rect_h);
    CHECK(d.y_100 == saved.y_100);
    CHECK(d.y_0 == saved.y_0);
    CHECK(d.conf == saved.conf);
    CHECK(d.hold_ms == saved.hold_ms);

    // …and re-saving without touching anything emits exactly what was loaded.
    std::optional<LevelCalibration> emitted;
    QObject::connect(&page, &LevelCalibrationPage::save_requested,
                     [&emitted](const LevelCalibration& c) { emitted = c; });
    QAbstractButton* save = find_button(page, QStringLiteral("levelSave"));
    REQUIRE(save != nullptr);
    REQUIRE(save->isEnabled());
    save->click();
    REQUIRE(emitted.has_value());
    CHECK(emitted->rect_x == saved.rect_x);
    CHECK(emitted->rect_y == saved.rect_y);
    CHECK(emitted->rect_w == saved.rect_w);
    CHECK(emitted->rect_h == saved.rect_h);
    CHECK(emitted->y_100 == saved.y_100);
    CHECK(emitted->y_0 == saved.y_0);
    CHECK(emitted->conf == saved.conf);
    CHECK(emitted->hold_ms == saved.hold_ms);
}

TEST_CASE("a fresh page seeds the shared defaults, not local literals",
          "[ball_wizard][ui]") {
    PageFixture f;
    CHECK(f.page.draft().draft().conf == denso::level::kDefaultConf);
    CHECK(f.page.draft().draft().hold_ms == denso::level::kDefaultHoldMs);
}

// ═════════════════════════════════════════════════════════════════════════════
// 5. THE WHOLE OPERATOR PATH, THROUGH THE REAL CONTROLLER.
//
// The cases above prove each widget in isolation. This drives the production
// CameraWizardController end to end, so a controller that wrote through the
// wrong API, skipped the chokepoint, persisted the model binding separately, or
// sent the Ball flow down the Digital Reader branch is caught here.
// ═════════════════════════════════════════════════════════════════════════════
namespace {

/// The five real wizard pages plus the real controller, wired as the dialog
/// wires them. `shown` records the page index the controller asked for.
struct WizardFixture {
    denso::ui::CameraAddPage add;
    denso::ui::CameraConfigurePage configure;
    ModelsPage models;
    denso::ui::CameraAreasPage areas;
    LevelCalibrationPage level;
    int shown = -1;
    denso::ui::CameraWizardController controller;

    explicit WizardFixture(QSqlDatabase db)
        : controller(db,
                     denso::ui::CameraWizardController::Pages{&add, &configure,
                                                              &models, &areas,
                                                              &level},
                     [this](int p) { shown = p; }) {
        models.set_db(db);
        // The page↔controller connections CameraDialog makes. Re-made here rather
        // than borrowed, because these tests deliberately do not build the modal;
        // that the production dialog makes the same two is asserted structurally
        // below, so this fixture cannot quietly pass a wiring the app lacks.
        QObject::connect(&level, &LevelCalibrationPage::save_requested, &controller,
                         &denso::ui::CameraWizardController::save_level_calibration);
        QObject::connect(&level, &LevelCalibrationPage::back_requested, &controller,
                         &denso::ui::CameraWizardController::level_back);
    }
};

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

/// Walk Source → Configure → the model step. `save_configured_camera` confirms
/// "no live preview" first (nothing was captured), so that modal is auto-answered.
void reach_model_step(WizardFixture& f, const denso::camera::Camera& cam) {
    f.controller.begin_edit(cam);
    f.configure.populate(cam);
    QTimer* t = answer_next_modal(QMessageBox::Yes, nullptr);
    f.controller.save_configured_camera();
    t->stop();
    t->deleteLater();
}

/// Give the calibration canvas a frame and rubber-band a rectangle out of it.
void draw_rect_on(LevelCalibrationPage& page, double x0, double y0, double x1,
                  double y1) {
    page.set_background(test_frame());
    REQUIRE(page.canvas() != nullptr);
    page.canvas()->resize(kW, kH);
    drag(page.canvas(), QPointF(wx(x0), wy(y0)), QPointF(wx(x1), wy(y1)));
}

}  // namespace

TEST_CASE("the Ball wizard saves through the level chokepoint and nowhere else",
          "[ball_wizard][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::BallLeveler));
    const int64_t cam = h.seed_camera("Tank 1");

    WizardFixture f(h.h());
    reach_model_step(f, denso::camera::all(h.h()).at(0));
    REQUIRE(f.shown == 3);

    // Choose the model. Only Float models are on offer — the policy decided that.
    QAbstractButton* small = button_named(f.models, QStringLiteral("modelChoice"),
                                          QStringLiteral("float-small"));
    REQUIRE(small != nullptr);
    small->setChecked(true);

    f.controller.save_models();
    CHECK(f.shown == 5);            // the level step, not the Areas step

    // NOTHING is written by choosing a model: the binding and the geometry are
    // one indivisible configuration, and they land together or not at all.
    CHECK_FALSE(denso::level::level_config_for(h.h(), cam).has_value());

    draw_rect_on(f.level, 0.25, 0.20, 0.75, 0.80);
    REQUIRE(f.level.draft().check().ok);
    const denso::level::LevelCalibration drawn = f.level.draft().draft();

    QAbstractButton* save = find_button(f.level, QStringLiteral("levelSave"));
    REQUIRE(save != nullptr);
    REQUIRE(save->isEnabled());
    save->click();

    // The row the ONE chokepoint writes, with the ONE binding it enforces.
    const auto stored = denso::level::level_config_for(h.h(), cam);
    REQUIRE(stored.has_value());
    CHECK(stored->camera_id == cam);
    CHECK(stored->model_id == h.id["float-small"]);
    CHECK(stored->class_id == 0);
    CHECK(stored->calibration.rect_x == drawn.rect_x);
    CHECK(stored->calibration.rect_y == drawn.rect_y);
    CHECK(stored->calibration.rect_w == drawn.rect_w);
    CHECK(stored->calibration.rect_h == drawn.rect_h);
    CHECK(stored->calibration.y_100 == drawn.y_100);
    CHECK(stored->calibration.y_0 == drawn.y_0);
    CHECK(stored->calibration.conf == drawn.conf);
    CHECK(stored->calibration.hold_ms == drawn.hold_ms);
    // The view the geometry was drawn against is fingerprinted with it.
    CHECK(stored->view_revision ==
          denso::camera::view_revision(denso::camera::all(h.h()).at(0)));

    // The ENSEMBLE binding table is untouched: Ball Leveler has its own authority
    // and must never write through the digit reader's.
    CHECK(denso::detection::models_for(h.h(), cam).empty());
}

TEST_CASE("reopening the Ball wizard reloads the stored configuration",
          "[ball_wizard][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::BallLeveler));
    const int64_t cam = h.seed_camera("Tank 1");

    // First pass: configure and save.
    denso::level::LevelCalibration saved;
    {
        WizardFixture f(h.h());
        reach_model_step(f, denso::camera::all(h.h()).at(0));
        button_named(f.models, QStringLiteral("modelChoice"),
                     QStringLiteral("float-big"))
            ->setChecked(true);
        f.controller.save_models();
        draw_rect_on(f.level, 0.31, 0.17, 0.69, 0.83);
        find_button(f.level, QStringLiteral("levelSave"))->click();
        saved = f.level.draft().draft();
    }
    REQUIRE(denso::level::level_config_for(h.h(), cam).has_value());

    // Second pass: a fresh set of pages, exactly as reopening the dialog gives.
    WizardFixture f2(h.h());
    reach_model_step(f2, denso::camera::all(h.h()).at(0));

    // The stored model comes back selected — and it is the one that was stored.
    QAbstractButton* big = button_named(f2.models, QStringLiteral("modelChoice"),
                                        QStringLiteral("float-big"));
    QAbstractButton* small = button_named(f2.models, QStringLiteral("modelChoice"),
                                          QStringLiteral("float-small"));
    REQUIRE(big != nullptr);
    REQUIRE(small != nullptr);
    CHECK(big->isChecked());
    CHECK_FALSE(small->isChecked());

    f2.controller.save_models();
    REQUIRE(f2.shown == 5);

    // …and the geometry reloads EXACTLY. Not approximately: a wizard that nudged
    // a stored calibration every time it was opened would walk a tank's zero
    // point away over a few visits, and every reading with it.
    const denso::level::LevelCalibration& back = f2.level.draft().draft();
    CHECK(back.rect_x == saved.rect_x);
    CHECK(back.rect_y == saved.rect_y);
    CHECK(back.rect_w == saved.rect_w);
    CHECK(back.rect_h == saved.rect_h);
    CHECK(back.y_100 == saved.y_100);
    CHECK(back.y_0 == saved.y_0);
    CHECK(back.conf == saved.conf);
    CHECK(back.hold_ms == saved.hold_ms);

    // Re-saving an untouched calibration stores the same numbers.
    find_button(f2.level, QStringLiteral("levelSave"))->click();
    const auto stored = denso::level::level_config_for(h.h(), cam);
    REQUIRE(stored.has_value());
    CHECK(stored->calibration.rect_x == saved.rect_x);
    CHECK(stored->calibration.y_100 == saved.y_100);
    CHECK(stored->calibration.y_0 == saved.y_0);
    CHECK(stored->model_id == h.id["float-big"]);
}

TEST_CASE("an edited calibration replaces the stored one, still one row",
          "[ball_wizard][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::BallLeveler));
    const int64_t cam = h.seed_camera("Tank 1");

    WizardFixture f(h.h());
    reach_model_step(f, denso::camera::all(h.h()).at(0));
    button_named(f.models, QStringLiteral("modelChoice"), QStringLiteral("float-small"))
        ->setChecked(true);
    f.controller.save_models();
    draw_rect_on(f.level, 0.25, 0.20, 0.75, 0.80);
    find_button(f.level, QStringLiteral("levelSave"))->click();
    const auto first = denso::level::level_config_for(h.h(), cam);
    REQUIRE(first.has_value());

    // Move the 0% line and save again.
    const double y0 = f.level.draft().draft().y_0;
    drag(f.level.canvas(), QPointF(wx(0.5), wy(y0)), QPointF(wx(0.5), wy(0.70)));
    const double moved = f.level.draft().draft().y_0;
    CHECK(moved != first->calibration.y_0);
    find_button(f.level, QStringLiteral("levelSave"))->click();

    const auto second = denso::level::level_config_for(h.h(), cam);
    REQUIRE(second.has_value());
    CHECK(second->calibration.y_0 == moved);
    CHECK(second->calibration.y_100 == f.level.draft().draft().y_100);
    // One configuration per camera, by schema — the edit replaced, not appended.
    const auto all = denso::level::cameras_with_valid_config(h.h());
    REQUIRE(all.size() == 1);
    CHECK(all.at(0) == cam);
}

TEST_CASE("the Ball wizard will not advance without a model chosen",
          "[ball_wizard][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::BallLeveler));
    const int64_t cam = h.seed_camera("Tank 1");

    WizardFixture f(h.h());
    reach_model_step(f, denso::camera::all(h.h()).at(0));
    REQUIRE(f.shown == 3);

    bool warned = false;
    QTimer* t = answer_next_modal(QMessageBox::Ok, &warned);
    f.controller.save_models();          // nothing selected
    t->stop();
    t->deleteLater();

    CHECK(warned);
    CHECK(f.shown == 3);                 // still on the model step
    CHECK_FALSE(denso::level::level_config_for(h.h(), cam).has_value());
}

// ═════════════════════════════════════════════════════════════════════════════
// REGRESSION: the Digital Reader wizard still ends at the Areas step and never
// reaches the level step.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("the digit_reader wizard still goes to the Areas step",
          "[ball_wizard][ui]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::DigitReader));
    const int64_t cam = h.seed_camera("Line 1");

    WizardFixture f(h.h());
    reach_model_step(f, denso::camera::all(h.h()).at(0));
    REQUIRE(f.shown == 3);

    for (QAbstractButton* b : f.models.findChildren<QAbstractButton*>()) {
        if (b->objectName() == QStringLiteral("modelCheck")) b->setChecked(true);
    }
    for (QCheckBox* cb : f.models.findChildren<QCheckBox*>()) {
        if (cb->objectName().isEmpty()) cb->setChecked(true);
    }
    f.controller.save_models();

    CHECK(f.shown == 4);                 // Areas, as it always was
    // The ensemble binding was written through its own API, and no Ball row exists.
    CHECK(denso::detection::models_for(h.h(), cam).size() == 1);
    CHECK_FALSE(denso::level::level_config_for(h.h(), cam).has_value());
}

// ═════════════════════════════════════════════════════════════════════════════
// 6. THE PRODUCTION GUARDS ARE STILL UP.
//
// Phase A builds the pages; Phase B unlocks them. Until then the operator cannot
// reach any of this, and that must be asserted rather than assumed — the whole
// point of building behind a guard is that the guard is what is shipping.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("ball_leveler still exposes no camera wizard and no calibration page",
          "[ball_wizard][ui][guard]") {
    Harness h;
    REQUIRE(denso::mode::save(h.h(), TargetMode::BallLeveler));
    h.seed_camera("Tank 1");

    const uint64_t streams_before = denso::ui::CameraStream::constructed_count();
    const uint64_t procs_before = denso::ui::DetectionProcessor::constructed_count();

    auto engines = std::make_shared<denso::ui::EngineRegistry>(
        denso::paths::models_dir().toStdString(),
        denso::paths::trt_cache_dir().toStdString(),
        std::set<std::string>{});          // nothing allow-listed: nothing can load
    auto warmup = std::make_unique<denso::ui::WarmupState>(engines);
    auto state = std::make_shared<denso::settings::Settings>();
    denso::ui::MainWindow window(h.h(), state, engines, warmup.get());

    // Guard 3 — the top-bar Camera button is disabled in this mode.
    const auto buttons =
        window.findChildren<QPushButton*>(QStringLiteral("cameraButton"));
    REQUIRE(buttons.size() == 1);
    CHECK_FALSE(buttons.at(0)->isEnabled());

    // …and it is an INVARIANT, not just an affordance: the slot itself refuses.
    window.open_camera();
    window.open_camera();

    CHECK(window.findChildren<denso::ui::CameraDialog*>().isEmpty());
    CHECK(window.findChildren<ModelsPage*>().isEmpty());
    CHECK(window.findChildren<LevelCalibrationPage*>().isEmpty());
    CHECK(window.findChildren<LevelCanvas*>().isEmpty());

    // No pipeline of any kind was built for the new mode.
    CHECK(denso::ui::CameraStream::constructed_count() == streams_before);
    CHECK(denso::ui::DetectionProcessor::constructed_count() == procs_before);
}

TEST_CASE("the mode-confirm copy still calls Ball Leveler unavailable",
          "[ball_wizard][ui][guard]") {
    // Guard 1 lives in the pure confirm-copy builder. Phase A must not soften it:
    // a wizard that exists but cannot be reached is only safe while the operator
    // is still told the destination is not available.
    const QString body = denso::ui::mode_confirm_body(TargetMode::BallLeveler);
    CHECK(body.contains(QStringLiteral("not available in this release")));
}

// ═════════════════════════════════════════════════════════════════════════════
// STRUCTURAL: the production dialog really does host and wire the level step.
//
// The wizard fixture above re-makes those connections itself, which is what lets
// these tests avoid building the modal — and is exactly why this case exists. A
// dialog that stopped hosting the page, or wired Save to nothing, would leave
// every behavioural case above green while the operator's Save did nothing.
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("the camera dialog hosts and wires the level calibration step",
          "[ball_wizard][ui]") {
    QFile f(QStringLiteral(DENSO_SOURCE_DIR "/src/app/ui/camera/camera_dialog.cpp"));
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(f.readAll());
    CHECK(text.contains(QStringLiteral("stack_->addWidget(level_page_)")));
    CHECK(text.contains(QStringLiteral("LevelCalibrationPage::save_requested")));
    CHECK(text.contains(QStringLiteral("CameraWizardController::save_level_calibration")));
    CHECK(text.contains(QStringLiteral("LevelCalibrationPage::back_requested")));
    CHECK(text.contains(QStringLiteral("CameraWizardController::level_back")));
    // …and the page reaches the controller as one of its Pages, or none of the
    // above would ever be populated.
    CHECK(text.contains(QStringLiteral("areas_page_, level_page_")));
}
