# Slice (b) — Readiness, Per-Zone Inhibition, Local Alarms — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the appliance correctly identify, isolate, and locally display zone faults — a faulty camera inhibits all its zones immediately, an incomplete digit reading softly holds one zone, and healthy zones keep reporting throughout.

**Architecture:** A pure readiness/integrity verdict in `denso_core` classifies faults by provenance (global blockers exit `EX_CONFIG`; per-zone issues boot degraded). A GUI-thread-owned `ZoneHealth` holds per-camera inhibit causes and drives a gate inside the existing `ZoneReporter` mutex. `ZoneAggregator` gains a bounded hold for incomplete readings. Wire format is unchanged.

**Tech Stack:** C++17, Qt6 (Core/Sql/Widgets/Network), OpenCV, Catch2 v3, CMake.

**Spec:** `docs/superpowers/specs/2026-07-20-zone-readiness-inhibition-design.md` — read §1.1 before starting.

## Global Constraints

- **Never claim per-zone fail-closed.** The backend is numeric-only; a faulted zone is invisible to it. Local alarm is the only fault channel (spec §1.1).
- **Wire format unchanged.** No new fields, no protocol change. Slice (c) owns reporting semantics.
- **Never emit an empty snapshot.** `build_brazing_payload({})` emits literal `"{}"`, which under an unverified backend could clear every zone (spec §3.3).
- **Never construct an integer for an incomplete reading** anywhere in the pipeline (spec §5.1).
- **`sync_models()`'s directory scan is RETAINED.** The production Jetson has no `manifest.json`; retiring the scan would globally block it (spec §2.3).
- **The asymmetry in spec §3.1.1 is load-bearing and must be repeated in code comments:** camera-level causes reject the *entire observation*; a zone-level hold-timeout suppresses *publication only*, while complete observations keep rebuilding recovery debounce. Collapsing these into one gate reintroduces a permanent-inhibit deadlock.
- **Build env (MSYS2 UCRT64):** `export PATH=/c/msys64/ucrt64/bin:$PATH` first, in every shell. A `build/` directory already exists (Ninja) — configure only if it is missing.
- Build: `cmake --build build` · Full suite: `ctest --test-dir build --output-on-failure`
- **Tag-scoped tests MUST use the Catch2 binary directly:** `./build/tests/denso_tests "[tag]"`. `ctest -R <tag>` matches test *names*, not tags, and silently reports "No tests were found!!!" — which reads as a passing run.
- Every new test file must be added to the `add_executable(denso_tests ...)` list in `tests/CMakeLists.txt`.
- Baseline before starting: **Windows `ctest -N` total = 415** (414 Catch2 + 1 `packaging_policy`); a run shows 414 passing + 1 skip, and ctest reports the Catch2 SKIP as `Failed` (the `run_migrate ... symlinked outside models_dir` case — no symlink privilege on Windows). Jetson total = 415 with the symlink case actually running. Verify deltas against `ctest -N` totals, never against a remembered pass count.

---

## File Structure

**Create:**
- `src/core/health/integrity.h` / `.cpp` — the pure readiness verdict (§2)
- `src/core/health/zone_health.h` / `.cpp` — per-camera cause-set owner, GUI thread, no mutex (§3.1–3.2)
- `src/core/health/status_file.h` / `.cpp` — atomic `status.json` writer (§7)
- `tests/test_integrity.cpp`, `tests/test_zone_health.cpp`, `tests/test_status_file.cpp`

**Modify:**
- `src/app/brazing/zone_reading.h` — add `ReadingKind`
- `src/app/camera/zone_assembly.h` / `.cpp` — sum type + gap guard (§5.1–5.2)
- `src/app/brazing/zone_aggregator.h` / `.cpp` — hold, cold start, timeout, evict (§5.3, §3.3c)
- `src/app/brazing/zone_reporter.h` / `.cpp` — gate, ownership, sequence (§3.3)
- `src/app/camera/frame_processor.h` / `.cpp` — infer-streak cause hook (§3.2)
- `src/app/ui/camera/grid/camera_grid.cpp` — wiring, drop-stale (§3.2, §8)
- `src/app/ui/camera/grid/camera_tile.h` / `.cpp` — `set_inhibited` (§7)
- `src/app/cli/run_headless.cpp` — `--check` exit contract (§2.1)
- `src/app/ui/startup.cpp` — replace both `app.exit(1)` (§8)
- `tests/CMakeLists.txt` — register new tests

---

### Task 1: Assembly sum type and the interim gap guard

Implements spec §5.1–§5.2. Pure, no Qt, no threading — the safest place to start.

**Files:**
- Modify: `src/app/brazing/zone_reading.h`
- Modify: `src/app/camera/zone_assembly.h`, `src/app/camera/zone_assembly.cpp`
- Test: `tests/test_zone_assembly.cpp` (exists — extend)

**Interfaces:**
- Produces: `denso::ui::ReadingKind{Complete,Incomplete,NoDigits}`; `ZoneReading{zone_no,value,conf,kind}`; `ZoneAssembly{kind,value}`; `ZoneAssembly assemble_zone_value(const std::vector<NamedDetection>&)`; `constexpr float kPitchPerHeight=0.70f, kGapFactor=1.60f`. `group_into_zones` now emits a reading for **every** zoned area, including `NoDigits`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_zone_assembly.cpp`:

```cpp
// Build a digit detection: name, box at (x,y) sized w*h.
static NamedDetection dg(const char* name, int x, int y, int w, int h) {
    NamedDetection d;
    d.name = name;
    d.box = cv::Rect(x, y, w, h);
    d.conf = 0.9f;
    return d;
}

