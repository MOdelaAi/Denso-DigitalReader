// Camera domain types. Ported 1:1 from Rust `camera::model`. As in the Rust
// tree these are defined but not yet wired into the app.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
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

    // Has the operator FINISHED the add wizard for this camera? Distinct from
    // `active` (enabled/disabled): the wizard must insert the row at the
    // Configure step because attaching models needs a real id, so a camera can
    // exist while its setup is still in progress. Only `active && setup_complete`
    // rows reach the runtime (camera::runtime) — an abandoned wizard leaves an
    // inert row the list shows as unfinished rather than a live, model-less
    // camera reporting nothing.
    //
    // Defaults FALSE while the DB column defaults TRUE, and the mismatch is
    // deliberate: the column's DEFAULT 1 grandfathers pre-existing rows on
    // upgrade, whereas a freshly constructed in-memory Camera is a NEW one, and
    // the fail-safe for a forgotten assignment is a dark camera (visible in the
    // list, recoverable) rather than a live half-configured one.
    bool setup_complete = false;

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

    // When true, the camera's ROI areas are quarantined pending operator review
    // after a view-significant source/geometry change: they are excluded from
    // detection ROI-filtering and zone reporting is PAUSED until the operator
    // re-verifies them against a live preview (Areas step → "Verify & save").
    bool areas_need_review = false;
};

/// A single polygon vertex, normalized to the frame: x and y are fractions in
/// [0, 1] of the (oriented) image width/height, so an area is resolution-
/// independent and lands on the same spot at any display or capture size.
struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

/// The reporting-zone number namespace: every legal zone id is an integer in
/// [kMinZone, kMaxZone] inclusive, and that closed range IS the whole namespace.
///
/// There is exactly ONE representation of "this area reports nothing": a
/// disengaged `std::optional<int>` (SQL NULL on disk). The number 0 is NOT it.
/// 0 is outside the namespace entirely — refused by the UI, by
/// `replace_areas` and by `save_level_configuration` alike.
///
/// Up to schema v16, a `camera_area.zone` of 0 WAS a second spelling of "not
/// reported", alongside NULL. Schema v17 normalises those legacy rows to NULL
/// once, at migration time, which is why no runtime code special-cases zero:
/// after the migration the invariant is simply NULL = unassigned, 1..99 =
/// assigned, and a 0 reaching a write chokepoint is a bug to be refused, not a
/// state to be interpreted.
///
/// It lives HERE, in core, rather than in the digit Areas page where it began,
/// because it is not a digit-reader fact: zone numbers are unique machine-wide
/// across every camera AND both operating modes (the brazing payload keys by
/// zone number alone and carries no camera identity). The Ball Leveler allocates
/// out of this same namespace, so a second copy of this bound in the Ball path
/// would be a second zone-numbering authority — the one thing the multi-zone
/// amendment forbids.
///
/// Note what this is NOT: it is not a per-camera cap. A digit camera has no
/// limit on how many zones it owns; a Ball camera is capped at
/// level::kMaxBallZones, which is a Ball rule enforced in the Ball write
/// chokepoint, not here.
inline constexpr int kMinZone = 1;
inline constexpr int kMaxZone = 99;

/// The ONE range predicate. Every write chokepoint and every validator asks
/// this rather than re-spelling the bound, so there is a single place where the
/// namespace can ever widen again.
inline constexpr bool zone_in_range(int zone) {
    return zone >= kMinZone && zone <= kMaxZone;
}

/// The next zone an automatic allocator should hand out given `used`, or nullopt
/// when the namespace is exhausted.
///
/// Plain ascending scan from kMinZone. There is no separate "preferred" order to
/// keep: the lowest legal zone is also the one a fresh machine should start at,
/// so validity and preference are the same fact and stay one constant. This is
/// the ONE automatic-allocation policy — both modes allocate out of one
/// machine-wide namespace, so a second ordering elsewhere would be a second
/// authority.
///
/// "Nothing free" is nullopt, never a number: every int this can return is a
/// zone the caller may legally persist, so a caller cannot mistake exhaustion
/// for an allocation.
inline std::optional<int> next_free_zone(const std::set<int>& used) {
    for (int z = kMinZone; z <= kMaxZone; ++z) {
        if (used.count(z) == 0) return z;
    }
    return std::nullopt;
}

/// One ROI area belonging to a camera. A camera can have many areas. The area
/// is a closed polygon of 3+ vertices (triangle, rectangle, …); the closing
/// edge from the last vertex back to the first is implicit, never stored.
struct CameraArea {
    int64_t id = 0;
    int64_t camera_id = 0;  // FK → camera.id
    std::string name;
    // kMinZone..kMaxZone reporting zone, or nullopt (ROI-only, not reported).
    // nullopt is the ONLY "not reported" value; 0 is not a zone number at all.
    std::optional<int> zone;
    // Where the decimal point sits among the reader's four digit positions:
    // 0 = 0000, 1 = 000.0, 2 = 00.00, 3 = 0.000. Per AREA, because one camera
    // can own several zones reading different instruments through one model.
    // 0 is the pre-v16 behaviour, so an untouched area reports as it always did.
    // Ball Leveler zones do not use this - their value is a whole percent.
    int decimal_places = 0;
    std::vector<Point> points;
};

// ─── Runtime (transient — never stored) ──────────────────────────────────────

/// Camera with its areas loaded — used for passing to the UI. An empty `areas`
/// is the no-area case.
struct CameraWithAreas {
    Camera camera;
    std::vector<CameraArea> areas;
};

} // namespace denso::camera
