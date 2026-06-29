# Hardware Spec (System Info) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show a read-only SYSTEM section in the Settings modal with OS, Device (hostname), RAM total, and Storage total, collected once at startup.

**Architecture:** Slint declares four `hw-*` string properties and renders a SYSTEM section with a reusable `SpecRow` (Task 1). A new `src/hardware.rs` module uses the `sysinfo` crate to collect formatted strings, which `main.rs` pushes into those properties at startup (Task 2).

**Tech Stack:** Rust, Slint 1.17, slint-build 1.17, serde 1, serde_json 1, sysinfo 0.33

**Task order rationale:** Slint properties first so the generated `set_hw_*` setters exist; then the Rust task can call them and build cleanly. Each task builds and is reviewable on its own.

## Global Constraints

- Slint + slint-build: exactly `"1.17.0"`; serde `{ version = "1", features = ["derive"] }`; serde_json `"1"`
- `sysinfo = "0.33"` (byte-based memory API)
- Cross-platform: must compile on Windows and Linux (no OS-specific code in this slice)
- Missing values render as `"Unknown"` — never blank or panic
- Concise/static: collected once at startup, no refresh
- UI rows: label left `Theme.txt-dim`, value right `Theme.txt`

---

### Task 1: SYSTEM section UI (Slint)

**Files:**
- Modify: `ui/app-window.slint`
- Modify: `ui/settings-modal.slint`

**Interfaces:**
- Produces on `AppWindow`: `in property <string> hw-os`, `hw-device`, `hw-ram`, `hw-storage` (default empty), which generate Rust setters `set_hw_os` / `set_hw_device` / `set_hw_ram` / `set_hw_storage` consumed by Task 2.
- Produces on `SettingsModal`: the same four `in property <string>` inputs, plus a `SpecRow` component.

- [ ] **Step 1: Add hw properties to `AppWindow` and forward them**

In `ui/app-window.slint`, add these properties immediately after the existing `in property <string> app-version;`:

```slint
    in property <string> hw-os;
    in property <string> hw-device;
    in property <string> hw-ram;
    in property <string> hw-storage;
```

In the `if root.settings-open : SettingsModal { ... }` block, add these forwards next to the existing `app-version: root.app-version;`:

```slint
        hw-os: root.hw-os;
        hw-device: root.hw-device;
        hw-ram: root.hw-ram;
        hw-storage: root.hw-storage;
```

- [ ] **Step 2: Add hw properties and a `SpecRow` component to `SettingsModal`**

In `ui/settings-modal.slint`, add the four input properties immediately after the existing `in property <string> app-version;`:

```slint
    in property <string> hw-os;
    in property <string> hw-device;
    in property <string> hw-ram;
    in property <string> hw-storage;
```

Add a `SpecRow` component right after the `Eyebrow` component definition:

```slint
// One labelled read-only spec row (label left, value right).
component SpecRow inherits HorizontalLayout {
    in property <string> label;
    in property <string> value;
    Text {
        text: root.label;
        color: Theme.txt-dim;
        vertical-alignment: center;
        horizontal-stretch: 1;
    }
    Text {
        text: root.value;
        color: Theme.txt;
        vertical-alignment: center;
    }
}
```

- [ ] **Step 3: Insert the SYSTEM section between DISPLAY and ABOUT**

In `ui/settings-modal.slint`, between the Display section's closing `}` and the `// ── About ──` comment, add:

```slint
            // ── System ──────────────────────────────
            VerticalLayout {
                spacing: 8px;

                Eyebrow { text: "SYSTEM"; }
                SpecRow { label: "OS"; value: root.hw-os; }
                SpecRow { label: "Device"; value: root.hw-device; }
                SpecRow { label: "RAM"; value: root.hw-ram; }
                SpecRow { label: "Storage"; value: root.hw-storage; }
            }
```

- [ ] **Step 4: Build to verify Slint compiles**

Run: `cargo build`
Expected: `Finished dev profile` — no errors. The SYSTEM rows will show empty values until Task 2 populates them.

- [ ] **Step 5: Commit**

```bash
git add ui/app-window.slint ui/settings-modal.slint
git commit -m "feat: add SYSTEM hardware spec section (empty until wired)"
```

---

### Task 2: Hardware collection module (Rust)

**Files:**
- Modify: `Cargo.toml`
- Create: `src/hardware.rs`
- Modify: `src/main.rs`

