# Network Slice 1: Tabbed Modal + Network Status — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor the settings modal into a tabbed two-pane layout and add a read-only Network tab showing Ethernet + Wi-Fi status, backed by a cross-platform command-parsing Rust module, fetched asynchronously.

**Architecture:** Slint sections become per-file components hosted by a tabbed shell (`active-tab` int). A `network` Rust module exposes a `NetworkBackend` trait with `cfg`-selected Windows/Linux backends whose pure parsers are unit-tested. `main.rs` handles `refresh-network()` by spawning a thread and pushing results back via `upgrade_in_event_loop`.

**Tech Stack:** Rust, Slint 1.17, slint-build 1.17, serde 1, serde_json 1, sysinfo 0.33

## Global Constraints

- Slint + slint-build: exactly `"1.17.0"`; no new runtime crates (network uses `std::process::Command`)
- Read-only this slice: no config/write, no privilege elevation
- Cross-platform: Windows + Linux backends behind `#[cfg]`; a `NullBackend` keeps other OSes compiling
- Missing/failed command → that interface `connected=false`, empty fields; never panic
- One primary Ethernet + one primary Wi-Fi card (not every adapter)
- Async: command execution never runs on the UI thread
- UI rows reuse existing pattern: label `Theme.txt-dim`, value `Theme.txt`; empty value renders `"—"`

---

### Task 1: Extract sections + shared widgets into files

Pure refactor — **no layout or behavior change**. The modal stays single-column; this only moves existing markup into focused files so later tasks can compose them.

**Files:**
- Create: `ui/widgets/eyebrow.slint`, `ui/widgets/spec-row.slint`, `ui/widgets/text-button.slint`, `ui/widgets/close-glyph.slint`
- Create: `ui/settings/appearance.slint`, `ui/settings/display.slint`, `ui/settings/system.slint`, `ui/settings/about.slint`
- Modify: `ui/settings-modal.slint` (import + compose the extracted components)

**Interfaces:**
- Produces shared widgets (exported): `Eyebrow`, `SpecRow`, `TextButton`, `CloseGlyph` (GoldButton already lives in `ui/widgets/gold-button.slint`).
- Produces section components (exported), each taking exactly the inputs/callbacks its content uses:
  - `Appearance` — uses the global `Theme` + `theme-changed(bool)` callback
  - `Display { in-out resolution-index: int; in-out fullscreen: bool; callback apply-resolution(int); callback toggle-fullscreen(bool); }`
  - `System { in os: string; in device: string; in ram: string; in storage: string; }`
  - `About { in app-version: string; }`

- [ ] **Step 1: Move shared components into `ui/widgets/`**

For each of `Eyebrow`, `SpecRow`, `TextButton`, `CloseGlyph`: create the named file under `ui/widgets/`, add `import { Theme } from "../theme.slint";` at the top, and move the component's existing definition from `ui/settings-modal.slint` into it **verbatim**, prefixing the component keyword with `export`. Example for `ui/widgets/eyebrow.slint`:

```slint
import { Theme } from "../theme.slint";

export component Eyebrow inherits Text {
    font-size: 11px;
    font-weight: 600;
    letter-spacing: 1.5px;
    color: Theme.txt-faint;
}
```

Do the same for `SpecRow` (→ `spec-row.slint`), `TextButton` (→ `text-button.slint`), `CloseGlyph` (→ `close-glyph.slint`), copying their current bodies from `settings-modal.slint` unchanged except for the `export` keyword and the Theme import.

- [ ] **Step 2: Create each section component file**

Create `ui/settings/appearance.slint`:

```slint
import { Switch } from "std-widgets.slint";
import { Theme } from "../theme.slint";
import { Eyebrow } from "../widgets/eyebrow.slint";

export component Appearance inherits VerticalLayout {
    callback theme-changed(bool);
    spacing: 10px;

    Eyebrow { text: "APPEARANCE"; }
    HorizontalLayout {
        Text {
            text: "Dark mode";
            color: Theme.txt;
            vertical-alignment: center;
            horizontal-stretch: 1;
        }
        Switch {
            text: "";
            checked <=> Theme.dark;
            toggled => { root.theme-changed(Theme.dark); }
        }
    }
}
```

