#include "brazing/brazing_retry_policy.h"

#include <algorithm>

namespace denso::ui {

BrazingRetryPolicy::BrazingRetryPolicy(int start_ms, int cap_ms)
    : start_ms_(start_ms), cap_ms_(cap_ms) {}

RetryAction BrazingRetryPolicy::maybe_send() {
    // pending_ == delivered_ means "the server already has the latest" — nothing
    // to send. The initial state (both empty) is therefore idle, and a genuinely
    // empty snapshot is a no-op; ZoneAggregator only emits a non-empty snapshot
    // on a real change, so the empty case never arises in practice.
    if (in_flight_ || pending_ == delivered_) {
        return {};  // Kind::None
    }
    in_flight_ = true;
    in_flight_snap_ = pending_;
    RetryAction a;
    a.kind = RetryAction::Kind::Send;
    a.snapshot = pending_;
    return a;
}

RetryAction BrazingRetryPolicy::submit(const std::map<int, int>& snapshot) {
    pending_ = snapshot;
    backoff_ms_ = 0;  // a fresh value resets the retry cadence
    return maybe_send();
}

RetryAction BrazingRetryPolicy::on_result(bool ok) {
    in_flight_ = false;
    if (ok) {
        delivered_ = in_flight_snap_;
        backoff_ms_ = 0;
        return maybe_send();  // send again if pending moved on, else None
    }
    backoff_ms_ = (backoff_ms_ == 0)
                      ? start_ms_
                      : std::min(backoff_ms_ * 2, cap_ms_);
    RetryAction a;
    a.kind = RetryAction::Kind::ArmRetry;
    a.delay_ms = backoff_ms_;
    return a;
}

RetryAction BrazingRetryPolicy::on_retry_tick() {
    return maybe_send();
}

std::optional<std::vector<int>> BrazingRetryPolicy::pending_zone_numbers() const {
    if (pending_.empty() || pending_ == delivered_) {
        return std::nullopt;  // nothing the server is still owed
    }
    // std::map iterates in key order, so the zone numbers come out ascending with
    // no sort. Only the keys are copied — the values never leave this object.
    std::vector<int> zones;
    zones.reserve(pending_.size());
    for (const auto& [zone_no, value] : pending_) {
        (void)value;
        zones.push_back(zone_no);
    }
    return zones;
}

} // namespace denso::ui
