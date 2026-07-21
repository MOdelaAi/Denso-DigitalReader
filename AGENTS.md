# Denso-DigitalReader — Agent Guide

C++/Qt Widgets desktop app that reads digit displays from 1–4 cameras. Config and
readings live in one SQLite DB (`denso.db`) beside the executable. Two homes:
**Windows dev** (MSYS2 UCRT64) and the real deployment target, an **NVIDIA Jetson
Orin Nano**. (See `CLAUDE.md` for the full source map and hard rules.)

## Build targets (split by concern)

| Target | Location | Role / deps |
|---|---|---|
| `denso_core` | `src/core/` | Testable logic + SQLite persistence. Qt Core/Sql only — **no** Widgets, OpenCV, or inference runtime. |
| `denso_detection` / `denso_brazing` / `denso_camera` | `src/app/{detection,brazing,camera}/` | Pure static subsystem libs (inference helpers / reporting logic / non-widget capture infra). **Both `denso` and `denso_tests` LINK these** — tests validate the shipped objects, not a second compile. Graph: `denso_core ← {detection, brazing, camera(→detection)} ← denso`. |
| `denso` | `src/app/` | Qt Widgets GUI, camera capture, inference backend, orchestration + the subsystem libs. Widgets live under `src/app/ui/camera/`; the subsystems moved out to `src/app/{camera,detection,brazing}/`. |
| `denso_tests` | `tests/` | Catch2 over pure logic + platform-independent helpers (links `denso_core` + the three subsystem libs). |

Keep `main.cpp` a thin orchestrator. Domain code must not depend on GUI types
(the domain↔view boundary lives in `src/core/ui/convert.*`). DB changes are new
version-gated migrations in `db::run_migrations` — never rewrite a shipped one.

## Platform-split inference (`if(WIN32)/else()` in the top `CMakeLists.txt`)

Both backends implement the `InferenceEngine` interface and are selected via the
`BackendEngine` alias in `engine_registry.h`. OpenCV letterbox + `decode_yolo` /
`decode_yolo_end2end` are **shared and identical** across backends.

**Windows / MSYS2 UCRT64** — ONNX Runtime (`third_party/onnxruntime/`,
git-ignored). `OrtEngine`, provider fallback TensorRT→CUDA→CPU, builds+caches its
TRT engine on first run. Warm-up stays off the capture/GUI threads.

**Linux / Jetson Orin Nano (real target)** — native TensorRT, linking `nvinfer`
+ `CUDA::cudart`.
- `TrtEngine` **only deserializes a prebuilt `.engine`** — the operator builds it
  on-device with `trtexec` for TensorRT 10.3 / `sm_87`. **Never build at runtime.**
- **No fallback.** A missing / incompatible / invalid engine **fails loud** at
  startup: the throwing `TrtEngine` ctor → `WarmupWorker` catches → `app.exit(1)`.
- Class names come from a `<engine>.names.json` **sidecar** (TRT engines carry no
  name metadata).

`models/*.engine`, TRT caches, and the local ONNX Runtime drop-in are **not**
repo artifacts.

## Camera capture (`gst_pipeline.cpp` + `camera_stream.cpp`)

RTSP uses **hardware NVDEC** GStreamer pipelines:
`rtspsrc → depay/parse → nvv4l2decoder → queue(leaky) → nvvidconv → BGR appsink`.
The **leaky queue sits after the decoder only** (dropping compressed access units
corrupts the stream).

A capture-backend **ladder** auto-discovers each source by opening candidates and
reading a frame — the first that opens *and reads* wins (remembered for
reconnects):
- RTSP: NVDEC **H.264 → H.265** → OpenCV FFMPEG (handles mixed-codec fleets).
- USB: MJPEG (`nvv4l2decoder mjpeg=1`) → YUYV → `CAP_ANY`.

Inference is **decoupled from display**: `DetectionProcessor` runs the model on a
worker thread over a drop-oldest latest-frame slot and publishes a detections
snapshot the display path overlays — video stays smooth regardless of model speed.
Preserve reconnect/backpressure/clean stop-join in `CameraStream`. **No SQLite on
a capture thread.**

## Build & test