Create `ui/settings/display.slint`:

```slint
import { ComboBox, Switch } from "std-widgets.slint";
import { Theme } from "../theme.slint";
import { Eyebrow } from "../widgets/eyebrow.slint";

export component Display inherits VerticalLayout {
    in-out property <int> resolution-index;
    in-out property <bool> fullscreen;
    callback apply-resolution(int);
    callback toggle-fullscreen(bool);
    spacing: 12px;

    Eyebrow { text: "DISPLAY"; }
    VerticalLayout {
        spacing: 6px;
        Text { text: "Resolution"; color: Theme.txt-dim; }
        ComboBox {
            model: ["800 × 600", "1280 × 720", "1600 × 900", "1920 × 1080"];
            current-index <=> root.resolution-index;
        }
    }
    HorizontalLayout {
        Text {
            text: "Fullscreen";
            color: Theme.txt;
            vertical-alignment: center;
            horizontal-stretch: 1;
        }
        Switch {
            text: "";
            checked <=> root.fullscreen;
            toggled => { root.toggle-fullscreen(root.fullscreen); }
        }
    }
}
```

Create `ui/settings/system.slint`:

```slint
import { Theme } from "../theme.slint";
import { Eyebrow } from "../widgets/eyebrow.slint";
import { SpecRow } from "../widgets/spec-row.slint";

export component System inherits VerticalLayout {
    in property <string> os;
    in property <string> device;
    in property <string> ram;
    in property <string> storage;
    spacing: 8px;

    Eyebrow { text: "SYSTEM"; }
    SpecRow { label: "OS"; value: root.os; }
    SpecRow { label: "Device"; value: root.device; }
    SpecRow { label: "RAM"; value: root.ram; }
    SpecRow { label: "Storage"; value: root.storage; }
}
```

Create `ui/settings/about.slint`:

```slint
import { Theme } from "../theme.slint";
import { Eyebrow } from "../widgets/eyebrow.slint";

export component About inherits VerticalLayout {
    in property <string> app-version;
    spacing: 6px;

    Eyebrow { text: "ABOUT"; }
    HorizontalLayout {
        Text {
            text: "Denso Digital Reader";
            color: Theme.txt-dim;
            vertical-alignment: center;
            horizontal-stretch: 1;
        }
        Text {
            text: "v\{root.app-version}";
            color: Theme.txt-faint;
            vertical-alignment: center;
        }
    }
}
```

- [ ] **Step 3: Rewrite `settings-modal.slint` to import and compose**

Replace the inline component definitions and the four inline section blocks. Keep the same dialog shell (centered, elevated, fade-in), the same header (now importing `CloseGlyph`), and footer (importing `TextButton`, `GoldButton`). The body keeps the **same single-column order** for now: Appearance, Display, System, About. Imports at top:

```slint
import { Button } from "std-widgets.slint";
import { Theme } from "theme.slint";
import { GoldButton } from "widgets/gold-button.slint";
import { CloseGlyph } from "widgets/close-glyph.slint";
import { TextButton } from "widgets/text-button.slint";
import { Appearance } from "settings/appearance.slint";
import { Display } from "settings/display.slint";
import { System } from "settings/system.slint";
import { About } from "settings/about.slint";
```

In the content `VerticalLayout` (padding 24, spacing 22), replace the four old section blocks with:

```slint
            Appearance {
                theme-changed(d) => { root.theme-changed(d); }
            }
            Display {
                resolution-index <=> root.resolution-index;
                fullscreen <=> root.fullscreen;
                apply-resolution(i) => { root.apply-resolution(i); }
                toggle-fullscreen(f) => { root.toggle-fullscreen(f); }
            }
            System {
                os: root.hw-os;
                device: root.hw-device;
                ram: root.hw-ram;
                storage: root.hw-storage;
            }
            About { app-version: root.app-version; }
```

