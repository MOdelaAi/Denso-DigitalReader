// Camera domain types. Ported 1:1 from Rust `camera::model`. As in the Rust
// tree these are defined but not yet wired into the app.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace denso::camera {

// ─── DB (persisted) ───────────────────────────────────────────────────────────

/// One camera entry. `camera_type` drives which optional fields are used.
struct Camera {
    int64_t id = 0;
    std::string name;
    std::string camera_type;  // "usb" | "ip"
    bool active = false;

    // USB only
    std::optional<uint32_t> index;

    // IP only. `rtsp` is the credential-free stream URL built from the
    // manufacturer template; `username`/`password` are injected at capture
    // time. (Plaintext in the local DB for now; OS secret store is later
    // hardening.)
    std::optional<std::string> ip;
    std::optional<std::string> rtsp;
    std::optional<std::string> username;
    std::optional<std::string> password;
    std::optional<uint32_t> channel;          // NVR/DVR channel number (1-based)
    std::optional<uint32_t> stream;           // 0 = main stream, 1 = sub stream
    std::optional<std::string> manufacturer;  // vendor name, e.g. "Dahua"

    // Capture
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;

    // Angle
    float pitch = 0.0f;
    float roll = 0.0f;
    uint32_t rotation = 0;  // 0 | 90 | 180 | 270
};

/// One ROI area belonging to a camera. A camera can have many areas.
struct CameraArea {
    int64_t id = 0;
    int64_t camera_id = 0;  // FK → camera.id
    std::string name;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
};

// ─── Runtime (transient — never stored) ──────────────────────────────────────

/// Camera with its areas loaded — used for passing to the UI. An empty `areas`
/// is the no-area case.
struct CameraWithAreas {
    Camera camera;
    std::vector<CameraArea> areas;
};

} // namespace denso::camera
