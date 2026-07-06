# Brazing Zone HTTP Reporter — Design

**Date:** 2026-07-06
**Status:** Approved (design), pending implementation plan
**Goal:** Read the multi-digit number in each camera zone, and push the current
value of every zone on the machine to a backend as a single combined JSON POST,
best-effort, whenever any zone's reading changes.

## Background

The app already runs YOLO digit detection per camera and confines detections to
named ROI polygons ("areas"). What it does *not* do yet is turn the loose digit
boxes into a **number**, or send anything to a server. This feature adds both.

Confirmed workflow (from the operator):

- A camera has **1 to 4 zones**. A zone is one ROI area — a region of the frame
  showing one multi-digit 7-segment number.
- The model detects individual digits (**0–9**). Within a zone, the digits are
  assembled **left-to-right** into a number (`0–999`, i.e. up to ~3 digits, but
  the count is not hard-limited).
- One machine has 4 cameras (for now) → up to **12 zones**.
- The current value of every zone is grouped into **one** payload and POSTed:

```
POST {base_url}/api/brazing/update
Content-Type: application/json

{ "zone1": 500, "zone2": 200 }
```

`{"zone2": 500}` alone (as in the original note) is just the moment when only
zone 2 has a known value yet — the payload carries whatever zones currently have
a value.

**Delivery model:** best-effort, latest-value-wins. No outbox, no queue, no
idempotency key. If a POST is lost, the next change re-sends the full snapshot
and supersedes it. (This is deliberately simpler than the DeepStream sibling's
reliable outbox — that project needs every event; this one only needs the server
to hold the current value.)

**Trigger:** on stable change. A zone's assembled value must be **stable** (same
value for N consecutive frames) before it counts; when a stable value differs
from what was last sent, a combined POST fires.

## The existing seam

`DetectionProcessor::process()` already computes `kept` — the per-frame,
per-camera digit detections (`NamedDetection{box, conf, name}`), confined to the
camera's ROI areas — and already calls a **dormant** `ReadingSink` hook with
them (`frame_processor.h`/`.cpp`). Its contract: invoked on the **capture
thread**, in the hot path; the implementation MUST NOT block or do I/O inline —
it hands off to a worker and returns.

This feature adds a parallel, purpose-built seam next to `ReadingSink` (leaving
`ReadingSink` untouched for the future DB logging feature): a `ZoneSink` that
receives **assembled zone values** rather than raw digit boxes.

## Architecture

Four new units (three pure/testable + one thin Qt network object), one processor
change, one migration, and a small config surface.

### 1. `zone_assembly` (pure, unit-tested) — `src/app/ui/camera/grid/zone_assembly.{h,cpp}`

The digits→number step, with no Qt/network. Depends only on `opencv2/core.hpp`
(for `NamedDetection`) and `camera.h` (for `CameraArea`/`Point`).

- `std::optional<int> assemble_zone_value(const std::vector<NamedDetection>& digits_in_zone);`
  — sort by box **x-center** ascending, concatenate each detection's `name`
  (a single `0–9` character), parse to `int`. Returns `nullopt` when the zone has
  no digits, or when the concatenation isn't a parseable integer.
- `std::vector<ZoneReading> group_into_zones(const std::vector<NamedDetection>& kept, const std::vector<camera::CameraArea>& areas);`
  — for each area with a zone number set, collect the kept digits whose box
  center falls inside that area's polygon (`camera::point_in_polygon`, existing),
  assemble, and emit a `ZoneReading{ int zone_no; int value; float conf; }` when
  a value is produced (`conf` = min digit confidence in the zone). Areas with no
  zone number, or that assemble to nothing, are skipped. Zones are assumed
  non-overlapping; a digit inside two areas is counted in each (documented, rare).

### 2. `DetectionProcessor` extension — `frame_processor.{h,cpp}`

Add an optional `ZoneSink* zone_sink_` member (mirrors the existing `sink_`
guard: a camera with no zone sink pays nothing). After `kept` is computed, if a
zone sink is present, call `group_into_zones(kept, areas_)` and pass the result:

