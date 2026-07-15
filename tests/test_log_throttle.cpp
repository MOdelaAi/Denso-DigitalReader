#include <catch2/catch_test_macros.hpp>

#include "logging/log_throttle.h"

using denso::logging::LogEpisode;

TEST_CASE("first failure logs, subsequent failures are suppressed", "[log]") {
    LogEpisode ep(/*interval_ms=*/1000);
    auto f0 = ep.on_failure(0);
    CHECK(f0.log);
    CHECK(f0.suppressed == 0);
    CHECK(ep.failing());

    CHECK_FALSE(ep.on_failure(100).log);
    CHECK_FALSE(ep.on_failure(500).log);
    CHECK_FALSE(ep.on_failure(999).log);  // still within the interval
}

TEST_CASE("a reminder logs once per interval with the suppressed count", "[log]") {
    LogEpisode ep(1000);
    ep.on_failure(0);              // logged
    ep.on_failure(200);            // suppressed 1
    ep.on_failure(400);            // suppressed 2
    auto rem = ep.on_failure(1000);  // interval elapsed → reminder
    CHECK(rem.log);
    CHECK(rem.suppressed == 2);
    // counter resets after a reminder
    ep.on_failure(1100);           // suppressed 1
    auto rem2 = ep.on_failure(2000);
    CHECK(rem2.log);
    CHECK(rem2.suppressed == 1);
}

TEST_CASE("recovery logs once with total failures and downtime", "[log]") {
    LogEpisode ep(1000);
    ep.on_failure(0);
    ep.on_failure(300);
    ep.on_failure(600);
    auto rec = ep.on_success(900);
    CHECK(rec.log);
    CHECK(rec.total_failures == 3);
    CHECK(rec.downtime_ms == 900);
    CHECK_FALSE(ep.failing());

    // A success with no active episode is a no-op.
    CHECK_FALSE(ep.on_success(1000).log);
}

TEST_CASE("a new episode after recovery starts fresh", "[log]") {
    LogEpisode ep(1000);
    ep.on_failure(0);
    ep.on_success(500);
    auto f = ep.on_failure(600);  // fresh episode → logs immediately
    CHECK(f.log);
    CHECK(f.suppressed == 0);
}