TEST_CASE("assemble: three evenly spaced digits are Complete", "[zone_assembly]") {
    // height 40 -> pitch 28; centres 28 apart = exactly 1.0 pitch.
    const auto r = assemble_zone_value({dg("1", 0, 0, 20, 40),
                                        dg("2", 28, 0, 20, 40),
                                        dg("3", 56, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Complete);
    REQUIRE(r.value == 123);
}

TEST_CASE("assemble: an internal missing digit is Incomplete", "[zone_assembly]") {
    // "1_3": centres 56 apart = 2.0 pitch > 1.6 threshold.
    const auto r = assemble_zone_value({dg("1", 0, 0, 20, 40),
                                        dg("3", 56, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Incomplete);
}

TEST_CASE("assemble: no detections is NoDigits", "[zone_assembly]") {
    REQUIRE(assemble_zone_value({}).kind == ReadingKind::NoDigits);
}

TEST_CASE("assemble: an incomplete reading carries NO usable value", "[zone_assembly]") {
    const auto r = assemble_zone_value({dg("1", 0, 0, 20, 40),
                                        dg("3", 56, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Incomplete);
    REQUIRE(r.value == 0);  // never 13 — spec §5.1
}

// ─────────────────────────────────────────────────────────────────────────────
// KNOWN LIMITATIONS — spec §5.2 / §10. These are NOT correctness expectations.
// They assert the guard does NOT fire, pinning residuals we have explicitly
// accepted for this slice so a future contributor cannot assume they are solved.
// Slice (b2)'s calibrated anchor/slot work is what closes them; when it lands,
// these cases flip to Incomplete and these tests SHOULD be rewritten.
// Tagged [known_limit] so they can be listed on demand.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("KNOWN LIMITATION: a missing LEADING digit is undetectable",
          "[zone_assembly][known_limit]") {
    const auto r = assemble_zone_value({dg("2", 28, 0, 20, 40),
                                        dg("3", 56, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Complete);
    REQUIRE(r.value == 23);
}

TEST_CASE("KNOWN LIMITATION: a missing TRAILING digit is undetectable",
          "[zone_assembly][known_limit]") {
    const auto r = assemble_zone_value({dg("1", 0, 0, 20, 40),
                                        dg("2", 28, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Complete);
    REQUIRE(r.value == 12);
}

TEST_CASE("KNOWN LIMITATION: a single remaining detection is undetectable",
          "[zone_assembly][known_limit]") {
    const auto r = assemble_zone_value({dg("3", 56, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Complete);
    REQUIRE(r.value == 3);
}

TEST_CASE("assemble: more than three digits is Incomplete, not a bogus value",
          "[zone_assembly]") {
    const auto r = assemble_zone_value({dg("1", 0, 0, 20, 40), dg("2", 28, 0, 20, 40),
                                        dg("3", 56, 0, 20, 40), dg("4", 84, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Incomplete);
}

TEST_CASE("group_into_zones emits NoDigits for a zoned area with no detections",
          "[zone_assembly]") {
    camera::CameraArea a;
    a.zone = 7;
    a.points = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    const auto out = group_into_zones({}, {a}, 640.0f, 480.0f);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].zone_no == 7);
    REQUIRE(out[0].kind == ReadingKind::NoDigits);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `./build/tests/denso_tests "[zone_assembly]"`
Expected: compile error — `ReadingKind` undeclared, `assemble_zone_value` returns `std::optional<int>`.

- [ ] **Step 3: Add `ReadingKind` to `zone_reading.h`**

Replace the struct in `src/app/brazing/zone_reading.h`:

```cpp
namespace denso::ui {

/// How much of a zone's number the detector actually produced this frame.
/// `Complete` is the ONLY kind carrying a usable `value` — see spec §5.1.
enum class ReadingKind { Complete, Incomplete, NoDigits };

struct ZoneReading {
    int         zone_no = 0;
    int         value   = 0;   // meaningful ONLY when kind == Complete
    float       conf    = 0.0f;
    ReadingKind kind    = ReadingKind::Complete;
};

} // namespace denso::health
```

- [ ] **Step 4: Implement the sum type and guard**

In `src/app/camera/zone_assembly.h`, replace the `assemble_zone_value` declaration:

```cpp
/// Interim digit-completeness constants (spec §5.2). Pitch is estimated from box
/// HEIGHT, not width: "1" is much narrower than other glyphs, but seven-segment
/// digits share a consistent height. Tuned on-device in slice (b2).
constexpr float kPitchPerHeight = 0.70f;
constexpr float kGapFactor      = 1.60f;

struct ZoneAssembly {
    ReadingKind kind  = ReadingKind::NoDigits;
    int         value = 0;   // meaningful ONLY when kind == Complete
};

/// Assemble the digits in one zone. Returns Complete only when the digits form a
/// plausible contiguous number. LIMITATION (spec §5.2): this detects only an
/// obvious INTERNAL missing position. A missing leading digit, a missing
/// trailing digit, and a single-remaining-detection all return Complete with a
/// shorter value — they are geometrically indistinguishable from genuine short
/// readings and need slice (b2)'s calibrated slots.
ZoneAssembly assemble_zone_value(const std::vector<NamedDetection>& digits_in_zone);
```

In `src/app/camera/zone_assembly.cpp`, replace the function body:

```cpp
ZoneAssembly assemble_zone_value(const std::vector<NamedDetection>& digits_in_zone) {
    if (digits_in_zone.empty()) {
        return {ReadingKind::NoDigits, 0};
    }
    std::vector<const NamedDetection*> ordered;
    ordered.reserve(digits_in_zone.size());
    for (const NamedDetection& d : digits_in_zone) {
        ordered.push_back(&d);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const NamedDetection* a, const NamedDetection* b) {
                  return (a->box.x + a->box.width * 0.5) < (b->box.x + b->box.width * 0.5);
              });

    std::string digits;
    for (const NamedDetection* d : ordered) {
        digits += d->name;
    }
    // Anything unparseable is Incomplete, never a value. A >3-digit group means a
    // spurious extra detection; a non-digit label means the model emitted a class
    // we cannot place. Both must hold the previous value rather than POST.
    if (digits.size() > 3 ||
        !std::all_of(digits.begin(), digits.end(),
                     [](unsigned char c) { return c >= '0' && c <= '9'; })) {
        return {ReadingKind::Incomplete, 0};
    }

    // Gap guard: estimate pitch from median box height, then look for an adjacent
    // pair sitting materially more than one pitch apart. One missing interior
    // digit yields ~2.0 pitch, comfortably past the 1.60 threshold; the margin is
    // deliberate, because a FALSE gap freezes a healthy zone, which is worse than
    // the missing-digit bug this mitigates.
    if (ordered.size() >= 2) {
        std::vector<float> heights;
        heights.reserve(ordered.size());
        for (const NamedDetection* d : ordered) {
            heights.push_back(static_cast<float>(d->box.height));
        }
        std::sort(heights.begin(), heights.end());
        const float median_h = heights[heights.size() / 2];
        const float max_gap = kGapFactor * kPitchPerHeight * median_h;
        for (std::size_t i = 1; i < ordered.size(); ++i) {
            const float prev_c = ordered[i - 1]->box.x + ordered[i - 1]->box.width * 0.5f;
            const float cur_c  = ordered[i]->box.x + ordered[i]->box.width * 0.5f;
            if (cur_c - prev_c > max_gap) {
                return {ReadingKind::Incomplete, 0};
            }
        }
    }
    return {ReadingKind::Complete, std::stoi(digits)};
}
```

Replace the tail of `group_into_zones` so every zoned area emits a reading:

```cpp
        const ZoneAssembly a = assemble_zone_value(in_zone);
        // Emit for EVERY zoned area, including NoDigits: the aggregator needs the
        // liveness signal, or the 10s expiry erases a held zone (spec §5.3).
        ZoneReading r;
        r.zone_no = *area.zone;
        r.kind    = a.kind;
        r.value   = a.value;
        r.conf    = in_zone.empty() ? 0.0f : min_conf;
        out.push_back(r);
    }
    return out;
}
```

- [ ] **Step 5: Run to verify they pass**

Run: `./build/tests/denso_tests "[zone_assembly]"`
Expected: all `[zone_assembly]` cases PASS.

- [ ] **Step 6: Commit**

```bash
git add src/app/brazing/zone_reading.h src/app/camera/zone_assembly.h \
        src/app/camera/zone_assembly.cpp tests/test_zone_assembly.cpp
git commit -m "feat(zones): assembly sum type + interim gap guard

An incomplete reading no longer collapses into a valid-looking integer:
123 with a missed tens digit returned 13 and was POSTed as a real value.
Limits are pinned by negative tests (missing leading/trailing digit and
single-detection cases remain undetectable until slice b2)."
```

---

### Task 2: Aggregator hold semantics

Implements spec §5.3 (warm path). No timeout yet — Task 4 adds it.

**Files:**
- Modify: `src/app/brazing/zone_aggregator.h`, `.cpp`
- Test: `tests/test_zone_aggregator.cpp` (exists — extend)

**Interfaces:**
- Consumes: `ReadingKind` (Task 1).
- Produces: `Debounce` gains `has_last_valid`, `last_valid`, `last_complete_ms`, `first_seen_ms`, `needs_reannounce`. `observe()` signature unchanged.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_zone_aggregator.cpp`:

```cpp
static ZoneReading rd(int zone, int value, ReadingKind k = ReadingKind::Complete) {
    ZoneReading r; r.zone_no = zone; r.value = value; r.conf = 0.9f; r.kind = k;
    return r;
}
// Drive `n` identical complete frames; returns the last snapshot emitted.
static std::optional<std::map<int,int>> feed(ZoneAggregator& a, int zone, int value,
                                             int n, int64_t& t) {
    std::optional<std::map<int,int>> last;
    for (int i = 0; i < n; ++i) { t += 100; if (auto s = a.observe({rd(zone, value)}, t)) last = s; }
    return last;
}

TEST_CASE("hold: an incomplete reading does not change the reported value",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    REQUIRE(feed(a, 1, 42, 5, t).has_value());          // 42 becomes stable and is sent
    t += 100;
    REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::Incomplete)}, t).has_value());
}

TEST_CASE("hold: incomplete readings keep the zone alive past the expiry window",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    // 15s of incomplete frames — longer than the 10s expiry. The zone must NOT be
    // evicted, so no shrunk snapshot is emitted.
    for (int i = 0; i < 150; ++i) {
        t += 100;
        REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::Incomplete)}, t).has_value());
    }
}

TEST_CASE("hold: NoDigits also keeps the zone alive", "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    for (int i = 0; i < 150; ++i) {
        t += 100;
        REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::NoDigits)}, t).has_value());
    }
}

TEST_CASE("hold: recovery re-announces even when the value is UNCHANGED",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    for (int i = 0; i < 10; ++i) { t += 100; a.observe({rd(1, 0, ReadingKind::Incomplete)}, t); }
    const auto snap = feed(a, 1, 42, 5, t);   // same value as before the hold
    REQUIRE(snap.has_value());
    REQUIRE((*snap)[1] == 42);
}

TEST_CASE("hold: frames either side of a gap do not combine into one stable run",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 3, t);                                  // 3 of the 5 needed
    t += 100; a.observe({rd(1, 0, ReadingKind::Incomplete)}, t);
    REQUIRE_FALSE(feed(a, 1, 42, 3, t).has_value());        // 3 more must NOT reach 5
    REQUIRE(feed(a, 1, 42, 2, t).has_value());              // 5 consecutive completes
}

