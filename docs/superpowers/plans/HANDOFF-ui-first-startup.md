# Handoff Prompt — UI-First Startup With Background Warm-Up

Give the AI implementer access to this repo and paste the prompt below.

---

You are implementing a feature in the **Denso-DigitalReader** repo (C++17 / Qt6 Widgets / CMake + Ninja, desktop app for reading a 7-segment display).

## Read first, in this order
1. `docs/superpowers/plans/2026-07-07-ui-first-startup-warmup.md` — **your task list**. Execute it top to bottom. It has 9 tasks, each with exact file paths, complete code, and exact commands.
2. `docs/superpowers/specs/2026-07-07-ui-first-startup-warmup-design.md` — the design rationale (the *why*).
3. `CLAUDE.md` — repo layout, build toolchain, and the project's hard rules.

## Goal (one sentence)
Show the main window immediately on launch instead of blocking on model warm-up; start each detection camera's capture thread only once its model(s) finish warming on the background worker. On a cache-hit (second) launch the window appears instantly.

## How to work
- Start on a NEW branch off `main`:
  ```
  git checkout main
  git checkout -b feature/ui-first-startup
  ```
- Execute the plan **one task at a time, in order**. Each task is TDD: write the failing test → run it (confirm it fails as the plan predicts) → implement (the code is given verbatim) → run tests (confirm green) → commit with the message in that task's Commit step.
- Do not batch tasks or skip the test steps. Do not improvise past a step that doesn't behave as the plan predicts — STOP and report instead.

## Build + test (MSYS2 UCRT64 toolchain — REQUIRED)
```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja          # first configure only
cmake --build build
ctest --test-dir build --output-on-failure
```
PowerShell equivalent:
```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake --build build; if ($?) { ctest --test-dir build --output-on-failure }
```
A task is done only when the build is clean **and** `ctest` is green. Tests are Catch2 v3 (fetched at configure time — needs network on first configure). The passing count differs per-OS (platform backend tests compile per-OS).

## Hard rule you must not break (the whole plan is built around it)
The minutes-long, non-interruptible **TensorRT engine build must run only on the warm-up worker thread**, never on a camera capture thread. A detection camera's `CameraStream` is created only after its model(s) are reported ready, so `EngineRegistry::get()` for it is always a cache-hit (no build) on the GUI thread. If you find yourself calling `get()` for a not-yet-warmed model from a capture thread, you have a bug.

Other constraints: `denso_core` must not link `Qt6::Widgets` (this feature is app-layer only, `src/app/…`). The `denso_tests` target links only `Qt6::Gui` (no Widgets), so `QWidget`-derived units are build-gated + on-device-smoke only, not unit-tested — the pure `PendingStart` gate (Task 1) is the unit-tested piece. Reuse the existing TensorRT cache config in `ort_engine.cpp` — do not add another cache.

## When done
1. Full suite green.
2. Run the plan's **Manual Verification** section on the real machine:
   - Cold first launch (delete `models/trt_cache/` to force a build): window appears immediately; a camera with no model streams at once; a detection camera's tile shows "Preparing model…", then goes live when its model finishes.
   - Warm second launch: window appears immediately, detection tiles go live within the cache-load time — no full-screen splash, no multi-second freeze.
   - Clean shutdown mid-warm-up; a camera with an unloadable model falls back to orientation-only after warm-up finishes.
3. Report build + ctest output and the smoke results. **Do not merge to `main` without asking the owner.**

## Notes
- Only Task 1 (pure `PendingStart`) can be verified without the Qt/GPU toolchain. Tasks 2–9 need the MSYS2/UCRT64 build machine to compile and run — you can write the code without it, but you cannot verify it.
- If you have the Superpowers skills available, run this with `superpowers:executing-plans` (or `superpowers:subagent-driven-development`) for the built-in task/review loop. If not, the plan works as a plain checklist.
