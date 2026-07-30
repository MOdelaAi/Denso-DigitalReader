// The Operating-Mode / Camera-Wizard model-refresh defect.
//
// Two independent faults are pinned here, both driven through the REAL ModelsPage
// against a REAL database and a REAL on-disk manifest:
//
//  1. AN EMPTY MODELS STEP MUST SAY WHY. The page used to render zero model
//     checkboxes and zero class rows with no message at all, so a missing
//     manifest, a failed provenance check and a wrong-mode model were visually
//     identical — and indistinguishable from a bug. The reasons it now shows are
//     the central policy's OWN stable reason codes; the page still decides
//     nothing.
//
//  2. RETURNING TO THE MODELS STEP MUST REFRESH. The wizard's Areas Back used to
//     call show_page_(3) directly. The page is constructed once and reused for
//     the application's lifetime, so that rendered whatever the previous load
//     left behind rather than the current committed mode's list.
//
// Every camera seeded here is model-less or points at a closed local port, and
// the manifest's "engines" are tiny text files, so no engine is ever deserialized
// and no device is contacted.
#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/repo.h"
#include "db/db.h"
#include "detection/detection.h"
#include "detection/repo.h"
#include "mode/config.h"
#include "mode/mode.h"
#include "models/hashing.h"
#include "models/manifest.h"
#include "models/model_identity.h"
#include "paths/paths.h"
#include "ui/camera/dialog/models_page.h"

#include <QByteArray>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using denso::mode::TargetMode;
using denso::models::Manifest;
using denso::models::ManifestView;
using denso::models::ModelGeneration;
using denso::models::PlatformInfo;
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

