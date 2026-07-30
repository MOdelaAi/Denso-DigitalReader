# Zone Runtime Overlay — local zone values on the camera grid

**Status:** agreed design (Claude + Codex debate, 2026-07-29). Supersedes nothing;
discharges part of the slice (b) §7 UI residual.

## 1. Problem

An operator testing the appliance cannot see what each zone is reading without
querying the backend. Worse, zone values are only *computed* when brazing
reporting is configured: `camera_grid.cpp:181` constructs the `ZoneReporter` only
under `bcfg.enabled && !bcfg.base_url.empty()`, and `camera_grid.cpp:340` passes
`review ? nullptr : reporter_.get()` as the `ZoneSink`. With reporting off there
is no sink, so `group_into_zones()` never runs and no zone state exists at all.

A local verification aid must not depend on the server it exists to cross-check.

## 2. Scope

**In:** always-on aggregation; a by-value runtime projection; 5 Hz grid polling;
per-tile overlay rendering of four states; draining `take_newly_inhibited()` to
log + status.

**Out (explicitly deferred):** backend delivery badges (`SENT`/`PENDING`/
`OFFLINE`/`FAILED`), delivery acknowledgement history, a second zone-state owner,
model/mode changes, Ball Leveler, DB migrations, packaging changes.

## 3. Locked architecture

```
Detection → group_into_zones() → ZoneAggregator / ZoneReporter
                                   ├── runtime_view() → grid overlay (ALWAYS)
                                   └── BrazingReporter → backend (OPTIONAL)
```

**Aggregation is unconditional.** `reporter_` is constructed whenever the grid is
built. Only `brazing_reporter_` and the marshalling callback stay conditional on
brazing config. This is safe today: both publish sites already guard
`if (snapshot && on_snapshot_)` (`zone_reporter.cpp:41,72`), so an absent callback
is a supported state, and `camera_grid.cpp:171` is already `if (reporter_)`.

**One policy authority.** `ZoneAggregator` remains the sole owner of debounce,
accepted value, last valid value, hold, expiry and inhibition. The UI consumes a
*projection*. No second cache, no recomputation in grid or tile.

## 4. Two ownership maps, different jobs

`ZoneReporter::camera_zones_` records ownership **from accepted observations**,
deliberately not from config, so a renumbered ROI still evicts the zone it
actually published under (§3.3b). It must not change.

But it cannot drive display: `on_zones()` returns on the camera-inhibit check
(`zone_reporter.cpp:26-28`) **before** recording ownership, so a camera inhibited
at boot — the quarantine case — never records any zone and could never render
`Paused`.

**Therefore a second, display-only map:** `configured_zones_` (camera_id →
set<zone_no>), installed by `CameraGrid` at reload from the persisted areas.
It routes display and detects conflicts. It holds no policy and never feeds
eviction.

## 5. Projection API

```cpp
// ZoneAggregator — pure, zone-keyed, no camera knowledge
enum class ZoneRuntimeState { Acquiring, Healthy, HoldingLastValid, Inhibited };
struct ZoneRuntimeState_Entry { int zone_no; ZoneRuntimeState state;
                                std::optional<int> value; };
std::vector<...> runtime_view() const;

// ZoneReporter — adds camera identity + camera-level override, under mutex_
enum class ZoneDisplayState { Acquiring, Healthy, HoldingLastValid,
                              Inhibited, Paused, Conflict };
struct ZoneRuntimeEntry { int64_t camera_id; int zone_no;
                          ZoneDisplayState state; std::optional<int> value; };
std::vector<ZoneRuntimeEntry> runtime_view() const;
```

Rules: taken under the existing `mutex_`; returns copies; exposes no counters, no
`last_sent_`, no `Debounce`, no delivery internals, no mutable references. Every
entry carries **both** `camera_id` and `zone_no`. Nothing joins by `zone_no` alone.

## 6. State derivation

