#include <catch2/catch_test_macros.hpp>

#include "db/db.h"
#include "models/compatibility.h"     // TargetMode-aware policy types
#include "models/manifest.h"
#include "models/model_identity.h"    // ManifestView, PlatformInfo
#include "mode/mode.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

namespace {
// An empty manifest view bound to a throwaway dir. With no declared generation,
// every model resolves undeclared → the policy rejects it (fail-closed). Enough
// for the callers below, which assert either "no rows" or "undeclared → excluded"
// and so never reach a hash comparison. The active-backend production ctor is
// used, mirroring startup.cpp.
denso::models::ManifestView empty_view() {
    return denso::models::ManifestView(denso::models::Manifest{},
                                       QStringLiteral("."));
}
// Plain const (NOT constexpr): PlatformInfo holds std::strings and constexpr
// std::string needs GCC 12+, but the Jetson gate builds with gcc11.
const denso::models::PlatformInfo kNoPlatform{};
} // namespace

using denso::db::Db;
using denso::db::run_migrations;

namespace {
Db mem() {
    auto d = Db::open_in_memory();
    REQUIRE(d.has_value());
    REQUIRE(run_migrations(d->handle()));
    return std::move(*d);
}
bool has_table(const QSqlDatabase& db, const char* name) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?"));
    q.addBindValue(QString::fromLatin1(name));
    return q.exec() && q.next();
}
} // namespace

TEST_CASE("migration v8 creates the detection tables") {
    auto d = mem();
    REQUIRE(has_table(d.handle(), "model"));
    REQUIRE(has_table(d.handle(), "camera_model"));
    REQUIRE(has_table(d.handle(), "camera_model_class"));
}

#include "detection/detection.h"
#include "detection/repo.h"

using denso::detection::DetectionModel;
using denso::detection::list_models;
using denso::detection::upsert_model;

TEST_CASE("upsert_model inserts and list_models returns it") {
    auto d = mem();
    DetectionModel m;
    m.name = "denso";
    m.filename = "denso.onnx";
    m.class_names = {"0", "1", "2"};
    const auto id = upsert_model(d.handle(), m);
    REQUIRE(id.has_value());

    const auto models = list_models(d.handle());
    REQUIRE(models.size() == 1);
    REQUIRE(models[0].id == *id);
    REQUIRE(models[0].name == "denso");
    REQUIRE(models[0].filename == "denso.onnx");
    REQUIRE(models[0].class_names == std::vector<std::string>{"0", "1", "2"});
}

TEST_CASE("upsert_model updates by filename without adding a row") {
    auto d = mem();
    DetectionModel m;
    m.name = "old";
    m.filename = "denso.onnx";
    m.class_names = {"0"};
    const auto id1 = upsert_model(d.handle(), m);
    m.name = "new";
    m.class_names = {"0", "1"};
    const auto id2 = upsert_model(d.handle(), m);
    REQUIRE(id1 == id2);
    const auto models = list_models(d.handle());
    REQUIRE(models.size() == 1);
    REQUIRE(models[0].name == "new");
    REQUIRE(models[0].class_names.size() == 2);
}

#include "camera/camera.h"
#include "camera/repo.h"
#include "detection/repo.h"

using denso::detection::CameraModel;
using denso::detection::detection_for;
using denso::detection::models_for;
using denso::detection::ModelClassSelection;
using denso::detection::set_camera_models;

namespace {
int64_t seed_camera(const QSqlDatabase& db) {
    denso::camera::Camera c;
    c.name = "Cam";
    c.camera_type = "usb";
    c.active = true;
    c.index = 0;
    c.width = 640; c.height = 480; c.fps = 30;
    return *denso::camera::insert(db, c);
}
int64_t seed_model(const QSqlDatabase& db) {
    denso::detection::DetectionModel m;
    m.name = "denso"; m.filename = "denso.onnx";
    m.class_names = {"0", "1", "2", "3"};
    return *upsert_model(db, m);
}
// Write an attachment DIRECTLY, bypassing set_camera_models' compatibility gate.
// The cases below are about the PERSISTENCE round-trip (rows, class selections,
// ordering) over a deliberately identity-less fixture model ("denso.onnx", which
// no manifest declares). Routing them through the policy would mean either
// inventing a declared manifest they do not test, or asserting a refusal that
// hides the SQL behaviour they exist to cover. The policy's own behaviour —
// allow, refuse, roll back — is covered in tests/test_selectable_models.cpp
// against a real declared fixture.
void attach_directly(const QSqlDatabase& db, int64_t cam, int64_t model_id,
                     const std::vector<denso::detection::ModelClassSelection>& cls) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO camera_model (camera_id, model_id) VALUES (?, ?)"));
    q.addBindValue(static_cast<qlonglong>(cam));
    q.addBindValue(static_cast<qlonglong>(model_id));
    REQUIRE(q.exec());
    const qlonglong cmid = q.lastInsertId().toLongLong();
    for (const auto& s : cls) {
        QSqlQuery c(db);
        c.prepare(QStringLiteral(
            "INSERT INTO camera_model_class (camera_model_id, class_id, conf) "
            "VALUES (?, ?, ?)"));
        c.addBindValue(cmid);
        c.addBindValue(s.class_id);
        c.addBindValue(static_cast<double>(s.conf));
        REQUIRE(c.exec());
    }
}
} // namespace