```
struct ZoneSink {
    virtual ~ZoneSink() = default;
    // Capture thread, hot path. MUST hand off; never block/do I/O inline.
    virtual void on_zones(int64_t camera_id, const std::vector<ZoneReading>& zones) = 0;
};
```

`DetectionProcessor`'s constructor gains a trailing `ZoneSink* zone_sink =
nullptr` parameter.

### 3. `ZoneReporter` (shared, thread-safe) — `src/app/ui/camera/grid/zone_reporter.{h,cpp}`

One instance per machine, implements `ZoneSink`, wired to every camera's
`DetectionProcessor`. Owns:

- **Debounce state** per `(zone_no)`: the last-seen candidate value + a run
  count. A candidate becomes **stable** after `kStableFrames` (default 5)
  identical consecutive `on_zones` observations. A zone absent from a frame's
  `on_zones` does **not** reset its already-stable value (transient occlusion
  keeps the last good number).
- **Latest stable map** `zone_no → value` and the **last-sent** snapshot.

`on_zones` (called from multiple capture threads) takes a mutex, updates
debounce/stable state, and if any zone's stable value now differs from
last-sent, builds the full snapshot `{zone_no → value}` and marshals it to the
GUI thread (via `common::post_to_gui` to the `BrazingClient`), then records it as
last-sent. The pure debounce/change logic is factored into a unit-tested helper
(feed a sequence of observations, assert when a snapshot is emitted and its
contents).

### 4. `BrazingClient` (Qt6::Network) — `src/app/ui/camera/grid/brazing_client.{h,cpp}`

A `QObject` living on the GUI thread. `send(const QMap<int,int>& zones)` builds
`{"zone<n>": value, …}` with `QJsonObject`, and POSTs to
`{base_url}/api/brazing/update` via `QNetworkAccessManager` (async, non-blocking).
A **bounded timeout** (5000 ms, consistent with the soak-hardening pass) aborts a
stuck request. Best-effort: on network error / non-2xx / timeout, log via
`qWarning` (throttled to at most once/sec) and drop it — no retry queue. Reads
`enabled` + `base_url` from config; when disabled, `send` is a no-op (and the
grid skips wiring the reporter entirely, so there's zero per-frame cost).

### Data flow

```
capture thread (per camera)
  DetectionProcessor::process
    → kept digit boxes (existing)
    → group_into_zones(kept, areas)         [pure]
    → ZoneReporter::on_zones(cam_id, zones) [mutex]
         debounce → stable → change?         [pure logic]
         if changed: post_to_gui(snapshot) ──┐
                                             │ queued
GUI thread                                   ▼
  BrazingClient::send(snapshot)
    → QJsonObject → POST {base}/api/brazing/update  (async, 5s timeout, best-effort)
```

## Data model / migration v10

Add a nullable `zone` integer column to `camera_area`:

```sql
ALTER TABLE camera_area ADD COLUMN zone INTEGER;  -- NULL = not a reporting zone
```

- `camera::CameraArea` gains `std::optional<int> zone;`.
- `camera/repo` read/replace of areas carries the new column (existing area
  CRUD is `read`/`replace`; both extend by one field).
- Migration is additive and version-gated in `db::run_migrations` (never edit a
  shipped migration). New `user_version` = 10.

## Config surface

Persisted server config (two fields): **enabled** (bool) + **base_url** (string,
e.g. `http://192.168.1.50:8098`). Path is fixed at `/api/brazing/update`. No auth
(matches the note; a header hook can be added later if needed — out of scope).

- **Storage:** the existing typed settings store (`src/core/settings/`), keys
  `brazing.enabled` / `brazing.base_url`. (If the settings repo's typed API can't
  take ad-hoc keys, fall back to a tiny `brazing_config` row in the same v10
  migration — decided at plan time against the actual repo API.)
- **UI:** a compact **"Server"** section in the Settings dialog — an enable
  checkbox + a base-URL text field + Save. (Follows the thin-view + panel pattern
  of `NetworkPanel`; kept minimal.) An optional "Test" button that fires one probe
  POST is a nice-to-have, listed but not required.
