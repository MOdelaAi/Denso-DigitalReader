// STRUCTURAL guards for the Zone Runtime Overlay wiring. These assert the two
// boundaries that a unit test cannot see: that AGGREGATION is not gated on
// backend configuration, and that CameraTile stays rendering-only. Same idiom as
// the ModelsPage policy-leak guard — read the production source and assert on it.
#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QIODevice>
#include <QString>
#include <QStringList>

namespace {

QString read_source(const char* rel) {
    QFile f(QStringLiteral(DENSO_SOURCE_DIR) + QString::fromUtf8(rel));
    REQUIRE(f.open(QIODevice::ReadOnly));
    return QString::fromUtf8(f.readAll());
}

} // namespace

// MUTATION: "construct ZoneReporter only when the backend is configured" must
// die. The reporter is the local aggregation owner; gating it on brazing config
// is exactly the defect this slice exists to remove.
TEST_CASE("CameraGrid constructs the zone reporter unconditionally",
          "[zone_runtime][wiring]") {
    const QString src = read_source("/src/app/ui/camera/grid/camera_grid.cpp");

    // The reporter is built from a possibly-empty callback, OUTSIDE the config
    // branch. The moved-from optional callback is the signature of that shape.
    CHECK(src.contains(QStringLiteral(
        "reporter_ = std::make_unique<ZoneReporter>(std::move(on_snapshot))")));

    // The delivery SENDER is still gated — aggregation must be unconditional,
    // but a POST with no configured URL would be a regression.
    CHECK(src.contains(QStringLiteral("bcfg.enabled && !bcfg.base_url.empty()")));
    CHECK(src.contains(QStringLiteral("brazing_reporter_ = std::make_unique<BrazingReporter>")));

    // The old defect: the reporter created INSIDE the config branch. If the
    // construction ever moves back under the condition, the assignment would
    // have to be re-introduced next to the sender's.
    const int sender_at = src.indexOf(
        QStringLiteral("brazing_reporter_ = std::make_unique<BrazingReporter>"));
    const int reporter_at = src.indexOf(QStringLiteral(
        "reporter_ = std::make_unique<ZoneReporter>(std::move(on_snapshot))"));
    REQUIRE(sender_at > 0);
    REQUIRE(reporter_at > 0);
    CHECK(reporter_at > sender_at);  // built after the branch closes, not within it

    // ...and it must be UNGUARDED. Mutation testing showed the substring checks
    // above survive `if (bcfg.enabled) reporter_ = ...` — the exact defect this
    // slice removed — so pin the statement shape, in both the one-line and
    // wrapped-condition forms.
    const QStringList lines = src.split(QLatin1Char('\n'));
    bool unguarded = false;
    for (int i = 0; i < lines.size(); ++i) {
        if (!lines.at(i).contains(
                QStringLiteral("reporter_ = std::make_unique<ZoneReporter>"))) {
            continue;
        }
        if (!lines.at(i).trimmed().startsWith(QStringLiteral("reporter_ ="))) {
            continue;   // guarded on the same line
        }
        // Walk back over comments/blank lines to the previous real statement.
        int j = i - 1;
        while (j >= 0) {
            const QString prev = lines.at(j).trimmed();
            if (prev.isEmpty() || prev.startsWith(QStringLiteral("//"))) { --j; continue; }
            break;
        }
        const QString prev = (j >= 0) ? lines.at(j).trimmed() : QString();
        if (!prev.startsWith(QStringLiteral("if ")) &&
            !prev.startsWith(QStringLiteral("else"))) {
            unguarded = true;
        }
    }
    CHECK(unguarded);
}

// MUTATION: "fail to set configured zones during reload" must die. Without this
// call a boot-inhibited camera renders nothing at all.
TEST_CASE("CameraGrid installs configured zone ownership on reload",
          "[zone_runtime][wiring]") {
    const QString src = read_source("/src/app/ui/camera/grid/camera_grid.cpp");
    CHECK(src.contains(QStringLiteral("set_configured_zones")));
    // Routed per camera, from the camera's own persisted areas.
    CHECK(src.contains(QStringLiteral("areas_for(db_, cam.id)")));
    // Zone 0 / unset is ROI-only and must never be routed as a reporting zone.
    CHECK(src.contains(QStringLiteral("*a.zone != 0")));
}

// MUTATION: "let CameraTile query the reporter directly" must die. The tile owns
// rendering state only — no policy, no aggregation, no delivery.
TEST_CASE("CameraTile stays rendering-only", "[zone_runtime][wiring]") {
    const QStringList paths{
        QStringLiteral("/src/app/ui/camera/grid/camera_tile.h"),
        QStringLiteral("/src/app/ui/camera/grid/camera_tile.cpp"),
    };
    const QStringList forbidden{
        QStringLiteral("zone_aggregator"),  QStringLiteral("ZoneAggregator"),
        QStringLiteral("zone_reporter"),    QStringLiteral("ZoneReporter"),
        QStringLiteral("brazing_reporter"), QStringLiteral("BrazingReporter"),
        QStringLiteral("brazing_client"),   QStringLiteral("BrazingClient"),
    };
    for (const QString& rel : paths) {
        const QString text = read_source(rel.toUtf8().constData());
        for (const QString& token : forbidden) {
            INFO("token '" << token.toStdString() << "' found in "
                           << rel.toStdString());
            CHECK_FALSE(text.contains(token));
        }
    }
}