TEST_CASE("set_camera_models + models_for round-trip attachments and classes") {
    auto d = mem();
    const int64_t cam = seed_camera(d.handle());
    const int64_t model = seed_model(d.handle());

    attach_directly(d.handle(), cam, model,
                    {ModelClassSelection{1, 0.6f}, ModelClassSelection{3, 0.4f}});

    const auto got = models_for(d.handle(), cam);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].model_id == model);
    REQUIRE(got[0].classes.size() == 2);
    REQUIRE(got[0].classes[0].class_id == 1);
    REQUIRE(got[0].classes[0].conf == 0.6f);
    REQUIRE(got[0].classes[1].class_id == 3);
}

TEST_CASE("set_camera_models clears a camera's attachments") {
    auto d = mem();
    const int64_t cam = seed_camera(d.handle());
    const int64_t model = seed_model(d.handle());
    attach_directly(d.handle(), cam, model, {ModelClassSelection{0, 0.5f}});
    attach_directly(d.handle(), cam, model, {ModelClassSelection{0, 0.5f}});
    REQUIRE(models_for(d.handle(), cam).size() == 2);
    // Detaching everything has nothing to judge, so it is allowed in every mode —
    // the operator must always be able to clear an attachment.
    REQUIRE(set_camera_models(d.handle(), cam, {}, denso::mode::TargetMode::DigitReader,
                              empty_view(), kNoPlatform));
    REQUIRE(models_for(d.handle(), cam).empty());
}

TEST_CASE("detection_for is fail-closed when no manifest declares the model",
          "[detection][repo]") {
    auto d = mem();
    const int64_t cam = seed_camera(d.handle());
    const int64_t model = seed_model(d.handle());  // denso.onnx — UNDECLARED
    attach_directly(d.handle(), cam, model, {ModelClassSelection{2, 0.7f}});

    // The row exists and joins cleanly, but with no declaration the central policy
    // rejects it (model_undeclared), so the camera is inhibited AS A WHOLE: no
    // resolved model reaches the runtime and the caller is told WHY, rather than
    // being handed an empty set it might mistake for "no detection configured".
    const auto det = detection_for(d.handle(), cam, denso::mode::TargetMode::DigitReader,
                                   empty_view(), kNoPlatform);
    REQUIRE(det.camera_id == cam);
    REQUIRE(det.models.empty());
    REQUIRE(det.compatibility_rejected);
    REQUIRE(det.policy_reason == "model_undeclared");
}

TEST_CASE("detection_for is empty and NOT rejected for a camera with no models",
          "[detection][repo]") {
    auto d = mem();
    const int64_t cam = seed_camera(d.handle());
    const auto det = detection_for(d.handle(), cam, denso::mode::TargetMode::DigitReader,
                                   empty_view(), kNoPlatform);
    REQUIRE(det.models.empty());
    // The distinction that keeps a model-less camera streaming: nothing was
    // rejected, so the caller still builds its OrientationProcessor.
    REQUIRE_FALSE(det.compatibility_rejected);
}

TEST_CASE("attached_model_filenames is empty when no manifest declares the model") {
    using denso::detection::attached_model_filenames;
    using denso::mode::TargetMode;
    auto d = mem();
    const auto view = empty_view();

    // No attachments → empty (the "no detection configured" case).
    REQUIRE(attached_model_filenames(d.handle(), TargetMode::DigitReader, view,
                                     kNoPlatform)
                .empty());

    const int64_t model = seed_model(d.handle());  // denso.onnx — UNDECLARED
    const int64_t cam1 = seed_camera(d.handle());
    const int64_t cam2 = seed_camera(d.handle());
    // Attached directly: set_camera_models would (correctly) refuse an undeclared
    // model, and the point here is that the READ path excludes it too.
    attach_directly(d.handle(), cam1, model, {});
    attach_directly(d.handle(), cam2, model, {});  // same model, 2 cams

    // Fail-closed: an undeclared attachment is excluded from the required set in
    // BOTH modes (no manifest → resolve_model_metadata reports it undeclared →
    // model_undeclared). The rich allowed/wrong-mode/dedup cases live in
    // tests/test_warmup_allowlist.cpp with a real declared manifest.
    REQUIRE(attached_model_filenames(d.handle(), TargetMode::DigitReader, view,
                                     kNoPlatform)
                .empty());
    REQUIRE(attached_model_filenames(d.handle(), TargetMode::BallLeveler, view,
                                     kNoPlatform)
                .empty());
}

TEST_CASE("try_attached_model_filenames distinguishes empty from unreadable",
          "[detection][repo]") {
    using denso::mode::TargetMode;
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db.has_value());
    const auto view = empty_view();

    SECTION("valid schema with no attachments yields an empty vector, not nullopt") {
        REQUIRE(denso::db::run_migrations(db->handle()));
        const auto got = denso::detection::try_attached_model_filenames(
            db->handle(), TargetMode::DigitReader, view, kNoPlatform);
        REQUIRE(got.has_value());
        REQUIRE(got->empty());
    }

    SECTION("a missing schema yields nullopt, NOT an empty vector") {
        // No migrations: camera_model/model do not exist, so the query fails
        // BEFORE any resolution runs. The old attached_model_filenames() returned
        // {} here, which would let a corrupt database pass --check as a fresh
        // install. Non-throwing contract preserved: nullopt, never an exception.
        REQUIRE_FALSE(denso::detection::try_attached_model_filenames(
                          db->handle(), TargetMode::DigitReader, view, kNoPlatform)
                          .has_value());
    }
}