Keep all existing `SettingsModal` properties/callbacks (`resolution-index`, `fullscreen`, `app-version`, `hw-*`, `apply-resolution`, `theme-changed`, `toggle-fullscreen`, `reset-defaults`, `close`) unchanged.

- [ ] **Step 4: Build**

Run: `cargo build`
Expected: `Finished dev profile` — no errors. The modal looks and behaves exactly as before (single column).

- [ ] **Step 5: Commit**

```bash
git add ui/widgets ui/settings ui/settings-modal.slint
git commit -m "refactor: extract settings sections and shared widgets into files"
```

---

### Task 2: Tabbed two-pane shell

Convert the single-column body into a left-nav + right-content layout.

**Files:**
- Create: `ui/widgets/nav-item.slint`
- Modify: `ui/settings-modal.slint`

**Interfaces:**
- Consumes: the section components from Task 1.
- Produces: a `NavItem` component; `SettingsModal` gains `property <int> active-tab: 0;`.

- [ ] **Step 1: Create `ui/widgets/nav-item.slint`**

```slint
import { Theme } from "../theme.slint";

export component NavItem inherits Rectangle {
    in property <string> text;
    in property <bool> selected;
    callback clicked;
    height: 36px;
    border-radius: 6px;
    background: selected ? Theme.panel-3
              : touch.has-hover ? Theme.panel-3.with-alpha(0.5)
              : transparent;
    animate background { duration: 120ms; easing: ease-out; }

    HorizontalLayout {
        spacing: 8px;
        Rectangle {
            width: 3px;
            background: selected ? Theme.gold : transparent;
        }
        Text {
            text: root.text;
            color: selected ? Theme.gold : Theme.txt-dim;
            vertical-alignment: center;
            horizontal-stretch: 1;
        }
    }
    touch := TouchArea { clicked => { root.clicked(); } }
}
```

- [ ] **Step 2: Widen the dialog and split body into nav + content**

In `ui/settings-modal.slint`: import `NavItem` (`import { NavItem } from "widgets/nav-item.slint";`), add `property <int> active-tab: 0;` to `SettingsModal`, and change the dialog `width: 384px;` to `width: 640px;`.

Replace the content `VerticalLayout` (the one holding the four sections, between Header and Footer) with a `HorizontalLayout` containing the nav and a content pane:

```slint
                HorizontalLayout {
                    spacing: 20px;

                    // Left nav
                    VerticalLayout {
                        width: 150px;
                        spacing: 4px;
                        NavItem { text: "Appearance"; selected: root.active-tab == 0; clicked => { root.active-tab = 0; } }
                        NavItem { text: "Display";    selected: root.active-tab == 1; clicked => { root.active-tab = 1; } }
                        NavItem { text: "System";     selected: root.active-tab == 2; clicked => { root.active-tab = 2; } }
                        NavItem { text: "Network";    selected: root.active-tab == 3; clicked => { root.active-tab = 3; } }
                        NavItem { text: "About";      selected: root.active-tab == 4; clicked => { root.active-tab = 4; } }
                    }

                    // Right content pane
                    VerticalLayout {
                        horizontal-stretch: 1;
                        if root.active-tab == 0 : Appearance {
                            theme-changed(d) => { root.theme-changed(d); }
                        }
                        if root.active-tab == 1 : Display {
                            resolution-index <=> root.resolution-index;
                            fullscreen <=> root.fullscreen;
                            apply-resolution(i) => { root.apply-resolution(i); }
                            toggle-fullscreen(f) => { root.toggle-fullscreen(f); }
                        }
                        if root.active-tab == 2 : System {
                            os: root.hw-os; device: root.hw-device; ram: root.hw-ram; storage: root.hw-storage;
                        }
                        if root.active-tab == 4 : About { app-version: root.app-version; }
                    }
                }
```