An earlier proposal derived "last reading was complete" from
`last_complete_ms == last_seen_ms`. **Rejected — unsound.** After an incomplete
frame the next complete frame refreshes `last_complete_ms` but may sit at
`count == 1`, so the rule reports `Healthy` while showing the *old* stable value;
and a Complete-then-Incomplete pair inside one millisecond aliases to equal
timestamps. Timestamp equality is not a state machine.

Instead `Debounce` gains one explicit field, `last_reading_complete`, set true on
a `Complete` reading and false otherwise. This records the aggregator's own
observation phase — it is not a second policy.

| State | Condition | Value shown |
|---|---|---|
| `Inhibited` | `zone_inhibit_.count(zone_no)` | none |
| `Healthy` | `has_stable && last_reading_complete && count >= stable_frames_` | `stable` |
| `HoldingLastValid` | `has_last_valid` and not Healthy | `last_valid` |
| `Acquiring` | neither stable nor last-valid | none |

A zone mid-transition (reading fine, new candidate still debouncing) renders as
`HoldingLastValid`: it is holding the last accepted value while the candidate
earns acceptance. That is honest and never claims `Healthy` for an unaccepted
number.

Reporter-level overrides, applied after the zone-level state:
- **`Paused`** — camera in `inhibited_cameras_`. Overrides everything; no value.
- **`Conflict`** — `zone_no` claimed by >1 camera in `configured_zones_`. No
  value, on every claiming tile. Saves already reject duplicates
  (`repo.cpp:251`), so this is legacy/corrupt-DB only; reusing `Inhibited` would
  misstate the cause and omitting the entry would hide broken config.

## 7. Grid polling

A ~5 Hz `QTimer` owned by `CameraGrid`: poll `runtime_view()`, group by
`camera_id`, push each camera's slice to its own tile, and **only when that
tile's data changed** (value comparison). Cameras with no zones are cleared.
The timer is stopped and disconnected in `clear()` **before** `reporter_.reset()`,
preserving the existing worker-join-before-reporter teardown order. Widgets are
touched only on the GUI thread.

## 8. Newly-inhibited visibility

`take_newly_inhibited()` currently has **no production caller** (tests only). The
same 5 Hz tick drains it on the GUI thread and, per zone, emits one
episode-collapsed ERROR through the existing logging system and reflects it in
the existing status output. Draining is destructive, so each escalation is
reported once; a drain also runs before teardown so a pending alarm is not lost.
No second status writer. Diagnostics carry zone numbers only — no URLs, no
credentials.

**Residual (not v1):** §7 also requires recovery events with episode duration and
persistent compositional status. `take_newly_inhibited()` carries neither
recovery events nor episode metadata, so this slice narrows to the
inhibit-onset half. The remainder stays owed.

## 9. Tile rendering

`CameraTile` gains `set_zone_runtime_view(...)` / `clear_zone_runtime_view()` and
stays **rendering-only**: it never queries the aggregator, calls the backend, or
computes debounce/expiry/inhibition, and holds no cross-camera state.

Compact panel, consistent zone ordering, no large opaque box:

```
Z1  128  OK
Z2   95  HOLD
Z3   --  ACQUIRING
Z4   --  INHIBITED
```

Placement must avoid the existing overlays: name (top-left), status dot + fps
(top-right), inhibit banner (bottom strip, 26 px). Minimum tile is 240×160.

## 10. Coordinate contract

`camera_tile.h:1` documents "aspect-fit" while `camera_tile.cpp:109-116` draws
into `QRectF(rect())` — stretch-to-fill. The stale comment is corrected first, so
overlay coordinates and the frame provably share one mapping. No other rendering
change.

## 11. Acceptance

- **Backend disabled** + detection active + zones defined ⇒ aggregation runs,
  `runtime_view()` is populated, the overlay updates, no backend object or
  callback exists, no null dereference.
- **Backend configured but offline** ⇒ values keep updating, hold/inhibit
  transitions continue, the overlay stays visible, retries never pause
  aggregation, delivery failure never clears the display.
- Identical detector input produces an identical projection with the backend
  enabled or disabled.