**Interfaces:**
- Consumes from Task 1: `AppWindow` setters `set_hw_os` / `set_hw_device` / `set_hw_ram` / `set_hw_storage` (each takes `SharedString` via `.into()`).
- Produces: `pub struct HardwareSpec { pub os: String, pub device: String, pub ram: String, pub storage: String }` and `pub fn collect() -> HardwareSpec`.

- [ ] **Step 1: Add `sysinfo` to `Cargo.toml`**

In `[dependencies]`, add `sysinfo` so the section reads:

```toml
[dependencies]
slint = "1.17.0"
serde = { version = "1", features = ["derive"] }
serde_json = "1"
sysinfo = "0.33"
```

- [ ] **Step 2: Create `src/hardware.rs` with `format_bytes` + failing tests**

Create `src/hardware.rs`:

```rust
fn format_bytes(_bytes: u64) -> String {
    String::new()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn formats_gigabytes_whole() {
        assert_eq!(format_bytes(16_000_000_000), "16 GB");
    }

    #[test]
    fn formats_gigabytes_rounded() {
        assert_eq!(format_bytes(512_110_190_592), "512 GB");
    }

    #[test]
    fn formats_terabytes_one_decimal() {
        assert_eq!(format_bytes(2_000_000_000_000), "2.0 TB");
    }

    #[test]
    fn formats_sub_gigabyte_as_zero_gb() {
        assert_eq!(format_bytes(500_000_000), "0 GB");
    }
}
```

Register the module in `src/main.rs` by adding `mod hardware;` next to `mod settings;`:

```rust
mod settings;
mod hardware;
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cargo test hardware::tests`
Expected: FAIL — all four assertions fail because `format_bytes` returns `""`.

- [ ] **Step 4: Implement `format_bytes`**

Replace the stub in `src/hardware.rs`:

```rust
fn format_bytes(bytes: u64) -> String {
    const GB: u64 = 1_000_000_000;
    const TB: u64 = 1_000_000_000_000;
    if bytes >= TB {
        format!("{:.1} TB", bytes as f64 / TB as f64)
    } else {
        format!("{} GB", bytes / GB)
    }
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cargo test hardware::tests`
Expected: PASS — 4 passed.

- [ ] **Step 6: Implement `HardwareSpec` and `collect()`**

Add to `src/hardware.rs` above the test module (and above or below `format_bytes`):

```rust
use sysinfo::{Disks, System};

pub struct HardwareSpec {
    pub os: String,
    pub device: String,
    pub ram: String,
    pub storage: String,
}

pub fn collect() -> HardwareSpec {
    let mut sys = System::new();
    sys.refresh_memory();

    let os = match (System::name(), System::os_version()) {
        (Some(n), Some(v)) => format!("{n} {v}"),
        (Some(n), None) => n,
        (None, Some(v)) => v,
        (None, None) => "Unknown".to_string(),
    };

    let device = System::host_name().unwrap_or_else(|| "Unknown".to_string());

    let ram = format_bytes(sys.total_memory());

    let disks = Disks::new_with_refreshed_list();
    let total_storage: u64 = disks.list().iter().map(|d| d.total_space()).sum();
    let storage = format_bytes(total_storage);

    HardwareSpec { os, device, ram, storage }
}
```

- [ ] **Step 7: Wire startup in `main.rs`**

In `main()`, immediately after the existing `app.set_app_version(...)` line, add:

```rust
    let hw = hardware::collect();
    app.set_hw_os(hw.os.into());
    app.set_hw_device(hw.device.into());
    app.set_hw_ram(hw.ram.into());
    app.set_hw_storage(hw.storage.into());
```

- [ ] **Step 8: Build and run all tests**

Run: `cargo build` then `cargo test`
Expected: `cargo build` finishes clean; `cargo test` → 9 passed (5 settings + 4 hardware).

- [ ] **Step 9: Run and visually verify**

Run: `cargo run`
Expected:
- Open Settings → SYSTEM section between DISPLAY and ABOUT
- Real values: OS (e.g. "Windows 10 Pro"), Device (hostname), RAM (e.g. "16 GB"), Storage (e.g. "512 GB")
- No blank rows; unavailable values read "Unknown"

- [ ] **Step 10: Commit**

```bash
git add Cargo.toml src/hardware.rs src/main.rs
git commit -m "feat: collect hardware spec via sysinfo and populate SYSTEM section"
```