(Network — tab 3 — is added in Task 3.) The overall dialog `VerticalLayout` (padding 24, spacing 22) now contains: Header, this `HorizontalLayout`, Footer.

- [ ] **Step 3: Build and verify**

Run: `cargo build` then `cargo run`
Expected: build clean. The modal is now ~640px wide with a left nav of five items; clicking switches the right pane. Appearance/Display/System/About work as before. Network shows nothing yet (Task 3).

- [ ] **Step 4: Commit**

```bash
git add ui/widgets/nav-item.slint ui/settings-modal.slint
git commit -m "feat: convert settings modal to tabbed two-pane layout"
```

---

### Task 3: Network tab UI

**Files:**
- Create: `ui/settings/network.slint`
- Modify: `ui/settings-modal.slint`, `ui/app-window.slint`

**Interfaces:**
- Produces: exported `struct NetStatus { connected: bool, ip: string, gateway: string, ssid: string, signal: string }`; a `Network` component with `in property <NetStatus> eth`, `in property <NetStatus> wifi`, `in property <bool> loading`, `callback refresh()`.
- Produces on `AppWindow` (for Task 4): `in property <NetStatus> eth`, `in property <NetStatus> wifi`, `in property <bool> net-loading`, `callback refresh-network()` — generating Rust `set_eth`, `set_wifi`, `set_net_loading`, `on_refresh_network`.

- [ ] **Step 1: Create `ui/settings/network.slint`**

```slint
import { Theme } from "../theme.slint";
import { Eyebrow } from "../widgets/eyebrow.slint";
import { SpecRow } from "../widgets/spec-row.slint";
import { TextButton } from "../widgets/text-button.slint";

export struct NetStatus {
    connected: bool,
    ip: string,
    gateway: string,
    ssid: string,
    signal: string,
}

// Connection card for one interface.
component NetCard inherits Rectangle {
    in property <string> title;
    in property <NetStatus> status;
    in property <bool> is-wifi;
    background: Theme.panel-3;
    border-radius: 8px;
    VerticalLayout {
        padding: 12px;
        spacing: 6px;
        HorizontalLayout {
            Text {
                text: root.title;
                color: Theme.txt;
                font-weight: 600;
                horizontal-stretch: 1;
                vertical-alignment: center;
            }
            Text {
                text: root.status.connected ? "Connected" : "Disconnected";
                color: root.status.connected ? Theme.status-ok : Theme.status-neutral;
                font-size: 12px;
                vertical-alignment: center;
            }
        }
        if root.is-wifi : SpecRow { label: "SSID"; value: root.status.ssid == "" ? "—" : root.status.ssid; }
        if root.is-wifi : SpecRow { label: "Signal"; value: root.status.signal == "" ? "—" : root.status.signal; }
        SpecRow { label: "IP"; value: root.status.ip == "" ? "—" : root.status.ip; }
        SpecRow { label: "Gateway"; value: root.status.gateway == "" ? "—" : root.status.gateway; }
    }
}

export component Network inherits VerticalLayout {
    in property <NetStatus> eth;
    in property <NetStatus> wifi;
    in property <bool> loading;
    callback refresh();
    spacing: 12px;

    HorizontalLayout {
        Eyebrow { text: "NETWORK"; vertical-alignment: center; horizontal-stretch: 1; }
        TextButton { text: root.loading ? "Loading…" : "Refresh"; clicked => { root.refresh(); } }
    }
    NetCard { title: "Ethernet"; status: root.eth; is-wifi: false; opacity: root.loading ? 0.5 : 1.0; }
    NetCard { title: "Wi-Fi"; status: root.wifi; is-wifi: true; opacity: root.loading ? 0.5 : 1.0; }
}
```

- [ ] **Step 2: Wire the Network tab into `settings-modal.slint`**

Import the `Network` component and `NetStatus` struct:

```slint
import { Network, NetStatus } from "settings/network.slint";
```

Add these properties/callback to `SettingsModal` (next to the existing `hw-*` properties):

