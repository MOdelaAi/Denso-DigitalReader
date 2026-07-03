# Jetson (JetPack/aarch64) Bring-up & Deploy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the `denso` GUI app compile, run, and deploy on JetPack/aarch64 (NVIDIA Jetson) with full Windows-parity networking, without changing any Windows behavior.

**Architecture:** Five units in dependency order — (1) build seam so the tree compiles under aarch64/GCC, (2) on-device runtime bring-up (aarch64 ORT `.so` + GStreamer camera), (3) nmcli Wi-Fi backend to fill the three throwing Linux stubs, (4) a pseudo-filesystem filter for the disk-sum, (5) a systemd deploy unit + install script. Units 3 and 4 are pure logic TDD'd off-device with Catch2; units 1, 2, 5 are verified on the Jetson.

**Tech Stack:** C++20, Qt6 (Core/Sql/Widgets/Multimedia/Network), CMake + Ninja, OpenCV (GStreamer backend), ONNX Runtime (TensorRT→CUDA→CPU), Catch2 v3, NetworkManager (`nmcli`), systemd.

## Global Constraints

- **No Windows regression.** Every change is additive (new Linux-only files/helpers) or guarded by `#ifdef _WIN32` / `if(WIN32)`. The Windows build and its `ctest` run must stay green and behavior byte-for-byte identical. The Windows build is the local gate for every CMake/source task.
- **`denso_core` must not link `Qt6::Widgets`, OpenCV, or ORT** — only `Qt6::Core`/`Sql`. New pure helpers for the Wi-Fi backend and disk filter go in `denso_core` and depend only on those.
- **Pure-helper + thin-runner split.** OS command logic lives in a pure, unit-tested helper (like `netsh::build_netsh_commands`); the `QProcess` runner stays a thin wrapper in the per-OS backend `.cpp`.
- **Wi-Fi PSK is never persisted** — it is handed to `nmcli` only, never written to `denso.db` (same contract as the Windows backend).
- **Migrations are append-only** — not touched by this plan (no schema change).
- Build/test commands (MSYS2 UCRT64 on Windows dev, native on Jetson):
  `cmake -S . -B build -G Ninja` · `cmake --build build` · `ctest --test-dir build`.

---

### Task 1: Build seam — compile on aarch64

Makes the tree build under the Jetson toolchain. No behavior change; Windows path stays identical via guards. This task's full verification (an aarch64 compile) happens on the Jetson in Task 2; the **local** gate here is that the Windows build + `ctest` are still green after the guarded edits.

**Files:**
- Modify: `CMakeLists.txt:21-26` (ORT imported target — OS branch)
- Modify: `src/app/ui/camera/shared/detection/ort_engine.cpp` (path-type `#ifdef`)
- Modify: `src/app/CMakeLists.txt:77-107` (guard `.dll` copy, add Linux `.so` copy + rpath)

**Interfaces:**
- Consumes: nothing (first task).
- Produces: an `onnxruntime` IMPORTED target that resolves to `.dll` on Windows and `libonnxruntime.so` on Linux; an `ort_engine.cpp` that compiles against both the wide-char (Windows) and narrow (Linux) `Ort::Session` path overloads.

- [ ] **Step 1: Branch the ORT imported target by OS**

In `CMakeLists.txt`, replace the current block (lines 21-26):

```cmake
set(ORT_DIR ${CMAKE_SOURCE_DIR}/third_party/onnxruntime)
add_library(onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION ${ORT_DIR}/lib/onnxruntime.dll
    IMPORTED_IMPLIB   ${ORT_DIR}/lib/onnxruntime.dll
    INTERFACE_INCLUDE_DIRECTORIES ${ORT_DIR}/include)
```

with:

```cmake
set(ORT_DIR ${CMAKE_SOURCE_DIR}/third_party/onnxruntime)
add_library(onnxruntime SHARED IMPORTED GLOBAL)
if(WIN32)
    # MinGW links the C API directly against the MSVC-built DLL (clean C ABI).
    set_target_properties(onnxruntime PROPERTIES
        IMPORTED_LOCATION ${ORT_DIR}/lib/onnxruntime.dll
        IMPORTED_IMPLIB   ${ORT_DIR}/lib/onnxruntime.dll
        INTERFACE_INCLUDE_DIRECTORIES ${ORT_DIR}/include)
else()
    # Jetson/aarch64: link the shared object directly (no import library).
    set_target_properties(onnxruntime PROPERTIES
        IMPORTED_LOCATION ${ORT_DIR}/lib/libonnxruntime.so
        INTERFACE_INCLUDE_DIRECTORIES ${ORT_DIR}/include)
endif()
```

