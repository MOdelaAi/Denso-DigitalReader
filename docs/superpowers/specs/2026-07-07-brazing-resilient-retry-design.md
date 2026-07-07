# Brazing Resilient Retry — Design

**Date:** 2026-07-07
**Status:** Approved (design), pending implementation plan
**Goal:** When a brazing zone POST fails (server down / unreachable / non-2xx),
keep retrying the **latest** zone snapshot until the server accepts it, instead
of dropping it. New detections merge forward into the pending snapshot in real
time, so the server always converges on the current values.

## Background

The [brazing zone reporter](2026-07-06-brazing-zone-reporter-design.md) POSTs the
combined `{"zoneN": value, …}` snapshot to `{base_url}/api/brazing/update` on
every **stable change**. Today `BrazingClient` is fire-and-forget: a failed /
slow / unreachable POST is logged and dropped, with no retry.

Two facts make this safe to improve cheaply:

1. **Every emitted snapshot is the full cumulative state.** `ZoneAggregator`
   already carries every zone's latest stable value forward and emits the
   complete `{zone_no → value}` map on each change. So delivery is
   **latest-value-wins**: we only ever need the server to receive the *newest*
   snapshot — intermediate lost ones are subsumed.
2. **The gap is only "no more changes."** Because the aggregator emits only on
   change, if the server is down and the display then sits static, the last
   value is never redelivered. That is the exact case this feature closes.

Operator intent (from `note.txt`):

```
1. {"zone1": 500, "zone2": 200} --> send failed
2. cache it; new value {"zone3": 540} merges -> {"zone1":500,"zone2":200,"zone3":540} --> send failed
3. cache it; new value {"zone1":600} merges -> {"zone1":600,"zone2":200,"zone3":540} --> 200 success
```

This is a **coalescing single-flight retry** of one pending snapshot — *not* a
queue of distinct payloads.

## Scope decisions (confirmed)

- **Persistence:** in-memory only. A pending undelivered snapshot lives in the
  GUI process; on app restart it is dropped and live detection repopulates the
  current values within seconds. No DB, no migration.
- **Retry cadence:** exponential backoff with a cap — 1s → ×2 → … → 30s, reset
  to the fast start on success **or** when a new snapshot arrives. Same shape as
  `stream_pacing`'s `next_backoff_ms`, but that helper caps at 10s; the 30s cap
  is computed inside the pure retry policy (see below) rather than reusing it.
- **UI:** silent background. No new widget; one throttled `qWarning` per failed
  attempt (as today). Delivery is observable on the `test-server` console.
- **Failure classification:** any network error, timeout, **or** non-2xx is
  retryable. The 30s backoff cap + latest-wins coalescing bound a persistent bad
  response (e.g. a 400) to one attempt per ≤30s of the newest value — it cannot
  spin. (Dead-lettering 4xx is deferred; not needed for latest-value-wins.)

## Architecture

The retry policy is separated from the raw transport, matching the codebase's
"pure/thin-Qt-shell + single responsibility" style. `ZoneAggregator` and
`ZoneReporter` are **unchanged** — they already emit full coalesced snapshots.

### 1. `BrazingTransport` (interface) — `src/app/ui/camera/grid/brazing_transport.h`

A seam so the retry coordinator is unit-testable with a stub (no real network):

```cpp
struct BrazingTransport {
    virtual ~BrazingTransport() = default;
    // Fire one POST of `zones`. Invoke `done(ok)` on completion (GUI thread).
    // `ok` == HTTP 2xx received; false on any network error / timeout / non-2xx.
    virtual void post(const std::map<int,int>& zones,
                      std::function<void(bool)> done) = 0;
};
```

### 2. `BrazingClient` (transport impl, slimmed) — `brazing_client.{h,cpp}`

Becomes **transport only**: implements `BrazingTransport`. `post()` fires the
async `QNetworkAccessManager` POST to `{base_url}/api/brazing/update` (bounded
5s `setTransferTimeout`, as today) and, in the reply `finished` handler, calls
`done(reply->error() == NoError && http_status is 2xx)`. It holds **no** retry
or pending state. Empty `base_url` → `done(false)` immediately (or the coordinator
skips it — decided at plan time; a disabled reporter is not wired at all, as
today).

### 3a. `BrazingRetryPolicy` (pure, unit-tested) — `brazing_retry_policy.{h,cpp}`

The retry **state machine**, extracted as pure std (no Qt) so it unit-tests like
`ZoneAggregator`. Holds `pending_` / `delivered_` / in-flight snapshot + a bool,
and the backoff counter (1s start, ×2, 30s cap, computed here). Three event
methods each return a `RetryAction{ Kind{None,Send,ArmRetry}, snapshot, delay_ms }`
telling the shell what to do: `submit(snapshot)`, `on_result(bool ok)`,
`on_retry_tick()`. The **single-flight + latest-wins** invariant lives here (a
`maybe_send()` helper only yields `Send` when nothing is in flight and
`pending_ != delivered_`).

### 3b. `BrazingReporter` (new, GUI-thread `QObject` shell) — `brazing_reporter.{h,cpp}`

The thin **coordinator shell** (like `ZoneReporter`: no unit test, covered by the
integration smoke). Owns a `BrazingRetryPolicy`, a single-shot `QTimer`, and a
`BrazingTransport`; it just executes the policy's `RetryAction`s (Send → call the
transport; ArmRetry → start the timer for `delay_ms`) and feeds transport results
+ timer ticks back into the policy.

