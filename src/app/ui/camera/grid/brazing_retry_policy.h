// Pure retry state machine for the brazing reporter: holds the latest snapshot
// we want the server to have (pending), the last one it 2xx-acked (delivered),
// and the one currently in flight, plus the backoff counter. Each event method
// returns the single next action for the (thin, Qt) shell to perform. No Qt —
// unit-tested like ZoneAggregator. Single-flight + latest-value-wins live here.
#pragma once

#include <map>

namespace denso::ui {

/// One instruction for the BrazingReporter shell.
struct RetryAction {
    enum class Kind { None, Send, ArmRetry };
    Kind kind = Kind::None;
    std::map<int, int> snapshot;  // the payload to POST, when kind == Send
    int delay_ms = 0;             // retry delay, when kind == ArmRetry
};

class BrazingRetryPolicy {
public:
    /// start_ms = first retry delay; cap_ms = maximum retry delay.
    explicit BrazingRetryPolicy(int start_ms = 1000, int cap_ms = 30000);

    /// A new full snapshot to deliver. Resets backoff; sends now if idle.
    RetryAction submit(const std::map<int, int>& snapshot);

    /// Result of the in-flight POST. ok == true iff HTTP 2xx received.
    RetryAction on_result(bool ok);

    /// The retry timer fired. Re-sends the current pending snapshot if idle.
    RetryAction on_retry_tick();

private:
    RetryAction maybe_send();  // Send iff !in_flight_ && pending_ != delivered_

    int start_ms_;
    int cap_ms_;
    int backoff_ms_ = 0;       // 0 = not backing off; last delay otherwise
    bool in_flight_ = false;
    std::map<int, int> pending_;
    std::map<int, int> delivered_;
    std::map<int, int> in_flight_snap_;
};

} // namespace denso::ui