- [ ] **Step 2: Make the ORT session path type cross-platform**

In `src/app/ui/camera/shared/detection/ort_engine.cpp`:

Replace the `widen` helper (the function `std::wstring widen(const std::string& s)`) with a path-type alias + converter:

```cpp
// ORT takes a wide path on Windows (const wchar_t*) and a narrow path on Linux
// (const char*). Alias the platform path type so make_session is portable; the
// model paths are ASCII (models/*.onnx), so the Windows widening is lossless.
#ifdef _WIN32
using OrtPathString = std::wstring;
inline OrtPathString to_ort_path(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}
#else
using OrtPathString = std::string;
inline OrtPathString to_ort_path(const std::string& s) { return s; }
#endif
```

Change `make_session`'s signature from `const std::wstring& path` to `const OrtPathString& path` (the body's `Ort::Session(env, path.c_str(), opts)` is unchanged — `.c_str()` yields `const wchar_t*` on Windows and `const char*` on Linux, both valid `Ort::Session` overloads).

In the `OrtEngine` constructor, change:

```cpp
const std::wstring wpath = widen(model_path);
```

to:

```cpp
const OrtPathString wpath = to_ort_path(model_path);
```

- [ ] **Step 3: Guard the runtime-library copy by OS in the app CMake**

In `src/app/CMakeLists.txt`, replace the single POST_BUILD block that copies the ORT DLLs **and** the models (lines 77-88) with a cross-platform models copy plus an OS-branched runtime-lib copy:

```cmake
# Models are deployed on every platform (the app scans <exe-dir>/models at
# startup). Configure-time glob — re-run cmake after adding a new .onnx.
file(GLOB MODEL_ONNX_FILES "${CMAKE_SOURCE_DIR}/models/*.onnx")
add_custom_command(TARGET denso POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:denso>/models
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MODEL_ONNX_FILES}
        $<TARGET_FILE_DIR:denso>/models
    VERBATIM)

if(WIN32)
    # ORT + provider DLLs must sit beside the exe (Windows DLL search).
    add_custom_command(TARGET denso POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ORT_DIR}/lib/onnxruntime.dll
            ${ORT_DIR}/lib/onnxruntime_providers_shared.dll
            ${ORT_DIR}/lib/onnxruntime_providers_cuda.dll
            ${ORT_DIR}/lib/onnxruntime_providers_tensorrt.dll
            $<TARGET_FILE_DIR:denso>
        VERBATIM)
else()
    # Jetson/aarch64: the .so resolves via $ORIGIN rpath from beside the exe.
    set_target_properties(denso PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN")
    add_custom_command(TARGET denso POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ORT_DIR}/lib/libonnxruntime.so
            $<TARGET_FILE_DIR:denso>
        VERBATIM)
endif()
```

Leave the `third_party/gpu_ep/*.dll` glob block (lines 90-107) unchanged — it is already a `if(GPU_EP_DLLS)` no-op-friendly glob and is Windows-DLL-specific; on Linux the glob simply matches nothing.

- [ ] **Step 4: Verify the Windows build + tests still pass (local gate)**

Run: `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`
Expected: build succeeds; `ctest` reports the same pass count as before this task (the guarded edits are inert on Windows).

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/app/CMakeLists.txt src/app/ui/camera/shared/detection/ort_engine.cpp
git commit -m "build: cross-platform ORT link + path type for aarch64 (Windows unchanged)"
```

---

### Task 2: On-device runtime bring-up + JETSON_SETUP.md

Provision the aarch64 ORT, compile on the Jetson, and verify the app runs with real inference + camera capture. Device-only — no automated tests; verification is the explicit on-Jetson smoke steps below. Produces `docs/JETSON_SETUP.md`.

**Files:**
- Create: `docs/JETSON_SETUP.md`

**Interfaces:**
- Consumes: the cross-platform CMake + `ort_engine.cpp` from Task 1.
- Produces: a documented, reproducible Jetson build + provisioning procedure; confirmation the TensorRT→CUDA→CPU ladder and GStreamer path work on device.

- [ ] **Step 1: Provision the aarch64 ONNX Runtime**

On the Jetson, obtain a JetPack-matched `onnxruntime-gpu` (aarch64) build with the TensorRT + CUDA execution providers (from NVIDIA / Jetson Zoo, matching the installed JetPack/TensorRT/CUDA versions). Lay it out exactly like the Windows build so CMake's `${ORT_DIR}` resolves:

```
third_party/onnxruntime/
  include/        # onnxruntime_cxx_api.h, ...
  lib/libonnxruntime.so
