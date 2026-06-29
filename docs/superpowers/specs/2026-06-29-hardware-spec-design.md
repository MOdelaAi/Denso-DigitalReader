# Hardware Spec (System Info) — Design Spec

**Date:** 2026-06-29
**Project:** Denso-DigitalReader (Slint 1.17 + Rust)
**Status:** Approved

---

## Goal

Show a read-only **SYSTEM** section in the Settings modal with the host's
hardware spec: OS, Device (hostname), RAM (total), Storage (total). Values
are collected once at startup and displayed; no live refresh.

## Scope

- Add the `sysinfo` crate (cross-platform: Windows + Linux).
- New Rust module `src/hardware.rs`: `HardwareSpec` struct, `collect()`,
  and a `format_bytes` helper, with unit tests for formatting.
- `main.rs` collects the spec at startup and pushes four strings to the UI.
- New SYSTEM section in `ui/settings-modal.slint`, four read-only rows.

## Out of Scope

- Live/periodic refresh (values are static for the session).
- RAM/Storage usage (used/free) — concise totals only.
- CPU model, core count, per-disk breakdown.
- Board/model name for "Device" (uses hostname; model lookup is a future
  enhancement needing OS-specific paths).
- The sidebar settings layout (deferred to the Network slice).

---

## Field Sources (via `sysinfo`)

| UI label | Value | sysinfo source | Format |
|----------|-------|----------------|--------|
| OS | name + version | `System::name()` + `System::os_version()` | `"Windows 10 Pro"` / `"Ubuntu 22.04"`; if either is `None`, show the available part, else `"Unknown"` |
| Device | hostname | `System::host_name()` | raw string; `"Unknown"` if `None` |
| RAM | total memory | `System::total_memory()` (bytes) | `format_bytes` → `"16 GB"` |
| Storage | total disk capacity | sum of `Disks::total_space()` across disks (bytes) | `format_bytes` → `"512 GB"` |

`format_bytes(bytes: u64) -> String`: converts to the largest sensible unit
(GB or TB), rounded to a whole number for GB and one decimal for TB.
Examples: `16_000_000_000 → "16 GB"`, `512_110_190_592 → "512 GB"`,
`2_000_000_000_000 → "2.0 TB"`.

> Note: `sysinfo` reports memory in bytes (v0.30+). The plan must pin the
> crate version and use the byte-based API current at implementation time.

---

## Rust Design

### `src/hardware.rs`

```rust
pub struct HardwareSpec {
    pub os: String,
    pub device: String,
    pub ram: String,
    pub storage: String,
}

pub fn collect() -> HardwareSpec { /* uses sysinfo */ }

fn format_bytes(bytes: u64) -> String { /* GB / TB */ }
```

- `collect()` builds a `sysinfo::System`, refreshes only what's needed
  (memory; OS/host info), sums disk totals via `sysinfo::Disks`, and
  returns formatted strings. Missing values fall back to `"Unknown"`.
- Pure formatting (`format_bytes`) is unit-tested; `collect()` itself is
  environment-dependent and not asserted on exact values.

### `Cargo.toml`

Add to `[dependencies]`:
```toml
sysinfo = "<pinned in plan>"
```

### `main.rs`

At startup (after `AppWindow::new()`):
```rust
mod hardware;
let hw = hardware::collect();
app.set_hw_os(hw.os.into());
app.set_hw_device(hw.device.into());
app.set_hw_ram(hw.ram.into());
app.set_hw_storage(hw.storage.into());
```

---

## UI Design (`ui/settings-modal.slint`)

New `AppWindow` properties (forwarded into `SettingsModal`):
```slint
in property <string> hw-os;
in property <string> hw-device;
in property <string> hw-ram;
in property <string> hw-storage;
```

New SYSTEM section, placed between DISPLAY and ABOUT, following the existing
section pattern (eyebrow + rows). Each row: label left (`Theme.txt-dim`),
value right (`Theme.txt`):

```slint
VerticalLayout {
    spacing: 8px;
    Eyebrow { text: "SYSTEM"; }
    // one SpecRow per field
}
```

A small `SpecRow` component (label + right-aligned value) avoids repeating
the HorizontalLayout four times:
```slint
component SpecRow inherits HorizontalLayout {
    in property <string> label;
    in property <string> value;
    Text { text: label; color: Theme.txt-dim; horizontal-stretch: 1; vertical-alignment: center; }
    Text { text: value; color: Theme.txt; vertical-alignment: center; }
}
```

Rows: `OS / hw-os`, `Device / hw-device`, `RAM / hw-ram`, `Storage / hw-storage`.

---

## Behavior

| Action | Result |
|--------|--------|
| Launch app | `hardware::collect()` runs once; SYSTEM rows populated |
| Open Settings → SYSTEM | Shows OS, Device, RAM, Storage |
| Value unavailable | Shows `"Unknown"` (never blank/crash) |

---

## Verification

- `cargo build` passes; `cargo test` passes (existing 5 + new format_bytes tests).
- Modal SYSTEM section shows four populated rows on the dev machine.
- `format_bytes` unit tests cover GB, TB, and a rounding case.