- **Areas step:** the Areas page (`ui/camera/dialog/areas_page`) gains a **zone
  number** input (1–12, or blank) per area, alongside its name. Blank = the area
  is ROI-only and not reported. Zone numbers should be unique across the machine;
  a duplicate is last-writer-wins in the snapshot and logs a warning (not
  enforced in UI for v1).

## Assembly & debounce rules (decisions)

- **Order:** digits sorted by box x-center, ascending (left→right).
- **Value:** concatenated digit chars parsed as `int`; leading zeros collapse
  (`"050"` → `50`). JSON value is a **number**, not a string.
- **Empty frame for a zone:** does not update or clear that zone; the last stable
  value persists (occlusion tolerance).
- **Stability:** `kStableFrames = 5` identical consecutive observations (~⅓ s at
  15 fps) before a value is eligible to send. Tunable constant.
- **Confidence:** reuses the per-class confidence already configured on the
  camera's models; `ZoneReading.conf` is the min digit conf in the zone (carried
  for logging/threshold use, not currently gating the send beyond the existing
  per-class filter).
- **Snapshot contents:** every zone that currently has a stable value (full
  latest-value snapshot), not just the changed zone — matches latest-value-wins.

## Threading & error handling

- Assembly (`group_into_zones`) runs on the capture thread — pure, cheap, no
  allocation beyond the small zone vectors.
- `ZoneReporter::on_zones` is the only cross-thread contention point: one mutex,
  short critical section (no I/O). It never blocks a capture thread on the
  network — the POST is marshaled to the GUI thread.
- `BrazingClient` uses async `QNetworkAccessManager` with a 5 s timeout; failures
  are logged (throttled) and dropped. A stuck/unreachable server cannot wedge a
  capture thread or the UI.
- Teardown: the reporter and client are owned by `CameraGrid` (or the app shell)
  and outlive the streams, which are stopped/joined first on reload — matching the
  existing grid teardown ordering.

## Testing & verification

- **Unit tests (Catch2):**
  - `zone_assembly`: left-to-right ordering, leading zeros, single/multi-digit,
    empty → nullopt, non-digit name → nullopt; `group_into_zones` assigns digits
    to the right area and skips zoneless/empty areas.
  - `zone_reporter` debounce/change logic: a value only sends after
    `kStableFrames`; unchanged stable values don't re-send; a change in one zone
    sends the full snapshot; occlusion (empty frame) keeps the last value.
  - `camera/repo` area round-trip carries the new `zone` column.
- **Build gate:** MSYS2 UCRT64 — `cmake --build build` clean, `ctest` green.
- **On-device / integration smoke** (needs the test server + real cameras):
  point `base_url` at `test-server` (`d:\workspace\test-server`,
  `python server.py`), watch a zone's number change on the display, and confirm a
  combined `{"zoneN": …}` POST lands in the server log with the right values;
  stop the server and confirm the app keeps running (best-effort drop, no hang).

## Documentation deliverables

- **`README.md`** (repo root): a "Brazing zone reporting" section for other
  developers — what a zone is, how to set a zone number on an ROI, the payload
  shape, the enable/URL settings, and how to test against `test-server`. (Create
  it if absent; the file is a developer-facing quick start.)
- **`docs/ARCHITECTURE.md`** + **`CLAUDE.md`**: add the reporter pipeline to the
  camera/detection sections and the new `zone` column to the persistence model.

## Out of scope / deferred

- Reliable delivery (outbox/queue/idempotency/dead-letter) — the DeepStream
  sibling's model; not needed for latest-value-wins.
- Auth / TLS / per-request signing.
- Server-side (the backend that receives the POST). `test-server` is a stand-in
  for integration testing only.
- Heartbeat / fixed-interval sends, and the DB reading-log (`ReadingSink`) feature
  — both remain future efforts.
- Zone-number uniqueness enforcement in the UI (v1 warns, doesn't block).