```slint
    in property <NetStatus> eth;
    in property <NetStatus> wifi;
    in property <bool> net-loading;
    callback refresh-network();
```

In the content pane, add the tab-3 branch (next to the other `if root.active-tab` branches):

```slint
                        if root.active-tab == 3 : Network {
                            eth: root.eth;
                            wifi: root.wifi;
                            loading: root.net-loading;
                            refresh => { root.refresh-network(); }
                        }
```

In the Network `NavItem`'s click handler, also trigger an initial fetch — change it to:

```slint
                        NavItem { text: "Network"; selected: root.active-tab == 3; clicked => { root.active-tab = 3; root.refresh-network(); } }
```

- [ ] **Step 3: Expose the properties on `AppWindow` and forward them**

In `ui/app-window.slint`, import the struct and add properties/callback:

```slint
import { NetStatus } from "settings/network.slint";
```
```slint
    in property <NetStatus> eth;
    in property <NetStatus> wifi;
    in property <bool> net-loading;
    callback refresh-network();
```

Forward into the `SettingsModal` instance (next to the `hw-*` forwards):

```slint
        eth: root.eth;
        wifi: root.wifi;
        net-loading: root.net-loading;
        refresh-network => { root.refresh-network(); }
```

- [ ] **Step 4: Build**

Run: `cargo build`
Expected: clean. The `refresh-network` callback is an unimplemented no-op stub until Task 4; clicking Network shows two cards with `"—"` values and a "Disconnected" pill.

- [ ] **Step 5: Commit**

```bash
git add ui/settings/network.slint ui/settings-modal.slint ui/app-window.slint
git commit -m "feat: add Network tab UI (status cards, refresh) — unwired"
```

---

### Task 4: Rust network module + async wiring

**Files:**
- Create: `src/network/mod.rs`, `src/network/windows.rs`, `src/network/linux.rs`
- Modify: `src/main.rs`

**Interfaces:**
- Consumes from Task 3: `AppWindow` generated API — `set_eth(NetStatus)`, `set_wifi(NetStatus)`, `set_net_loading(bool)`, `on_refresh_network(...)`, and the generated `NetStatus` struct (in scope via `slint::include_modules!()`).
- Produces: `network::backend() -> Box<dyn NetworkBackend>`, `NetworkBackend::snapshot(&self) -> NetworkSnapshot`, `NetworkSnapshot { ethernet, wifi: InterfaceStatus }`.

- [ ] **Step 1: Create `src/network/mod.rs` with types, trait, selector, and a value test**

```rust
#[cfg(windows)]
mod windows;
#[cfg(target_os = "linux")]
mod linux;

#[derive(Default, Clone, PartialEq, Debug)]
pub struct InterfaceStatus {
    pub connected: bool,
    pub ip: String,
    pub gateway: String,
    pub ssid: String,
    pub signal: String,
}

#[derive(Default, Clone, Debug)]
pub struct NetworkSnapshot {
    pub ethernet: InterfaceStatus,
    pub wifi: InterfaceStatus,
}

pub trait NetworkBackend {
    fn snapshot(&self) -> NetworkSnapshot;
}

struct NullBackend;
impl NetworkBackend for NullBackend {
    fn snapshot(&self) -> NetworkSnapshot {
        NetworkSnapshot::default()
    }
}

pub fn backend() -> Box<dyn NetworkBackend> {
    #[cfg(windows)]
    {
        return Box::new(windows::WindowsBackend);
    }
    #[cfg(target_os = "linux")]
    {
        return Box::new(linux::LinuxBackend);
    }
    #[allow(unreachable_code)]
    {
        Box::new(NullBackend)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn null_backend_is_all_disconnected() {
        let snap = NullBackend.snapshot();
        assert!(!snap.ethernet.connected);
        assert!(!snap.wifi.connected);
        assert_eq!(snap.ethernet.ip, "");
    }
}
```

- [ ] **Step 2: Register the module and run the test**

