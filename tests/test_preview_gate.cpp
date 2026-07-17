#include <catch2/catch_test_macros.hpp>

#include "camera/preview_gate.h"

using denso::ui::PreviewGate;

TEST_CASE("a fresh gate has no live frame", "[preview_gate]") {
    PreviewGate g;
    REQUIRE_FALSE(g.has_live_frame());
    REQUIRE_FALSE(g.is_capturing());
}

TEST_CASE("begin marks a capture in flight until it settles", "[preview_gate]") {
    PreviewGate g;
    const uint64_t gen = g.begin();
    REQUIRE(g.is_capturing());
    REQUIRE_FALSE(g.has_live_frame());  // nothing proven until it settles
    REQUIRE(g.settle(gen, true));
    REQUIRE_FALSE(g.is_capturing());
    REQUIRE(g.has_live_frame());
}

TEST_CASE("a failed capture leaves no live frame", "[preview_gate]") {
    PreviewGate g;
    REQUIRE(g.settle(g.begin(), false));
    REQUIRE_FALSE(g.has_live_frame());
}

// THE BUG THIS UNIT EXISTS FOR. The old controller kept `last_frame_` from an
// earlier success and only ever asked "is it null?". A refresh that FAILED left
// the stale image in place, so the Areas page still let the operator "verify"
// quarantined ROIs — clearing the flag and resuming zone reporting against an
// image that no longer reflects the camera. Liveness must be the LAST attempt's
// result, not "some attempt once worked".
TEST_CASE("a failed refresh after a success is NOT live", "[preview_gate]") {
    PreviewGate g;
    REQUIRE(g.settle(g.begin(), true));
    REQUIRE(g.has_live_frame());

    REQUIRE(g.settle(g.begin(), false));  // camera unplugged / went offline
    REQUIRE_FALSE(g.has_live_frame());    // must NOT still count as verified
}

// The hole was open for the DURATION of a refresh, not just after it failed:
// liveness used to survive begin(), so between "operator taps Refresh" and "the
// failure lands" the gate still said verified. On an offline camera that window
// is the full 5s open + 5s read timeout — ample time to tap "Verify & save" and
// resume reporting on an unconfirmed view.
TEST_CASE("starting a refresh revokes liveness before it settles", "[preview_gate]") {
    PreviewGate g;
    REQUIRE(g.settle(g.begin(), true));
    REQUIRE(g.has_live_frame());

    const uint64_t refresh = g.begin();
    REQUIRE_FALSE(g.has_live_frame());  // nothing is verified while in flight
    REQUIRE(g.is_capturing());

    REQUIRE(g.settle(refresh, true));   // only a fresh success restores it
    REQUIRE(g.has_live_frame());
}

TEST_CASE("a superseded result does not apply", "[preview_gate]") {
    PreviewGate g;
    const uint64_t first = g.begin();
    const uint64_t second = g.begin();
    REQUIRE(first != second);

    // The older capture finishes late; it is not the newest request, so it must
    // neither be drawn nor count as proof of the CURRENT source.
    REQUIRE_FALSE(g.settle(first, true));
    REQUIRE_FALSE(g.has_live_frame());
    REQUIRE(g.is_capturing());  // still waiting on `second`

    REQUIRE(g.settle(second, true));
    REQUIRE(g.has_live_frame());
}

// The mirror of the above, and the reason liveness cannot just be "last settle
// wins": a STALE failure must not revoke a NEWER success.
TEST_CASE("a late failure does not revoke a newer success", "[preview_gate]") {
    PreviewGate g;
    const uint64_t stale = g.begin();
    const uint64_t fresh = g.begin();

    REQUIRE(g.settle(fresh, true));
    REQUIRE(g.has_live_frame());

    REQUIRE_FALSE(g.settle(stale, false));  // superseded — ignored
    REQUIRE(g.has_live_frame());            // the newer success still stands
}

TEST_CASE("invalidate drops liveness", "[preview_gate]") {
    PreviewGate g;
    REQUIRE(g.settle(g.begin(), true));
    REQUIRE(g.has_live_frame());

    g.invalidate();  // the source changed — what is on screen is not it
    REQUIRE_FALSE(g.has_live_frame());
}

TEST_CASE("invalidate supersedes an in-flight capture", "[preview_gate]") {
    PreviewGate g;
    const uint64_t gen = g.begin();
    g.invalidate();

    // The capture that was running against the OLD source must not land as proof
    // of the new one.
    REQUIRE_FALSE(g.settle(gen, true));
    REQUIRE_FALSE(g.has_live_frame());
    REQUIRE_FALSE(g.is_capturing());
}

TEST_CASE("settling the same generation twice applies once", "[preview_gate]") {
    PreviewGate g;
    const uint64_t gen = g.begin();
    REQUIRE(g.settle(gen, true));
    REQUIRE(g.has_live_frame());

    // A duplicate delivery must not resurrect a settled generation.
    REQUIRE_FALSE(g.settle(gen, false));
    REQUIRE(g.has_live_frame());
}
