#include <catch2/catch_test_macros.hpp>

#include "brazing/zone_aggregator.h"

using denso::ui::ZoneAggregator;
using denso::ui::ZoneReading;
using denso::ui::ReadingKind;

static std::vector<ZoneReading> obs(int zone, int value) {
    return {ZoneReading{zone, value, 0.9f}};
}

TEST_CASE("value sends only after it is stable for kStableFrames", "[zone_aggregator]") {
    ZoneAggregator agg(3);  // stable after 3 identical observations
    CHECK_FALSE(agg.observe(obs(1, 500)).has_value());  // 1
    CHECK_FALSE(agg.observe(obs(1, 500)).has_value());  // 2
    auto snap = agg.observe(obs(1, 500));               // 3 -> stable + new
    REQUIRE(snap.has_value());
    CHECK((*snap).at(1) == 500);
}

TEST_CASE("an unchanged stable value does not re-send", "[zone_aggregator]") {
    ZoneAggregator agg(1);  // stable immediately
    REQUIRE(agg.observe(obs(1, 500)).has_value());       // first send
    CHECK_FALSE(agg.observe(obs(1, 500)).has_value());   // same -> no send
}

TEST_CASE("a change in one zone sends the full snapshot of all zones", "[zone_aggregator]") {
    ZoneAggregator agg(1);
    REQUIRE(agg.observe(obs(1, 500)).has_value());
    REQUIRE(agg.observe(obs(2, 200)).has_value());
    auto snap = agg.observe(obs(1, 510));  // zone1 changed
    REQUIRE(snap.has_value());
    CHECK((*snap).at(1) == 510);
    CHECK((*snap).at(2) == 200);  // full snapshot carries zone2 too
}

TEST_CASE("a frame missing a zone keeps its last stable value", "[zone_aggregator]") {
    ZoneAggregator agg(1);
    REQUIRE(agg.observe(obs(1, 500)).has_value());     // zone1 stable at 500
    CHECK(agg.observe(obs(2, 200)).has_value());       // zone2 new -> sends
    CHECK_FALSE(agg.observe(obs(2, 200)).has_value()); // zone2 unchanged -> no send
    // Observations that never mention zone 1 must not have reset or cleared it:
    // a later change still carries zone1's retained 500 base alongside the change.
    auto s2 = agg.observe(obs(1, 505));                // zone1 changes 500 -> 505
    REQUIRE(s2.has_value());
    CHECK((*s2).at(1) == 505);
    CHECK((*s2).at(2) == 200);  // zone2 retained
}

TEST_CASE("an expired zone is dropped from the emitted snapshot", "[zone_aggregator]") {
    ZoneAggregator agg(1, 100);  // stable immediately, expire after 100 ms

    auto first = agg.observe(obs(1, 500), 0);
    REQUIRE(first.has_value());
    CHECK(first->at(1) == 500);

    // A later observation of a different zone, past zone1's expiry window, must
    // evict zone1 from the payload rather than keep re-sending its stale value.
    auto after_expiry = agg.observe(obs(2, 200), 101);
    REQUIRE(after_expiry.has_value());
    CHECK_FALSE(after_expiry->contains(1));
    REQUIRE(after_expiry->contains(2));
    CHECK(after_expiry->at(2) == 200);
}

TEST_CASE("a zone refreshed within the expiry window is retained", "[zone_aggregator]") {
    ZoneAggregator agg(1, 100);

    REQUIRE(agg.observe(obs(1, 500), 0).has_value());
    CHECK_FALSE(agg.observe(obs(1, 500), 100).has_value());  // refreshed at t=100

    auto snapshot = agg.observe(obs(2, 200), 150);  // still within zone1's window
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->contains(1));
    CHECK(snapshot->at(1) == 500);
    REQUIRE(snapshot->contains(2));
    CHECK(snapshot->at(2) == 200);
}