Windows (MSYS2 UCRT64):
```sh
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build && cmake --build build && ctest --test-dir build
```
Jetson:
```sh
cmake -S . -B build -G "Unix Makefiles" && cmake --build build --target denso -j6
```
**Real-device testing:** the shared hardware registry is **`d:\workspace\devices.md`**
(outside this repo) — it has the Jetson's IP/user, the passwordless-SSH and
AnyDesk details, and the JetPack/CUDA/TensorRT versions. Look there first rather
than re-deriving how to reach a box. Anything that needs `sm_87`, real TensorRT,
NVDEC, or a GDM session can only be proven there, not on the Windows dev box.
Jetson runtime deps: `qt6-base-dev`, `qt6-multimedia-dev`, `libqt6sql6-sqlite`,
`gstreamer1.0-libav` (avdec fallback), OpenCV 4.8, TensorRT 10.3, CUDA 12.6.
Catch2 v3 is fetched at first configure (needs net once).

## Deployment — SHIPPED (`.deb`)

Built **on an aarch64 Jetson** (no cross-toolchain; engines are `sm_87`/TRT 10.3
pinned). `tools/build_package.sh --model models/digitv3.engine` → `dist/`.
Install: `sudo ./dist/preflight-denso-<ver>.sh <deb>` (bound to that exact `.deb`
by SHA-256; guards the JetPack stack) → `sudo apt install --no-install-recommends
./dist/<deb>` → `sudo denso-setup configure --user <u>` → `sudo denso-setup verify`
(`cmd_verify` calls `need_root`; a non-sudo `verify` cannot work).
**Never `dpkg -i`** — no dependency resolution.

To install on an appliance that is **not** the build host, move the single
`dist/<name>.tar.gz` bundle (the `.deb`, its preflight guard, `SHA256SUMS` and a
generated `INSTALL.txt`) and follow the `INSTALL.txt` inside it. The `.deb` and
its guard are useless apart — the guard refuses any other `.deb` by embedded
SHA-256 — so they ship as one file. Assembled by `packaging/lib/gen_bundle.sh`
(a sourceable emitter, like `gen_preflight.sh`, so `tests/packaging/run.sh` can
prove its shape off-Jetson).

**Clean builds are byte-reproducible on one machine** — required, because the
clean artifact name carries no content hash, so a non-reproducible rebuild
silently replaced a different artifact under an identical filename. Sources of
variance that had to be closed, all of them non-obvious: `SOURCE_DATE_EPOCH`
(**must** equal the commit time on a clean build — a differing one is refused,
since honouring it would give two clean builds of one commit different bytes
under the same name) for the MANIFEST date *and* dpkg-deb's mtime clamping;
every bundle, payload and Debian-metadata mode pinned rather than
umask-inherited (`DEBIAN/control`/`md5sums` explicitly `0644`), gated by a
two-umask rebuild — note `dpkg-deb` normalizes *control* modes for you but
**not** payload modes, so a `>`-created payload file is the one that would bite;
`tar --mtime/--sort/--owner/--group` plus **`gzip -n`** (gzip writes
its own timestamp into its header, so `tar -czf` is not reproducible even when
every tar entry is pinned); and **stripping ASLR load addresses from the
MANIFEST's `ldd` output** — that one alone re-randomized the .deb on every
build and is invisible, since only the hex changes.

**Testing this tree** — it is shell, so **`ctest` does not cover it**:
`tests/packaging/run.sh` (130 assertions natively; 124 on MSYS2, where the
file-mode ones are skipped) is the harness to run for *any* packaging change, and
`tests/manual/repro_build.sh <engine>` is the Jetson-only reproducibility gate
(19). The latter must run **exclusively**: it refuses a dirty tree, then makes and
reverts its own edits to `packaging/lib/policy.sh`, so a concurrent edit to that
file is discarded by its restore. Design rationale for all of the above is
**Packaging & ship pipeline** in `docs/ARCHITECTURE.md`.

Layout: `/opt/denso/bin/denso` (package-owned) · `/opt/denso/data` (**operator**-
owned: db, log, models, lock) · `/usr/bin/denso-digitalreader` (the launcher —
the stable public command; it exports `DENSO_DATA_DIR`) · `/usr/bin/denso-setup`.

**Verified on hardware** (192.168.1.15, 2026-07-17): build → preflight → install
→ configure → `verify: PASS` → `apt remove` keeps data → upgrade keeps data →
`apt purge` removes it. Bundle + reproducibility gated natively 2026-07-21
(packaging 130/130, repro 19/19, ctest 485/485). **NOT verified:** `denso-setup
configure --autostart --enable-autologin` and `unconfigure`'s GDM restore — the
XDG/GDM path has never run on a real box; and an actual `apt install` of a
*bundled* `.deb` on a second appliance, since only one Jetson exists (the bundle's
shape, checksums and extraction are proven, the install-from-bundle is not). Do
not represent either as working.

