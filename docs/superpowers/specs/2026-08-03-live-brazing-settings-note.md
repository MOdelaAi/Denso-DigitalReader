# Implementation note — live Backend settings + base-URL normalization

Two defects, one branch (`wip/live-brazing-settings`). Deliberately small: no new
reporting pipeline, no second normalization authority, no restart of anything.

## A. Live reload of the Backend reporting stack

**Seam chosen:** `CameraGrid::apply_brazing_config()`.

Today `build_zone_reporting()` (camera_grid.cpp) reads `brazing::load(db_)` once
per grid build, and — only when reporting is configured — constructs the
`BrazingReporter` AND captures it inside the `on_snapshot` callback that is baked
into the `ZoneReporter` at construction. Both facts have to change, and only
those two:

1. The `on_snapshot` callback is now **always** installed and marshals to the
   **grid** (`post_to_gui(this, …)`), dispatching to whatever `brazing_reporter_`
   holds at delivery time. So the sender becomes swappable without rebuilding the
   `ZoneReporter` — which is what keeps zone debounce/hold/overlay state alive
   across a settings change. The callback carries the grid `generation_` it was
   created in and drops itself after a `clear()`, replacing the lifetime guard the
   old "marshal to the BrazingReporter object" trick provided for free.
2. Sender construction moves out of `build_zone_reporting()` into
   `apply_brazing_config()`, which `build_zone_reporting()` calls once at the end.
   One construction site, used by boot and by Save.

`apply_brazing_config()` is idempotent and decides from three facts — is
reporting enabled, does the stored URL normalize, and is the live sender already
on that canonical URL:

| state | action |
|---|---|
| disabled / empty / invalid URL | `brazing_reporter_.reset()` — destroys the QTimer (retry cancelled), the transport and its QNAM (in-flight replies aborted); the `QPointer` in `BrazingReporter::apply` already stops any late `done()` re-entering. Queued payloads die with the policy. |
| enabled, no live sender | build `BrazingClient(canonical)` + `BrazingReporter`, then reset the delivery baseline (below) |
| enabled, URL changed | reset the old sender **first**, then build the replacement, then reset the baseline |
| enabled, same URL | **no-op** — no duplicate reporter, no duplicate POST, delivery state untouched |

No camera, stream, processor, engine or tile is touched: `reload_invocations_`
and `generation_` do not move, `streams_` is not read.

**Signal path:** `SettingsDialog::brazing_config_changed` (emitted only after
`brazing::save()` returns true — `save()` gains a checked `bool`, like
`mode::save`) → `MainWindow::on_brazing_config_changed` →
`CameraView::apply_brazing_config` → `CameraGrid::apply_brazing_config`.

### First reading after a reload

`ZoneAggregator::last_sent_` is what suppresses an unchanged value, so a sender
created after the fact would never hear about a zone that is already stable. New
`ZoneAggregator::reset_sent_baseline()` (forwarded by
`ZoneReporter::reset_delivery_baseline()`, under the reporter mutex) clears
**only** `last_sent_`. Debounce counters, hold state, inhibit sets and
`runtime_view()` are untouched, nothing is emitted, and no value is invented: the
next observation that meets the existing stable-frame bar simply finds an empty
baseline and publishes once. Normal unchanged-value suppression resumes from that
snapshot. Called only when a sender is actually created or replaced.

### The reload barrier

Snapshots reach the GUI thread as queued calls, so one published moments before
a Save can still be in the event queue when the replacement sender is built —
and would be handed to it. `reset_delivery_baseline()` therefore also **returns**
the sequence number of the last snapshot published before the call, read under
the same mutex so nothing can slip between the reset and the read.
`apply_brazing_config()` raises `last_applied_seq_` to it, which reuses the
existing drop-stale guard: a snapshot at or below the barrier belonged to the
retired sender and is discarded. One place judges a snapshot too old, not two.

## B. Base-URL normalization

One authority, in `denso_core`: `src/core/brazing/url.{h,cpp}`.

* `kEndpointPath = "/api/brazing/update"` — the single definition of the endpoint.
* `normalize_base_url(input) -> {ok, base_url, error}`: trims whitespace; empty
  input is OK and means "unset"; parses strictly; requires an `http`/`https`
  scheme and a host; rejects userinfo, query and fragment; accepts a path that is
  empty, `/`, or **exactly** `kEndpointPath` (optionally with one trailing slash,
  case-sensitive) and rejects every other path. Canonical output is
  `scheme://host[:port]`, no trailing slash.
* `endpoint_url(base)`: the defensive transport guard, built on the same
  normalizer. A base that normalizes gives `canonical + kEndpointPath`; anything
  the normalizer refuses gives an **empty string** — no endpoint, no request, and
  no rewriting of a path nobody validated.

`BrazingClient` composes its URL through `endpoint_url()` once at construction,
so an externally written config ending in the endpoint cannot produce the doubled
path, and one that would fail UI validation cannot reach the wire through a
construction path that skipped the dialog (it logs once and reports `done(false)`
instead). `SettingsDialog` and `CameraGrid` gate on `normalize_base_url()`. UI,
runtime and transport therefore share one rule by construction.

`brazing::save()` writes both rows in one checked transaction and rolls back on
failure, so a `false` result truthfully means nothing was persisted — which is
what lets the dialog refuse to emit and the runtime stay untouched.

UI: label "Server base URL", placeholder `http://192.168.1.112:8080`, help text
"The application automatically sends to /api/brazing/update.", plus an inline
status/error label. Save rejects invalid input with a visible message and does
not persist or emit; on success it writes the canonical value back into the
field. The Server page re-seeds from the DB on `showEvent`, so a mode switch that
set `brazing.enabled = 0` cannot leave a stale ticked box behind.

## Unchanged on purpose

Endpoint path, JSON payload + fixed-point serialization, zone numbering, debounce
`kStableFrames`, hold timeout, retry start/cap, the 5 s transfer timeout, Ball vs
Digital aggregator parameters, mode-switch's `brazing.enabled = 0`, and all
camera/model configuration.