Add `mod network;` to `src/main.rs` next to the other `mod` lines. Run: `cargo test network::tests`
Expected: PASS (1 test). On Windows the `linux` module is `cfg`-excluded and vice-versa.

- [ ] **Step 3: Create `src/network/windows.rs` with failing parser tests**

```rust
use super::{InterfaceStatus, NetworkBackend, NetworkSnapshot};

pub struct WindowsBackend;

impl NetworkBackend for WindowsBackend {
    fn snapshot(&self) -> NetworkSnapshot {
        let ipcfg = run("ipconfig", &[]);
        let wlan = run("netsh", &["wlan", "show", "interfaces"]);
        build_snapshot(&ipcfg, &wlan)
    }
}

fn run(cmd: &str, args: &[&str]) -> String {
    std::process::Command::new(cmd)
        .args(args)
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .unwrap_or_default()
}

/// Value after the last ':' on a line, trimmed; strips a trailing "(Preferred)".
fn value_after_colon(line: &str) -> String {
    match line.rsplit_once(':') {
        Some((_, v)) => v.trim().replace("(Preferred)", "").trim().to_string(),
        None => String::new(),
    }
}

/// Parse `ipconfig` into (ethernet, wifi) IP/gateway. Adapter headers start at
/// column 0; their fields are indented. Wi-Fi headers contain "Wireless" or
/// "Wi-Fi"; wired headers contain "Ethernet".
fn parse_ipconfig(out: &str) -> (InterfaceStatus, InterfaceStatus) {
    let mut eth = InterfaceStatus::default();
    let mut wifi = InterfaceStatus::default();
    let mut target: Option<bool> = None; // Some(true)=wifi, Some(false)=eth
    for line in out.lines() {
        if !line.starts_with([' ', '\t']) && line.contains("adapter") {
            let h = line.to_lowercase();
            target = if h.contains("wireless") || h.contains("wi-fi") {
                Some(true)
            } else if h.contains("ethernet") {
                Some(false)
            } else {
                None
            };
            continue;
        }
        let Some(is_wifi) = target else { continue };
        let slot = if is_wifi { &mut wifi } else { &mut eth };
        let l = line.trim();
        if l.starts_with("IPv4 Address") {
            slot.ip = value_after_colon(line);
            slot.connected = !slot.ip.is_empty();
        } else if l.starts_with("Default Gateway") {
            let g = value_after_colon(line);
            if !g.is_empty() {
                slot.gateway = g;
            }
        }
    }
    (eth, wifi)
}

/// Parse `netsh wlan show interfaces` for (connected, ssid, signal).
fn parse_netsh_wlan(out: &str) -> (bool, String, String) {
    let mut connected = false;
    let mut ssid = String::new();
    let mut signal = String::new();
    for line in out.lines() {
        let l = line.trim();
        if l.starts_with("State") {
            connected = value_after_colon(line).eq_ignore_ascii_case("connected");
        } else if l.starts_with("SSID") && !l.starts_with("BSSID") {
            ssid = value_after_colon(line);
        } else if l.starts_with("Signal") {
            signal = value_after_colon(line);
        }
    }
    (connected, ssid, signal)
}

fn build_snapshot(ipcfg: &str, wlan: &str) -> NetworkSnapshot {
    let (eth, mut wifi) = parse_ipconfig(ipcfg);
    let (wconn, ssid, signal) = parse_netsh_wlan(wlan);
    wifi.connected = wifi.connected || wconn;
    wifi.ssid = ssid;
    wifi.signal = signal;
    NetworkSnapshot { ethernet: eth, wifi }
}

#[cfg(test)]
mod tests {
    use super::*;

    const IPCONFIG: &str = "\
Windows IP Configuration

Ethernet adapter Ethernet:

   IPv4 Address. . . . . . . . . . . : 192.168.1.50(Preferred)
   Subnet Mask . . . . . . . . . . . : 255.255.255.0
   Default Gateway . . . . . . . . . : 192.168.1.1

Wireless LAN adapter Wi-Fi:

   IPv4 Address. . . . . . . . . . . : 192.168.1.77(Preferred)
   Default Gateway . . . . . . . . . : 192.168.1.1
";

    const NETSH: &str = "\
There is 1 interface on the system:

    Name                   : Wi-Fi
    State                  : connected
    SSID                   : MyNetwork
    Signal                 : 72%
";

    #[test]
    fn ipconfig_extracts_eth_and_wifi() {
        let (eth, wifi) = parse_ipconfig(IPCONFIG);
        assert_eq!(eth.ip, "192.168.1.50");
        assert_eq!(eth.gateway, "192.168.1.1");
        assert!(eth.connected);
        assert_eq!(wifi.ip, "192.168.1.77");
    }

    #[test]
    fn netsh_extracts_ssid_signal_state() {
        let (connected, ssid, signal) = parse_netsh_wlan(NETSH);
        assert!(connected);
        assert_eq!(ssid, "MyNetwork");
        assert_eq!(signal, "72%");
    }

    #[test]
    fn build_snapshot_merges_wifi() {
        let snap = build_snapshot(IPCONFIG, NETSH);
        assert_eq!(snap.ethernet.ip, "192.168.1.50");
        assert_eq!(snap.wifi.ssid, "MyNetwork");
        assert_eq!(snap.wifi.signal, "72%");
        assert!(snap.wifi.connected);
    }
}
```

