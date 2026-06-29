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
