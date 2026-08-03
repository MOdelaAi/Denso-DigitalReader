// Pure retry state machine for the brazing reporter: holds the latest snapshot
// we want the server to have (pending), the last one it 2xx-acked (delivered),
// and the one currently in flight, plus the backoff counter. Each event method
// returns the single next action for the (thin, Qt) shell to perform. No Qt —
// unit-tested like ZoneAggregator. Single-flight + latest-value-wins live here.
#pragma once

#include "brazing/zone_reading.h"   // ZoneValue

#include <map>
#include <optional>
#include <vector>

namespace denso::ui {

/// One instruction for the BrazingReporter shell.
struct RetryAction {
    enum class Kind { None, Send, ArmRetry };
    Kind kind = Kind::None;
    std::map<int, ZoneValue> snapshot;  // the payload to POST, when kind == Send
    int delay_ms = 0;             // retry delay, when kind == ArmRetry
};

class BrazingRetryPolicy {
public:
    /// start_ms = first retry delay; cap_ms = maximum retry delay.
    explicit BrazingRetryPolicy(int start_ms = 1000, int cap_ms = 30000);

    /// A new full snapshot to deliver. Resets backoff; sends now if idle.
    RetryAction submit(const std::map<int, ZoneValue>& snapshot);

    /// Result of the in-flight POST. ok == true iff HTTP 2xx received.
    RetryAction on_result(bool ok);

    /// The retry timer fired. Re-sends the current pending snapshot if idle.
    RetryAction on_retry_tick();

    /// The zone NUMBERS of a snapshot the server has not acked, ascending; or
    /// nullopt when nothing is undelivered. Read-only observation for the discard
    /// log a teardown emits (spec §6.6-R4) — it changes no state and drives no
    /// send/retry decision, so interleaving it anywhere is invisible to the
    /// machine.
    ///
    /// Deliberately exposes KEYS ONLY: reading values are process data and must
    /// never reach the bounded 24/7 log file, which leaves the appliance in
    /// support bundles.
    ///
    /// "Undelivered" is maybe_send()'s own test — pending_ != delivered_ — plus a
    /// non-empty pending_. The extra clause matters only in an unreachable state
    /// (ZoneAggregator never emits an empty snapshot); it keeps a caller from
    /// logging "discarded 0 zones", which would be noise carrying no information.
    std::optional<std::vector<int>> pending_zone_numbers() const;

private:
    RetryAction maybe_send();  // Send iff !in_flight_ && pending_ != delivered_

    int start_ms_;
    int cap_ms_;
    int backoff_ms_ = 0;       // 0 = not backing off; last delay otherwise
    bool in_flight_ = false;
    std::map<int, ZoneValue> pending_;
    std::map<int, ZoneValue> delivered_;
    std::map<int, ZoneValue> in_flight_snap_;
};

} // namespace denso::ui
