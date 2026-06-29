# Network Settings — Slice 1: Tabbed Modal + Network Status — Design Spec

**Date:** 2026-06-29
**Project:** Denso-DigitalReader (Slint 1.17 + Rust)
**Status:** Approved

---

## Goal

Refactor the settings modal into a tabbed (two-pane) layout and add a
**Network** tab that displays read-only connection status for one Ethernet
and one Wi-Fi interface, using a cross-platform (Windows + Linux) backend
that shells out to OS commands. Status is fetched on a background thread
when the tab opens and via a Refresh button.

This is Slice 1 of the larger network feature. **No configuration** (static
IP, DHCP, Wi-Fi connect) is in scope here — those are Slices 2–3. Slice 1
establishes the tab layout, the data flow, and the platform-abstraction
trait that later slices extend.

## Scope

- Refactor `settings-modal.slint` into a shell: left **nav** + right
  **content pane** + shared footer. Tabs: Appearance, Display, System,
  Network, About.
- Split each section into its own file under `ui/settings/`.
- Add a `NavItem` component and an `active-tab` selector.
- Add the Network tab UI: Ethernet + Wi-Fi cards, Loading state, Refresh.
- Add a Rust `network` module: `NetworkBackend` trait, OS-specific
  command-parsing backends, `cfg`-selected `backend()`.
- Wire async refresh: `refresh-network()` → background thread →
  `upgrade_in_event_loop` updates UI properties.

## Out of Scope (later slices)

