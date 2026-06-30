// Boundary converters between the feature modules' domain types
// (`network::{InterfaceStatus,NetConfig,WifiNetwork}`) and the UI view-model
// types (`ui::{NetStatus,NetConfigUi,WifiRow}`). Feature modules never see the
// view types — this is the only place the two worlds meet. Ported 1:1 from the
// Rust `wiring/convert.rs`; SharedString conversions collapse to plain string
// copies here.
#pragma once

#include "network/model.h"
#include "ui/viewmodel.h"

#include <string>
#include <vector>

namespace denso::ui {

/// Live interface status → view status.
NetStatus to_net_status(const network::InterfaceStatus& s);

/// Domain config → editable view model (empty string for unset fields).
NetConfigUi to_ui_config(const network::NetConfig& c);

/// Editable view model → domain config. Blank fields become unset; an
/// unparseable prefix becomes unset (apply will reject a static config that
/// then lacks a prefix). Wi-Fi join fields are out of scope here.
network::NetConfig from_ui_config(const std::string& iface, const NetConfigUi& u);

/// Scanned networks → view-model rows: flag the currently-connected SSID and
/// sort it to the top (stable, so the rest keep their scan order).
std::vector<WifiRow> wifi_rows(const std::vector<network::WifiNetwork>& nets,
                               const std::string& current_ssid);

} // namespace denso::ui
