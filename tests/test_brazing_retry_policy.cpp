#include "brazing/brazing_retry_policy.h"

#include <catch2/catch_test_macros.hpp>

using denso::ui::BrazingRetryPolicy;
using denso::ui::RetryAction;
using Kind = denso::ui::RetryAction::Kind;
using Snap = std::map<int, int>;

TEST_CASE("first submit sends immediately") {
    BrazingRetryPolicy p;
    RetryAction a = p.submit(Snap{{1, 500}, {2, 200}});
    REQUIRE(a.kind == Kind::Send);
    REQUIRE(a.snapshot == Snap{{1, 500}, {2, 200}});
}

TEST_CASE("failure then success delivers the same snapshot on retry") {
    BrazingRetryPolicy p;
    p.submit(Snap{{1, 500}});
    RetryAction f = p.on_result(false);
    REQUIRE(f.kind == Kind::ArmRetry);
    REQUIRE(f.delay_ms == 1000);
    RetryAction t = p.on_retry_tick();
    REQUIRE(t.kind == Kind::Send);
    REQUIRE(t.snapshot == Snap{{1, 500}});
    RetryAction ok = p.on_result(true);
    REQUIRE(ok.kind == Kind::None);  // nothing left to send
}

TEST_CASE("note.txt scenario: failed sends merge forward into one snapshot") {
    BrazingRetryPolicy p;
    // 1. {z1:500,z2:200} -> send -> fail
    REQUIRE(p.submit(Snap{{1, 500}, {2, 200}}).kind == Kind::Send);
    REQUIRE(p.on_result(false).kind == Kind::ArmRetry);
    // 2. new value merges z3 -> full snapshot -> send -> fail
    RetryAction s2 = p.submit(Snap{{1, 500}, {2, 200}, {3, 540}});
    REQUIRE(s2.kind == Kind::Send);
    REQUIRE(s2.snapshot == Snap{{1, 500}, {2, 200}, {3, 540}});
    REQUIRE(p.on_result(false).kind == Kind::ArmRetry);
    // 3. new value z1:600 merges -> send -> success
    RetryAction s3 = p.submit(Snap{{1, 600}, {2, 200}, {3, 540}});
    REQUIRE(s3.kind == Kind::Send);
    REQUIRE(s3.snapshot == Snap{{1, 600}, {2, 200}, {3, 540}});
    REQUIRE(p.on_result(true).kind == Kind::None);
}

TEST_CASE("backoff doubles to the cap and resets on success") {
    BrazingRetryPolicy p(1000, 30000);
    p.submit(Snap{{1, 1}});
    REQUIRE(p.on_result(false).delay_ms == 1000);
    REQUIRE(p.on_retry_tick().kind == Kind::Send);
    REQUIRE(p.on_result(false).delay_ms == 2000);
    REQUIRE(p.on_retry_tick().kind == Kind::Send);
    REQUIRE(p.on_result(false).delay_ms == 4000);
    // …jump ahead: keep failing until the cap
    for (int i = 0; i < 10; ++i) {
        p.on_retry_tick();
        p.on_result(false);
    }
    p.on_retry_tick();
    REQUIRE(p.on_result(false).delay_ms == 30000);  // capped
    // recover
    p.on_retry_tick();
    REQUIRE(p.on_result(true).kind == Kind::None);
    // a fresh failure starts from 1s again
    p.submit(Snap{{1, 2}});
    REQUIRE(p.on_result(false).delay_ms == 1000);
}

TEST_CASE("submit resets backoff to fast start") {
    BrazingRetryPolicy p;
    p.submit(Snap{{1, 1}});
    REQUIRE(p.on_result(false).delay_ms == 1000);
    p.on_retry_tick();
    REQUIRE(p.on_result(false).delay_ms == 2000);
    // a new value arrives while idle (no POST in flight) -> sends immediately,
    // and the backoff is reset.
    RetryAction s = p.submit(Snap{{1, 9}});
    REQUIRE(s.kind == Kind::Send);
    REQUIRE(s.snapshot == Snap{{1, 9}});
    // if that send now fails, the delay is back to the fast start (proves reset).
    REQUIRE(p.on_result(false).delay_ms == 1000);
}

TEST_CASE("single-flight: submit mid-flight defers; newest wins on completion") {
    BrazingRetryPolicy p;
    REQUIRE(p.submit(Snap{{1, 1}}).kind == Kind::Send);   // in flight = {1:1}
    RetryAction mid = p.submit(Snap{{1, 2}});             // arrives mid-flight
    REQUIRE(mid.kind == Kind::None);                       // no second concurrent send
    RetryAction after = p.on_result(true);                // stale {1:1} succeeds
    REQUIRE(after.kind == Kind::Send);                     // …but pending moved on
    REQUIRE(after.snapshot == Snap{{1, 2}});
    REQUIRE(p.on_result(true).kind == Kind::None);         // {1:2} delivered
}

TEST_CASE("retry tick with nothing pending is a no-op") {
    BrazingRetryPolicy p;
    REQUIRE(p.on_retry_tick().kind == Kind::None);
    p.submit(Snap{{1, 1}});
    p.on_result(true);                    // delivered, idle
    REQUIRE(p.on_retry_tick().kind == Kind::None);
}

TEST_CASE("retry tick while a send is in flight is a no-op") {
    BrazingRetryPolicy p;
    REQUIRE(p.submit(Snap{{1, 1}}).kind == Kind::Send);  // in flight
    // A stale retry timer fires after a fresh send already started -> no second
    // concurrent POST (single-flight).
    REQUIRE(p.on_retry_tick().kind == Kind::None);
    REQUIRE(p.on_result(true).kind == Kind::None);       // delivered, idle
}