TEST_CASE("an expired zone is sent again when it reappears", "[zone_aggregator]") {
    ZoneAggregator agg(1, 100);

    REQUIRE(agg.observe(obs(1, 500), 0).has_value());

    // Zone1 ages out and it was the only zone, so the resulting snapshot would be
    // EMPTY. The empty-snapshot rule (spec §3.3) suppresses that instead of
    // emitting a bare "{}" that an unqualified backend could read as "clear all".
    auto expired = agg.observe({}, 101);  // no zones observed; zone1 ages out
    REQUIRE_FALSE(expired.has_value());

    auto reappeared = agg.observe(obs(1, 500), 102);
    REQUIRE(reappeared.has_value());
    REQUIRE(reappeared->contains(1));
    CHECK(reappeared->at(1) == 500);
}

static ZoneReading rd(int zone, int value, ReadingKind k = ReadingKind::Complete) {
    ZoneReading r; r.zone_no = zone; r.value = value; r.conf = 0.9f; r.kind = k;
    return r;
}
// Drive `n` identical complete frames; returns the last snapshot emitted.
static std::optional<std::map<int,int>> feed(ZoneAggregator& a, int zone, int value,
                                             int n, int64_t& t) {
    std::optional<std::map<int,int>> last;
    for (int i = 0; i < n; ++i) { t += 100; if (auto s = a.observe({rd(zone, value)}, t)) last = s; }
    return last;
}

TEST_CASE("hold: an incomplete reading does not change the reported value",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    REQUIRE(feed(a, 1, 42, 5, t).has_value());          // 42 becomes stable and is sent
    t += 100;
    REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::Incomplete)}, t).has_value());
}

TEST_CASE("hold: incomplete readings keep the zone alive past the expiry window",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    // 15s of incomplete frames — longer than the 10s expiry. The zone must NOT be
    // evicted, so no shrunk snapshot is emitted.
    for (int i = 0; i < 150; ++i) {
        t += 100;
        REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::Incomplete)}, t).has_value());
    }
}

TEST_CASE("hold: NoDigits also keeps the zone alive", "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    for (int i = 0; i < 150; ++i) {
        t += 100;
        REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::NoDigits)}, t).has_value());
    }
}

TEST_CASE("hold: recovery re-announces even when the value is UNCHANGED",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    for (int i = 0; i < 10; ++i) { t += 100; a.observe({rd(1, 0, ReadingKind::Incomplete)}, t); }
    const auto snap = feed(a, 1, 42, 5, t);   // same value as before the hold
    REQUIRE(snap.has_value());
    REQUIRE(snap->at(1) == 42);
}

TEST_CASE("hold: frames either side of a gap do not combine into one stable run",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 3, t);                                  // 3 of the 5 needed
    t += 100; a.observe({rd(1, 0, ReadingKind::Incomplete)}, t);
    REQUIRE_FALSE(feed(a, 1, 42, 3, t).has_value());        // 3 more must NOT reach 5
    REQUIRE(feed(a, 1, 42, 2, t).has_value());              // 5 consecutive completes
}

TEST_CASE("REGRESSION: many consecutive Incomplete/NoDigits readings never "
          "stabilise to a phantom value 0",
          "[zone_aggregator]") {
    // Guards the severe review finding: Incomplete/NoDigits carry value==0 in the
    // ZoneReading struct, but that 0 must never be treated as a candidate the
    // debounce counter can accumulate toward a stable, emitted snapshot. A zone
    // that has NEVER produced a Complete reading must never appear in any emitted
    // snapshot — least of all stabilised at the placeholder value 0.
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    for (int i = 0; i < 200; ++i) {
        t += 100;
        const auto snap = a.observe({rd(1, 0, ReadingKind::Incomplete)}, t);
        REQUIRE_FALSE(snap.has_value());
    }
    for (int i = 0; i < 200; ++i) {
        t += 100;
        const auto snap = a.observe({rd(2, 0, ReadingKind::NoDigits)}, t);
        REQUIRE_FALSE(snap.has_value());
    }
}