Run: `cargo test network` (on Windows). Expected: PASS (these tests + the mod test). On Linux this file is `cfg`-excluded — see Step 4.

- [ ] **Step 4: Create `src/network/linux.rs` with parser tests**

```rust
use super::{InterfaceStatus, NetworkBackend, NetworkSnapshot};

pub struct LinuxBackend;

impl NetworkBackend for LinuxBackend {
    fn snapshot(&self) -> NetworkSnapshot {
        let dev = run("nmcli", &["-t", "-f", "DEVICE,TYPE,STATE", "device"]);
        let (eth_dev, wifi_dev) = pick_devices(&dev);

        let mut ethernet = InterfaceStatus::default();
        if let Some(d) = eth_dev {
            ethernet = device_ip(&d);
        }
        let mut wifi = InterfaceStatus::default();
        if let Some(d) = wifi_dev {
            wifi = device_ip(&d);
            let w = run("nmcli", &["-t", "-f", "ACTIVE,SSID,SIGNAL", "device", "wifi"]);
            let (ssid, signal) = parse_wifi(&w);
            wifi.ssid = ssid;
            wifi.signal = signal;
        }
        NetworkSnapshot { ethernet, wifi }
    }
}

fn run(cmd: &str, args: &[&str]) -> String {
    std::process::Command::new(cmd)
        .args(args)
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .unwrap_or_default()
}

fn device_ip(dev: &str) -> InterfaceStatus {
    let out = run(
        "nmcli",
        &["-t", "-f", "IP4.ADDRESS,IP4.GATEWAY", "device", "show", dev],
    );
    let (ip, gateway) = parse_device_show(&out);
    InterfaceStatus {
        connected: !ip.is_empty(),
        ip,
        gateway,
        ..Default::default()
    }
}

/// From `nmcli -t -f DEVICE,TYPE,STATE device`, return the first connected
/// (device, _) for ethernet and wifi respectively.
fn pick_devices(out: &str) -> (Option<String>, Option<String>) {
    let mut eth = None;
    let mut wifi = None;
    for line in out.lines() {
        let f: Vec<&str> = line.split(':').collect();
        if f.len() < 3 {
            continue;
        }
        let (dev, ty, state) = (f[0], f[1], f[2]);
        let connected = state == "connected";
        if ty == "ethernet" && eth.is_none() && connected {
            eth = Some(dev.to_string());
        } else if ty == "wifi" && wifi.is_none() && connected {
            wifi = Some(dev.to_string());
        }
    }
    (eth, wifi)
}

/// From `nmcli -t -f IP4.ADDRESS,IP4.GATEWAY device show <dev>`, return
/// (ip-without-prefix, gateway).
fn parse_device_show(out: &str) -> (String, String) {
    let mut ip = String::new();
    let mut gateway = String::new();
    for line in out.lines() {
        if let Some((key, val)) = line.split_once(':') {
            if key.starts_with("IP4.ADDRESS") && ip.is_empty() {
                ip = val.split('/').next().unwrap_or("").trim().to_string();
            } else if key.starts_with("IP4.GATEWAY") {
                gateway = val.trim().to_string();
            }
        }
    }
    (ip, gateway)
}

/// From `nmcli -t -f ACTIVE,SSID,SIGNAL device wifi`, return the active
/// (ssid, "NN%").
fn parse_wifi(out: &str) -> (String, String) {
    for line in out.lines() {
        let f: Vec<&str> = line.split(':').collect();
        if f.len() >= 3 && f[0] == "yes" {
            return (f[1].to_string(), format!("{}%", f[2]));
        }
    }
    (String::new(), String::new())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn picks_first_connected_devices() {
        let out = "eth0:ethernet:connected\nwlan0:wifi:connected\nlo:loopback:unmanaged\n";
        let (eth, wifi) = pick_devices(out);
        assert_eq!(eth.as_deref(), Some("eth0"));
        assert_eq!(wifi.as_deref(), Some("wlan0"));
    }

    #[test]
    fn parses_device_show_ip_gateway() {
        let out = "IP4.ADDRESS[1]:192.168.1.50/24\nIP4.GATEWAY:192.168.1.1\n";
        let (ip, gw) = parse_device_show(out);
        assert_eq!(ip, "192.168.1.50");
        assert_eq!(gw, "192.168.1.1");
    }

    #[test]
    fn parses_active_wifi() {
        let out = "no:OtherNet:40\nyes:MyNetwork:72\n";
        let (ssid, signal) = parse_wifi(out);
        assert_eq!(ssid, "MyNetwork");
        assert_eq!(signal, "72%");
    }
}
```

