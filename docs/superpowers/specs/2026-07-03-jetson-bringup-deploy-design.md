# Jetson (JetPack/aarch64) bring-up & deploy — design

**Date:** 2026-07-03
**Status:** approved (design), pending implementation plan
**Scope target:** Windows + JetPack only (macOS explicitly out of scope)

## Goal

Make the `denso` GUI app **compile, run, and deploy on JetPack/aarch64** (NVIDIA
Jetson) with full Windows-parity networking. Windows is already the working dev
platform; this spec closes every remaining gap on the Jetson deploy target.

## Non-goals

- **No Windows behavior change.** Every change is additive (new Linux-only
  files/helpers) or guarded by `#ifdef _WIN32` / `if(WIN32)`. The Windows build
  and its `ctest` run must stay identical.
- macOS support (a `NullBackend` remains the fallback for any other OS).
- Model retraining or detection-accuracy changes.
- A `.deb` / AppImage / Flatpak. Deploy is a systemd unit + install script.

## Background (grounded in current code)

- **Two CMake targets:** `denso_core` (static lib, `Qt6::Core`/`Sql` only) and
  `denso` (GUI exe, adds Widgets/Multimedia/Network + OpenCV + ONNX Runtime).
  `denso_core` never links OpenCV/ORT.
- **Network backend** is an abstract `NetworkBackend` (`network/backend.h`) with
  one implementation compiled per OS, selected in `src/core/CMakeLists.txt`
  (`if(WIN32)` → `windows_backend.cpp`, `elseif(UNIX AND NOT APPLE)` →
  `linux_backend.cpp`). The Windows backend splits into **pure, unit-tested
  command helpers** (`netsh::build_netsh_commands`, `wifi::parse_wifi_networks`)
  + a thin `QProcess` runner. The Linux backend currently implements only
  `snapshot()`; `apply_config`/`scan_wifi`/`connect_wifi` **throw
  "not yet implemented"**.
- **ORT execution-provider ladder** is `TensorRT → CUDA → CPU`
  (`ort_engine.cpp`), correct for Jetson as-is. The TensorRT engine build is
  minutes-long and runs during startup `EngineRegistry::warm_up()`.
- **The Denso repo has no existing deploy tooling** — systemd/install is
  greenfield.

## Architecture — five units, dependency order

Build order: **1 → 2 → (3, 4 independent) → 5**. Units 3 and 4 are pure-logic
and TDD'd off-device on the dev box; Units 1, 2, 5 are device-verified on the
Jetson.

### Unit 1 — Build seam (compiles on aarch64)

No behavior change; makes the tree build under the Jetson GCC/aarch64 toolchain.

- **Top `CMakeLists.txt`:** replace the hardcoded Windows ORT imported target
  (`onnxruntime.dll` for both `IMPORTED_LOCATION` and `IMPORTED_IMPLIB`) with an
  OS branch:
  - `if(WIN32)`: unchanged — `.dll` for location + implib.
  - `else()` (Linux): `IMPORTED_LOCATION ${ORT_DIR}/lib/libonnxruntime.so`, no
    implib, same `INTERFACE_INCLUDE_DIRECTORIES`.
- **`src/app/ui/camera/shared/detection/ort_engine.cpp`:** the **only source
  file** touched for the seam. Currently uses `std::wstring` + `widen()` +
  `path.c_str()` → the Windows-only `Ort::Session(const wchar_t*)` overload,
  which does not compile against the Linux ORT headers. Guard it:
  - `#ifdef _WIN32`: keep `std::wstring`/`widen()`/wide-char session.
  - `#else`: pass the narrow `std::string` model path to the `const char*`
    `Ort::Session` overload directly (no `widen`).
- **`src/app/CMakeLists.txt`:** guard the `.dll`-copy POST_BUILD step under
  `if(WIN32)`. On Linux:
  - set `INSTALL_RPATH`/`BUILD_RPATH` to `$ORIGIN` so the co-located `.so`
    resolves at runtime;
  - copy `libonnxruntime.so` (and any staged provider `.so`s) beside the exe,
    mirroring the Windows copy.
  - The `models/*.onnx` copy and `third_party/gpu_ep/` glob stay as-is
    (path-agnostic).
- `add_executable(denso WIN32 …)` needs no change — CMake ignores `WIN32` on
  non-Windows platforms.

**Exit criteria:** `cmake -S . -B build -G Ninja && cmake --build build`
succeeds on the Jetson; `ctest --test-dir build` runs (now compiling the Linux
backend + new Linux tests). Windows build + `ctest` unchanged.

### Unit 2 — Runtime bring-up (runs on device)

Prove the app launches and does real inference + capture on the Jetson.

- Provision an NVIDIA JetPack-matched **aarch64 `onnxruntime-gpu`** (TensorRT +
  CUDA execution providers) into `third_party/onnxruntime/`, mirroring the
  Windows layout (`lib/`, `include/`). Documented in a new `docs/JETSON_SETUP.md`.
- The existing `TensorRT → CUDA → CPU` ladder and `warm_up()` startup path are
  already correct — **verify** on device: the TensorRT engine builds and caches
  under `models/trt_cache/`, warm-up completes before the window shows.