TEST_CASE("hold: a sibling zone on the same camera keeps reporting",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    for (int i = 0; i < 5; ++i) { t += 100; a.observe({rd(1, 11), rd(2, 22)}, t); }
    // Zone 1 goes incomplete; zone 2 changes and must still be reported.
    std::optional<std::map<int,int>> snap;
    for (int i = 0; i < 5; ++i) {
        t += 100;
        if (auto s = a.observe({rd(1, 0, ReadingKind::Incomplete), rd(2, 33)}, t)) snap = s;
    }
    REQUIRE(snap.has_value());
    REQUIRE((*snap)[2] == 33);
    REQUIRE((*snap)[1] == 11);   // zone 1 still carries its held value
}

TEST_CASE("hold: a sibling's snapshot commit must not swallow this zone's "
          "own recovery re-announce",
          "[zone_aggregator]") {
    // Reproduces the review defect: zone A goes incomplete (owes a re-announce),
    // then WHILE A is still mid-recovery (has not yet reached stable_frames_ again)
    // zone B completes its OWN debounce run and commits a full snapshot that
    // carries A's held value. That commit must NOT clear A's needs_reannounce
    // just because A appears in the snapshot at its held value -- only a commit
    // in which A ITSELF completed recovery in the same observe() call may consume
    // the flag. If it wrongly clears here, A's later recovery to the SAME value
    // it already held will not force a report (last_sent_ already matches),
    // violating the "one forced report on recovery" contract.
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    // Frames 1-5: both zones reach their initial stable values (A=42, B=100).
    for (int i = 0; i < 5; ++i) { t += 100; a.observe({rd(1, 42), rd(2, 100)}, t); }

    // Frame 6: zone A goes incomplete (count resets, needs_reannounce=true since
    // it has a last-valid value). Zone B starts changing to a new value on this
    // SAME frame, so B's new debounce run is exactly one frame ahead of A's
    // recovery run.
    t += 100;
    a.observe({rd(1, 0, ReadingKind::Incomplete), rd(2, 200)}, t);

    // Frames 7-10 (4 more): A accumulates 4 of the 5 consecutive complete frames
    // it needs to recover -- NOT yet stable again. B's new-value run reaches its
    // 5th consecutive frame on the last of these, so IT commits a full snapshot
    // that carries A's still-held value (42) while A has not completed recovery.
    std::optional<std::map<int, int>> sibling_commit;
    for (int i = 0; i < 4; ++i) {
        t += 100;
        if (auto s = a.observe({rd(1, 42), rd(2, 200)}, t)) sibling_commit = s;
    }
    REQUIRE(sibling_commit.has_value());
    CHECK(sibling_commit->at(1) == 42);   // A's held value, carried but NOT recovered
    CHECK(sibling_commit->at(2) == 200);  // B's own change

    // Frame 11: A's 5th consecutive complete frame -- it now completes recovery
    // with the SAME value (42) it held before the gap. Per the contract this must
    // still force a report, because A itself never got to consume its own
    // re-announce.
    t += 100;
    const auto recovery_snap = a.observe({rd(1, 42), rd(2, 200)}, t);
    REQUIRE(recovery_snap.has_value());
    CHECK(recovery_snap->at(1) == 42);
}

TEST_CASE("evict: removes the zone and emits the shrunk snapshot", "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    for (int i = 0; i < 5; ++i) { t += 100; a.observe({rd(1, 11), rd(2, 22)}, t); }
    const auto snap = a.evict_zones({1});
    REQUIRE(snap.has_value());
    REQUIRE(snap->count(1) == 0);
    REQUIRE(snap->at(2) == 22);
}

TEST_CASE("evict: never emits an EMPTY snapshot", "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    // Evicting the only zone would yield {} — which build_brazing_payload renders
    // as literal "{}" and could clear every zone on an unqualified backend.
    REQUIRE_FALSE(a.evict_zones({1}).has_value());
}

TEST_CASE("evict: evicting an unknown zone changes nothing", "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    REQUIRE_FALSE(a.evict_zones({99}).has_value());
}