- Any write/configure: static IP, DHCP toggle, DNS, Wi-Fi connect/password.
- Listing every adapter (Slice 1 shows the primary Ethernet + primary Wi-Fi).
- Auto/timer refresh (manual + on-open only).
- Privilege elevation (read-only commands don't need it).

---

## Architecture

```
┌─ Slint UI ───────────────────────────────────────────────┐
│ SettingsModal shell: NavItem list + content switch + footer│
│ Network tab: eth card, wifi card, Loading, Refresh button │
│   props: eth: NetStatus, wifi: NetStatus, net-loading      │
│   callback: refresh-network()                              │
└───────────────┬───────────────────────────────────────────┘
                │ refresh-network()
┌───────────────▼─ Rust (main.rs) ──────────────────────────┐
│ on_refresh_network: set net-loading=true; spawn thread     │
│   thread: let snap = network::backend().snapshot();        │
│           weak.upgrade_in_event_loop(set eth/wifi/loading) │
└───────────────┬───────────────────────────────────────────┘
                │
┌───────────────▼─ network module ──────────────────────────┐
│ trait NetworkBackend { fn snapshot(&self)->NetworkSnapshot }│
│ #[cfg(windows)] WindowsBackend  (ipconfig, netsh wlan)     │
│ #[cfg(target_os="linux")] LinuxBackend (ip, nmcli)         │
│ pure parsers: fn parse_*(out: &str) -> ...  (unit-tested)  │
└────────────────────────────────────────────────────────────┘
```

---

## UI Design

### Tabbed shell (`ui/settings-modal.slint`)

The dialog (now ~640×460, still centered, elevated, fade-in) contains:

- **Header:** "Settings" title + gold underline + ✕ (unchanged).
- **Body:** `HorizontalLayout` — left `nav` column (~150px) of `NavItem`s,
  right content pane.
- **Footer:** Reset / Close / Apply (unchanged), spanning full width.

`active-tab` is an `int` property (0=Appearance … 4=About). Each content
pane is shown with `if active-tab == N : <SectionComponent> { ... }`.

```slint
// ui/settings/nav-item.slint
export component NavItem inherits Rectangle {
    in property <string> text;
    in property <bool> selected;
    callback clicked;
    height: 36px;
    border-radius: 6px;
    background: selected ? Theme.panel-3
              : touch.has-hover ? Theme.panel-3.with-alpha(0.5)
              : transparent;
    HorizontalLayout {
        padding-left: 12px;
        // gold left-edge bar when selected
        Rectangle { width: 3px; background: selected ? Theme.gold : transparent; }
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

Section files under `ui/settings/`: `appearance.slint`, `display.slint`,
`system.slint`, `network.slint`, `about.slint`. Each exports a component
taking the inputs/callbacks it needs (e.g. `display.slint` takes
`resolution-index`, `fullscreen`, the resolution/fullscreen callbacks).
The existing Appearance/Display/System/About content moves into these files
verbatim (re-homed, not rewritten). Shared bits (`Eyebrow`, `SpecRow`,
`TextButton`, `CloseGlyph`, `GoldButton`) move to
`ui/widgets/` so every section file can import them.

### Network tab (`ui/settings/network.slint`)

```slint
export struct NetStatus {
    connected: bool,
    ip: string,
    gateway: string,
    ssid: string,    // Wi-Fi only; "" for Ethernet
    signal: string,  // Wi-Fi only, e.g. "72%"
}
```

Component inputs: `eth: NetStatus`, `wifi: NetStatus`, `loading: bool`;
callback `refresh()`.

Layout: eyebrow "NETWORK", then two cards. A card shows a header row
(`Ethernet` / `Wi-Fi` + a Connected/Disconnected pill using
`Theme.status-ok` / `Theme.status-neutral`) and `SpecRow`s for IP, Gateway
(and SSID, Signal for Wi-Fi). When `loading`, the cards dim and a
"Loading…" line shows. A `TextButton` "Refresh" sits at the top-right of
the tab and calls `refresh()`.

Empty/disconnected values render `"—"`.

---

## Rust Design

### Module layout

```
src/network/mod.rs       — public types, trait, backend() selector, shared helpers
src/network/windows.rs   — #[cfg(windows)] WindowsBackend + parsers
src/network/linux.rs     — #[cfg(target_os = "linux")] LinuxBackend + parsers
```

### Types & trait (`src/network/mod.rs`)

```rust
#[derive(Default, Clone)]
pub struct InterfaceStatus {
    pub connected: bool,
    pub ip: String,       // "" when none
    pub gateway: String,  // "" when none
    pub ssid: String,     // Wi-Fi only
    pub signal: String,   // Wi-Fi only, e.g. "72%"
}

#[derive(Default, Clone)]
pub struct NetworkSnapshot {
    pub ethernet: InterfaceStatus,
    pub wifi: InterfaceStatus,
}

pub trait NetworkBackend {
    fn snapshot(&self) -> NetworkSnapshot;
}

pub fn backend() -> Box<dyn NetworkBackend> {
    #[cfg(windows)]
    { Box::new(windows::WindowsBackend) }
    #[cfg(target_os = "linux")]
    { Box::new(linux::LinuxBackend) }
    #[cfg(not(any(windows, target_os = "linux")))]
    { Box::new(NullBackend) } // returns Default snapshot on unsupported OS
}
```

A `NullBackend` returning `NetworkSnapshot::default()` keeps the app
compiling/running on any other platform.

### Backends are thin; parsers are pure and tested

Each backend method runs a command (`std::process::Command`), captures
stdout, and delegates to a **pure parser function** that takes `&str` and
returns the structured value. Only the parsers are unit-tested (with
captured real-world sample output); the command execution itself is not
asserted (environment-dependent).

Examples of parser signatures (exact set finalized in the plan):
- Windows: `fn parse_ipconfig(out: &str) -> (InterfaceStatus /*eth*/, InterfaceStatus /*wifi ip/gw*/)`,
  `fn parse_netsh_wlan(out: &str) -> (bool, String /*ssid*/, String /*signal*/)`
- Linux: `fn parse_nmcli_dev(out: &str) -> ...`, `fn parse_ip_route(out: &str) -> String /*gateway*/`

Commands used (read-only):
- Windows: `ipconfig`, `netsh wlan show interfaces`
- Linux: `nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device`,
  `nmcli -t -f IP4.ADDRESS,IP4.GATEWAY device show <dev>`,
  `nmcli -t -f ACTIVE,SSID,SIGNAL device wifi`

If a command is missing or fails, that interface is reported
disconnected with empty fields (never panics).

### Async wiring (`src/main.rs`)

```rust
mod network;

// in main(), after other setup:
let weak = app.as_weak();
app.on_refresh_network(move || {
    let Some(app) = weak.upgrade() else { return; };
    app.set_net_loading(true);
    let weak2 = app.as_weak();
    std::thread::spawn(move || {
        let snap = network::backend().snapshot();
        let _ = weak2.upgrade_in_event_loop(move |app| {
            app.set_eth(to_slint(&snap.ethernet, false));
            app.set_wifi(to_slint(&snap.wifi, true));
            app.set_net_loading(false);
        });
    });
});
```

`to_slint(&InterfaceStatus, is_wifi) -> NetStatus` maps the Rust struct to
the generated Slint struct (empty strings → `"—"` handled in UI or mapper).

The Network tab triggers an initial refresh when selected: the Network
`NavItem`'s click handler both sets `active-tab` and calls
`refresh-network()` (and the Refresh button calls it again on demand).

---

## Data Flow

1. User clicks the **Network** nav item → `active-tab = 3` and
   `refresh-network()` fires.
2. Rust sets `net-loading = true`, spawns a thread.
3. Thread runs OS commands, parses output into `NetworkSnapshot`.
4. `upgrade_in_event_loop` pushes `eth`/`wifi` structs, sets
   `net-loading = false`.
5. UI shows the two cards; Refresh repeats from step 2.

---

## Error Handling

- Missing/failing command → that interface `connected = false`, fields
  empty; UI shows `"—"`. No panics, no error dialogs in Slice 1.
- Unsupported OS → `NullBackend` yields an empty snapshot (both
  disconnected).

---

## Testing

- **Parser unit tests** (the core): feed captured sample stdout from
  `ipconfig`, `netsh wlan show interfaces`, and the `nmcli` commands;
  assert the extracted `InterfaceStatus` fields. Cover connected and
  disconnected samples, and a Wi-Fi-off sample.
- Existing 11 tests remain green.
- `cargo build` on Windows (and, where available, Linux) compiles all
  backends behind their `cfg`.
- Manual: open Network tab → cards populate within ~1s without freezing
  the UI; Refresh updates; toggling Wi-Fi off and refreshing shows
  disconnected.

---

## Verification

- `cargo test` passes (existing 11 + new parser tests).
- Settings modal shows a left nav with five tabs; switching is instant.
- Network tab shows Ethernet + Wi-Fi cards with real IP/gateway/SSID/signal
  on the dev machine; Loading appears briefly; Refresh works; UI never
  freezes during fetch.
- App still launches, themes, resizes, and shows System/About as before
  (content re-homed, not regressed).