Facts that will bite whoever touches this:
- **Dependencies are DERIVED**, not hand-written: `packaging/debian/shlibs.local`
  supplies the metadata NVIDIA omits (`libcudart`, `libopencv` ship no
  `.shlibs`/`.symbols`), after which `dpkg-shlibdeps` exits 0 and emits the full
  version-constrained set. **`--ignore-missing-info` is forbidden** — it drops
  deps silently while still looking derived. Only dlopen/exec deps are declared by
  hand in `control.in` (`qt6-qpa-plugins`, `libqt6sql6-sqlite`, `gstreamer1.0-*`,
  `network-manager`, `procps`).
- `shlibs.local` maps ONLY directly-`NEEDED` sonames, and `build_package.sh`
  verifies every mapping against `dpkg -S` — the file overrides authoritative
  metadata and shlibdeps trusts it blindly (a wrong mapping was written once —
  `libcudla` → `nvidia-l4t-cuda`, when `dpkg -S` says `libcudla-12-6` — which
  would have declared a dependency nothing provides; caught before shipping).
- **Debian policy forbids `/usr/local` for packages** → the launcher is `/usr/bin`.
- **`--check-running` is TRI-STATE**: `0` running / `1` definitely not / `4` cannot
  determine. `prerm` proceeds ONLY on exactly `1`. `if denso --check-running; then
  …` is WRONG — it treats "couldn't tell" as safe.
- Anything touching `/opt/denso/data` runs **as the target user** (`runuser`);
  root-owned artifacts there make the app unable to write, and a root-owned lock
  makes every later `--check-running` return `4`.
- **`| head`/`grep -q`/`awk {exit}` under `set -o pipefail`**: an early-exiting
  consumer can SIGPIPE a producer that hasn't finished, killing the script. Large
  output made this deterministic for `ldconfig -p` (172KB, over the ~64KB pipe
  buffer, so it blocks mid-write). The other current producers are safe because
  they complete in a **single write** (231/91/42 bytes) — **not** because
  sub-64KB output is universally safe: a small producer doing multiple writes can
  still be caught between them. Check the producer, don't assume a size rule.
- `models/*.engine`, `*.names.json` and `trt_cache/` are git-ignored: a sidecar is
  generated on-device beside its engine. Build outputs go to `dist/` (ignored) —
  writing them in the repo root made the build dirty its own tree and refuse the
  next run.

## Gotchas

- Tracked architecture doc is **`docs/ARCHITECTURE.md` (UPPERCASE)** — `git add
  docs/architecture.md` stages nothing.
- Jetson is **gcc 11 / Qt 6.2**: include `<cstddef>` for bare `size_t`; the 3-arg
  `QTransform::rotate` is Qt 6.5+ (keep the version guard).
- `digitv3.onnx` is **static batch=1** — a dynamic `opt=4` engine needs
  re-exporting the ONNX with `dynamic=True`.
- Don't leak Qt Widgets / OpenCV / ORT / CUDA / TensorRT into `denso_core`.
- Keep platform behavior behind the existing interfaces; the shared
  letterbox/decode must stay identical across backends.
- **`models/` is git-ignored by pattern** (`*.onnx`, `*.pt`, `*.engine`,
  `*.names.json`, `trt_cache/`) — they are build/training artifacts, device- and
  version-specific. **NOTHING under `models/` is tracked** — `denso.onnx` was the
  last exception (tracked before that rule, and gitignore cannot untrack an
  already-tracked file) until it was `git rm`'d with the digitv2 family, so a
  fresh clone has an EMPTY `models/`: see **Provisioning a model** in README.md.
  This USED to be a `git add -A` landmine (the
  ignore list named `models/digitv2.onnx` *specifically*, so every new model was
  sweepable); the pattern closed it. It also stopped a subtler bug: an unignored
  new model permanently dirties the tree, and `tools/build_package.sh` refuses to
  package a dirty tree — so dropping in `digitv3.onnx` silently blocked the build.
- The DB migration chain is at **v13** (`model_migration_receipt` — the
  rollback-complete receipt for a model-generation swap; **v12** was
  `camera.setup_complete`; **v11** was
  `camera.areas_need_review`, added for the
  editable-source / ROI-quarantine feature). Add a new migration — never edit a
  shipped one.
- Logging is a bounded rotating file sink (`src/app/logging/`, ~25 MiB cap) meant
  for 24/7 runs; `qDebug/qWarning/qCritical` route through it. `DENSO_LOG_LEVEL`
  sets the floor.