TEST_CASE("evict: re-entry forces a fresh report of an unchanged value",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 11, 5, t);
    feed(a, 2, 22, 5, t);
    a.evict_zones({1});
    const auto snap = feed(a, 1, 11, 5, t);   // same value it had before eviction
    REQUIRE(snap.has_value());
    REQUIRE(snap->at(1) == 11);
}

TEST_CASE("observe: an all-empty result is suppressed, not emitted",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 1000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    t += 5000;   // past expiry with nothing observed -> would expire to {}
    REQUIRE_FALSE(a.observe({}, t).has_value());
}

TEST_CASE("expiry: a never-stable zone's state is dropped after the expiry "
          "window, not held forever",
          "[zone_aggregator]") {
    // Reproduces the review defect: the expiry sweep only considered entries
    // with has_stable == true. A zone that never reaches stable_frames_ creates
    // Debounce state that, before the fix, is never evicted no matter how long
    // it goes unobserved -- a later hold-timeout sweep could then read a stale
    // first_seen_ms and mis-inhibit. We can't reach into privates, so we assert
    // observable behaviour: leftover partial debounce progress (a count short of
    // stable_frames_) must NOT survive the expiry window. If it does, a single
    // post-window observation of the same value it was partway through would
    // wrongly complete the run and emit; it must instead behave as a first-ever
    // observation and need the full run again.
    ZoneAggregator a(3, 100);  // stable after 3 consecutive completes; expire after 100ms
    int64_t t = 0;

    // Zone 1 makes PARTIAL progress toward stability (2 of the 3 needed
    // consecutive complete observations of the same value) but never gets there.
    // has_stable stays false the whole time.
    REQUIRE_FALSE(a.observe({rd(1, 77)}, t += 10).has_value());  // count 1
    REQUIRE_FALSE(a.observe({rd(1, 77)}, t += 10).has_value());  // count 2

    // It then goes completely silent, well past the expiry window. A
    // never-stable entry must expire on age exactly like a stable one.
    t += 200;
    REQUIRE_FALSE(a.observe({}, t).has_value());

    // Zone 1 reappears with a SINGLE complete observation of the SAME value it
    // was partway through before. If the stale Debounce entry (count==2) had
    // survived, this one observation would complete the run to 3 and wrongly
    // emit. It must instead behave as a first-ever observation (count restarts
    // at 1) and NOT emit yet.
    REQUIRE_FALSE(a.observe({rd(1, 77)}, t + 10).has_value());
}

TEST_CASE("evict: an empty zone set is a no-op", "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    REQUIRE_FALSE(a.evict_zones({}).has_value());
}

TEST_CASE("timeout: a hold that exceeds kHoldTimeoutMs inhibits the zone",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 11, 5, t);
    feed(a, 2, 22, 5, t);
    for (int i = 0; i < 400; ++i) {   // 40s of incomplete frames
        t += 100;
        a.observe({rd(1, 0, ReadingKind::Incomplete), rd(2, 22)}, t);
    }
    REQUIRE(a.take_newly_inhibited().count(1) == 1);
}

TEST_CASE("timeout: recovery BEFORE the timeout resumes normally",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    for (int i = 0; i < 100; ++i) {   // 10s — well under 30s
        t += 100;
        a.observe({rd(1, 0, ReadingKind::Incomplete)}, t);
    }
    REQUIRE(a.take_newly_inhibited().empty());
    REQUIRE(feed(a, 1, 42, 5, t).has_value());
}

TEST_CASE("timeout: a zone-inhibited zone still RECOVERS via debounce",
          "[zone_aggregator]") {
    // The permanent-inhibit trap: quarantined recovery means observations keep
    // rebuilding debounce while publication is suppressed (spec §3.1.1).
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 11, 5, t);
    feed(a, 2, 22, 5, t);
    for (int i = 0; i < 400; ++i) {
        t += 100;
        a.observe({rd(1, 0, ReadingKind::Incomplete), rd(2, 22)}, t);
    }
    REQUIRE(a.take_newly_inhibited().count(1) == 1);
    const auto snap = feed(a, 1, 11, 5, t);   // five complete readings
    REQUIRE(snap.has_value());
    REQUIRE((*snap).at(1) == 11);              // published again — not stuck
}