```

- [ ] **Step 2: Configure + build on the Jetson**

Run (on the Jetson):
```bash
cmake -S . -B build -G Ninja
cmake --build build
```
Expected: compiles clean. If `ort_engine.cpp` or the ORT link fails, the Task 1 guards or the `${ORT_DIR}` layout are wrong — fix before proceeding.

- [ ] **Step 3: Verify runtime — inference + capture**

Run: `./build/src/app/denso`
Verify on device:
- Startup warm-up completes and the window shows (the TensorRT engine builds + caches under `models/trt_cache/` on first run — minutes-long, expected).
- The log line `[ort] loaded <model> on TensorRT` (or `CUDA`) appears — confirming GPU EP, not the CPU fallback.
- A **USB** camera streams with detection overlays.
- An **RTSP** camera streams via GStreamer (`CAP_GSTREAMER`) with detection overlays and no live-pipeline `cap.set()` crash (the USB-only gate holds).

- [ ] **Step 4: Write `docs/JETSON_SETUP.md`**

Document the exact steps that worked: JetPack/CUDA/TensorRT versions, the ORT build source + URL, the `third_party/onnxruntime/` layout, the GStreamer plugin packages needed (NVIDIA stack), and the build/run commands. Cross-reference `docs/GPU_SETUP.md` for the shared ORT/TensorRT notes. Record the resolved answer to the spec's open question (exact JetPack + ORT build).

- [ ] **Step 5: Commit**

```bash
git add docs/JETSON_SETUP.md
git commit -m "docs: Jetson/aarch64 ONNX Runtime provisioning + runtime bring-up"
```

---

### Task 3: nmcli config-apply command builder (pure, TDD)

Pure helper that turns a `NetConfig` into the ordered `nmcli` argv lists to apply it — the testable half of `apply_config`. Mirrors `netsh::build_netsh_commands`.

**Files:**
- Modify: `src/core/network/linux/nmcli.h` (declare `build_nmcli_commands`)
- Modify: `src/core/network/linux/nmcli.cpp` (implement it)
- Modify: `tests/CMakeLists.txt` (already lists `test_nmcli.cpp` — no change needed)
- Test: `tests/test_nmcli.cpp` (append cases)

**Interfaces:**
- Consumes: `denso::network::NetConfig` (`network/model.h`) — fields `iface`, `mode` (`"dhcp"`|`"static"`), `ip`, `prefix`, `gateway`, `dns1`, `dns2`, `ssid`, `security`.
- Produces: `std::vector<std::vector<std::string>> denso::network::nmcli::build_nmcli_commands(const NetConfig& c, const std::string& connection)` — argv lists (each a full `nmcli` command, argv[0] = `"connection"`/`"device"` sub-args, **without** the leading `"nmcli"`), applied in order. Throws `std::runtime_error` for a static config missing `ip`/`prefix`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_nmcli.cpp`:

