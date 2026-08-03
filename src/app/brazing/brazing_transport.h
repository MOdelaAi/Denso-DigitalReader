// The one-shot POST seam the BrazingReporter drives. Abstract so the reporter's
// retry logic can be exercised with a fake in tests, and so the real Qt-network
// client is the only thing that touches QNetworkAccessManager.
#pragma once

#include "brazing/zone_reading.h"   // ZoneValue

#include <functional>
#include <map>

namespace denso::ui {

struct BrazingTransport {
    virtual ~BrazingTransport() = default;

    /// Fire one POST of `zones`. Invoke `done(ok)` on completion, on the GUI
    /// thread. `ok` is true iff an HTTP 2xx was received; false on any network
    /// error, timeout, or non-2xx response.
    virtual void post(const std::map<int, ZoneValue>& zones,
                      std::function<void(bool)> done) = 0;
};

} // namespace denso::ui
