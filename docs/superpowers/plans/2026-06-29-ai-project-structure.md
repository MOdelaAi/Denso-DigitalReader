# AI-Friendly Project Structure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate per-session re-exploration of the codebase (the main token sink) by adding a lean root `CLAUDE.md` + on-demand `docs/ARCHITECTURE.md`, and by splitting the two largest source files into small, focused units.

**Architecture:** Pure documentation + mechanical refactor. The two large files (`network/windows.rs`, `wiring.rs`) become directory modules whose existing functions and tests move *verbatim* into concern-focused submodules; nothing about runtime behavior, the DB schema, or the UI changes. Docs are written last so they describe the final tree.

**Tech Stack:** Rust (edition 2024), Slint 1.17, rusqlite, sysinfo. Markdown for docs.

## Global Constraints

- All artifacts stay **inside the project folder** (`Denso-DigitalReader/`). Nothing is written outside it.
- `edition = "2024"` requires Rust ≥ 1.85 (dev machine has cargo 1.96 — OK).
- **No behavior change, no schema change, no UI change, no new dependencies, no unrelated refactoring.**
- Refactor tasks are verified by: `cargo build` passes, `cargo fmt` leaves no diff, and `cargo test` still reports **50 passed; 0 failed** (the baseline on Windows — moving tests must not change the count).
- Do not split any file already under ~200 lines.
- Commit after each task. Commit messages end with:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

### Task 1: Split `network/windows.rs` into a directory module

Convert the single 487-line file into `network/windows/` with four concern-focused files. Functions and their `#[cfg(test)]` tests move **verbatim** (byte-for-byte bodies); only module scaffolding, `use` lines, and item visibility change.

**Files:**
- Delete: `src/network/windows.rs`
- Create: `src/network/windows/mod.rs` (backend impl)
- Create: `src/network/windows/netsh.rs` (config command building + execution)
- Create: `src/network/windows/wifi.rs` (Wi-Fi scan parsing + profile XML)
- Create: `src/network/windows/parse.rs` (status parsing)
- Unchanged: `src/network/mod.rs` (its `#[cfg(windows)] mod windows;` resolves to the directory identically)

**Interfaces:**
- Consumes: domain types from `crate::network` — `NetConfig`, `NetworkBackend`, `NetworkSnapshot`, `WifiNetwork`, `InterfaceStatus`.
- Produces: `network::windows::WindowsBackend` (a `pub struct`, same path as before — `network/mod.rs::backend()` keeps using `windows::WindowsBackend` unchanged).

**Cross-submodule visibility map** (apply exactly):
- `netsh.rs`: `build_netsh_commands`, `run_checked`, `run` → `pub(super)`; `adapter_name`, `prefix_to_mask` → private.
- `wifi.rs`: `parse_wifi_networks`, `build_profile_xml` → `pub(super)`; `xml_escape` → private.
- `parse.rs`: `build_snapshot`, `value_after_colon` → `pub(super)` (`value_after_colon` is also used by `wifi.rs`); `parse_ipconfig`, `parse_netsh_wlan` → private.

- [ ] **Step 1: Create `src/network/windows/parse.rs`**

Move `value_after_colon` (old `windows.rs:221-226`), `parse_ipconfig` (`228-261`), `parse_netsh_wlan` (`263-279`), `build_snapshot` (`281-288`) **verbatim**, with the header + imports + new visibility below. Move the matching tests (`ipconfig_extracts_eth_and_wifi`, `netsh_extracts_ssid_signal_state`, `value_with_colon_is_kept_intact`, `build_snapshot_merges_wifi`) and the `IPCONFIG` / `NETSH` consts from the old test module.

```rust
//! Parse Windows status CLIs (`ipconfig`, `netsh wlan show interfaces`) into
//! the domain [`NetworkSnapshot`]. Pure string work, unit-tested off-device.

use crate::network::{InterfaceStatus, NetworkSnapshot};

// value_after_colon: change signature to `pub(super) fn value_after_colon(...)`
// parse_ipconfig, parse_netsh_wlan: keep private (`fn ...`)
// build_snapshot: change signature to `pub(super) fn build_snapshot(...)`
// (bodies copied verbatim from old windows.rs)

#[cfg(test)]
mod tests {
    use super::*;
    // IPCONFIG, NETSH consts + the 4 tests listed above, verbatim
}
```