TEST_CASE("hold: a sibling zone on the same camera keeps reporting",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    for (int i = 0; i < 5; ++i) { t += 100; a.observe({rd(1, 11), rd(2, 22)}, t); }
    // Zone 1 goes incomplete; zone 2 changes and must still be reported.
    std::optional<std::map<int,int>> snap;
    for (int i = 0; i < 5; ++i) {
        t += 100;
        if (auto s = a.observe({rd(1, 0, ReadingKind::Incomplete), rd(2, 33)}, t)) snap = s;
    }
    REQUIRE(snap.has_value());
    REQUIRE((*snap)[2] == 33);
    REQUIRE((*snap)[1] == 11);   // zone 1 still carries its held value
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `./build/tests/denso_tests "[zone_aggregator]"`
Expected: FAIL — `rd()` won't compile until `ZoneReading::kind` is honoured, and holds are treated as observations.

- [ ] **Step 3: Extend `Debounce` in `zone_aggregator.h`**

```cpp
    struct Debounce {
        int     candidate = 0;
        int     count = 0;
        bool    has_stable = false;
        int     stable = 0;
        int64_t last_seen_ms = 0;      // ANY frame, incl. incomplete — liveness
        // ── Hold state (spec §5.3) ──
        bool    has_last_valid = false;
        int     last_valid = 0;
        int64_t last_complete_ms = 0;  // ONLY complete readings — hold timeout base
        int64_t first_seen_ms = 0;     // cold-start timeout base (spec §5.3.1)
        bool    needs_reannounce = false;
    };
```

- [ ] **Step 4: Implement the hold branch in `observe()`**

Replace the per-reading loop in `src/app/brazing/zone_aggregator.cpp`:

```cpp
    for (const ZoneReading& z : zones) {
        Debounce& d = zones_[z.zone_no];
        if (d.first_seen_ms == 0) {
            d.first_seen_ms = now_ms;
        }
        // Liveness is refreshed by ANY frame. An incomplete or no-digit frame still
        // proves the camera is alive; without this the 10s expiry would erase a held
        // zone long before the 30s hold timeout could run (spec §5.3).
        d.last_seen_ms = now_ms;

        if (z.kind != ReadingKind::Complete) {
            // Soft hold: break the stable run so frames either side of the gap cannot
            // combine into five "consecutive" observations. Leave stable/has_stable
            // and last_sent_ untouched so the held value keeps being reported, and
            // owe a re-announce so recovery reports even an UNCHANGED value.
            d.count = 0;
            if (d.has_last_valid) {
                d.needs_reannounce = true;
            }
            continue;   // NOT a fresh observation: last_complete_ms is not refreshed
        }

        d.last_complete_ms = now_ms;
        if (z.value == d.candidate) {
            ++d.count;
        } else {
            d.candidate = z.value;
            d.count = 1;
        }
        if (d.count >= stable_frames_) {
            const bool newly_stable = (!d.has_stable || d.stable != d.candidate);
            if (newly_stable) {
                d.has_stable = true;
                d.stable = d.candidate;
            }
            d.has_last_valid = true;
            d.last_valid = d.stable;
            const auto it = last_sent_.find(z.zone_no);
            if (it == last_sent_.end() || it->second != d.stable || d.needs_reannounce) {
                changed = true;
            }
        }
    }
```

Then clear the re-announce flags **only on commit**, immediately after `last_sent_ = snapshot;`:

```cpp
    last_sent_ = snapshot;
    // Consume re-announce ONLY when the snapshot is actually committed. Clearing it
    // on read would let a suppressed or superseded snapshot swallow the forced
    // report (spec §5.3).
    for (auto& [zone_no, d] : zones_) {
        if (snapshot.count(zone_no) > 0) {
            d.needs_reannounce = false;
        }
    }
    return snapshot;
```

- [ ] **Step 5: Run to verify they pass**

Run: `./build/tests/denso_tests "[zone_aggregator]"`
Expected: all `[zone_aggregator]` cases PASS.

- [ ] **Step 6: Commit**

```bash
git add src/app/brazing/zone_aggregator.h src/app/brazing/zone_aggregator.cpp \
        tests/test_zone_aggregator.cpp
git commit -m "feat(zones): bounded soft hold for incomplete readings

An incomplete or no-digit frame refreshes liveness but is not an observation:
the held value keeps reporting, the stable run breaks, and recovery
re-announces even an unchanged value."
```

---

### Task 3: Eviction, empty-snapshot suppression

Implements spec §3.3(c) and the empty-snapshot rule.

**Files:**
- Modify: `src/app/brazing/zone_aggregator.h`, `.cpp`
- Test: `tests/test_zone_aggregator.cpp`

**Interfaces:**
- Produces: `std::optional<std::map<int,int>> ZoneAggregator::evict_zones(const std::set<int>& zone_nos);` — erases from `zones_` **and** `last_sent_`, returning the new snapshot when the payload changed, or `nullopt` when nothing changed **or the result would be empty**.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("evict: removes the zone and emits the shrunk snapshot", "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    for (int i = 0; i < 5; ++i) { t += 100; a.observe({rd(1, 11), rd(2, 22)}, t); }
    const auto snap = a.evict_zones({1});
    REQUIRE(snap.has_value());
    REQUIRE(snap->count(1) == 0);
    REQUIRE((*snap)[2] == 22);
}

TEST_CASE("evict: never emits an EMPTY snapshot", "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    // Evicting the only zone would yield {} — which build_brazing_payload renders
    // as literal "{}" and could clear every zone on an unqualified backend.
    REQUIRE_FALSE(a.evict_zones({1}).has_value());
}

TEST_CASE("evict: evicting an unknown zone changes nothing", "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    REQUIRE_FALSE(a.evict_zones({99}).has_value());
}

TEST_CASE("evict: re-entry forces a fresh report of an unchanged value",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 11, 5, t);
    feed(a, 2, 22, 5, t);
    a.evict_zones({1});
    const auto snap = feed(a, 1, 11, 5, t);   // same value it had before eviction
    REQUIRE(snap.has_value());
    REQUIRE((*snap)[1] == 11);
}

TEST_CASE("observe: an all-empty result is suppressed, not emitted",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 1000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    t += 5000;   // past expiry with nothing observed -> would expire to {}
    REQUIRE_FALSE(a.observe({}, t).has_value());
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `./build/tests/denso_tests "[zone_aggregator]"`
Expected: FAIL — `evict_zones` not declared.

- [ ] **Step 3: Implement**

Declare in `zone_aggregator.h` (add `#include <set>`):

```cpp
    /// Drop these zones from both the debounce state and the last-sent payload,
    /// as an inhibit does. Returns the shrunk snapshot when the payload actually
    /// changed, else nullopt. NEVER returns an empty snapshot (spec §3.3).
    std::optional<std::map<int, int>> evict_zones(const std::set<int>& zone_nos);
```

Define in `zone_aggregator.cpp`:

```cpp
std::optional<std::map<int, int>> ZoneAggregator::evict_zones(
    const std::set<int>& zone_nos) {
    bool changed = false;
    for (const int zone_no : zone_nos) {
        zones_.erase(zone_no);
        if (last_sent_.erase(zone_no) > 0) {
            changed = true;
        }
    }
    if (!changed) {
        return std::nullopt;
    }
    return build_snapshot();
}
```

Factor the snapshot tail of `observe()` into a shared private helper, and make it
enforce the empty rule in one place:

```cpp
// Build the full snapshot of every zone holding a stable value and commit it.
// Returns nullopt when the result would be EMPTY: build_brazing_payload({})
// renders literal "{}" and, under an unverified backend, could clear every zone.
std::optional<std::map<int, int>> ZoneAggregator::build_snapshot() {
    std::map<int, int> snapshot;
    for (const auto& [zone_no, d] : zones_) {
        if (d.has_stable) {
            snapshot[zone_no] = d.stable;
        }
    }
    if (snapshot.empty()) {
        return std::nullopt;
    }
    last_sent_ = snapshot;
    for (auto& [zone_no, d] : zones_) {
        if (snapshot.count(zone_no) > 0) {
            d.needs_reannounce = false;
        }
    }
    return snapshot;
}
```

Declare `std::optional<std::map<int,int>> build_snapshot();` in the private section, and
replace `observe()`'s tail with `return build_snapshot();`.

- [ ] **Step 4: Run to verify they pass**

Run: `./build/tests/denso_tests "[zone_aggregator]"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/app/brazing/zone_aggregator.h src/app/brazing/zone_aggregator.cpp \
        tests/test_zone_aggregator.cpp
git commit -m "feat(zones): evict_zones + never emit an empty snapshot"
```

---

### Task 4: Hold timeout, cold start, and zone-level inhibit

Implements spec §5.3 (timeout) and §5.3.1 (cold start).

**Files:**
- Modify: `src/app/brazing/zone_aggregator.h`, `.cpp`
- Test: `tests/test_zone_aggregator.cpp`

**Interfaces:**
- Produces: `constexpr int64_t kHoldTimeoutMs = 30000;`; `std::set<int> ZoneAggregator::take_newly_inhibited();` returning zones that just escalated. Zone-level inhibit **suppresses publication only** — observations keep rebuilding debounce (spec §3.1.1).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("timeout: a hold that exceeds kHoldTimeoutMs inhibits the zone",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 11, 5, t);
    feed(a, 2, 22, 5, t);
    for (int i = 0; i < 400; ++i) {   // 40s of incomplete frames
        t += 100;
        a.observe({rd(1, 0, ReadingKind::Incomplete), rd(2, 22)}, t);
    }
    REQUIRE(a.take_newly_inhibited().count(1) == 1);
}

TEST_CASE("timeout: recovery BEFORE the timeout resumes normally",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    for (int i = 0; i < 100; ++i) {   // 10s — well under 30s
        t += 100;
        a.observe({rd(1, 0, ReadingKind::Incomplete)}, t);
    }
    REQUIRE(a.take_newly_inhibited().empty());
    REQUIRE(feed(a, 1, 42, 5, t).has_value());
}

TEST_CASE("timeout: a zone-inhibited zone still RECOVERS via debounce",
          "[zone_aggregator]") {
    // The permanent-inhibit trap: quarantined recovery means observations keep
    // rebuilding debounce while publication is suppressed (spec §3.1.1).
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 11, 5, t);
    feed(a, 2, 22, 5, t);
    for (int i = 0; i < 400; ++i) {
        t += 100;
        a.observe({rd(1, 0, ReadingKind::Incomplete), rd(2, 22)}, t);
    }
    REQUIRE(a.take_newly_inhibited().count(1) == 1);
    const auto snap = feed(a, 1, 11, 5, t);   // five complete readings
    REQUIRE(snap.has_value());
    REQUIRE((*snap)[1] == 11);                // published again — not stuck
}

// ── Cold start (spec §5.3.1) ──
TEST_CASE("cold start: repeated incompletes publish NOTHING", "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    for (int i = 0; i < 100; ++i) {
        t += 100;
        REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::NoDigits)}, t).has_value());
    }
}

TEST_CASE("cold start: first valid value before the timeout publishes normally",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    for (int i = 0; i < 50; ++i) { t += 100; a.observe({rd(1, 0, ReadingKind::NoDigits)}, t); }
    const auto snap = feed(a, 1, 77, 5, t);
    REQUIRE(snap.has_value());
    REQUIRE((*snap)[1] == 77);
}

TEST_CASE("cold start: timeout with no previous valid value inhibits and emits nothing",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    for (int i = 0; i < 400; ++i) {   // 40s, never a complete reading
        t += 100;
        REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::NoDigits)}, t).has_value());
    }
    REQUIRE(a.take_newly_inhibited().count(1) == 1);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `./build/tests/denso_tests "[zone_aggregator]"`
Expected: FAIL — `take_newly_inhibited` not declared.

- [ ] **Step 3: Implement**

In `zone_aggregator.h`:

```cpp
constexpr int64_t kHoldTimeoutMs = 30000;  // hold -> Inhibited escalation (spec §5.3)
```

Add to the public section:

```cpp
    /// Zones that escalated from hold to inhibited since the last call. Draining
    /// is destructive so a caller raises each alarm exactly once.
    std::set<int> take_newly_inhibited();
```