```cpp
#include "network/linux/nmcli.h"   // if not already included at top
#include "network/model.h"

using denso::network::NetConfig;
using denso::network::nmcli::build_nmcli_commands;

TEST_CASE("build_nmcli_commands: static config sets manual ipv4 then brings up") {
    NetConfig c;
    c.iface = "ethernet";
    c.mode = "static";
    c.ip = "192.168.1.50";
    c.prefix = 24;
    c.gateway = "192.168.1.1";
    c.dns1 = "8.8.8.8";
    c.dns2 = "1.1.1.1";

    const auto cmds = build_nmcli_commands(c, "Wired connection 1");

    REQUIRE(cmds.size() == 2);
    REQUIRE(cmds[0] == std::vector<std::string>{
        "connection", "modify", "Wired connection 1",
        "ipv4.method", "manual",
        "ipv4.addresses", "192.168.1.50/24",
        "ipv4.gateway", "192.168.1.1",
        "ipv4.dns", "8.8.8.8 1.1.1.1"});
    REQUIRE(cmds[1] == std::vector<std::string>{"connection", "up", "Wired connection 1"});
}

TEST_CASE("build_nmcli_commands: dhcp clears static fields then brings up") {
    NetConfig c;
    c.iface = "ethernet";
    c.mode = "dhcp";

    const auto cmds = build_nmcli_commands(c, "Wired connection 1");

    REQUIRE(cmds.size() == 2);
    REQUIRE(cmds[0] == std::vector<std::string>{
        "connection", "modify", "Wired connection 1",
        "ipv4.method", "auto",
        "ipv4.addresses", "",
        "ipv4.gateway", "",
        "ipv4.dns", ""});
    REQUIRE(cmds[1] == std::vector<std::string>{"connection", "up", "Wired connection 1"});
}

TEST_CASE("build_nmcli_commands: static without ip/prefix throws") {
    NetConfig c;
    c.iface = "wifi";
    c.mode = "static";  // no ip/prefix
    REQUIRE_THROWS_AS(build_nmcli_commands(c, "conn"), std::runtime_error);
}

TEST_CASE("build_nmcli_commands: static with only dns1 emits single dns") {
    NetConfig c;
    c.iface = "ethernet"; c.mode = "static";
    c.ip = "10.0.0.2"; c.prefix = 8; c.dns1 = "9.9.9.9";
    const auto cmds = build_nmcli_commands(c, "conn");
    REQUIRE(cmds[0] == std::vector<std::string>{
        "connection", "modify", "conn",
        "ipv4.method", "manual",
        "ipv4.addresses", "10.0.0.2/8",
        "ipv4.dns", "9.9.9.9"});
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ctest --test-dir build -R nmcli` (or build first: `cmake --build build`)
Expected: FAIL — `build_nmcli_commands` not declared.

- [ ] **Step 3: Declare `build_nmcli_commands`**

Add to `src/core/network/linux/nmcli.h` (inside `namespace denso::network::nmcli`, and add `#include "network/model.h"`, `#include <vector>` to the header includes):

```cpp
/// Build the ordered `nmcli` argument lists (each without the leading "nmcli")
/// that apply `c` to `connection` — the NetworkManager connection profile name.
/// Static requires ip+prefix (throws std::runtime_error otherwise); dhcp clears
/// the static fields. Pure — the runner lives in linux_backend.cpp.
std::vector<std::vector<std::string>> build_nmcli_commands(const NetConfig& c,
                                                           const std::string& connection);
```

- [ ] **Step 4: Implement it**

Add to `src/core/network/linux/nmcli.cpp` (inside the namespace; add `#include <stdexcept>`):

```cpp
std::vector<std::vector<std::string>> build_nmcli_commands(const NetConfig& c,
                                                           const std::string& connection) {
    std::vector<std::string> mod{"connection", "modify", connection};
    if (c.mode == "static") {
        if (!c.ip || !c.prefix) {
            throw std::runtime_error("static config requires ip and prefix");
        }
        mod.push_back("ipv4.method");
        mod.push_back("manual");
        mod.push_back("ipv4.addresses");
        mod.push_back(*c.ip + "/" + std::to_string(*c.prefix));
        if (c.gateway) {
            mod.push_back("ipv4.gateway");
            mod.push_back(*c.gateway);
        }
        std::string dns;
        if (c.dns1) dns = *c.dns1;
        if (c.dns2) dns += (dns.empty() ? "" : " ") + *c.dns2;
        if (!dns.empty()) {
            mod.push_back("ipv4.dns");
            mod.push_back(dns);
        }
    } else {  // dhcp — clear any prior static config
        mod.insert(mod.end(), {"ipv4.method", "auto",
                               "ipv4.addresses", "",
                               "ipv4.gateway", "",
                               "ipv4.dns", ""});
    }
    return {mod, {"connection", "up", connection}};
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -R nmcli`
Expected: PASS (all `build_nmcli_commands` cases).

- [ ] **Step 6: Commit**

```bash
git add src/core/network/linux/nmcli.h src/core/network/linux/nmcli.cpp tests/test_nmcli.cpp
git commit -m "feat(net): pure nmcli config-apply command builder (Linux)"
```

---

### Task 4: nmcli Wi-Fi scan parser (pure, TDD)

Pure helper that parses `nmcli -t -f SSID,SIGNAL,SECURITY device wifi list` terse output into `WifiNetwork`s — the testable half of `scan_wifi`.