State:
- `pending_` — the latest snapshot we want the server to have (`std::map<int,int>`).
- `last_delivered_` — the last snapshot the server 2xx-acked.
- `in_flight_` — snapshot of the POST currently awaiting `done`, or empty.
- `backoff_ms_` — current retry delay (seeded/reset via `next_backoff_ms`).
- `retry_timer_` — single-shot, RAII-owned by the QObject.

Behavior:
- `submit(const std::map<int,int>& snapshot)` (called via `post_to_gui` from the
  `ZoneReporter` callback): set `pending_ = snapshot`; reset backoff; if nothing
  is in flight, kick a send.
- `kick_send()`: if `pending_ == last_delivered_` → idle (nothing to do). Else
  `in_flight_ = pending_`; `transport_->post(in_flight_, on_done)`.
- `on_done(ok)`:
  - `ok` → `last_delivered_ = in_flight_`; reset backoff; clear `in_flight_`. If
    `pending_ != last_delivered_` (a newer snapshot arrived mid-flight) → send
    again immediately; else idle.
  - `!ok` → clear `in_flight_`; arm `retry_timer_` for `backoff_ms_`, then advance
    `backoff_ms_ = next_backoff_ms(backoff_ms_)`.
- `retry_timer_` fires → `kick_send()` (re-copies the possibly-grown `pending_`).

**Single-flight + latest-wins:** at most one POST outstanding; the newest
`pending_` always wins; a stale in-flight success never marks a newer value
delivered (guarded by the `pending_ != last_delivered_` re-check). This is
exactly the `note.txt` example.

### 4. Wiring — `camera_grid.cpp` (`reload`)

Today: `BrazingClient` is created and the `ZoneReporter` callback does
`post_to_gui(client, [] { client->send(snap); })`.

New: create a `BrazingReporter` (which owns a `BrazingClient` as its transport);
the `ZoneReporter` callback marshals to `reporter->submit(snap)` via
`post_to_gui`. Ownership/teardown unchanged: the reporter+client outlive the
streams, which are stopped/joined first in `clear()`; the retry timer is torn
down with the `BrazingReporter` (no dangling timer, no work after teardown —
soak-reliability rule).

### Data flow

```
capture threads → ZoneReporter (mutex + ZoneAggregator)   [unchanged]
   → full snapshot on change → post_to_gui →
       BrazingReporter::submit(snapshot)          [GUI thread]
          pending_ = snapshot; reset backoff; kick_send() if idle
          → BrazingTransport::post(in_flight_) → done(ok)
               ok   → last_delivered_ = in_flight_; if pending_ moved on, send again; else idle
               fail → arm retry_timer_(backoff_ms_); backoff_ms_ = next_backoff_ms(...)
                        timer fires → kick_send()   (retries newest pending_)
```

## Threading & error handling

- `BrazingReporter` lives entirely on the GUI thread; `submit` is only ever
  reached via `post_to_gui`, so no locking is needed inside it. Capture threads
  never touch it (they touch only `ZoneReporter`, unchanged).
- The transport POST is async with a bounded 5s timeout; a stuck/unreachable
  server never wedges the UI or a capture thread — it just yields `done(false)`.
- Failures log one throttled `qWarning` per attempt (existing behavior).
- Backoff resets on success and on any new value, so recovery is within ~1s of
  the server returning (or immediately when a fresh detection arrives).

## Testing & verification

- **Unit (Catch2, pure — no Qt event loop):** `BrazingRetryPolicy` driven by a
  scripted `submit` / `on_result(ok)` / `on_retry_tick` sequence, asserting the
  returned `RetryAction`s:
  - fail → pending retained; retry re-sends the same snapshot.
  - `submit` of a new snapshot while failing → next send carries the merged
    latest (the `note.txt` 3-step example, asserted end to end).
  - success → `last_delivered_` advances; no further send when idle.
  - new `submit` mid-flight → after the in-flight `on_result(ok)`, the newer
    snapshot is sent (stale success does not mark it delivered).
  - backoff sequence: `on_result(false)` returns `ArmRetry` with 1s, 2s, 4s … 30s
    (cap); reset to 1s on the next success or `submit`.
  - `on_retry_tick()` re-sends the current `pending_` when not in flight, else
    `None` — tested directly (pure, no timer/event loop).
- **Build gate:** MSYS2 UCRT64 — `cmake --build build` clean, `ctest` green.
- **Integration smoke:** point `base_url` at `test-server`
  (`d:\workspace\test-server`, `python server.py`); change a zone; stop the
  server mid-run and confirm the app keeps running and keeps retrying (backoff
  logs); restart the server and confirm the **latest** merged snapshot lands
  once, with the current values.

## What does not change

- `ZoneAggregator` / `ZoneReporter` (already emit full coalesced snapshots).
- `test-server` `/api/brazing/update` (already accepts these POSTs).
- The payload shape, the settings surface, migration state.

## Out of scope / deferred

- Cross-restart persistence of the pending snapshot (chose in-memory).
- Dead-lettering permanent 4xx (backoff cap already bounds spin).
- Any UI status indicator (chose silent background).
- Fixed-interval heartbeat sends.

## Documentation deliverables

- **`docs/ARCHITECTURE.md`** + **`CLAUDE.md`**: update the brazing pipeline notes
  from "best-effort, log-and-drop, no retry" to the coalescing single-flight
  retry (transport + `BrazingReporter`).
- **`README.md`** brazing section: note that the reporter now retries a downed
  server automatically (latest value wins).