Add to the private section:

```cpp
    int64_t       hold_timeout_ms_ = kHoldTimeoutMs;
    std::set<int> zone_inhibit_;         // publication suppressed; debounce continues
    std::set<int> newly_inhibited_;      // drained by take_newly_inhibited()
```

In `zone_aggregator.cpp`, inside `observe()` after the reading loop and before the
expiry sweep, add the timeout sweep:

```cpp
    // Hold-timeout sweep. One rule covers warm and cold start: measure against the
    // last COMPLETE reading, or — for a zone that has never read successfully —
    // against its first observation of any kind (spec §5.3.1).
    for (auto& [zone_no, d] : zones_) {
        if (zone_inhibit_.count(zone_no) > 0) {
            continue;   // already inhibited; recovery is handled in the stable branch
        }
        const int64_t base = d.has_last_valid ? d.last_complete_ms : d.first_seen_ms;
        if (base != 0 && now_ms - base > hold_timeout_ms_) {
            zone_inhibit_.insert(zone_no);
            newly_inhibited_.insert(zone_no);
            if (last_sent_.erase(zone_no) > 0) {
                changed = true;
            }
        }
    }
```

In the stable branch, clear the inhibit when a complete reading reaches stability —
this is what makes quarantined recovery work:

```cpp
        if (d.count >= stable_frames_) {
            const bool newly_stable = (!d.has_stable || d.stable != d.candidate);
            if (newly_stable) { d.has_stable = true; d.stable = d.candidate; }
            d.has_last_valid = true;
            d.last_valid = d.stable;
            // Quarantined recovery: a hold-timeout inhibit suppresses PUBLICATION
            // only, so the zone kept accumulating debounce and can clear itself
            // here. Gating the observation instead would deadlock it forever.
            const bool was_inhibited = zone_inhibit_.erase(z.zone_no) > 0;
            const auto it = last_sent_.find(z.zone_no);
            if (it == last_sent_.end() || it->second != d.stable ||
                d.needs_reannounce || was_inhibited) {
                changed = true;
            }
        }
```

And exclude inhibited zones from the payload in `build_snapshot()`:

```cpp
    for (const auto& [zone_no, d] : zones_) {
        if (d.has_stable && zone_inhibit_.count(zone_no) == 0) {
            snapshot[zone_no] = d.stable;
        }
    }
```

Define the drain:

```cpp
std::set<int> ZoneAggregator::take_newly_inhibited() {
    std::set<int> out;
    out.swap(newly_inhibited_);
    return out;
}
```

- [ ] **Step 4: Run to verify they pass**

Run: `./build/tests/denso_tests "[zone_aggregator]"`
Expected: PASS — in particular "a zone-inhibited zone still RECOVERS".

- [ ] **Step 5: Commit**

```bash
git add src/app/brazing/zone_aggregator.h src/app/brazing/zone_aggregator.cpp \
        tests/test_zone_aggregator.cpp
git commit -m "feat(zones): bounded hold timeout + cold start + quarantined recovery

A hold escalates to zone-inhibited after kHoldTimeoutMs. Inhibition suppresses
publication only: observations keep rebuilding debounce, so the zone can clear
itself. Gating the observations instead would deadlock it permanently."
```

---

### Task 5: Reporter gate, ownership, and sequence

Implements spec §3.3(a)(b)(d).

**Files:**
- Modify: `src/app/brazing/zone_reporter.h`, `.cpp`
- Modify: `src/app/CMakeLists.txt` — **move `brazing/zone_reporter.cpp` from the `denso` exe into the `denso_brazing` library**, and update the stale comment at line ~28 claiming the `ZoneSink` impl must stay in the executable. It has **zero Qt dependencies** (`chrono`, `utility`, `functional`, `map`, `mutex` only), so it belongs in `denso_brazing` — "the pure, testable reporting logic". `denso_tests` links `denso_brazing` but **not** the `denso` exe, so without this move none of this task's tests can link. Compile it in exactly one target.
- Modify: `CLAUDE.md` — the `denso_brazing` row and the `zone_reporter` row both state it stays in `denso`; correct both.
- Test: `tests/test_zone_reporter.cpp` (create)

**Interfaces:**
- Consumes: `ZoneAggregator::evict_zones`, `take_newly_inhibited` (Tasks 3–4).
- Produces: `ZoneReporter(std::function<void(const std::map<int,int>&, uint64_t seq)>, int stable_frames = kStableFrames)`; `void set_camera_inhibited(int64_t camera_id, bool on);`. **Callback signature gains `seq`** — Task 9 updates the call site.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_zone_reporter.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "brazing/zone_reporter.h"
#include <map>
#include <vector>
using namespace denso::ui;

namespace {
struct Captured { std::map<int,int> snap; uint64_t seq; };

static ZoneReading rd(int zone, int value, ReadingKind k = ReadingKind::Complete) {
    ZoneReading r; r.zone_no = zone; r.value = value; r.conf = 0.9f; r.kind = k;
    return r;
}
} // namespace

TEST_CASE("reporter: an inhibited camera's observations are DROPPED", "[zone_reporter]") {
    std::vector<Captured> got;
    ZoneReporter r([&](const std::map<int,int>& s, uint64_t q){ got.push_back({s,q}); }, 5);
    for (int i = 0; i < 5; ++i) r.on_zones(1, {rd(10, 42)});
    REQUIRE(got.size() == 1);
    got.clear();
    r.set_camera_inhibited(1, true);
    // Simulates the resurrection race: an observation that was already in flight
    // when the inhibit landed must not re-create the zone.
    for (int i = 0; i < 20; ++i) r.on_zones(1, {rd(10, 99)});
    for (const auto& c : got) REQUIRE(c.snap.count(10) == 0);
}

TEST_CASE("reporter: inhibiting a camera evicts its zones and emits", "[zone_reporter]") {
    std::vector<Captured> got;
    ZoneReporter r([&](const std::map<int,int>& s, uint64_t q){ got.push_back({s,q}); }, 5);
    for (int i = 0; i < 5; ++i) r.on_zones(1, {rd(10, 42)});
    for (int i = 0; i < 5; ++i) r.on_zones(2, {rd(20, 7)});
    got.clear();
    r.set_camera_inhibited(1, true);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].snap.count(10) == 0);
    REQUIRE(got[0].snap.at(20) == 7);
}

TEST_CASE("reporter: release lets the camera report again with a forced value",
          "[zone_reporter]") {
    std::vector<Captured> got;
    ZoneReporter r([&](const std::map<int,int>& s, uint64_t q){ got.push_back({s,q}); }, 5);
    for (int i = 0; i < 5; ++i) r.on_zones(1, {rd(10, 42)});
    for (int i = 0; i < 5; ++i) r.on_zones(2, {rd(20, 7)});
    r.set_camera_inhibited(1, true);
    got.clear();
    r.set_camera_inhibited(1, false);
    for (int i = 0; i < 5; ++i) r.on_zones(1, {rd(10, 42)});   // unchanged value
    REQUIRE_FALSE(got.empty());
    REQUIRE(got.back().snap.at(10) == 42);
}

TEST_CASE("reporter: ownership is recorded, so a renumbered ROI evicts the OLD zone",
          "[zone_reporter]") {
    std::vector<Captured> got;
    ZoneReporter r([&](const std::map<int,int>& s, uint64_t q){ got.push_back({s,q}); }, 5);
    for (int i = 0; i < 5; ++i) r.on_zones(1, {rd(7, 42)});    // camera 1 owns zone 7
    for (int i = 0; i < 5; ++i) r.on_zones(2, {rd(20, 1)});
    got.clear();
    r.set_camera_inhibited(1, true);
    REQUIRE_FALSE(got.empty());
    REQUIRE(got.back().snap.count(7) == 0);   // the OLD zone is gone
}

TEST_CASE("reporter: sequence numbers increase and skip suppressed snapshots",
          "[zone_reporter]") {
    std::vector<Captured> got;
    ZoneReporter r([&](const std::map<int,int>& s, uint64_t q){ got.push_back({s,q}); }, 5);
    for (int i = 0; i < 5; ++i) r.on_zones(1, {rd(10, 1)});
    for (int i = 0; i < 5; ++i) r.on_zones(1, {rd(10, 2)});
    REQUIRE(got.size() == 2);
    REQUIRE(got[1].seq > got[0].seq);
    const uint64_t last = got.back().seq;
    r.set_camera_inhibited(1, true);   // evicting the only zone -> empty -> suppressed
    for (const auto& c : got) REQUIRE(c.seq <= last + 1);
}
```

- [ ] **Step 2: Register the test and run to verify it fails**

Add `test_zone_reporter.cpp` to `add_executable(denso_tests ...)` in `tests/CMakeLists.txt`.

Run: `cmake --build build && ./build/tests/denso_tests "[zone_reporter]"`
Expected: FAIL — `set_camera_inhibited` not declared; callback arity mismatch.

- [ ] **Step 3: Implement**

Replace `src/app/brazing/zone_reporter.h`'s class body:

```cpp
class ZoneReporter : public ZoneSink {
public:
    /// on_snapshot receives the full payload plus a monotonic sequence number.
    /// The sequence lets the GUI side drop a snapshot that overtook a newer one:
    /// callbacks fire OUTSIDE the mutex and marshal from several threads, so an
    /// eviction and a recovery can otherwise arrive reversed (spec §3.3d).
    explicit ZoneReporter(
        std::function<void(const std::map<int, int>&, uint64_t)> on_snapshot,
        int stable_frames = kStableFrames);

    void on_zones(int64_t camera_id, const std::vector<ZoneReading>& zones) override;