// ── Cold start (spec §5.3.1) ──
TEST_CASE("cold start: repeated incompletes publish NOTHING", "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    for (int i = 0; i < 100; ++i) {
        t += 100;
        REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::NoDigits)}, t).has_value());
    }
}

TEST_CASE("cold start: first valid value before the timeout publishes normally",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    for (int i = 0; i < 50; ++i) { t += 100; a.observe({rd(1, 0, ReadingKind::NoDigits)}, t); }
    const auto snap = feed(a, 1, 77, 5, t);
    REQUIRE(snap.has_value());
    REQUIRE((*snap).at(1) == 77);
}

TEST_CASE("cold start: timeout with no previous valid value inhibits and emits nothing",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    for (int i = 0; i < 400; ++i) {   // 40s, never a complete reading
        t += 100;
        REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::NoDigits)}, t).has_value());
    }
    REQUIRE(a.take_newly_inhibited().count(1) == 1);
}

// ── Review fix wave (lifecycle) ──

TEST_CASE("DEFECT1: timeout fires with a baseline at t==0 (cold start)",
          "[zone_aggregator]") {
    // A zone first seen at t==0 that never produces a Complete reading. Before
    // the fix, the sweep guarded on `base != 0`, so first_seen_ms==0 could never
    // exceed the hold timeout -- the zone would NEVER inhibit no matter how long
    // it went un-completed.
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::NoDigits)}, t).has_value());  // first_seen_ms = 0
    for (int i = 0; i < 400; ++i) {   // 40s, never a complete reading
        t += 100;
        REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::NoDigits)}, t).has_value());
    }
    REQUIRE(a.take_newly_inhibited().count(1) == 1);
}

TEST_CASE("DEFECT1: timeout fires with a baseline at t==0 (warm path)",
          "[zone_aggregator]") {
    // A zone whose LATEST Complete reading lands at t==0 (last_complete_ms==0).
    // Before the fix, `base != 0` guarded this out too.
    ZoneAggregator a(5, 10000);
    int64_t t = -500;  // feed() pre-increments by 100, so the 5th complete frame lands at t==0
    REQUIRE(feed(a, 1, 42, 5, t).has_value());
    REQUIRE(t == 0);
    for (int i = 0; i < 400; ++i) {   // 40s of incomplete frames past the t==0 baseline
        t += 100;
        a.observe({rd(1, 0, ReadingKind::Incomplete)}, t);
    }
    REQUIRE(a.take_newly_inhibited().count(1) == 1);
}

TEST_CASE("DEFECT2: a zone whose timeout and expiry are both due in the same "
          "call is expired, not escalated, and re-observation is fresh",
          "[zone_aggregator]") {
    // Zone 1 stabilises, then stops being observed AT ALL (unlike the hold
    // tests, no incomplete frames keep refreshing last_seen_ms). The NEXT call
    // that mentions zone 1 at all is a single huge jump past BOTH the 10s
    // expiry window and the 30s hold-timeout window -- so both conditions
    // become true in the SAME sweep, reproducing the ordering defect (as
    // opposed to expiry quietly winning across many small, incrementally
    // ticked calls, which would mask the bug).
    ZoneAggregator a(5, 10000);  // expiry_ms = 10000, hold_timeout_ms = 30000 (default)
    int64_t t = 0;
    feed(a, 1, 42, 5, t);   // zone 1 stable at 42, last_complete_ms == last_seen_ms == t
    feed(a, 2, 99, 5, t);   // zone 2 stable too, keeps the snapshot non-empty

    const int64_t jump = t + 31000;  // 31s past zone 1's last activity: > both windows
    a.observe({rd(2, 99)}, jump);    // ONE call; zone 1 is not in this reading list

    // Zone 1 must have been expired, not escalated -- no orphaned inhibit event.
    REQUIRE(a.take_newly_inhibited().count(1) == 0);

    // Re-observing zone 1 now must behave as a genuinely fresh zone: it needs a
    // full fresh debounce run (stable_frames_ = 5) before it emits again, and it
    // must NOT be pre-inhibited (an orphaned zone_inhibit_ entry would suppress
    // its publication even after re-stabilising).
    int64_t t2 = jump;
    auto snap = feed(a, 1, 42, 5, t2);
    REQUIRE(snap.has_value());
    REQUIRE(snap->at(1) == 42);
}