**Files:**
- Modify: `src/core/network/linux/nmcli.h` (declare `parse_wifi_list`)
- Modify: `src/core/network/linux/nmcli.cpp` (implement it)
- Test: `tests/test_nmcli.cpp` (append cases)

**Interfaces:**
- Consumes: raw terse `nmcli` wifi-list stdout.
- Produces: `std::vector<denso::network::WifiNetwork> denso::network::nmcli::parse_wifi_list(const std::string& out)` — `secured = !security.empty()`; blank-SSID lines skipped.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_nmcli.cpp`:

```cpp
#include "network/model.h"   // WifiNetwork (if not already included)
using denso::network::WifiNetwork;
using denso::network::nmcli::parse_wifi_list;

TEST_CASE("parse_wifi_list: parses ssid/signal/security and secured flag") {
    const std::string out =
        "HomeNet:82:WPA2\n"
        "OpenCafe:47:\n"
        "OfficeAP:63:WPA1 WPA2\n";
    const auto nets = parse_wifi_list(out);
    REQUIRE(nets.size() == 3);
    REQUIRE(nets[0] == WifiNetwork{"HomeNet", "82", true});
    REQUIRE(nets[1] == WifiNetwork{"OpenCafe", "47", false});
    REQUIRE(nets[2] == WifiNetwork{"OfficeAP", "63", true});
}