/// One declared generation, with its artifacts written to `dir`. When
/// `corrupt_provenance` is set the manifest records a hash the file does not
/// have — exactly the state the rebuilt Float engines are in on the appliance.
ModelGeneration declare(const QString& dir, const std::string& id,
                        const std::string& family,
                        const std::vector<std::string>& classes,
                        bool corrupt_provenance = false) {
    const QByteArray body = QByteArrayLiteral("model-bytes-") + id.c_str();
    ModelGeneration g;
    g.declared = true;
    g.name = id;
    g.installed_utc = "2026-07-30T00:00:00Z";
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
    if (corrupt_provenance) ort.model_sha256 = std::string(64, 'b');
    ort.class_metadata_source = denso::models::kSourceOnnxMetadataNames;
    g.runtime.onnxruntime = ort;
#else
    denso::models::TensorRtArtifact trt;
    trt.engine = id + ".engine";
    trt.engine_sha256 = write_and_hash(dir, trt.engine, body);
    if (corrupt_provenance) trt.engine_sha256 = std::string(64, 'b');
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
                   "\"task\": \"detect\", \"installed_utc\": \"2026-07-30T00:00:00Z\", "
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

/// A real data dir + migrated database with the three production models
/// catalogued. The manifest is written separately, so a test can start with NONE
/// (the appliance's actual state) and add one later.
struct Fixture {
    QTemporaryDir data;
    ScopedDataDir guard{data.isValid() ? data.path().toUtf8() : QByteArray()};
    std::optional<denso::db::Db> db;
    std::map<std::string, int64_t> id;
    // Written by write_manifest so the artifacts survive for later hashing.
    std::vector<std::string> digit_classes{"0", "1", "2", "3", "4",
                                           "5", "6", "7", "8", "9"};

    Fixture() {
        REQUIRE(data.isValid());
        REQUIRE(QDir(data.path()).mkpath(QStringLiteral("models")));
        db = denso::db::Db::open_in_memory();
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));
        for (const auto& [stem, classes] :
             std::vector<std::pair<std::string, std::vector<std::string>>>{
                 {"digitv3", digit_classes},
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

    /// The three-model manifest the appliance is meant to run.
    /// `bad_floats` reproduces the rebuilt-engine state: declared, correct family
    /// and classes, but hashes that no longer match the files.
    void write_manifest(bool bad_floats = false) {
        Manifest m;
        m.schema = 2;
        m.generations.push_back(declare(models(), "digitv3", "digit_numeric",
                                        digit_classes));
        m.generations.push_back(
            declare(models(), "float-small", "float_ball", {"Small"}, bad_floats));
        m.generations.push_back(
            declare(models(), "float-big", "float_ball", {"Big"}, bad_floats));
        QFile mf(QDir(models()).filePath(QStringLiteral("manifest.json")));
        REQUIRE(mf.open(QIODevice::WriteOnly));
        REQUIRE(mf.write(manifest_json(m).toUtf8()) > 0);
        mf.close();
    }

    /// Exactly what the wizard's enter_models() passes to the page — the same
    /// three authorization inputs, resolved the same way, re-read every time.
    void load(ModelsPage& page, int64_t camera_id = 0) {
        page.load_for(camera_id, denso::mode::load(h()),
                      denso::models::load_manifest_view(denso::paths::models_dir()),
                      kPlatform);
    }
};

std::vector<QString> offered_models(const ModelsPage& page) {
    std::vector<QString> out;
    for (const QCheckBox* cb : page.findChildren<QCheckBox*>()) {
        if (cb->objectName() == QStringLiteral("modelCheck")) out.push_back(cb->text());
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// The class rows currently rendered — the checkboxes that are NOT model cards.
std::vector<QString> class_rows(const ModelsPage& page) {
    std::vector<QString> out;
    for (const QCheckBox* cb : page.findChildren<QCheckBox*>()) {
        if (cb->objectName() != QStringLiteral("modelCheck")) out.push_back(cb->text());
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// A source function body with its // comments removed. A structural assertion
/// about which call a function makes must look at CODE: a comment that merely
/// NAMES the rejected call is documentation, not a call site, and matching it
/// would make the guard unfalsifiable — it would fail on a correct fix whose
/// comment explains what it replaced.
QString code_without_comments(const QString& body) {
    QString out;
    for (const QString& line : body.split(QLatin1Char('\n'))) {
        const int slashes = line.indexOf(QStringLiteral("//"));
        out += (slashes < 0 ? line : line.left(slashes)) + QLatin1Char('\n');
    }
    return out;
}

/// The body of `signature`'s definition, comments stripped.
QString function_body(const QString& src, const QString& signature) {
    const int at = src.indexOf(signature);
    REQUIRE(at > 0);
    const int end = src.indexOf(QStringLiteral("\n}\n"), at);
    REQUIRE(end > at);
    return code_without_comments(src.mid(at, end - at));
}

const QLabel* empty_state(const ModelsPage& page) {
    return page.findChild<QLabel*>(QStringLiteral("modelsEmptyState"));
}

/// The empty-state text, or an empty string when the banner is hidden.
QString empty_text(const ModelsPage& page) {
    const QLabel* l = empty_state(page);
    if (l == nullptr || l->isHidden()) return QString();
    return l->text();
}

void check_all_models(ModelsPage& page) {
    for (QCheckBox* cb : page.findChildren<QCheckBox*>()) {
        if (cb->objectName() == QStringLiteral("modelCheck")) cb->setChecked(true);
    }
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// 1. The blank page. This is the defect the operator actually reported.
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("a missing manifest explains itself instead of rendering blank",
          "[models_refresh][ui]") {
    Fixture f;   // deliberately NO manifest — the appliance's real state
    REQUIRE(denso::mode::save(f.h(), TargetMode::DigitReader));
    ModelsPage page;
    page.set_db(f.h());
    f.load(page);

    // The regression that shipped: nothing offered AND nothing said.
    CHECK(offered_models(page).empty());
    const QString t = empty_text(page);
    REQUIRE_FALSE(t.isEmpty());
    CHECK(t.contains(QStringLiteral("model_undeclared")));
    CHECK(t.contains(QStringLiteral("digit_reader")));
    // Every catalog model is accounted for by name, not just counted.
    CHECK(t.contains(QStringLiteral("digitv3")));
}

TEST_CASE("provenance-failed Float models give an explicit provenance reason",
          "[models_refresh][ui]") {
    Fixture f;
    f.write_manifest(/*bad_floats=*/true);   // rebuilt engines, stale hashes
    REQUIRE(denso::mode::save(f.h(), TargetMode::BallLeveler));
    ModelsPage page;
    page.set_db(f.h());
    f.load(page);

    CHECK(offered_models(page).empty());
    const QString t = empty_text(page);
    REQUIRE_FALSE(t.isEmpty());
    CHECK(t.contains(QStringLiteral("model_provenance_failed")));
    CHECK(t.contains(QStringLiteral("float-small")));
    CHECK(t.contains(QStringLiteral("float-big")));
    // digitv3 is in the same catalog and is refused for a DIFFERENT reason. Both
    // must appear: collapsing every rejection to one cause is the mutation.
    CHECK(t.contains(QStringLiteral("model_mode_incompatible")));
}

TEST_CASE("the empty state disappears once a model becomes selectable",
          "[models_refresh][ui]") {
    Fixture f;
    REQUIRE(denso::mode::save(f.h(), TargetMode::DigitReader));
    ModelsPage page;
    page.set_db(f.h());
    f.load(page);
    REQUIRE_FALSE(empty_text(page).isEmpty());   // no manifest yet

    f.write_manifest();
    f.load(page);
    CHECK(offered_models(page) == std::vector<QString>{QStringLiteral("digitv3")});
    // Stale banner left visible over a working list would be its own defect.
    CHECK(empty_text(page).isEmpty());
    CHECK(empty_state(page)->isHidden());
}

// ═════════════════════════════════════════════════════════════════════════════
// 2. The three-model manifest: the mode decides, and only the mode.
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("the three-model manifest offers only digitv3 in digit_reader",
          "[models_refresh][ui]") {
    Fixture f;
    f.write_manifest();
    REQUIRE(denso::mode::save(f.h(), TargetMode::DigitReader));
    ModelsPage page;
    page.set_db(f.h());
    f.load(page);
    CHECK(offered_models(page) == std::vector<QString>{QStringLiteral("digitv3")});
    CHECK(empty_text(page).isEmpty());
}

TEST_CASE("the three-model manifest offers both Float models in ball_leveler",
          "[models_refresh][ui]") {
    Fixture f;
    f.write_manifest();
    REQUIRE(denso::mode::save(f.h(), TargetMode::BallLeveler));
    ModelsPage page;
    page.set_db(f.h());
    f.load(page);
    CHECK(offered_models(page) ==
          std::vector<QString>{QStringLiteral("float-big"), QStringLiteral("float-small")});
    CHECK(empty_text(page).isEmpty());
}

TEST_CASE("one invalid artifact never hides the unrelated valid models",
          "[models_refresh][ui]") {
    // digitv3 is catalog id 1 — FIRST. Breaking it proves the evaluation does not
    // stop, skip or fail closed for the whole catalog on the first bad model.
    Fixture f;
    Manifest m;
    m.schema = 2;
    m.generations.push_back(
        declare(f.models(), "digitv3", "digit_numeric", f.digit_classes, /*corrupt=*/true));
    m.generations.push_back(declare(f.models(), "float-small", "float_ball", {"Small"}));
    m.generations.push_back(declare(f.models(), "float-big", "float_ball", {"Big"}));
    QFile mf(QDir(f.models()).filePath(QStringLiteral("manifest.json")));
    REQUIRE(mf.open(QIODevice::WriteOnly));
    REQUIRE(mf.write(manifest_json(m).toUtf8()) > 0);
    mf.close();

    REQUIRE(denso::mode::save(f.h(), TargetMode::BallLeveler));
    ModelsPage page;
    page.set_db(f.h());
    f.load(page);
    CHECK(offered_models(page) ==
          std::vector<QString>{QStringLiteral("float-big"), QStringLiteral("float-small")});
}

// ═════════════════════════════════════════════════════════════════════════════
// 3. Repeated mode switching on ONE long-lived page instance — the reported
//    reproduction. The page is never reconstructed, exactly as in the app.
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("digit_reader -> ball_leveler -> digit_reader restores digitv3",
          "[models_refresh][ui]") {
    Fixture f;
    f.write_manifest();
    ModelsPage page;   // ONE instance for the whole round trip
    page.set_db(f.h());

    const std::vector<QString> digit{QStringLiteral("digitv3")};
    const std::vector<QString> floats{QStringLiteral("float-big"),
                                      QStringLiteral("float-small")};

    REQUIRE(denso::mode::save(f.h(), TargetMode::DigitReader));
    f.load(page);
    REQUIRE(offered_models(page) == digit);

    REQUIRE(denso::mode::save(f.h(), TargetMode::BallLeveler));
    f.load(page);
    REQUIRE(offered_models(page) == floats);

    REQUIRE(denso::mode::save(f.h(), TargetMode::DigitReader));
    f.load(page);
    CHECK(offered_models(page) == digit);        // the reported symptom
    CHECK(empty_text(page).isEmpty());
}

TEST_CASE("ball_leveler -> digit_reader -> ball_leveler restores both Float models",
          "[models_refresh][ui]") {
    Fixture f;
    f.write_manifest();
    ModelsPage page;
    page.set_db(f.h());
    const std::vector<QString> floats{QStringLiteral("float-big"),
                                      QStringLiteral("float-small")};

    REQUIRE(denso::mode::save(f.h(), TargetMode::BallLeveler));
    f.load(page);
    REQUIRE(offered_models(page) == floats);

    REQUIRE(denso::mode::save(f.h(), TargetMode::DigitReader));
    f.load(page);
    REQUIRE(offered_models(page) == std::vector<QString>{QStringLiteral("digitv3")});

    REQUIRE(denso::mode::save(f.h(), TargetMode::BallLeveler));
    f.load(page);
    CHECK(offered_models(page) == floats);
    CHECK(empty_text(page).isEmpty());
}

TEST_CASE("switching modes repeatedly never empties the model list",
          "[models_refresh][ui]") {
    Fixture f;
    f.write_manifest();
    ModelsPage page;
    page.set_db(f.h());
    for (int i = 0; i < 6; ++i) {
        const bool digit = (i % 2) == 0;
        REQUIRE(denso::mode::save(
            f.h(), digit ? TargetMode::DigitReader : TargetMode::BallLeveler));
        f.load(page);
        INFO("iteration " << i);
        // The defect under test is an EMPTY list, so assert non-empty every time
        // as well as correct — a list that is right on the last pass but blank in
        // the middle would still be the reported bug.
        REQUIRE_FALSE(offered_models(page).empty());
        CHECK(offered_models(page).size() == (digit ? 1u : 2u));
        CHECK(empty_text(page).isEmpty());
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// 4. Model cards AND class rows are both rebuilt. Two assertions, because
//    rebuilding one and not the other is a distinct mutation each way.
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("class rows are rebuilt when the mode changes the offered models",
          "[models_refresh][ui]") {
    Fixture f;
    f.write_manifest();
    ModelsPage page;
    page.set_db(f.h());

    REQUIRE(denso::mode::save(f.h(), TargetMode::DigitReader));
    f.load(page);
    check_all_models(page);   // ensemble digitv3 -> its ten digit classes
    REQUIRE(class_rows(page).size() == 10);
    REQUIRE(std::find(class_rows(page).begin(), class_rows(page).end(),
                      QStringLiteral("7")) != class_rows(page).end());

    // Ball Leveler: a ONE-class Float model. The digit rows must be gone.
    REQUIRE(denso::mode::save(f.h(), TargetMode::BallLeveler));
    f.load(page);
    check_all_models(page);
    const auto floats_classes = class_rows(page);
    CHECK(floats_classes == std::vector<QString>{QStringLiteral("Big"),
                                                 QStringLiteral("Small")});

    // ...and coming back restores the digit class list, not the Float one.
    REQUIRE(denso::mode::save(f.h(), TargetMode::DigitReader));
    f.load(page);
    check_all_models(page);
    const auto back = class_rows(page);
    CHECK(back.size() == 10);
    CHECK(std::find(back.begin(), back.end(), QStringLiteral("Small")) == back.end());
    CHECK(std::find(back.begin(), back.end(), QStringLiteral("Big")) == back.end());
}

TEST_CASE("an empty offered set leaves no stale model card or class row behind",
          "[models_refresh][ui]") {
    Fixture f;
    f.write_manifest();
    ModelsPage page;
    page.set_db(f.h());
    REQUIRE(denso::mode::save(f.h(), TargetMode::DigitReader));
    f.load(page);
    check_all_models(page);
    REQUIRE(offered_models(page).size() == 1);
    REQUIRE(class_rows(page).size() == 10);

    // Remove the manifest: every model becomes undeclared. Cards AND classes must
    // both clear — leaving either behind would offer a model the write path
    // refuses, or classes belonging to a model that is no longer selectable.
    REQUIRE(QFile::remove(QDir(f.models()).filePath(QStringLiteral("manifest.json"))));
    f.load(page);
    CHECK(offered_models(page).empty());
    CHECK(class_rows(page).empty());
    CHECK(empty_text(page).contains(QStringLiteral("model_undeclared")));
}

// ═════════════════════════════════════════════════════════════════════════════
// 5. The refresh trigger itself: areas_back() must re-enter, not just raise.
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("areas_back re-enters the Models step instead of raising a stale page",
          "[models_refresh][structural]") {
    // Structural, because the defect is precisely which FUNCTION is called: a
    // behavioural test that only looks at the rendered list cannot distinguish
    // "refreshed" from "happened to still be correct". The wizard is driven
    // end-to-end by the behavioural cases above; this pins the seam.
    QFile f(QStringLiteral(DENSO_SOURCE_DIR
                           "/src/app/ui/camera/wizard_controller.cpp"));
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString src = QString::fromUtf8(f.readAll());

    const QString body = function_body(
        src, QStringLiteral("void CameraWizardController::areas_back()"));

    // It must re-enter through the loader...
    CHECK(body.contains(QStringLiteral("enter_models()")));
    // ...and must NOT raise page 3 directly, which is the whole defect.
    CHECK_FALSE(body.contains(QStringLiteral("show_page_(3)")));
}

TEST_CASE("enter_models re-reads the committed mode, manifest and platform",
          "[models_refresh][structural]") {
    // The refresh is only a refresh if all THREE authorization inputs are re-read
    // at call time. Caching any of them in the controller or capturing them in a
    // lambda at construction would reintroduce the stale-mode class of bug.
    QFile f(QStringLiteral(DENSO_SOURCE_DIR
                           "/src/app/ui/camera/wizard_controller.cpp"));
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString src = QString::fromUtf8(f.readAll());
    const QString body = function_body(
        src, QStringLiteral("void CameraWizardController::enter_models()"));

    CHECK(body.contains(QStringLiteral("mode::load(db_)")));
    CHECK(body.contains(QStringLiteral("load_manifest_view")));
    CHECK(body.contains(QStringLiteral("measured_platform_info()")));
}

// ═════════════════════════════════════════════════════════════════════════════
// 6. The repository seam keeps ONE evaluation behind both readings.
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("evaluated_models reports every catalog row, selectable_models filters",
          "[models_refresh]") {
    Fixture f;
    f.write_manifest();
    REQUIRE(denso::mode::save(f.h(), TargetMode::DigitReader));
    const auto view = denso::models::load_manifest_view(f.models());

    const auto all = denso::detection::evaluated_models(
        f.h(), TargetMode::DigitReader, view, kPlatform);
    const auto offered = denso::detection::selectable_models(
        f.h(), TargetMode::DigitReader, view, kPlatform);

    // Nothing is dropped from the evaluation, and catalog-id order is kept.
    REQUIRE(all.size() == 3);
    CHECK(all[0].row.name == "digitv3");
    CHECK(all[0].result.allowed());
    CHECK(all[0].result.reason_code == "model_allowed");
    CHECK_FALSE(all[1].result.allowed());
    CHECK(all[1].result.reason_code == "model_mode_incompatible");
    CHECK_FALSE(all[2].result.allowed());
    CHECK(all[2].result.reason_code == "model_mode_incompatible");

    // ...and the offered list is exactly the allowed subset of it.
    REQUIRE(offered.size() == 1);
    CHECK(offered[0].row.name == "digitv3");
}

TEST_CASE("evaluated_models mutates no row", "[models_refresh]") {
    Fixture f;
    f.write_manifest();
    const auto before = denso::detection::list_models(f.h());
    const auto view = denso::models::load_manifest_view(f.models());
    (void)denso::detection::evaluated_models(f.h(), TargetMode::DigitReader, view,
                                             kPlatform);
    (void)denso::detection::evaluated_models(f.h(), TargetMode::BallLeveler, view,
                                             kPlatform);
    const auto after = denso::detection::list_models(f.h());
    REQUIRE(before.size() == after.size());
    for (size_t i = 0; i < before.size(); ++i) {
        CHECK(before[i].id == after[i].id);
        CHECK(before[i].filename == after[i].filename);
        CHECK(before[i].class_names == after[i].class_names);
    }
}
