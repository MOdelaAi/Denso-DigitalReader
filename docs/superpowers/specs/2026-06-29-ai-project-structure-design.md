# AI-Friendly Project Structure — Design

**Date:** 2026-06-29
**Goal:** Restructure the Denso-DigitalReader project so an AI agent can do work
efficiently with minimal token usage — chiefly by eliminating per-session
re-exploration of the codebase, and by keeping individual files small enough to
hold in context cleanly.

All artifacts live **inside the project folder** (`Denso-DigitalReader/`).
Nothing is written outside it.

---

## Problem

The codebase is already well-organized (small files, clear feature modules, good
`//!` headers on domain modules). The inefficiency is not the code — it is that
**every agent session re-discovers the layout from scratch** (listing `src/`,
reading `main.rs` + `wiring.rs`, grepping for structure). That repeated
reconnaissance is the main recurring token sink. A secondary issue: a couple of
files are large enough that an agent must read the whole file to make a small
edit.

## Principles

1. **`CLAUDE.md` is loaded every session, so it must stay lean.** A bloated
   `CLAUDE.md` would *add* recurring token cost. Keep it to a tight map +
   commands + hard rules + pointers. Push depth into an on-demand doc.
2. **Detail lives in `docs/ARCHITECTURE.md`, read only when needed.**
3. **Smaller files = cheaper, more reliable edits.** Split only the genuinely
   large file(s); do not churn well-sized code.
4. **Document existing conventions; do not invent new ones.** The structure
   already encodes good rules — write them down so agents follow without
   re-deriving.

---

## Deliverables

### 1. `CLAUDE.md` (project root) — lean, ~80 lines

Auto-loaded into context each session. Contents:

- **One-line description** of the app.
- **Commands** — build / run / test / format / lint (`cargo build`,
  `cargo run`, `cargo test`, `cargo fmt`, `cargo clippy`).
- **Module map** — a compact table, one row per `src/` module:
  `module → one-line purpose`. No prose.
- **Hard rules** (the conventions that prevent mistakes):
  - Domain types never see Slint types; the **only** boundary is `wiring.rs`
    (and its `convert` submodule).
  - `main.rs` stays a thin orchestrator (build window → open DB → migrate →
    wire → run).
  - Each feature module follows `mod.rs` (API) / `model.rs` (domain type) /
    `repo.rs` (persistence only).
  - Persistence is one SQLite file (`denso.db`) with version-gated migrations
    in `db/migrations.rs`; access policy is by repo API surface, not SQL grants.
  - OS-specific work sits behind the `NetworkBackend` trait, split into
    `network/linux.rs` and `network/windows/`.
- **UI/Slint map** — one line per `.slint` file (entry point
  `ui/app-window.slint`).
- **Workflow pointer** — superpowers SDD flow: specs in
  `docs/superpowers/specs/`, plans in `docs/superpowers/plans/`, ledger in
  `.superpowers/sdd/progress.md`.
- **Pointer** to `docs/ARCHITECTURE.md` for depth.

### 2. `docs/ARCHITECTURE.md` (on-demand) — detailed map

Read only when an agent needs depth. Contents:

- **Data flow** — boot sequence (`main.rs`), the `wiring.rs` UI↔domain
  boundary, how a settings/network change travels UI → wiring → repo → DB → OS.
- **Persistence model** — the `denso.db` single-file store, WAL mode, the
  `user_version`-gated migration chain, the key/value `settings` table vs typed
  tables.
- **Network feature** — the status-vs-config split (`snapshot` is live
  read-only; `NetConfig` is user-owned and reasserted to the OS at boot via
  `reassert`), and the platform-backend trait.
- **Per-module deep notes** — anything not obvious from the `//!` header.
- **Gotchas** (migrated from the SDD ledger so they survive):
  - `edition = "2024"` requires Rust ≥ 1.85.
  - Linux disk sum over-counts loop/tmpfs/overlay mounts; sub-GB renders
    "0 GB" (embedded MB-range accepted).
  - `nmcli -t` SSID escaping (`\:`) and VLAN device names (`eth0:0`) not yet
    handled — deferred to on-device validation.
  - `*.png` is git-ignored (note for assets).

### 3. Split `network/windows.rs` (487L) → `network/windows/` directory

Current single file groups cleanly into four seams. Convert the file into a
directory module:

| New file | Contents (moved verbatim) |
|---|---|
| `network/windows/mod.rs` | `//!` header + `WindowsBackend` struct & `NetworkBackend` impl; declares the submodules |
| `network/windows/netsh.rs` | `adapter_name`, `prefix_to_mask`, `build_netsh_commands`, `run_checked`, `run` |
| `network/windows/wifi.rs` | `parse_wifi_networks`, `xml_escape`, `build_profile_xml` |
| `network/windows/parse.rs` | `value_after_colon`, `parse_ipconfig`, `parse_netsh_wlan`, `build_snapshot` |

- Functions move verbatim; visibility adjusted to `pub(super)`/`pub(crate)` as
  needed across the submodule boundary.
- Existing `#[cfg(test)]` unit tests move next to the functions they cover.
- `network/mod.rs` already does `#[cfg(windows)] mod windows;` — unchanged; a
  directory module resolves identically.
- Add a `//!` header to each new file (closes the per-module-docs gap for the
  platform layer).

### 4. Split `wiring.rs` (296L) → `wiring/` directory

| New file | Contents |
|---|---|
| `wiring/mod.rs` | `//!` header + `apply_startup`, `install_handlers`, `load_config_or_default` |
| `wiring/convert.rs` | Slint↔domain converters: `to_net_status`, `to_ui_config`, `from_ui_config`, `wifi_rows` |

- Isolates the Slint↔domain mapping (the boundary the hard rules describe).
- `main.rs` already does `mod wiring;` — unchanged.

### 5. Add `//!` headers to `network/linux.rs`

Currently starts with `use`; add a one-paragraph header matching the
`windows/mod.rs` style for parity.

---

## Non-goals (YAGNI)

- No splitting of files already < ~200 lines.
- No changes to runtime behavior, the DB schema, or the UI.
- No new dependencies.
- No unrelated refactoring.

---

## Verification

- After each code split: `cargo build` **and** `cargo test` must pass
  (same test count as before — the splits move tests, they don't add/remove
  behavior). `cargo fmt` clean.
- Docs deliverables: self-review for accuracy against the actual tree; every
  path and command referenced must exist / run.
- The full change is committed via the SDD workflow with the progress ledger
  updated.

## Success criterion

A fresh agent session can answer "where does X live and how do I build/test"
from `CLAUDE.md` alone, without listing `src/` or reading `main.rs` — and can
edit Windows network logic by opening one ~120-line file instead of a 487-line
one.