- [ ] **Step 5: Wire async refresh into `src/main.rs`**

Add a mapper near the top of `main.rs` (after the `mod` lines):

```rust
fn to_net_status(s: &network::InterfaceStatus) -> NetStatus {
    NetStatus {
        connected: s.connected,
        ip: s.ip.clone().into(),
        gateway: s.gateway.clone().into(),
        ssid: s.ssid.clone().into(),
        signal: s.signal.clone().into(),
    }
}
```

In `main()`, after the existing callback wiring, add:

```rust
    let weak = app.as_weak();
    app.on_refresh_network(move || {
        let Some(app) = weak.upgrade() else { return; };
        app.set_net_loading(true);
        let weak2 = app.as_weak();
        std::thread::spawn(move || {
            let snap = network::backend().snapshot();
            let _ = weak2.upgrade_in_event_loop(move |app| {
                app.set_eth(to_net_status(&snap.ethernet));
                app.set_wifi(to_net_status(&snap.wifi));
                app.set_net_loading(false);
            });
        });
    });
```

- [ ] **Step 6: Build and run all tests**

Run: `cargo build` then `cargo test`
Expected: build clean (no warnings); tests = existing 11 + network mod (1) + platform parser tests (Windows: 3, Linux: 3 — only the host platform's run). On Windows: 15 passed.

- [ ] **Step 7: Run and verify manually**

Run: `cargo run`
Expected:
- Open Settings → click **Network** → "Loading…" briefly, then Ethernet + Wi-Fi cards populate with real IP/Gateway (and SSID/Signal for Wi-Fi) on the dev machine.
- UI does not freeze during fetch.
- **Refresh** re-fetches. Disconnecting Wi-Fi and refreshing shows it Disconnected with `"—"`.

- [ ] **Step 8: Commit**

```bash
git add src/network src/main.rs
git commit -m "feat: cross-platform network status backend + async refresh wiring"
```
