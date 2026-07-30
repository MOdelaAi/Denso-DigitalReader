// What has actually reached status.json, and what is still owed.
//
// Split out of CameraGrid because the answer to "did the write succeed?" is
// load-bearing twice over, and both cases lose information if a failure is
// treated as a success:
//
//   • the current-state THROTTLE — the 5 Hz poll rewrites status.json only when
//     the zone picture moves, so the published cache may advance ONLY on a write
//     that actually committed. Recording an unpublished projection as published
//     would suppress every retry until the picture happened to change again,
//     leaving the file silently stale for as long as the appliance stayed steady;
//
//   • the onset BUFFER — ZoneAggregator hands escalations over DESTRUCTIVELY, so
//     once an onset has been drained this buffer is the only thing holding it.
//     Clearing it on a failed write would lose the alarm from status output for
//     good.
//
// Pure (no Qt widgets, no I/O, no clock): the grid performs the write and reports
// the outcome here. Bounded, because the appliance runs for months — see
// kMaxPending.
#pragma once

#include "health/status_file.h"   // ZoneInhibitRecord

#include <cstddef>
#include <set>
#include <utility>
#include <vector>

namespace denso::ui {

class ZoneStatusPublication {
public:
    /// {held, inhibited} zone numbers — the current-state half of the document.
    using Projection = std::pair<std::set<int>, std::set<int>>;

    /// The rotating log is the DURABLE record of every alarm (each onset is
    /// logged before it is ever queued here), so a status file that stays
    /// unwritable sheds the oldest buffered records rather than growing without
    /// limit. Losing a duplicate of a logged line is the lesser failure.
    static constexpr std::size_t kMaxPending = 64;

    /// Take ownership of onsets that have been drained and logged.
    void enqueue(const std::vector<health::ZoneInhibitRecord>& onsets);

    /// Records still owed to status.json.
    const std::vector<health::ZoneInhibitRecord>& pending() const { return pending_; }

    /// A write is due when nothing has been published yet, when an alarm is owed,
    /// or when the picture has moved. An owed alarm forces the write even against
    /// an unchanged picture: an expired zone leaves the projection entirely while
    /// still owing its onset.
    bool needs_write(const Projection& now) const;

    /// Report the outcome of one write_status_file() call. ONLY a success
    /// advances the published picture or clears the owed alarms.
    void on_write(bool ok, const Projection& written);

    /// Forget what was published (the grid was rebuilt) WITHOUT discarding an
    /// alarm that has been logged but not yet published.
    void reset_published();

private:
    // "Nothing published yet" is an explicit flag, NOT an empty `published_`.
    // An empty projection is a perfectly ordinary state (no held or inhibited
    // zones), so using it as the sentinel would make a rebuild whose new picture
    // is also empty look already-published — and if the file still described the
    // OLD grid's inhibited zones, nothing would ever correct it.
    bool published_valid_ = false;
    Projection published_;
    std::vector<health::ZoneInhibitRecord> pending_;
};

} // namespace denso::ui