    /// Assert or clear a CAMERA-level inhibit. Asserting evicts the camera's
    /// recorded zones atomically with marking it inhibited. Call on the GUI thread.
    void set_camera_inhibited(int64_t camera_id, bool on);

private:
    std::function<void(const std::map<int, int>&, uint64_t)> on_snapshot_;
    std::mutex      mutex_;
    ZoneAggregator  aggregator_;
    std::set<int64_t>                  inhibited_cameras_;
    std::map<int64_t, std::set<int>>   camera_zones_;   // recorded from accepted obs
    uint64_t                           seq_ = 0;
};
```

Add `#include <cstdint>`, `#include <set>` to the header.

Replace `src/app/brazing/zone_reporter.cpp`:

```cpp
#include "brazing/zone_reporter.h"

#include <chrono>
#include <utility>

namespace denso::ui {

ZoneReporter::ZoneReporter(
    std::function<void(const std::map<int, int>&, uint64_t)> on_snapshot,
    int stable_frames)
    : on_snapshot_(std::move(on_snapshot)), aggregator_(stable_frames) {}

void ZoneReporter::on_zones(int64_t camera_id, const std::vector<ZoneReading>& zones) {
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    std::optional<std::map<int, int>> snapshot;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // CAMERA-level causes drop the WHOLE observation. Checking this under the
        // same mutex as observe() is what kills the resurrection race: a worker
        // already blocked here cannot re-create a zone the inhibit just evicted.
        // Contrast the zone-level hold-timeout inhibit, which suppresses only
        // PUBLICATION so the zone can still recover (spec §3.1.1).
        if (inhibited_cameras_.count(camera_id) > 0) {
            return;
        }
        // Ownership is recorded from observations we ACCEPT, never derived from
        // current config: a renumbered ROI must still evict the zone we actually
        // published under (spec §3.3b).
        auto& owned = camera_zones_[camera_id];
        for (const ZoneReading& z : zones) {
            owned.insert(z.zone_no);
        }
        snapshot = aggregator_.observe(zones, now_ms);
        if (snapshot) {
            seq = ++seq_;
        }
    }
    if (snapshot && on_snapshot_) {
        on_snapshot_(*snapshot, seq);
    }
}

void ZoneReporter::set_camera_inhibited(int64_t camera_id, bool on) {
    std::optional<std::map<int, int>> snapshot;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (on) {
            // Mark and evict atomically — any gap between them is a window in which
            // a blocked observation could republish the zone.
            inhibited_cameras_.insert(camera_id);
            const auto it = camera_zones_.find(camera_id);
            if (it != camera_zones_.end()) {
                snapshot = aggregator_.evict_zones(it->second);
            }
        } else {
            // Release clears the flag only. The zone re-enters naturally on the next
            // observation, rebuilds a fresh Debounce (so it re-earns the debounce),
            // and reports even an unchanged value because last_sent_ no longer
            // holds it (spec §3.4).
            inhibited_cameras_.erase(camera_id);
        }
        // A suppressed (empty) snapshot must NOT consume a sequence number, or the
        // GUI's drop-stale rule would discard the next genuine snapshot (spec §3.3).
        if (snapshot) {
            seq = ++seq_;
        }
    }
    if (snapshot && on_snapshot_) {
        on_snapshot_(*snapshot, seq);
    }
}

} // namespace denso::health
```

- [ ] **Step 4: Run to verify they pass**

Run: `./build/tests/denso_tests "[zone_reporter]"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/app/brazing/zone_reporter.h src/app/brazing/zone_reporter.cpp \
        tests/test_zone_reporter.cpp tests/CMakeLists.txt
git commit -m "feat(zones): reporter inhibit gate, recorded ownership, snapshot sequence

Checking the gate under the same mutex as observe() kills the resurrection
race; ownership recorded from accepted observations survives ROI renumbering."
```

---

### Task 6: Readiness/integrity verdict

Implements spec §2. Pure and read-only — no mutation, so `--check` parity holds.

**Files:**
- Create: `src/core/health/integrity.h`, `src/core/health/integrity.cpp`
- Modify: `src/core/CMakeLists.txt` (add the two files to `denso_core`)
- Test: `tests/test_integrity.cpp` (create)

**Interfaces:**
- Produces: `denso::health::Readiness{Ready,Degraded,Blocked}`; `GlobalBlocker{Kind,detail}`; `ZoneIssue{Kind,camera_id,detail}`; `IntegrityVerdict{status,blockers,issues}`; `IntegrityVerdict evaluate_integrity(const QSqlDatabase&, const QString& models_dir);`

- [ ] **Step 1: Write the failing tests**

Create `tests/test_integrity.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "health/integrity.h"
#include "db/db.h"
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QSqlQuery>
using namespace denso;

static void put(const QDir& d, const QString& name, const QByteArray& bytes) {
    QFile f(d.filePath(name)); REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(bytes); f.close();
}

TEST_CASE("integrity: a clean empty install is Ready", "[integrity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir models(tmp.path()); REQUIRE(models.mkpath("models"));
    auto db = db::Db::open(QDir(tmp.path()).filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    const auto v = health::evaluate_integrity(db->handle(),
                                              QDir(tmp.path()).filePath("models"));
    REQUIRE(v.status == health::Readiness::Ready);
}

TEST_CASE("integrity: an unreadable models_dir is a GLOBAL blocker", "[integrity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    auto db = db::Db::open(QDir(tmp.path()).filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    const auto v = health::evaluate_integrity(db->handle(),
                                              QDir(tmp.path()).filePath("nope"));
    REQUIRE(v.status == health::Readiness::Blocked);
    REQUIRE_FALSE(v.blockers.empty());
    REQUIRE(v.blockers[0].kind == health::GlobalBlocker::Kind::ModelsDirUnreadable);
}

TEST_CASE("integrity: a corrupt manifest is a GLOBAL blocker", "[integrity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir root(tmp.path()); REQUIRE(root.mkpath("models"));
    QDir models(root.filePath("models"));
    put(models, "manifest.json", "{ this is not json");
    auto db = db::Db::open(root.filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    const auto v = health::evaluate_integrity(db->handle(), models.path());
    REQUIRE(v.status == health::Readiness::Blocked);
    REQUIRE(v.blockers[0].kind == health::GlobalBlocker::Kind::ManifestCorrupt);
}

TEST_CASE("integrity: an engine on disk but absent from the manifest is DEGRADED",
          "[integrity]") {
    // The production-Jetson compatibility case (spec §2.3): reported, never blocking.
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir root(tmp.path()); REQUIRE(root.mkpath("models"));
    QDir models(root.filePath("models"));
    put(models, "digitv3.engine", "ENGINE");
    put(models, "digitv3.names.json", R"(["0","1"])");
    auto db = db::Db::open(root.filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    const auto v = health::evaluate_integrity(db->handle(), models.path());
    REQUIRE(v.status == health::Readiness::Degraded);
    bool found = false;
    for (const auto& i : v.issues) {
        if (i.kind == health::ZoneIssue::Kind::EnginesUnmanifested) found = true;
    }
    REQUIRE(found);
    REQUIRE(v.blockers.empty());   // MUST NOT block — it would brick production
}

TEST_CASE("integrity: a camera attached to a missing engine is a per-zone issue",
          "[integrity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir root(tmp.path()); REQUIRE(root.mkpath("models"));
    QDir models(root.filePath("models"));
    auto db = db::Db::open(root.filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    QSqlQuery q(db->handle());
    REQUIRE(q.exec("INSERT INTO camera(id,name,camera_type,width,height,fps,"
                   "pitch,roll,rotation,active,setup_complete) "
                   "VALUES(1,'C1','usb',640,480,15,0,0,0,1,1)"));
    REQUIRE(q.exec("INSERT INTO model(id,name,filename,class_names) "
                   "VALUES(1,'m','gone.engine','[\"0\"]')"));
    REQUIRE(q.exec("INSERT INTO camera_model(camera_id,model_id) VALUES(1,1)"));
    const auto v = health::evaluate_integrity(db->handle(), models.path());
    REQUIRE(v.status == health::Readiness::Degraded);
    bool found = false;
    for (const auto& i : v.issues) {
        if (i.kind == health::ZoneIssue::Kind::EngineMissing && i.camera_id == 1) found = true;
    }
    REQUIRE(found);
}
```

- [ ] **Step 2: Register and run to verify it fails**

Add `test_integrity.cpp` to `tests/CMakeLists.txt`.

Run: `cmake --build build && ./build/tests/denso_tests "[integrity]"`
Expected: FAIL — `health/integrity.h` not found.

- [ ] **Step 3: Implement the header**

Create `src/core/health/integrity.h`:

```cpp
// The single readiness verdict, shared by boot and --check. READ-ONLY by
// contract: it observes state and never mutates, which is what lets --check keep
// its non-mutating guarantee while boot separately runs sync_models() first
// (spec §2.1). Classification is by ERROR PROVENANCE, not severity guesswork.
#pragma once

#include <QSqlDatabase>
#include <QString>
#include <cstdint>
#include <vector>

namespace denso::health {

enum class Readiness { Ready, Degraded, Blocked };

/// Whole-machine faults: no restart fixes these, so boot exits EX_CONFIG (78).
struct GlobalBlocker {
    enum class Kind {
        DbUnopenable, SchemaNewer, MigrationFailed, DbQueryFailed,
        ModelsDirUnreadable, ManifestCorrupt, SharedBackendFailure
    };
    Kind    kind;
    QString detail;
};

/// Faults scoped to one camera's zones: the app boots and healthy zones report.
/// ONLY kinds with a real producer are declared. Do NOT add speculative values
/// to "stabilise" status.json — that file uses stable STRING reason codes
/// (reason_code below), so new kinds are additive without placeholders.
struct ZoneIssue {
    enum class Kind { EngineMissing, EnginesUnmanifested };
    Kind    kind;
    int64_t camera_id = 0;   // 0 = not camera-scoped (e.g. EnginesUnmanifested)
    QString detail;
};

struct IntegrityVerdict {
    Readiness                  status = Readiness::Ready;
    std::vector<GlobalBlocker> blockers;
    std::vector<ZoneIssue>     issues;
};

/// Evaluate the installation. `db` must already be open and migrated.
IntegrityVerdict evaluate_integrity(const QSqlDatabase& db, const QString& models_dir);

/// Process exit code for a verdict: 0 Ready / 10 Degraded / 78 Blocked (spec §2.1).
int exit_code_for(Readiness r);

/// Stable, machine-readable reason codes for status.json. These strings are a
/// FILE FORMAT: never renumber, never reuse, only add. Deliberately not enum
/// ordinals — an ordinal shifts whenever a value is inserted, silently
/// remapping every historical status.json.
QString reason_code(GlobalBlocker::Kind k);
QString reason_code(ZoneIssue::Kind k);

} // namespace denso::health
```