// MUTATION: "remove the production take_newly_inhibited() drain" must die. The
// event set had NO production caller at all before this slice — every escalation
// accumulated in the aggregator and was never reported to anyone.
TEST_CASE("CameraGrid drains the inhibit onset in production",
          "[zone_runtime][wiring]") {
    const QString src = read_source("/src/app/ui/camera/grid/camera_grid.cpp");

    // One shared consume path, so polling and teardown behave identically.
    CHECK(src.contains(QStringLiteral("void CameraGrid::consume_zone_onsets()")));
    CHECK(src.contains(QStringLiteral("reporter_->take_newly_inhibited()")));

    // Reached from the 5 Hz poll...
    const int poll_at = src.indexOf(QStringLiteral("void CameraGrid::poll_zone_runtime()"));
    const int clear_at = src.indexOf(QStringLiteral("void CameraGrid::clear()"));
    REQUIRE(poll_at > 0);
    REQUIRE(clear_at > 0);
    const QString poll_body = src.mid(poll_at, clear_at - poll_at);
    CHECK(poll_body.contains(QStringLiteral("consume_zone_onsets();")));

    // ...and exactly once from the drain helper itself.
    CHECK(src.count(QStringLiteral("reporter_->take_newly_inhibited()")) == 1);
}

// MUTATION: "drop the final drain, or run it after the reporter is destroyed"
// must die. A pending alarm at teardown must still be reported, and the drain
// must happen while the reporter that owns the event still exists.
TEST_CASE("CameraGrid drains pending onsets before tearing the reporter down",
          "[zone_runtime][wiring]") {
    const QString src = read_source("/src/app/ui/camera/grid/camera_grid.cpp");
    const int clear_at = src.indexOf(QStringLiteral("void CameraGrid::clear()"));
    REQUIRE(clear_at > 0);
    const QString body = src.mid(clear_at);

    // Match STATEMENTS, not prose: the surrounding comments name these calls
    // too, and an assertion that a comment can satisfy proves nothing.
    const int drain_at = body.indexOf(QStringLiteral("consume_zone_onsets();"));
    const int reset_at = body.indexOf(QStringLiteral("reporter_.reset();"));
    const int join_at  = body.indexOf(QStringLiteral("delete s;"));
    REQUIRE(drain_at > 0);
    REQUIRE(reset_at > 0);
    REQUIRE(join_at > 0);
    // After the worker join (no producer can race in), before the reset (the
    // event set still exists to be drained).
    CHECK(join_at < drain_at);
    CHECK(drain_at < reset_at);
}

// MUTATION: "emit/log the same inhibition onset repeatedly" must die. The 5 Hz
// timer must not re-log a standing inhibit, and must not rewrite status.json on
// every tick — but a non-empty onset batch must always force the write through.
TEST_CASE("Zone status writes are change-gated and onsets bypass the gate",
          "[zone_runtime][wiring]") {
    const QString src = read_source("/src/app/ui/camera/grid/camera_grid.cpp");

    // The current-state write is throttled against what was actually PUBLISHED...
    CHECK(src.contains(QStringLiteral("zone_status_.needs_write(")));
    // ...and the owed onsets are passed straight to the ONE existing writer.
    CHECK(src.contains(QStringLiteral("zone_status_.pending()")));

    // The write's OUTCOME is what advances the throttle — never the intent. A
    // failed write must leave the projection unpublished and the alarms owed,
    // otherwise the throttle suppresses every retry and a destructively-drained
    // onset is lost for good.
    CHECK(src.contains(QStringLiteral("const bool ok = health::write_status_file(")));
    CHECK(src.contains(QStringLiteral("zone_status_.on_write(ok, zones)")));
    // The grid must not keep a second, hand-rolled published-state cache.
    CHECK_FALSE(src.contains(QStringLiteral("last_zone_status_")));

    // MUTATION: "let publish_idle_status() write without the owed alarms" must
    // die. On the ball_leveler switch path clear() has already drained and torn
    // the reporter down, so this is the LAST writer — a document sent without
    // them would silently drop an escalation logged moments earlier.
    const int idle_at = src.indexOf(QStringLiteral("void CameraGrid::publish_idle_status()"));
    REQUIRE(idle_at > 0);
    const QString idle_body = src.mid(idle_at, 1600);
    CHECK(idle_body.contains(QStringLiteral("zone_status_.pending()")));
    CHECK(idle_body.contains(QStringLiteral("zone_status_.on_write(ok,")));

    // EVERY production write of status.json must CAPTURE its result and report
    // it; a write whose result is discarded is what let a failure masquerade as
    // published. Counting both forms matters — asserting only that the captured
    // form appears somewhere is satisfied by the OTHER call site.
    const int writes = src.count(QStringLiteral("health::write_status_file("));
    CHECK(writes > 0);
    CHECK(src.count(QStringLiteral("const bool ok = health::write_status_file(")) == writes);
    CHECK(src.count(QStringLiteral("zone_status_.on_write(ok,")) == writes);

    // The log line is emitted from the drain helper, which runs once per drained
    // event — never from the per-tick render path.
    const int consume_at = src.indexOf(QStringLiteral("void CameraGrid::consume_zone_onsets()"));
    REQUIRE(consume_at > 0);
    CHECK(src.mid(consume_at, 1200).contains(QStringLiteral("[zone] camera")));
}

