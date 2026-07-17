// The runtime/management split: which cameras may actually stream. The wizard
// must insert a camera at the Configure step (attaching models needs a real id),
// so a row can exist while its setup is still unfinished. Before this split the
// grid took camera::all() and streamed everything — backing out at the Models
// step left a LIVE, model-less camera the operator thought they had cancelled.
#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/repo.h"
#include "db/db.h"

#include <QSqlQuery>
#include <QVariant>

#include <utility>

using denso::camera::all;
using denso::camera::Camera;
using denso::camera::insert;
using denso::camera::mark_setup_complete;
using denso::camera::runtime;
using denso::camera::update;
using denso::db::Db;
using denso::db::run_migrations;

namespace {

Db db() {
    auto d = Db::open_in_memory();
    REQUIRE(d.has_value());
    REQUIRE(run_migrations(d->handle()));
    return std::move(*d);
}

Camera cam(const char* name) {
    Camera c;
    c.name = name;
    c.camera_type = "usb";
    c.active = true;
    c.index = 0;
    c.width = 1280;
    c.height = 720;
    c.fps = 30;
    return c;
}

} // namespace

TEST_CASE("a fresh Camera is NOT setup_complete", "[camera_runtime]") {
    // Fail-safe: a forgotten assignment must leave a camera dark (visible in the
    // list, recoverable), never live and half-configured.
    REQUIRE_FALSE(Camera{}.setup_complete);
}

TEST_CASE("setup_complete round-trips", "[camera_runtime]") {
    Db d = db();
    Camera c = cam("A");
    c.setup_complete = true;
    const auto id = insert(d.handle(), c);
    REQUIRE(id.has_value());

    const std::vector<Camera> got = all(d.handle());
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].setup_complete);
}

TEST_CASE("an unfinished camera is listed but never runs", "[camera_runtime]") {
    Db d = db();
    Camera c = cam("Half-configured");
    c.active = true;
    c.setup_complete = false;  // operator backed out at the Models step
    REQUIRE(insert(d.handle(), c).has_value());

    REQUIRE(all(d.handle()).size() == 1);      // the list can show + resume it
    REQUIRE(runtime(d.handle()).empty());      // but it must not stream or report
}

TEST_CASE("runtime requires BOTH enabled and finished", "[camera_runtime]") {
    Db d = db();

    Camera done_on = cam("done+enabled");
    done_on.active = true;
    done_on.setup_complete = true;
    REQUIRE(insert(d.handle(), done_on).has_value());

    Camera done_off = cam("done+disabled");
    done_off.active = false;
    done_off.setup_complete = true;
    REQUIRE(insert(d.handle(), done_off).has_value());

    Camera draft_on = cam("draft+enabled");
    draft_on.active = true;
    draft_on.setup_complete = false;
    REQUIRE(insert(d.handle(), draft_on).has_value());

    // active and setup_complete are DIFFERENT states and both must hold. This is
    // why setup_complete is its own column rather than a reuse of active.
    const std::vector<Camera> run = runtime(d.handle());
    REQUIRE(run.size() == 1);
    REQUIRE(run[0].name == "done+enabled");
    REQUIRE(all(d.handle()).size() == 3);
}

TEST_CASE("mark_setup_complete promotes a draft into the runtime", "[camera_runtime]") {
    Db d = db();
    Camera c = cam("Finishing");
    c.setup_complete = false;
    const auto id = insert(d.handle(), c);
    REQUIRE(id.has_value());
    REQUIRE(runtime(d.handle()).empty());

    REQUIRE(mark_setup_complete(d.handle(), *id));

    const std::vector<Camera> run = runtime(d.handle());
    REQUIRE(run.size() == 1);
    REQUIRE(run[0].id == *id);
    REQUIRE(run[0].setup_complete);
}

TEST_CASE("editing a finished camera does not un-finish it", "[camera_runtime]") {
    Db d = db();
    Camera c = cam("Live");
    c.setup_complete = true;
    const auto id = insert(d.handle(), c);
    REQUIRE(id.has_value());

    // The edit path loads the row, mutates source fields and writes it back. The
    // flag must survive that round trip or every edit would darken a working
    // camera.
    Camera edited = all(d.handle())[0];
    edited.name = "Live renamed";
    REQUIRE(update(d.handle(), edited));
    REQUIRE(runtime(d.handle()).size() == 1);
}

// The upgrade-safety guarantee, and the reason setup_complete is a NEW column
// rather than a reuse of `active`. An older build could not record
// incompleteness, so there is no signal to recover: every pre-existing row MUST
// be grandfathered as finished, or a working appliance goes dark on upgrade.
//
// This builds a REAL v11 database — the v4 camera table plus the ALTERs through
// v11, a row inserted while `setup_complete` does not exist, and user_version=11
// — then runs the current migrations over it. Asserting against an already-v12
// table would only prove the column's DEFAULT applies to new INSERTs, which is a
// different claim and not the one that matters on upgrade.
TEST_CASE("the v12 migration grandfathers a real v11 row as finished",
          "[camera_runtime]") {
    auto d = Db::open_in_memory();
    REQUIRE(d.has_value());
    const QSqlDatabase h = d->handle();

    const auto run_sql = [&h](const char* sql) {
        QSqlQuery q(h);
        return q.exec(QString::fromUtf8(sql));
    };

    // The camera table exactly as v11 left it: the v4 CREATE plus every additive
    // ALTER up to v11. No setup_complete — that is what v12 adds.
    REQUIRE(run_sql("CREATE TABLE camera ("
                    "    id          INTEGER PRIMARY KEY,"
                    "    name        TEXT    NOT NULL,"
                    "    camera_type TEXT    NOT NULL,"
                    "    active      INTEGER NOT NULL,"
                    "    cam_index   INTEGER,"
                    "    ip          TEXT,"
                    "    rtsp        TEXT,"
                    "    username    TEXT,"
                    "    width       INTEGER NOT NULL,"
                    "    height      INTEGER NOT NULL,"
                    "    fps         INTEGER NOT NULL,"
                    "    pitch       REAL    NOT NULL,"
                    "    roll        REAL    NOT NULL,"
                    "    rotation    INTEGER NOT NULL)"));
    REQUIRE(run_sql("ALTER TABLE camera ADD COLUMN password TEXT"));
    REQUIRE(run_sql("ALTER TABLE camera ADD COLUMN channel INTEGER"));
    REQUIRE(run_sql("ALTER TABLE camera ADD COLUMN stream INTEGER"));
    REQUIRE(run_sql("ALTER TABLE camera ADD COLUMN manufacturer TEXT"));
    REQUIRE(run_sql("ALTER TABLE camera ADD COLUMN areas_need_review "
                    "INTEGER NOT NULL DEFAULT 0"));

    // A camera this operator has been running for months.
    REQUIRE(run_sql("INSERT INTO camera (name, camera_type, active, cam_index, "
                    "width, height, fps, pitch, roll, rotation) "
                    "VALUES ('Legacy', 'usb', 1, 0, 1280, 720, 30, 0.0, 0.0, 0)"));
    REQUIRE(run_sql("PRAGMA user_version = 11"));

    // The upgrade.
    REQUIRE(run_migrations(h));

    const std::vector<Camera> run = runtime(h);
    REQUIRE(run.size() == 1);
    REQUIRE(run[0].name == "Legacy");
    REQUIRE(run[0].setup_complete);  // still running after the upgrade
}