TEST_CASE("DEFECT3: evicting an already-inhibited zone drops it from "
          "newly_inhibited_ too -- no false alarm for a zone that no longer exists",
          "[zone_aggregator]") {
    // Escalate zone 1 to inhibited, but do NOT drain take_newly_inhibited() yet
    // (mirrors a caller that hasn't polled since the escalation). Then evict the
    // zone before it is ever drained. evict_zones() must remove it from
    // newly_inhibited_ (and zone_inhibit_) as well as zones_/last_sent_ -- a
    // zone that no longer exists must not later be reported as newly inhibited.
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 11, 5, t);
    feed(a, 2, 22, 5, t);
    for (int i = 0; i < 400; ++i) {   // 40s of incomplete -> zone 1 escalates to inhibited
        t += 100;
        a.observe({rd(1, 0, ReadingKind::Incomplete), rd(2, 22)}, t);
    }

    a.evict_zones({1});   // evicted BEFORE the caller ever drained the escalation

    REQUIRE(a.take_newly_inhibited().count(1) == 0);   // no false alarm

    // Re-observing zone 1 must also behave as fresh: no leftover zone_inhibit_
    // entry should suppress its publication once it re-stabilises.
    const auto snap = feed(a, 1, 11, 5, t);
    REQUIRE(snap.has_value());
    REQUIRE(snap->at(1) == 11);
}

TEST_CASE("DEFECT4: a zone that recovers BEFORE the drain is not reported by "
          "take_newly_inhibited()",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    for (int i = 0; i < 400; ++i) {   // 40s -> escalates to inhibited (NOT drained yet)
        t += 100;
        a.observe({rd(1, 0, ReadingKind::Incomplete)}, t);
    }
    // Recover BEFORE draining take_newly_inhibited().
    REQUIRE(feed(a, 1, 42, 5, t).has_value());

    // The escalation has already been undone -- it must not be reported.
    REQUIRE(a.take_newly_inhibited().count(1) == 0);
}

TEST_CASE("DEFECT5: duplicate readings for one zone in one call (Complete then "
          "Incomplete) still owe and later emit a forced recovery report",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);  // zone 1 stable at 42, sent

    // One incomplete frame: breaks the run, arms needs_reannounce (has_last_valid).
    t += 100;
    a.observe({rd(1, 0, ReadingKind::Incomplete)}, t);

    // 4 of the 5 consecutive completes needed to recover -- NOT yet stable again.
    for (int i = 0; i < 4; ++i) { t += 100; a.observe({rd(1, 42)}, t); }

    // The 5th consecutive complete would complete the recovery run (count hits
    // stable_frames_, entering "recovered this call") -- but a DUPLICATE
    // Incomplete reading for the SAME zone follows it in the SAME call, which
    // resets count back to 0 and re-arms needs_reannounce. The zone's FINAL
    // state this call is Incomplete (still owing), so the bookkeeping must not
    // treat zone 1 as having completed its own recovery in this call.
    t += 100;
    a.observe({rd(1, 42), rd(1, 0, ReadingKind::Incomplete)}, t);

    // Finish the recovery run for real with 5 fresh consecutive completes.
    // Since the duplicate-reading call above must NOT have consumed
    // needs_reannounce, this must still force a report of the SAME value (42).
    const auto snap = feed(a, 1, 42, 5, t);
    REQUIRE(snap.has_value());
    REQUIRE(snap->at(1) == 42);
}