- [ ] **Step 2: Create `src/network/windows/netsh.rs`**

Move `adapter_name` (`49-54`), `prefix_to_mask` (`57-70`), `build_netsh_commands` (`74-115`), `run_checked` (`191-207`), `run` (`209-215`) **verbatim**. Move tests `prefix_to_mask_common_values`, `dhcp_sets_address_and_dns_to_dhcp`, `static_builds_address_then_dns_with_mask_and_gateway`, `static_without_gateway_or_dns_emits_only_address`, `static_without_ip_is_an_error`, plus the test helpers `cfg` and `lines`.

```rust
//! Apply a [`NetConfig`] via `netsh interface ip`: build the ordered command
//! argument lists (pure, unit-tested) and run them. Also the raw command
//! runners shared with the backend.

use crate::network::NetConfig;

// adapter_name, prefix_to_mask: private
// build_netsh_commands, run_checked, run: `pub(super)`
// (bodies verbatim)

#[cfg(test)]
mod tests {
    use super::*;
    // cfg(), lines() helpers + the 6 netsh/prefix tests, verbatim
}
```

- [ ] **Step 3: Create `src/network/windows/wifi.rs`**

Move `parse_wifi_networks` (`119-148`), `xml_escape` (`151-157`), `build_profile_xml` (`161-187`) **verbatim**. Move tests `parse_networks_extracts_ssid_signal_secured`, `parse_networks_skips_hidden_empty_ssid`, `xml_escape_encodes_specials`, `profile_xml_secured_carries_escaped_psk`, `profile_xml_open_has_no_shared_key`, plus the `NETWORKS` const.

```rust
//! Wi-Fi support: parse `netsh wlan show networks` into [`WifiNetwork`]s and
//! build the WLAN profile XML used to join a network.

use crate::network::WifiNetwork;
use super::parse::value_after_colon;

// parse_wifi_networks, build_profile_xml: `pub(super)`
// xml_escape: private
// (bodies verbatim)

#[cfg(test)]
mod tests {
    use super::*;
    // NETWORKS const + the 5 wifi/xml tests, verbatim
}
```

- [ ] **Step 4: Create `src/network/windows/mod.rs`**

Holds the backend impl (old `windows.rs:3-45`) verbatim, declares the submodules, and imports the helpers it calls.

```rust
//! Windows network backend. Drives the OS through `ipconfig` / `netsh` CLIs;
//! helpers split by concern: [`netsh`] (config apply), [`wifi`] (scan + join),
//! [`parse`] (status parsing).

use super::{NetConfig, NetworkBackend, NetworkSnapshot, WifiNetwork};

mod netsh;
mod parse;
mod wifi;

use netsh::{build_netsh_commands, run, run_checked};
use parse::build_snapshot;
use wifi::{build_profile_xml, parse_wifi_networks};

pub struct WindowsBackend;

impl NetworkBackend for WindowsBackend {
    // body verbatim from old windows.rs:6-44
}
```

- [ ] **Step 5: Delete the old file**

```bash
git rm src/network/windows.rs
```

- [ ] **Step 6: Build, format, test**

Run:
```bash
cargo build
cargo fmt
cargo test
```
Expected: `cargo build` succeeds; `cargo fmt` produces no diff (`git diff --quiet` after); `cargo test` ends with `test result: ok. 50 passed; 0 failed; 0 ignored`.

- [ ] **Step 7: Commit**

```bash
git add src/network/
git commit -m "refactor(network): split windows.rs into windows/ submodules

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Split `wiring.rs` into a directory module

Convert the 296-line file into `wiring/` with the Slint↔domain converters isolated from the callback handlers.

**Files:**
- Delete: `src/wiring.rs`
- Create: `src/wiring/mod.rs` (`apply_startup`, `install_handlers`, `load_config_or_default`, `type State`)
- Create: `src/wiring/convert.rs` (`to_net_status`, `to_ui_config`, `from_ui_config`, `wifi_rows` + their tests)
- Unchanged: `src/main.rs` (its `mod wiring;` resolves to the directory identically)

**Interfaces:**
- Consumes: domain types from `crate::network`, `crate::hardware`, `crate::settings`; Slint view types `AppWindow`, `NetConfigUi`, `NetStatus`, `Theme`, `WifiRow`.
- Produces: `wiring::apply_startup(&AppWindow, &Rc<Connection>, &State)` and `wiring::install_handlers(&AppWindow, &Rc<Connection>, &State)` — same public signatures as before (`main.rs` calls them unchanged).

**Visibility map:** `convert.rs`'s `to_net_status`, `to_ui_config`, `from_ui_config`, `wifi_rows` → `pub(super)` (called from `mod.rs`).

- [ ] **Step 1: Create `src/wiring/convert.rs`**

Move `to_net_status` (old `wiring.rs:16-24`), `to_ui_config` (`27-36`), `from_ui_config` (`41-57`), `wifi_rows` (`61-73`) **verbatim**, and the `#[cfg(test)] mod tests` block (`271-296`: `wifi_rows_floats_connected_to_top_and_flags_it`, `wifi_rows_marks_none_when_no_current_ssid`, and the `net` helper).