// The zone VALUES no longer travel to the tiles: they are burned into each
// camera frame by the frame processor. This pins that routing.
//
// MUTATION: "push zone rows to the tile again" must die — that would restore the
// duplicate Qt panel alongside the frame annotation.
TEST_CASE("CameraGrid routes zone values into the frame rather than the tiles",
          "[zone_runtime][wiring]") {
    const QString src = read_source("/src/app/ui/camera/grid/camera_grid.cpp");

    // A per-camera provider, filtered on camera_id so a frame can never carry a
    // zone number belonging to another camera.
    CHECK(src.contains(QStringLiteral("ZoneViewFn zone_view")));
    CHECK(src.contains(QStringLiteral("e.camera_id == cid")));

    // Handed to BOTH processors: a model-less camera must keep its overlay.
    CHECK(src.count(QStringLiteral("cam.pitch, cam.roll, zone_view")) == 2);
    CHECK(src.contains(QStringLiteral("zone_view);")));

    // And the timer no longer paints anything.
    const int poll_at = src.indexOf(QStringLiteral("void CameraGrid::poll_zone_runtime()"));
    const int next_at = src.indexOf(QStringLiteral("CameraGrid::zone_status_projection"));
    REQUIRE(poll_at > 0);
    REQUIRE(next_at > poll_at);
    const QString body = src.mid(poll_at, next_at - poll_at);
    CHECK_FALSE(body.contains(QStringLiteral("set_zone_runtime_view")));
    CHECK_FALSE(body.contains(QStringLiteral("clear_zone_runtime_view")));
    // What it DOES keep: the alarm drain and the status write.
    CHECK(body.contains(QStringLiteral("consume_zone_onsets();")));
    CHECK(body.contains(QStringLiteral("refresh_status_file();")));
}

// REQUIREMENT 5 + 10: zone diagnostics are credential-safe, and the slice must
// not smuggle in a delivery/acknowledgement vocabulary it cannot honour.
TEST_CASE("Zone diagnostics leak no credentials and claim no delivery",
          "[zone_runtime][wiring]") {
    const QStringList paths{
        QStringLiteral("/src/app/ui/camera/grid/camera_grid.cpp"),
        QStringLiteral("/src/app/brazing/zone_reporter.cpp"),
        QStringLiteral("/src/app/brazing/zone_aggregator.cpp"),
        QStringLiteral("/src/core/health/status_file.cpp"),
    };
    for (const QString& rel : paths) {
        const QString text = read_source(rel.toUtf8().constData());
        // No status vocabulary that would assert a backend accepted anything.
        for (const QString& badge : {QStringLiteral("\"SENT\""), QStringLiteral("\"PENDING\""),
                                     QStringLiteral("\"OFFLINE\""), QStringLiteral("\"REPORTED\"")}) {
            INFO("badge " << badge.toStdString() << " in " << rel.toStdString());
            CHECK_FALSE(text.contains(badge));
        }
    }
    // The onset record itself is two integers plus a fixed reason token, so no
    // URL or credential can reach the log or status through it.
    const QString hdr = read_source("/src/core/health/status_file.h");
    CHECK(hdr.contains(QStringLiteral("struct ZoneInhibitRecord")));
    const QString grid = read_source("/src/app/ui/camera/grid/camera_grid.cpp");
    const int consume_at = grid.indexOf(QStringLiteral("void CameraGrid::consume_zone_onsets()"));
    REQUIRE(consume_at > 0);
    const QString body = grid.mid(consume_at, 1200);
    CHECK_FALSE(body.contains(QStringLiteral("rtsp")));
    CHECK_FALSE(body.contains(QStringLiteral("password")));
    CHECK_FALSE(body.contains(QStringLiteral("username")));
    CHECK_FALSE(body.contains(QStringLiteral(".url")));
}

// The coordinate contract the overlay depends on. The header documented
// "aspect-fit" while paintEvent draws into the full rect() — a stale contract on
// the exact mapping every overlay shares.
TEST_CASE("CameraTile documents the real stretch-to-fill contract",
          "[zone_runtime][wiring]") {
    const QString hdr = read_source("/src/app/ui/camera/grid/camera_tile.h");
    CHECK_FALSE(hdr.contains(QStringLiteral("aspect-fit")));
    CHECK(hdr.contains(QStringLiteral("STRETCHED TO")));

    // And the implementation still actually does that, so the comment stays true.
    const QString impl = read_source("/src/app/ui/camera/grid/camera_tile.cpp");
    CHECK(impl.contains(QStringLiteral("const QRectF img(rect())")));
}
