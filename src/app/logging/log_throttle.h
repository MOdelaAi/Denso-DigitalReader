// Episode-based log throttling — no Qt, unit-tested. A repeating failure (a
// camera that keeps failing to reconnect, a backend that keeps rejecting POSTs)
// should NOT log every attempt over a 24/7 run. Each throttled site owns one
// LogEpisode: it logs the first failure, then at most once per interval while
// still failing (carrying a suppressed count), and once on recovery (with total
// failures + downtime). Per-object state — never a global map keyed by arbitrary
// error text (that would grow unbounded).
#pragma once

#include <cstdint>

namespace denso::logging {

class LogEpisode {
public:
    static constexpr int64_t kDefaultIntervalMs = 5 * 60 * 1000;  // 5 minutes

    explicit LogEpisode(int64_t interval_ms = kDefaultIntervalMs)
        : interval_ms_(interval_ms) {}

    struct FailureLog {
        bool log = false;        // caller should emit a failure line now
        int64_t suppressed = 0;  // failures hidden since the last logged one
    };
    /// Record a failure at monotonic `now_ms`; returns whether to log now.
    FailureLog on_failure(int64_t now_ms);

    struct RecoveryLog {
        bool log = false;            // caller should emit a recovery line
        int64_t total_failures = 0;  // failures during the episode
        int64_t downtime_ms = 0;     // first failure → recovery
    };
    /// Record a success at monotonic `now_ms`; returns recovery info if an
    /// episode just ended (otherwise log=false).
    RecoveryLog on_success(int64_t now_ms);

    bool failing() const { return failing_; }

private:
    int64_t interval_ms_;
    bool failing_ = false;
    int64_t episode_start_ms_ = 0;
    int64_t last_logged_ms_ = 0;
    int64_t total_failures_ = 0;
    int64_t suppressed_since_log_ = 0;
};

} // namespace denso::logging