```rust
//! Boundary converters between the feature modules' domain types and the
//! Slint-generated view types. Feature modules never see Slint types — this is
//! the only place the two worlds meet.

use crate::network;
use crate::{NetConfigUi, NetStatus, WifiRow};

// all four fns: change `fn` -> `pub(super) fn`; bodies + doc comments verbatim
// (`from_ui_config` uses the fully-qualified `slint::SharedString`, no import)

#[cfg(test)]
mod tests {
    use super::*;
    // net() helper + the 2 wifi_rows tests, verbatim
}
```

- [ ] **Step 2: Create `src/wiring/mod.rs`**

Holds the handlers + startup. Move `type State` (`14`), `load_config_or_default` (`76-91`), `apply_startup` (`95-115`), `install_handlers` (`119-269`) **verbatim**. Declare `mod convert;` and import the converters.

```rust
//! UI callback wiring, kept out of `main` so the entry point stays a thin
//! orchestrator (build window → init DB → wire → run). Domain↔Slint mapping
//! lives in [`convert`]; this file installs the callbacks.

use crate::settings::Settings;
use crate::{hardware, network, settings};
use crate::{AppWindow, Theme};
use rusqlite::Connection;
use slint::{ComponentHandle, ModelRc, VecModel};
use std::cell::RefCell;
use std::rc::Rc;

mod convert;
use convert::{from_ui_config, to_net_status, to_ui_config, wifi_rows};

type State = Rc<RefCell<Settings>>;

// load_config_or_default (private fn), apply_startup (pub fn),
// install_handlers (pub fn) — bodies verbatim
```

Note: `NetConfigUi`, `NetStatus`, `WifiRow` are no longer named in `mod.rs` (they were only used by the moved converters), so they are dropped from `mod.rs`'s `use crate::{...}` — keep only `AppWindow, Theme`.

- [ ] **Step 3: Delete the old file**

```bash
git rm src/wiring.rs
```

- [ ] **Step 4: Build, format, test**

Run:
```bash
cargo build
cargo fmt
cargo test
```
Expected: build succeeds; `cargo fmt` no diff; `cargo test` ends with `test result: ok. 50 passed; 0 failed`. If the compiler warns about an unused import in `mod.rs`, remove that import (do not silence with `#[allow]`).

- [ ] **Step 5: Commit**

```bash
git add src/wiring/ src/main.rs
git commit -m "refactor(wiring): split into wiring/ with convert submodule

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Add module doc header to `network/linux.rs`

Closes the per-module-docs gap on the platform layer (parity with `windows/mod.rs`). Doc-comment only — no logic change. `linux.rs` compiles on Linux only (`#[cfg(target_os = "linux")]`), so on Windows the build is unaffected; a `//!` comment cannot break compilation.

**Files:**
- Modify: `src/network/linux.rs` (insert header above line 1)

- [ ] **Step 1: Insert the header**

Add at the very top of the file, before `use super::{...};`:

```rust
//! Linux network backend. Drives the OS through `nmcli` (NetworkManager):
//! status via device/Wi-Fi queries, config apply, and Wi-Fi scan/join. Mirrors
//! the [`super::windows`] backend behind the shared [`NetworkBackend`] trait.

```

- [ ] **Step 2: Verify build unaffected**

Run: `cargo build`
Expected: succeeds (no change to compiled-on-Windows code).

- [ ] **Step 3: Commit**