- [ ] **Step 4: Implement the body**

Create `src/core/health/integrity.cpp`:

```cpp
#include "health/integrity.h"

#include "models/manifest.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <set>
#include <string>

namespace denso::health {
namespace {

bool read_text(const QString& path, std::string& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    out = f.readAll().toStdString();
    return true;
}

} // namespace

QString reason_code(GlobalBlocker::Kind k) {
    switch (k) {
        case GlobalBlocker::Kind::DbUnopenable:        return QStringLiteral("db_unopenable");
        case GlobalBlocker::Kind::SchemaNewer:         return QStringLiteral("schema_newer");
        case GlobalBlocker::Kind::MigrationFailed:     return QStringLiteral("migration_failed");
        case GlobalBlocker::Kind::DbQueryFailed:       return QStringLiteral("db_query_failed");
        case GlobalBlocker::Kind::ModelsDirUnreadable: return QStringLiteral("models_dir_unreadable");
        case GlobalBlocker::Kind::ManifestCorrupt:     return QStringLiteral("manifest_corrupt");
        case GlobalBlocker::Kind::SharedBackendFailure:return QStringLiteral("shared_backend_failure");
    }
    return QStringLiteral("unknown");
}

QString reason_code(ZoneIssue::Kind k) {
    switch (k) {
        case ZoneIssue::Kind::EngineMissing:       return QStringLiteral("engine_missing");
        case ZoneIssue::Kind::EnginesUnmanifested: return QStringLiteral("engines_unmanifested");
    }
    return QStringLiteral("unknown");
}

int exit_code_for(Readiness r) {
    switch (r) {
        case Readiness::Ready:    return 0;
        case Readiness::Degraded: return 10;
        case Readiness::Blocked:  return 78;   // EX_CONFIG
    }
    return 78;
}

IntegrityVerdict evaluate_integrity(const QSqlDatabase& db, const QString& models_dir) {
    IntegrityVerdict v;

    // ── Global: the models dir must exist and be listable ────────────────────
    QDir dir(models_dir);
    if (!QFileInfo(models_dir).isDir() || !dir.isReadable()) {
        v.blockers.push_back({GlobalBlocker::Kind::ModelsDirUnreadable, models_dir});
        v.status = Readiness::Blocked;
        return v;
    }

    // ── Global: a manifest, if present, must parse and validate ──────────────
    std::set<std::string> manifested;
    const QString manifest_path = dir.filePath(QStringLiteral("manifest.json"));
    const bool has_manifest = QFileInfo::exists(manifest_path);
    if (has_manifest) {
        std::string text;
        if (!read_text(manifest_path, text)) {
            v.blockers.push_back({GlobalBlocker::Kind::ManifestCorrupt,
                                  QStringLiteral("unreadable: %1").arg(manifest_path)});
            v.status = Readiness::Blocked;
            return v;
        }
        auto pr = denso::models::parse_manifest(text);
        if (!pr.manifest) {
            v.blockers.push_back({GlobalBlocker::Kind::ManifestCorrupt,
                                  QString::fromStdString(pr.error)});
            v.status = Readiness::Blocked;
            return v;
        }
        if (auto err = denso::models::validate_manifest(*pr.manifest)) {
            v.blockers.push_back({GlobalBlocker::Kind::ManifestCorrupt,
                                  QString::fromStdString(*err)});
            v.status = Readiness::Blocked;
            return v;
        }
        for (const auto& g : pr.manifest->generations) manifested.insert(g.engine);
    }

    // ── Degraded: engines on disk that the manifest does not describe ────────
    // COMPATIBILITY (spec §2.3): the production Jetson has engines and no
    // manifest, and works only because sync_models() scans the directory. This
    // is reported as actionable, and MUST NEVER block.
    const QStringList on_disk =
        dir.entryList({QStringLiteral("*.engine"), QStringLiteral("*.onnx")}, QDir::Files);
    for (const QString& f : on_disk) {
        if (manifested.count(f.toStdString()) == 0) {
            v.issues.push_back({ZoneIssue::Kind::EnginesUnmanifested, 0, f});
        }
    }

    // ── Per-zone: every camera-attached engine must exist on disk ────────────
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT cm.camera_id, m.filename FROM camera_model cm "
            "JOIN model m ON m.id = cm.model_id"))) {
        // A FAILED query is a global blocker. Conflating it with "no rows" would
        // turn a broken DB into a silently empty fleet (spec §2.2).
        v.blockers.push_back({GlobalBlocker::Kind::DbQueryFailed, q.lastError().text()});
        v.status = Readiness::Blocked;
        return v;
    }
    while (q.next()) {
        const int64_t camera_id = q.value(0).toLongLong();
        const QString filename  = q.value(1).toString();
        if (!QFileInfo::exists(dir.filePath(filename))) {
            v.issues.push_back({ZoneIssue::Kind::EngineMissing, camera_id, filename});
        }
    }

    if (!v.blockers.empty())      v.status = Readiness::Blocked;
    else if (!v.issues.empty())   v.status = Readiness::Degraded;
    else                          v.status = Readiness::Ready;
    return v;
}

} // namespace denso::health
```

Add both files to the `denso_core` source list in `src/core/CMakeLists.txt`.

- [ ] **Step 5: Run to verify they pass**

Run: `cmake --build build && ./build/tests/denso_tests "[integrity]"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/core/health/integrity.h src/core/health/integrity.cpp \
        src/core/CMakeLists.txt tests/test_integrity.cpp tests/CMakeLists.txt
git commit -m "feat(health): read-only readiness verdict classified by provenance

Global blockers exit EX_CONFIG; per-zone issues boot degraded. Unmanifested
engines are reported and never block, so the production Jetson still boots."
```

---

### Task 7: Atomic `status.json`

Implements spec §7.

**Files:**
- Create: `src/core/health/status_file.h`, `.cpp`
- Modify: `src/core/CMakeLists.txt` (add to `denso_core`)
- Test: `tests/test_status_file.cpp` (create)

**Interfaces:**
- Consumes: `health::IntegrityVerdict` (Task 6).
- Produces: `bool denso::health::write_status_file(const QString& path, const health::IntegrityVerdict&, const std::map<int64_t,uint32_t>& camera_causes, const std::set<int>& held_zones, const std::set<int>& inhibited_zones);`

- [ ] **Step 1: Write the failing test**

Create `tests/test_status_file.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "health/status_file.h"
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
using namespace denso;

TEST_CASE("status.json: writes a parseable document", "[status_file]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    const QString path = QDir(tmp.path()).filePath("status.json");
    health::IntegrityVerdict v;
    v.status = health::Readiness::Degraded;
    v.issues.push_back({health::ZoneIssue::Kind::EngineMissing, 1, "gone.engine"});
    REQUIRE(health::write_status_file(path, v, {{1, 0x08}}, {5}, {9}));

    QFile f(path); REQUIRE(f.open(QIODevice::ReadOnly));
    const auto doc = QJsonDocument::fromJson(f.readAll());
    REQUIRE(doc.isObject());
    REQUIRE(doc.object()["status"].toString() == "degraded");
    REQUIRE(doc.object()["issues"].toArray()[0].toObject()["reason"].toString()
            == "engine_missing");   // stable string code, not an enum ordinal
    REQUIRE(doc.object()["held_zones"].toArray().size() == 1);
    REQUIRE(doc.object()["inhibited_zones"].toArray().size() == 1);
}

TEST_CASE("status.json: rewriting leaves no temp file behind", "[status_file]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    const QString path = dir.filePath("status.json");
    health::IntegrityVerdict v;
    REQUIRE(health::write_status_file(path, v, {}, {}, {}));
    REQUIRE(health::write_status_file(path, v, {}, {}, {}));
    // Atomic write = temp + rename; a leftover temp would mean a partial write
    // could be observed after a crash.
    REQUIRE(dir.entryList({"*.tmp"}, QDir::Files).isEmpty());
}
```

- [ ] **Step 2: Register and run to verify it fails**

Add `test_status_file.cpp` to `tests/CMakeLists.txt`.
Run: `cmake --build build && ./build/tests/denso_tests "[status_file]"`
Expected: FAIL — header not found.

- [ ] **Step 3: Implement**

Create `src/core/health/status_file.h`:

```cpp
// Machine-readable local health, for SSH/denso-setup inspection. Written
// ATOMICALLY (temp + rename): an abrupt restart must never leave a half-written
// file that reads as misleading status (spec §7).
#pragma once

#include "health/integrity.h"

#include <QString>
#include <cstdint>
#include <map>
#include <set>

namespace denso::health {

bool write_status_file(const QString& path,
                       const health::IntegrityVerdict& verdict,
                       const std::map<int64_t, uint32_t>& camera_causes,
                       const std::set<int>& held_zones,
                       const std::set<int>& inhibited_zones);

} // namespace denso::health
```