TEST_CASE("parse_wifi_list: skips blank-ssid and malformed lines") {
    const std::string out =
        ":90:WPA2\n"      // hidden / blank SSID
        "GoodNet:55:WPA2\n"
        "garbage-no-colons\n";
    const auto nets = parse_wifi_list(out);
    REQUIRE(nets.size() == 1);
    REQUIRE(nets[0].ssid == "GoodNet");
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build && ctest --test-dir build -R nmcli`
Expected: FAIL — `parse_wifi_list` not declared.

- [ ] **Step 3: Declare `parse_wifi_list`**

Add to `src/core/network/linux/nmcli.h` (add `#include <vector>` if not present from Task 3):

```cpp
/// From `nmcli -t -f SSID,SIGNAL,SECURITY device wifi list`, return the visible
/// networks. secured = the SECURITY field is non-empty; blank-SSID lines are
/// skipped. Pure.
std::vector<WifiNetwork> parse_wifi_list(const std::string& out);
```

- [ ] **Step 4: Implement it**

Add to `src/core/network/linux/nmcli.cpp` (reuses the file-local `split_colons` and `split_lines`):

```cpp
std::vector<WifiNetwork> parse_wifi_list(const std::string& out) {
    std::vector<WifiNetwork> nets;
    for (const auto& line : split_lines(out)) {
        const auto f = split_colons(line);
        if (f.size() < 3) continue;      // need SSID:SIGNAL:SECURITY
        if (f[0].empty()) continue;      // hidden / blank SSID
        nets.push_back(WifiNetwork{f[0], f[1], !f[2].empty()});
    }
    return nets;
}
```

Note: `WifiNetwork` is already visible via `network/model.h` (included by `nmcli.h` from Task 3).

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -R nmcli`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/core/network/linux/nmcli.h src/core/network/linux/nmcli.cpp tests/test_nmcli.cpp
git commit -m "feat(net): pure nmcli wifi-scan parser (Linux)"
```

---

### Task 5: Wire the LinuxBackend runner (apply/scan/connect)

Fill the three throwing `LinuxBackend` stubs using the pure helpers + a thin `QProcess` runner. Device-only behavior (verified on the Jetson in Step 4); the change must compile on Windows too (the file is Linux-only-compiled, so the Windows gate is just "still green").

**Files:**
- Modify: `src/core/network/linux/linux_backend.cpp`

**Interfaces:**
- Consumes: `nmcli::build_nmcli_commands` (Task 3), `nmcli::parse_wifi_list` (Task 4).
- Produces: a fully-implemented `LinuxBackend` (`apply_config`/`scan_wifi`/`connect_wifi` no longer throw "not implemented").

- [ ] **Step 1: Add a checked runner + resolve the connection name**

In `src/core/network/linux/linux_backend.cpp`, add near the existing `run(...)` helper (in the anonymous namespace):

```cpp
/// Run one command, throwing std::runtime_error on spawn failure or non-zero
/// exit (mirrors the Windows backend's run_checked).
void run_checked(const QString& cmd, const QStringList& args) {
    QProcess p;
    p.start(cmd, args);
    if (!p.waitForStarted()) {
        throw std::runtime_error("failed to spawn " + cmd.toStdString() + ": " +
                                 p.errorString().toStdString());
    }
    p.waitForFinished(-1);
    if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) return;
    const QString err = QString::fromUtf8(p.readAllStandardError());
    const QString detail =
        err.trimmed().isEmpty() ? QString::fromUtf8(p.readAllStandardOutput()) : err;
    throw std::runtime_error(cmd.toStdString() + " " + args.join(' ').toStdString() +
                             ": " + detail.trimmed().toStdString());
}

QStringList to_qargs(const std::vector<std::string>& args) {
    QStringList q;
    for (const auto& a : args) q << QString::fromStdString(a);
    return q;
}

/// The active NetworkManager connection name for a device type ("ethernet" |
/// "wifi"), or empty if none. Reads `nmcli -t -f NAME,TYPE connection show --active`.
std::string active_connection(const std::string& iface) {
    const std::string want = (iface == "wifi") ? "wifi" : "ethernet";
    const std::string out =
        run("nmcli", {"-t", "-f", "NAME,TYPE", "connection", "show", "--active"});
    for (const auto& line : denso::strutil::split_lines(out)) {
        const auto pos = line.rfind(':');
        if (pos == std::string::npos) continue;
        if (line.substr(pos + 1) == want) return line.substr(0, pos);
    }
    return {};
}
```

Add the includes this needs at the top of the file: `#include "network/linux/nmcli.h"` (already present), `#include "util/strutil.h"`, and ensure `<stdexcept>` is present (it is).

- [ ] **Step 2: Implement the three methods**

Replace the three throwing stub bodies in `class LinuxBackend`:

```cpp
void apply_config(const NetConfig& config) const override {
    const std::string conn = active_connection(config.iface);
    if (conn.empty()) {
        throw std::runtime_error("no active nmcli connection for " + config.iface);
    }
    for (const auto& args : nmcli::build_nmcli_commands(config, conn)) {
        run_checked("nmcli", to_qargs(args));
    }
}

std::vector<WifiNetwork> scan_wifi() const override {
    const std::string out =
        run("nmcli", {"-t", "-f", "SSID,SIGNAL,SECURITY", "device", "wifi", "list"});
    return nmcli::parse_wifi_list(out);
}

void connect_wifi(const std::string& ssid,
                  const std::optional<std::string>& password) const override {
    QStringList args{"device", "wifi", "connect", QString::fromStdString(ssid)};
    if (password) {
        args << "password" << QString::fromStdString(*password);
    }
    run_checked("nmcli", args);
}
```

- [ ] **Step 3: Verify the Windows build + tests still pass (local gate)**

Run: `cmake --build build && ctest --test-dir build`
Expected: unchanged pass count — `linux_backend.cpp` is not compiled on Windows, so this only confirms nothing else broke and the Task 3/4 helpers still pass.

- [ ] **Step 4: On-Jetson verification** (during/after Task 2's device session)

On the Jetson: open Settings → Network. Verify: applying a static Ethernet config takes effect (`nmcli connection show` reflects it); a Wi-Fi scan lists nearby networks; connecting to a WPA2 network with a password succeeds. A failed apply must surface non-fatally (collected by `reassert` at boot), never crash.

- [ ] **Step 5: Commit**

```bash
git add src/core/network/linux/linux_backend.cpp
git commit -m "feat(net): implement nmcli apply/scan/connect on Linux backend"
```

---

### Task 6: Disk-sum pseudo-filesystem filter (pure, TDD)

Stop `total_storage()` over-counting Linux pseudo-filesystems (tmpfs/overlay/loop/squashfs) by filtering mounts through a pure, testable predicate.

**Files:**
- Modify: `src/core/hardware/collect.h` (declare `is_real_storage`)
- Modify: `src/core/hardware/collect.cpp` (implement + use it)
- Create: `tests/test_collect.cpp`
- Modify: `tests/CMakeLists.txt` (register the new test source)

**Interfaces:**
- Consumes: nothing new.
- Produces: `bool denso::hardware::is_real_storage(const QString& fs_type, const QString& device)` — false for pseudo/virtual filesystems and loop devices, true for real disks (ext4/ntfs/vfat/…).

- [ ] **Step 1: Write the failing test**

Create `tests/test_collect.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "hardware/collect.h"

using denso::hardware::is_real_storage;

TEST_CASE("is_real_storage: excludes pseudo filesystems") {
    REQUIRE_FALSE(is_real_storage("tmpfs", "tmpfs"));
    REQUIRE_FALSE(is_real_storage("devtmpfs", "devtmpfs"));
    REQUIRE_FALSE(is_real_storage("overlay", "overlay"));
    REQUIRE_FALSE(is_real_storage("squashfs", "/dev/loop0"));
    REQUIRE_FALSE(is_real_storage("ext4", "/dev/loop3"));  // snap loop mount
}

TEST_CASE("is_real_storage: keeps real disks") {
    REQUIRE(is_real_storage("ext4", "/dev/mmcblk0p1"));
    REQUIRE(is_real_storage("ext4", "/dev/nvme0n1p1"));
    REQUIRE(is_real_storage("ntfs", "C:"));
    REQUIRE(is_real_storage("vfat", "/dev/sda1"));
}
```

- [ ] **Step 2: Register the new test source and run to verify it fails**

In `tests/CMakeLists.txt`, add `test_collect.cpp` to the `denso_tests` source list (e.g. right after `test_format.cpp` on line 18):

```cmake
    test_format.cpp
    test_collect.cpp
```

Run: `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build -R collect`
Expected: FAIL — `is_real_storage` not declared.

- [ ] **Step 3: Declare the predicate**

In `src/core/hardware/collect.h`, add (inside `namespace denso::hardware`, with `#include <QString>` if not already present):

```cpp
/// True if a mounted volume is real backing storage — false for pseudo/virtual
/// filesystems (tmpfs/overlay/squashfs/…) and loop devices, which would
/// otherwise inflate the total (a known Linux over-count). Pure/testable.
bool is_real_storage(const QString& fs_type, const QString& device);
```

- [ ] **Step 4: Implement it and use it in `total_storage`**

In `src/core/hardware/collect.cpp`, add the definition (outside the anonymous namespace, in `namespace denso::hardware`), and filter the loop in `total_storage()`:

```cpp
bool is_real_storage(const QString& fs_type, const QString& device) {
    static const QStringList kPseudo = {
        "tmpfs", "devtmpfs", "devfs", "sysfs", "proc", "overlay",
        "squashfs", "ramfs", "cgroup", "cgroup2", "autofs", "mqueue"};
    if (kPseudo.contains(fs_type.toLower())) return false;
    if (device.startsWith("/dev/loop")) return false;
    return true;
}
```

Change `total_storage()`'s loop body to consult it:

```cpp
uint64_t total_storage() {
    uint64_t total = 0;
    for (const QStorageInfo& v : QStorageInfo::mountedVolumes()) {
        if (!v.isValid() || !v.isReady()) continue;
        if (!is_real_storage(QString::fromUtf8(v.fileSystemType()),
                             QString::fromUtf8(v.device()))) {
            continue;
        }
        const qint64 bytes = v.bytesTotal();
        if (bytes > 0) total += static_cast<uint64_t>(bytes);
    }
    return total;
}
```

Update the `total_storage` doc comment: the "KNOWN LIMITATION … over-counting storage" note is now resolved — replace it with a line explaining pseudo-fs/loop mounts are excluded via `is_real_storage`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -R collect`
Expected: PASS. Also run the full suite (`ctest --test-dir build`) — unchanged elsewhere.

- [ ] **Step 6: Commit**

```bash
git add src/core/hardware/collect.h src/core/hardware/collect.cpp tests/test_collect.cpp tests/CMakeLists.txt
git commit -m "fix(hardware): exclude pseudo-fs/loop mounts from disk total"
```

---

### Task 7: systemd deploy unit + install script

Package the Jetson deploy: a systemd unit + an install script that stages the exe, models, and ORT `.so` into a deploy dir and enables the service. Greenfield; device-verified.

**Files:**
- Create: `deploy/denso.service`
- Create: `deploy/install.sh`
- Modify: `docs/JETSON_SETUP.md` (append a Deploy section)

**Interfaces:**
- Consumes: the built `denso` exe + co-located `models/` + `libonnxruntime.so` from Tasks 1-2; the deploy dir convention decided here.
- Produces: an installable, reboot-surviving service.

- [ ] **Step 1: Create the systemd unit**

Create `deploy/denso.service` (deploy dir `/opt/denso` — the app resolves `models/` and `denso.db` relative to the exe via `WorkingDirectory`):

```ini
[Unit]
Description=Denso Digital Reader
After=graphical-session.target network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/denso
ExecStart=/opt/denso/denso
Restart=on-failure
RestartSec=3
# GUI session — set to the Jetson's active display server.
Environment=DISPLAY=:0
Environment=QT_QPA_PLATFORM=xcb

[Install]
WantedBy=graphical.target
```

- [ ] **Step 2: Create the install script**

Create `deploy/install.sh` (mark executable: `chmod +x deploy/install.sh`):

```bash
#!/usr/bin/env bash
# Build denso and install it as a systemd service on the Jetson.
# Run from the repo root on the Jetson after provisioning third_party/onnxruntime/
# (see docs/JETSON_SETUP.md). Requires sudo for the /opt install + systemctl.
set -euo pipefail

DEPLOY_DIR=/opt/denso
BUILD_DIR=build

cmake -S . -B "$BUILD_DIR" -G Ninja
cmake --build "$BUILD_DIR"

sudo mkdir -p "$DEPLOY_DIR"
# The POST_BUILD step already co-located models/ + libonnxruntime.so beside the exe.
EXE_DIR="$(dirname "$(find "$BUILD_DIR" -name denso -type f | head -n1)")"
sudo cp -r "$EXE_DIR/denso" "$EXE_DIR/models" "$EXE_DIR/libonnxruntime.so" "$DEPLOY_DIR/"

sudo cp deploy/denso.service /etc/systemd/system/denso.service
sudo systemctl daemon-reload
sudo systemctl enable denso.service
sudo systemctl restart denso.service
echo "Installed. Status: sudo systemctl status denso  |  Logs: journalctl -u denso -f"
```

- [ ] **Step 3: Install + verify on the Jetson**

Run (on the Jetson): `./deploy/install.sh`
Verify:
- `systemctl status denso` shows `active (running)` and the window appears.
- Reboot the Jetson; the app auto-starts (`systemctl is-enabled denso` → `enabled`).
- `journalctl -u denso` shows the startup + `[ort] loaded … on TensorRT` line.

- [ ] **Step 4: Document the deploy flow**

Append a "Deploy (systemd)" section to `docs/JETSON_SETUP.md`: the `/opt/denso` layout, `./deploy/install.sh`, the display/`QT_QPA_PLATFORM` env to adjust per Jetson, and the `journalctl`/`systemctl` operational commands. Record the resolved deploy-dir convention (the spec's second open question).

- [ ] **Step 5: Commit**

```bash
git add deploy/denso.service deploy/install.sh docs/JETSON_SETUP.md
git commit -m "feat(deploy): systemd unit + install script for Jetson"
```

---

## Self-Review

**Spec coverage** (each spec unit → task):
- Spec Unit 1 (build seam: CMake ORT branch, `ort_engine.cpp` `#ifdef`, app-CMake `.dll` guard + rpath) → **Task 1**. ✓
- Spec Unit 2 (runtime bring-up: aarch64 ORT, TRT ladder verify, GStreamer verify, `JETSON_SETUP.md`) → **Task 2**. ✓
- Spec Unit 3 (nmcli Wi-Fi: pure `build_nmcli_commands` + `parse_wifi_list`, backend apply/scan/connect) → **Tasks 3, 4, 5**. ✓
- Spec Unit 4 (disk-sum pseudo-fs filter, pure predicate) → **Task 6**. ✓
- Spec Unit 5 (systemd unit + install.sh + docs) → **Task 7**. ✓
- Cross-cutting: Windows-regression gate is an explicit step in Tasks 1, 5, 6; PSK-not-persisted honored in Task 5 `connect_wifi`; pure-helper/thin-runner split honored in Tasks 3-5. ✓
- Spec open questions (JetPack+ORT build; deploy dir) → recorded in Task 2 Step 4 and Task 7 Step 4. ✓

**Placeholder scan:** no TBD/TODO; every code step shows complete code; every command has an expected result. ✓

**Type consistency:** `build_nmcli_commands(const NetConfig&, const std::string&)`, `parse_wifi_list(const std::string&) -> std::vector<WifiNetwork>`, `is_real_storage(const QString&, const QString&) -> bool`, `active_connection(const std::string&) -> std::string`, `run_checked(const QString&, const QStringList&)`, `to_qargs(const std::vector<std::string>&) -> QStringList` — names/signatures match across their producing and consuming tasks (Task 5 consumes the exact signatures Tasks 3/4 produce). ✓