```bash
git add src/network/linux.rs
git commit -m "docs(network): add module header to linux backend

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Write lean root `CLAUDE.md`

The session-loaded navigation map. Keep it tight — it costs tokens every session, so it carries the map + commands + hard rules + pointers, and pushes depth to `docs/ARCHITECTURE.md` (written in Task 5).

**Files:**
- Create: `CLAUDE.md` (project root)

- [ ] **Step 1: Write `CLAUDE.md`**

```markdown
# Denso-DigitalReader

Desktop app (Rust + Slint) that reads a 4-digit 7-segment display and logs the
readings, with a settings UI for display resolution, theme, hardware spec, and
network configuration. Single SQLite store (`denso.db`) next to the executable.

## Commands

| Action | Command |
|---|---|
| Build | `cargo build` |
| Run | `cargo run` |
| Test | `cargo test` |
| Format | `cargo fmt` |
| Lint | `cargo clippy` |

Baseline: 50 tests pass on Windows (platform-gated tests differ on Linux).

## Source map (`src/`)

| Path | Responsibility |
|---|---|
| `main.rs` | Thin orchestrator: build window → open DB → migrate → reassert network → wire → run. |
| `wiring/mod.rs` | Installs all UI callbacks (settings persistence, network status/scan/connect). |
| `wiring/convert.rs` | The **only** Slint↔domain type boundary. |
| `db/mod.rs` | SQLite base: open `denso.db` in WAL mode. |
| `db/migrations.rs` | `user_version`-gated schema migrations (the one ordered chain). |
| `settings/` | Persisted app settings (window size, theme, fullscreen). `mod`=API, `model`=type, `repo`=persistence. |
| `hardware/` | Read-only host spec via `sysinfo` (collected fresh, not stored). |
| `reader/` | Append-only digit-reader log. |
| `network/mod.rs` | `NetworkBackend` trait + `reassert` (app owns config, pushes to OS at boot). |
| `network/model.rs`, `network/repo.rs` | Network domain types + config persistence. |
| `network/windows/` | Windows backend: `netsh` (config), `wifi` (scan/join), `parse` (status). |
| `network/linux.rs` | Linux backend (`nmcli`). |

## UI map (`ui/`, Slint)

| Path | Responsibility |
|---|---|
| `app-window.slint` | Root window (build entry point referenced by `build.rs`). |
| `theme.slint` | Global `Theme` + color palette. |
| `settings-modal.slint`, `camera-modal.slint` | Modal shells. |
| `settings/*.slint` | Tab panels: about, appearance, display, network, system. |
| `widgets/*.slint` | Reusable components (buttons, nav-item, spec-row, glyphs). |

## Hard rules

- Domain types **never** see Slint types. The only boundary is `wiring/convert.rs`.
- `main.rs` stays a thin orchestrator — no business logic.
- Each feature module is `mod.rs` (API) / `model.rs` (domain type) / `repo.rs` (persistence only). Access policy is the repo's API surface, not SQL grants.
- Persistence is one SQLite file with version-gated migrations in `db/migrations.rs`.
- OS-specific work sits behind the `NetworkBackend` trait (`network/windows/`, `network/linux.rs`). Keep both platforms in sync.
- `*.png` is git-ignored (see `.gitignore`).

## Workflow

Superpowers SDD: specs in `docs/superpowers/specs/`, plans in
`docs/superpowers/plans/`, progress ledger in `.superpowers/sdd/progress.md`.

See `docs/ARCHITECTURE.md` for data flow, persistence model, and gotchas.
```

- [ ] **Step 2: Verify referenced paths exist**

Run:
```bash
for p in src/main.rs src/wiring/mod.rs src/wiring/convert.rs src/db/migrations.rs src/network/windows/parse.rs src/network/linux.rs ui/app-window.slint ui/theme.slint docs/ARCHITECTURE.md .superpowers/sdd/progress.md; do test -e "$p" && echo "OK $p" || echo "MISSING $p"; done
```
Expected: every line `OK` **except** `docs/ARCHITECTURE.md` (created in Task 5 — acceptable here; re-check after Task 5). All `src/` and `ui/` paths must be `OK` (they depend on Tasks 1–3 being done first).

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: add lean CLAUDE.md navigation map

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Write `docs/ARCHITECTURE.md`

The on-demand depth doc, read only when an agent needs more than the map.

**Files:**
- Create: `docs/ARCHITECTURE.md`

- [ ] **Step 1: Write `docs/ARCHITECTURE.md`**

```markdown
# Architecture

