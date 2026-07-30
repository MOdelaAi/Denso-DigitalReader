#include "ui/camera/grid/zone_status_publication.h"

namespace denso::ui {

void ZoneStatusPublication::enqueue(
    const std::vector<health::ZoneInhibitRecord>& onsets) {
    pending_.insert(pending_.end(), onsets.begin(), onsets.end());
    if (pending_.size() > kMaxPending) {
        // Shed the OLDEST: the newest escalation is the one an operator looking
        // at a stale status file most needs to see, and every one of them is
        // already in the log.
        pending_.erase(pending_.begin(),
                       pending_.begin() +
                           static_cast<std::ptrdiff_t>(pending_.size() - kMaxPending));
    }
}

bool ZoneStatusPublication::needs_write(const Projection& now) const {
    return !published_valid_ || !pending_.empty() || now != published_;
}

void ZoneStatusPublication::on_write(bool ok, const Projection& written) {
    if (!ok) {
        // Nothing reached the file. Leave BOTH the published picture and the owed
        // alarms exactly as they were, so the next tick retries instead of
        // believing it already published.
        return;
    }
    published_valid_ = true;
    published_ = written;
    pending_.clear();
}

void ZoneStatusPublication::reset_published() {
    published_valid_ = false;
    published_ = Projection{};
}

} // namespace denso::ui