- **Verify the GStreamer camera path** (`CAP_GSTREAMER`, FFMPEG fallback) against
  the NVIDIA GStreamer stack. The `cap.set()`-USB-only gate already protects the
  live-pipeline segfault; confirm it holds on the Jetson RTSP path.

**Exit criteria:** app launches via `./build/src/app/denso`; warm-up completes; a
USB camera and an RTSP camera both stream with detection overlays; inference runs
on TensorRT (or CUDA), falling back to CPU only if the GPU stack is absent.

### Unit 3 — nmcli Wi-Fi backend (parity with Windows)

Fill the three throwing `LinuxBackend` stubs, mirroring the Windows
pure-helper + thin-runner split so the logic is unit-tested off-device.

- **New pure, unit-tested helpers in `network/linux/nmcli.{h,cpp}`:**
  - `build_nmcli_commands(const NetConfig&) -> std::vector<std::vector<std::string>>`
    — the argv lists to apply one interface's config (static vs dhcp:
    `nmcli con mod <con> ipv4.method ...`, addresses/gateway/dns, then
    `nmcli con up <con>`). Mirrors `netsh::build_netsh_commands`.
  - `parse_wifi_list(const std::string& out) -> std::vector<WifiNetwork>`
    — parse `nmcli -t -f SSID,SIGNAL,SECURITY device wifi list` terse output
    (colon-separated, reusing the existing `split_colons` helper);
    `secured = !security.empty()`, skip blank SSIDs.
- **`LinuxBackend` (`linux_backend.cpp`):** add a `run_checked(cmd, args)` (throws
  `std::runtime_error` on spawn failure / non-zero exit, like the Windows one)
  and implement:
  - `apply_config`: run each list from `build_nmcli_commands`.
  - `scan_wifi`: run the wifi-list command → `parse_wifi_list`.
  - `connect_wifi`: `nmcli device wifi connect <ssid> [password <pw>]`. The PSK is
    passed to nmcli only — never persisted in our DB (same contract as Windows).

**Exit criteria:** new Catch2 tests over `build_nmcli_commands` /
`parse_wifi_list` pass on the dev box (off-device); on-device manual verification
of apply / scan / connect against a real Wi-Fi network.

### Unit 4 — Disk-sum gotcha

`hardware/collect.cpp total_storage()` currently sums **every** mounted volume,
over-counting pseudo-filesystems on Linux (tmpfs/overlay/loop/squashfs).

- Extract a **pure, testable predicate** (e.g. `is_real_storage(fileSystemType)`)
  that excludes `tmpfs`, `devtmpfs`, `overlay`, `squashfs`, and loop mounts, and
  filter `QStorageInfo::mountedVolumes()` through it.
- Windows is unaffected — NTFS/FAT/exFAT pass the filter.

**Exit criteria:** Catch2 test asserting the pseudo-fs list is excluded and real
filesystems are kept; on-device sanity check that reported storage matches the
Jetson's real disk.

### Unit 5 — systemd deploy (greenfield, minimal)

- **`deploy/denso.service`** — systemd unit: `ExecStart` the installed exe,
  `Restart=on-failure`, correct graphical-session env (`DISPLAY`/`WAYLAND_DISPLAY`),
  `WorkingDirectory` = the deploy dir (the app resolves `models/` and `denso.db`
  relative to the executable).
- **`deploy/install.sh`** — build → stage exe + `models/` + `libonnxruntime.so`
  (+ provider `.so`s) into a deploy dir → install and `systemctl enable` the unit.
  `denso.db` is created beside the exe on first run (existing behavior).
- Document build + provisioning + install in **`docs/JETSON_SETUP.md`**.

**Exit criteria:** `systemctl start denso` launches the app on the Jetson; it
survives a reboot (`systemctl enable`); logs visible via `journalctl -u denso`.

## Cross-cutting concerns

- **Windows regression guard.** Additive or `#ifdef`/`if(WIN32)`-guarded changes
  only. After Units 1, 3, 4: Windows build + `ctest` must stay green and
  behavior byte-for-byte identical.
- **Testing split.** Units 3 & 4 are TDD'd off-device with Catch2 (pure helpers);
  Units 1, 2, 5 are verified by the explicit on-Jetson smoke steps above. Platform
  backend tests are compiled per-OS, so the Linux passing count will differ from
  Windows (already expected — see CLAUDE.md).
- **Error handling.** Unchanged conventions: `apply_config` failures surface
  non-fatally via the existing `reassert` `(iface, message)` collection at boot;
  nmcli spawn/exit errors thrown as `std::runtime_error`, matching the Windows
  `run_checked`.

## Risks & mitigations

- **aarch64 ORT / TensorRT version skew vs JetPack** — documented prerequisite in
  `JETSON_SETUP.md`; the CPU execution-provider tier is the guaranteed fallback.
- **nmcli field ordering / locale differences** — mitigated by pure parser tests
  built from real captured `nmcli -t` output.
- **GStreamer decode-plugin gaps on the specific JetPack image** — same
  silent-FFMPEG-fallback caveat already documented in CLAUDE.md; verified in
  Unit 2.

## Open questions

- Exact JetPack release + matching `onnxruntime-gpu` build URL (resolved during
  Unit 2 provisioning; captured in `JETSON_SETUP.md`).
- Deploy dir location convention on the Jetson (e.g. `/opt/denso`) — decided in
  Unit 5.