Depth reference for Denso-DigitalReader. For the quick map and commands, see
the root `CLAUDE.md`.

## Boot sequence (`main.rs`)

1. `AppWindow::new()` builds the Slint window.
2. `db::open(default_path())` opens `denso.db` (next to the exe) in WAL mode;
   `db::run_migrations` applies the `user_version`-gated chain.
3. `settings::import_legacy` does a one-time import of any pre-SQLite
   `settings.json` sitting beside the DB.
4. `network::reassert` re-applies every saved interface config to the OS — the
   app is the source of truth. Best-effort and non-fatal: failures are logged,
   never block startup.
5. `settings::load` seeds the in-memory `Rc<RefCell<Settings>>`.
6. `wiring::apply_startup` populates read-only fields + applies settings;
   `wiring::install_handlers` wires callbacks. `app.run()`.

## UI ↔ domain boundary (`wiring/`)

Feature modules never reference Slint types. `wiring/convert.rs` is the single
crossing point: `to_*` build view models from domain types, `from_ui_config`
parses an editable view model back to a domain `NetConfig` (blank/unparseable
fields become `None`). `wiring/mod.rs` installs callbacks that call the
converters and persist through each feature's `repo`.

A config change travels: UI edit → callback in `wiring/mod.rs` →
`from_ui_config` → `network::repo::save` (persist; app owns truth) →
`backend().apply_config` (push to OS) → status string back to the UI.

## Persistence model (`db/`)

One file, `denso.db`, WAL mode so the UI reads while a background thread writes.
Schema lives entirely in `db/migrations.rs` as a single ordered, `user_version`
-gated chain — add a migration, never edit a shipped one. Each feature's `repo`
exposes only the operations its data policy allows (e.g. `reader` is
append-only; `hardware` is not stored at all). The `settings` table is a typed
key/value store; other tables are typed columns.

## Network feature (`network/`)

Two distinct datasets share the Network tab:
- **Live status** — `snapshot()` reads the OS (`ipconfig`/`netsh` on Windows,
  `nmcli` on Linux). Read-only, transient.
- **Config** — `NetConfig` is user-owned, persisted, and reasserted to the OS
  at boot via `reassert`.

OS work sits behind the `NetworkBackend` trait. `backend()` returns the
platform impl (`WindowsBackend`, `LinuxBackend`, or a `NullBackend` fallback).
The Windows backend is split into `netsh` (build + run config commands),
`wifi` (parse scans, build WLAN profile XML), and `parse` (status parsing);
the pure helpers are unit-tested off-device.

## Gotchas

- `edition = "2024"` requires Rust ≥ 1.85.
- Linux disk sum over-counts loop/tmpfs/overlay mounts; sub-GB renders "0 GB"
  (embedded MB-range accepted). Verify on a real Linux device.
- `nmcli -t` SSID escaping (`\:`) and VLAN device names (`eth0:0`) are not yet
  handled — deferred to on-device validation.
- Platform backend tests are `#[cfg(...)]`-gated, so the passing test count
  differs between Windows and Linux.
- `*.png` is git-ignored (`assets/icon.png` is embedded at build time by
  `build.rs`, not committed as a build output).
```

- [ ] **Step 2: Re-verify the CLAUDE.md path check now fully passes**

Run:
```bash
test -e docs/ARCHITECTURE.md && echo "OK docs/ARCHITECTURE.md"
```
Expected: `OK docs/ARCHITECTURE.md`.

- [ ] **Step 3: Commit**

```bash
git add docs/ARCHITECTURE.md
git commit -m "docs: add ARCHITECTURE.md depth reference

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification

- [ ] `cargo build` succeeds, `cargo test` reports `50 passed; 0 failed`, `cargo fmt` leaves no diff.
- [ ] `cargo clippy` produces no new warnings from the moved code.
- [ ] `src/network/windows.rs` and `src/wiring.rs` no longer exist; `src/network/windows/` and `src/wiring/` do.
- [ ] `CLAUDE.md` and `docs/ARCHITECTURE.md` exist; every path each references resolves.
- [ ] Update `.superpowers/sdd/progress.md` with the completed tasks and commit ranges.
```
