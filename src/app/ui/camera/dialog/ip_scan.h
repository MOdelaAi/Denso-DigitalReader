// Crude IP-camera discovery: probe the local /24 subnet(s) for hosts with the
// RTSP port open. Returns the responsive IPs (no names/credentials — that's the
// trade-off vs ONVIF). Blocking + parallel: meant to run on a worker thread
// (it spins a local event loop), so call it off the GUI thread like the Wi-Fi
// scan. Uses Qt6::Network.
#pragma once

#include <QString>

#include <cstdint>
#include <vector>

namespace denso::ui {

/// Responsive hosts (as IPv4 strings) with `port` open on the local /24(s),
/// giving up after `deadline_ms`.
std::vector<QString> scan_rtsp_subnet(uint16_t port = 554, int deadline_ms = 3000);

} // namespace denso::ui