Create `src/core/health/status_file.cpp`:

```cpp
#include "health/status_file.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace denso::ui {
namespace {

QString status_text(health::Readiness r) {
    switch (r) {
        case health::Readiness::Ready:    return QStringLiteral("ready");
        case health::Readiness::Degraded: return QStringLiteral("degraded");
        case health::Readiness::Blocked:  return QStringLiteral("blocked");
    }
    return QStringLiteral("blocked");
}

} // namespace

bool write_status_file(const QString& path,
                       const health::IntegrityVerdict& verdict,
                       const std::map<int64_t, uint32_t>& camera_causes,
                       const std::set<int>& held_zones,
                       const std::set<int>& inhibited_zones) {
    QJsonObject root;
    root["status"] = status_text(verdict.status);

    QJsonArray blockers;
    for (const auto& b : verdict.blockers) {
        QJsonObject o;
        o["reason"] = health::reason_code(b.kind);   // stable string, never an ordinal
        o["detail"] = b.detail;
        blockers.append(o);
    }
    root["blockers"] = blockers;

    QJsonArray issues;
    for (const auto& i : verdict.issues) {
        QJsonObject o;
        o["reason"] = health::reason_code(i.kind);   // stable string, never an ordinal
        // Ids as STRINGS: QJsonValue is a double, so >2^53 would lose precision.
        o["camera_id"] = QString::number(i.camera_id);
        o["detail"] = i.detail;
        issues.append(o);
    }
    root["issues"] = issues;

    QJsonArray causes;
    for (const auto& [camera_id, mask] : camera_causes) {
        QJsonObject o;
        o["camera_id"] = QString::number(camera_id);
        o["causes"] = static_cast<int>(mask);
        causes.append(o);
    }
    root["camera_causes"] = causes;

    QJsonArray held;
    for (const int z : held_zones) held.append(z);
    root["held_zones"] = held;

    QJsonArray inhibited;
    for (const int z : inhibited_zones) inhibited.append(z);
    root["inhibited_zones"] = inhibited;

    // QSaveFile is write-to-temp + atomic rename on commit, and removes its temp
    // on failure — so no partial file is ever observable at `path`.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return f.commit();
}

} // namespace denso::health
```

Add both files to the `denso_core` source list in `src/core/CMakeLists.txt`.

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build build && ./build/tests/denso_tests "[status_file]"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/health/status_file.h src/core/health/status_file.cpp \
        src/app/CMakeLists.txt tests/test_status_file.cpp tests/CMakeLists.txt
git commit -m "feat(health): atomic status.json writer"
```

---

### Task 8: `ZoneHealth` cause-set

Implements spec §3.1–§3.2. Single-threaded by construction — **no mutex**.

**Files:**
- Create: `src/core/health/zone_health.h`, `.cpp`
- Modify: `src/core/CMakeLists.txt` (add to `denso_core`)
- Test: `tests/test_zone_health.cpp` (create)

**Interfaces:**
- Consumes: `ZoneReporter::set_camera_inhibited` (Task 5).
- Produces: `enum class ZoneCause : uint32_t {AreasNeedReview=1,ModelUnavailable=2,ModelInvalid=4,CaptureOffline=8,InferenceWorkerFailed=16}`; `class ZoneHealth` with `void set_cause(int64_t camera_id, ZoneCause c, bool on); bool is_inhibited(int64_t) const; uint32_t causes(int64_t) const; const std::map<int64_t,uint32_t>& all() const;`

- [ ] **Step 1: Write the failing tests**

Create `tests/test_zone_health.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "health/zone_health.h"
#include <vector>
using namespace denso::health;

TEST_CASE("zone_health: causes compose and release only when ALL clear",
          "[zone_health]") {
    std::vector<std::pair<int64_t,bool>> calls;
    ZoneHealth h([&](int64_t id, bool on){ calls.push_back({id, on}); });

    h.set_cause(1, ZoneCause::CaptureOffline, true);
    h.set_cause(1, ZoneCause::InferenceWorkerFailed, true);
    REQUIRE(h.is_inhibited(1));
    REQUIRE(calls.size() == 1);            // one transition into inhibited
    REQUIRE(calls[0] == std::make_pair<int64_t,bool>(1, true));

    h.set_cause(1, ZoneCause::CaptureOffline, false);
    REQUIRE(h.is_inhibited(1));            // still held by the other cause
    REQUIRE(calls.size() == 1);            // no spurious release

    h.set_cause(1, ZoneCause::InferenceWorkerFailed, false);
    REQUIRE_FALSE(h.is_inhibited(1));
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[1] == std::make_pair<int64_t,bool>(1, false));
}

TEST_CASE("zone_health: setting the same cause twice does not re-notify",
          "[zone_health]") {
    std::vector<std::pair<int64_t,bool>> calls;
    ZoneHealth h([&](int64_t id, bool on){ calls.push_back({id, on}); });
    h.set_cause(1, ZoneCause::CaptureOffline, true);
    h.set_cause(1, ZoneCause::CaptureOffline, true);
    REQUIRE(calls.size() == 1);
}

TEST_CASE("zone_health: cameras are independent", "[zone_health]") {
    std::vector<std::pair<int64_t,bool>> calls;
    ZoneHealth h([&](int64_t id, bool on){ calls.push_back({id, on}); });
    h.set_cause(1, ZoneCause::CaptureOffline, true);
    REQUIRE(h.is_inhibited(1));
    REQUIRE_FALSE(h.is_inhibited(2));
}
```

- [ ] **Step 2: Register and run to verify it fails**

Add `test_zone_health.cpp` to `tests/CMakeLists.txt`.
Run: `cmake --build build && ./build/tests/denso_tests "[zone_health]"`
Expected: FAIL — header not found.

- [ ] **Step 3: Implement**

Create `src/core/health/zone_health.h`:

```cpp
// Per-camera inhibit causes. Causes are evaluated PER CAMERA and conservatively
// projected onto every zone that camera owns — this is NOT precise per-zone
// isolation, and the spec (§3.1) says so deliberately.
//
// THREADING: every cause transition is made on the GUI thread. Three of the four
// sources are already GUI-thread; the inference-worker source is marshalled with
// common::post_to_gui. That single owner is why this class needs no mutex and no
// revision counter. Do not call it from a worker thread.
#pragma once

#include <cstdint>
#include <functional>
#include <map>

namespace denso::health {

enum class ZoneCause : uint32_t {
    AreasNeedReview       = 1u << 0,
    ModelUnavailable      = 1u << 1,
    ModelInvalid          = 1u << 2,
    CaptureOffline        = 1u << 3,
    InferenceWorkerFailed = 1u << 4,
};

class ZoneHealth {
public:
    /// `on_inhibit_changed(camera_id, inhibited)` fires ONLY on a 0<->non-0
    /// transition, so the reporter is not churned by every cause edit.
    explicit ZoneHealth(std::function<void(int64_t, bool)> on_inhibit_changed);

    void set_cause(int64_t camera_id, ZoneCause c, bool on);
    bool is_inhibited(int64_t camera_id) const;
    uint32_t causes(int64_t camera_id) const;
    const std::map<int64_t, uint32_t>& all() const { return causes_; }

private:
    std::function<void(int64_t, bool)> on_inhibit_changed_;
    std::map<int64_t, uint32_t> causes_;
};

} // namespace denso::health
```

Create `src/core/health/zone_health.cpp`:

```cpp
#include "health/zone_health.h"

#include <utility>

namespace denso::health {

ZoneHealth::ZoneHealth(std::function<void(int64_t, bool)> on_inhibit_changed)
    : on_inhibit_changed_(std::move(on_inhibit_changed)) {}

void ZoneHealth::set_cause(int64_t camera_id, ZoneCause c, bool on) {
    uint32_t& mask = causes_[camera_id];
    const uint32_t before = mask;
    const uint32_t bit = static_cast<uint32_t>(c);
    if (on) {
        mask |= bit;
    } else {
        mask &= ~bit;
    }
    if (mask == before) {
        return;   // no change — do not re-notify
    }
    // A camera releases only when ALL causes clear.
    const bool was = before != 0;
    const bool now = mask != 0;
    if (was != now && on_inhibit_changed_) {
        on_inhibit_changed_(camera_id, now);
    }
}

bool ZoneHealth::is_inhibited(int64_t camera_id) const {
    return causes(camera_id) != 0;
}

uint32_t ZoneHealth::causes(int64_t camera_id) const {
    const auto it = causes_.find(camera_id);
    return it == causes_.end() ? 0u : it->second;
}

} // namespace denso::health
```

Add both files to the `denso_core` source list in `src/core/CMakeLists.txt`.

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build build && ./build/tests/denso_tests "[zone_health]"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/health/zone_health.h src/core/health/zone_health.cpp \
        src/app/CMakeLists.txt tests/test_zone_health.cpp tests/CMakeLists.txt
git commit -m "feat(health): per-camera composable inhibit cause-set"
```

---

### Task 9: Wiring — adapters, boot ordering, `--check`, UI

Implements spec §3.2 (adapters), §8 (boot), §2.1 (`--check`), §7 (UI). No new logic — connect what Tasks 1–8 built.

**Files:**
- Modify: `src/app/ui/camera/grid/camera_grid.cpp`
- Modify: `src/app/camera/frame_processor.h`, `.cpp`
- Modify: `src/app/ui/camera/grid/camera_tile.h`, `.cpp`
- Modify: `src/app/cli/run_headless.cpp`
- Modify: `src/app/ui/startup.cpp`

**Interfaces:**
- Consumes: everything from Tasks 1–8.

- [ ] **Step 1: Add the inference-failure hook**

In `src/app/camera/frame_processor.h`, add to `DetectionProcessor`'s ctor parameters
and members a failure callback:

```cpp
    /// Raised when consecutive inference failures cross the threshold, cleared on
    /// recovery. CONTRACT: invoked on the INFERENCE WORKER THREAD — the wiring
    /// must marshal to the GUI thread (common::post_to_gui) before touching
    /// ZoneHealth, which is single-threaded by design.
    using WorkerFailedFn = std::function<void(int64_t camera_id, bool failed)>;
```

Add `WorkerFailedFn on_worker_failed_;` and a setter `void set_worker_failed_handler(WorkerFailedFn f);`.

In `src/app/camera/frame_processor.cpp`'s `infer_loop()`, extend the existing catch and
recovery lines:

```cpp
        } catch (const std::exception& e) {
            if (infer_fail_streak_++ % kInferFailLogEvery == 0) {
                qWarning().noquote()
                    << "[infer] camera" << camera_id_ << "inference failed ("
                    << infer_fail_streak_ << " in a row):" << e.what();
            }
            if (infer_fail_streak_ == kInferFailInhibitAfter && on_worker_failed_) {
                on_worker_failed_(camera_id_, true);
            }
            continue;
        }
        if (infer_fail_streak_ >= kInferFailInhibitAfter && on_worker_failed_) {
            on_worker_failed_(camera_id_, false);
        }
        infer_fail_streak_ = 0;  // recovered
```

Add near `kInferFailLogEvery`:

```cpp
constexpr int kInferFailInhibitAfter = 10;  // consecutive failures -> inhibit the camera
```

- [ ] **Step 2: Wire the adapters in `camera_grid.cpp`**

Add members to `CameraGrid`: `std::unique_ptr<ZoneHealth> health_;` and
`uint64_t last_applied_seq_ = 0;`.

Construct `health_` so it drives the reporter, and update the snapshot lambda to
drop stale sequences:

```cpp
    health_ = std::make_unique<ZoneHealth>([this](int64_t camera_id, bool inhibited) {
        if (reporter_) reporter_->set_camera_inhibited(camera_id, inhibited);
        refresh_status_file();
        refresh_tiles();
    });
```

In the existing snapshot wiring, add the drop-stale guard:

```cpp
    common::post_to_gui(reporter, [reporter, snap, seq, this] {
        // Callbacks fire outside the reporter mutex and marshal from several
        // threads, so an older eviction can overtake a newer recovery. Drop it
        // rather than let whole-snapshot latest-wins clobber the recovery.
        if (seq <= last_applied_seq_) return;
        last_applied_seq_ = seq;
        reporter->submit(snap);
    });
```

Connect the two runtime causes:

```cpp
    connect(stream, &CameraStream::status_changed, this,
            [this, id = cam.id](int s) {
                // GUI-affine receiver + AutoConnection => queued delivery, which is
                // what makes ZoneHealth's single-owner (mutex-free) design valid.
                // A contextless functor or DirectConnection would break it.
                health_->set_cause(id, ZoneCause::CaptureOffline,
                                   s == static_cast<int>(CameraStream::Status::Offline));
            });
```

and, on the detection processor:

```cpp
    proc->set_worker_failed_handler([this](int64_t camera_id, bool failed) {
        // Marshal off the inference worker thread — ZoneHealth is GUI-thread only.
        common::post_to_gui(this, [this, camera_id, failed] {
            health_->set_cause(camera_id, ZoneCause::InferenceWorkerFailed, failed);
        });
    });
```

Replace the existing `areas_need_review` line so it feeds the cause-set:

```cpp
    health_->set_cause(cam.id, ZoneCause::AreasNeedReview, cam.areas_need_review);
```

- [ ] **Step 3: Boot ordering and `EX_CONFIG` in `startup.cpp`**

Replace both `app.exit(1)` calls. On warmup failure and on a blocked verdict:

```cpp
    const auto verdict = health::evaluate_integrity(db.handle(), paths::models_dir());
    health::write_status_file(QDir(paths::data_dir()).filePath("status.json"),
                          verdict, {}, {}, {});
    if (verdict.status == health::Readiness::Blocked) {
        for (const auto& b : verdict.blockers) {
            qCritical().noquote() << "[startup] BLOCKED:" << b.detail;
        }
        // EX_CONFIG (78), not 1: tells systemd this is a configuration fault no
        // restart can fix, so it does not restart-loop (spec §2.2).
        app.exit(health::exit_code_for(verdict.status));
        return;
    }
    // Per-zone issues boot DEGRADED: install every cause BEFORE any observation
    // can publish (spec §8), then start capture.
    for (const auto& i : verdict.issues) {
        if (i.camera_id != 0) {
            grid->health().set_cause(i.camera_id, ZoneCause::ModelUnavailable, true);
        }
    }
```

- [ ] **Step 4: `--check` exit contract in `run_headless.cpp`**

In `run_check`, after the existing checks, compute and report the verdict:

```cpp
    const auto verdict = denso::health::evaluate_integrity(db->handle(),
                                                           denso::paths::models_dir());
    for (const auto& b : verdict.blockers) {
        std::fprintf(stderr, "check: BLOCKED: %s\n", qPrintable(b.detail));
    }
    for (const auto& i : verdict.issues) {
        std::fprintf(stdout, "check: degraded: %s\n", qPrintable(i.detail));
    }
    // 0 ready / 10 degraded / 78 blocked — 10 is distinct from a generic failure
    // so a degraded-but-serviceable appliance is never read as a hard failure.
    return denso::health::exit_code_for(verdict.status);
```

- [ ] **Step 5: UI reasons on the tile**

In `camera_tile.h`, add alongside `set_review_paused`:

```cpp
    /// Reason-specific banner. Supersedes set_review_paused, which only ever knew
    /// about one cause.
    void set_inhibited(uint32_t causes);
    void set_holding(bool holding);
```

Implement in `camera_tile.cpp` rendering the cause bits as text (e.g. "Camera
offline", "Model unavailable", "Areas need review"), and a distinct, visibly
different indicator for holding — a hold is not an inhibit.

- [ ] **Step 6: Build and run the FULL suite**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all previously-passing tests still pass, plus the new ones. No regressions.

- [ ] **Step 7: Commit**

```bash
git add src/app/ui/camera/grid/camera_grid.cpp src/app/camera/frame_processor.h \
        src/app/camera/frame_processor.cpp src/app/ui/camera/grid/camera_tile.h \
        src/app/ui/camera/grid/camera_tile.cpp src/app/cli/run_headless.cpp \
        src/app/ui/startup.cpp
git commit -m "feat(health): wire causes, boot ordering, --check contract, UI reasons

Replaces the blanket app.exit(1) with EX_CONFIG for global blockers only, and
adds drop-stale on the snapshot sequence."
```

---

### Task 10: On-device Jetson verification

The gate that cannot run on the Windows host. Jetson: `modela@192.168.1.15`,
repo `~/project/Denso-DigitalReader` (passwordless SSH).

- [ ] **Step 1: Build and run the full suite on-device**

```bash
ssh modela@192.168.1.15 "cd ~/project/Denso-DigitalReader && git pull --ff-only && \
  cd build && cmake . && make -j4 && QT_QPA_PLATFORM=offscreen ctest --output-on-failure"
```
Expected: 0 build errors; all tests pass. Note the Jetson runs one MORE test than
Windows (the symlink case Windows skips).

- [ ] **Step 2: `--check` exit contract on-device**

```bash
ssh modela@192.168.1.15 "cd ~/project/Denso-DigitalReader/build/src/app && \
  QT_QPA_PLATFORM=offscreen ./denso --check; echo exit=\$?"
```
Expected: exit **10** with `check: degraded: digitv3.engine` — the real Jetson has
engines and no manifest, which is exactly the `EnginesUnmanifested` compatibility
case. **An exit of 78 here is a FAILURE**: it would mean the appliance is blocked,
the outcome spec §2.3 exists to prevent.

- [ ] **Step 3: Camera-fault interlock with real cameras**

Run the app with all 4 cameras live, then unplug/black-hole one (e.g. block its IP).
Expected: that camera's zones stop reporting **immediately**, not after 10s; the
other cameras keep reporting; the tile shows "Camera offline"; `status.json` lists
the camera cause. On reconnect, its zones resume with one forced fresh value.

- [ ] **Step 4: Record results**

Append the outcomes to `.superpowers/sdd/progress.md` (gitignored, local only).

---

## Self-Review

**Spec coverage:** §2.1 → Task 6 + 9.4 · §2.2/§2.3 → Task 6 · §3.1/§3.1.1 → Tasks 4, 8 · §3.2 → Tasks 8, 9.1–9.2 · §3.3 → Tasks 3, 5 · §3.4 → Tasks 3, 5 · §4 → Task 1 · §5.1/§5.2 → Task 1 · §5.3/§5.3.1 → Tasks 2, 4 · §6 → deferred by design · §7 → Tasks 7, 9.5 · §8 → Task 9.3 · §9 → tests throughout · §10 → Task 1 negative tests.

**Known gap accepted:** `ClassSelectionIncompatible`, `ShaMismatch`, `SidecarMissingOrInvalid`, and `ArtifactDeserializeFailed` are declared in the `ZoneIssue::Kind` enum but only `EngineMissing` and `EnginesUnmanifested` are produced in Task 6. The remaining kinds need engine-load and manifest-hash checks that belong with the warmup path; they are declared now so the enum is stable for `status.json` consumers, and an implementer should not invent producers for them in this slice.

**Type consistency:** `ReadingKind` (Task 1) is used unchanged in Tasks 2–5. `evict_zones`/`take_newly_inhibited` (Tasks 3–4) match their call sites in Task 5. The `on_snapshot` callback gains `uint64_t seq` in Task 5 and every call site is updated in Task 9.2. `health::Readiness`/`exit_code_for` (Task 6) are used identically in Tasks 7 and 9.
