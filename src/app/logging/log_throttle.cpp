#include "logging/log_throttle.h"

namespace denso::logging {

LogEpisode::FailureLog LogEpisode::on_failure(int64_t now_ms) {
    ++total_failures_;
    if (!failing_) {
        // First failure of a new episode — always log.
        failing_ = true;
        episode_start_ms_ = now_ms;
        last_logged_ms_ = now_ms;
        suppressed_since_log_ = 0;
        return {true, 0};
    }
    // Already failing — log at most once per interval, carrying the suppressed
    // count since the last logged line.
    if (now_ms - last_logged_ms_ >= interval_ms_) {
        const int64_t suppressed = suppressed_since_log_;
        suppressed_since_log_ = 0;
        last_logged_ms_ = now_ms;
        return {true, suppressed};
    }
    ++suppressed_since_log_;
    return {false, 0};
}

LogEpisode::RecoveryLog LogEpisode::on_success(int64_t now_ms) {
    if (!failing_) {
        return {false, 0, 0};
    }
    const RecoveryLog r{true, total_failures_, now_ms - episode_start_ms_};
    failing_ = false;
    total_failures_ = 0;
    suppressed_since_log_ = 0;
    return r;
}

} // namespace denso::logging
