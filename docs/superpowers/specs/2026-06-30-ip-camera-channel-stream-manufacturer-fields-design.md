# IP Camera: Channel + structured stream/manufacturer fields

**Date:** 2026-06-30
**Status:** Approved design — ready for implementation plan

## Problem

The Add IP Camera form lets the user pick Manufacturer, Stream (main/sub), IP,
Username and Password, then builds a credential-free RTSP URL from the vendor
template. But the Dahua template **hardcodes `channel=1`**:

```
rtsp://%1:554/cam/realmonitor?channel=1&subtype=0
```

So a camera behind an NVR/DVR (or any non-channel-1 stream) cannot be addressed
— the form has no Channel input. We forgot the channel.

A second, related gap: the structured inputs that *build* the RTSP URL are only
partially persisted. We store `ip`, `username`, `password` as their own columns
alongside the generated `rtsp` string, but **manufacturer** and **stream
(main/sub)** are baked into the URL only. A future edit-camera flow would have
to reverse-parse the URL to repopulate the form.

## Goals

1. Add a **Channel** input to the Add IP Camera form and inject it into the RTSP
   URL.
2. Persist the *full* set of structured RTSP inputs so the form can be
   reconstructed later without parsing the URL: add `channel`, `stream`, and
   `manufacturer` columns.

Non-goals: building the edit-camera flow itself; OS secret store; adding more
vendors. Storing the structured fields is the forward-looking prep, not the
edit UI.

## Design

### 1. Schema migration (`src/core/db/db.cpp`)

New migration block, mirroring the v5 `password` precedent (one column per
`ALTER`; SQLite has no multi-add):

```cpp
if (version < 6) {
    // Structured RTSP inputs persisted alongside the generated `rtsp` URL, so a
    // future edit flow can repopulate the form without parsing the URL. All
    // nullable — USB cameras have none. `channel` addresses NVR/DVR streams;
    // `stream` is 0=main / 1=sub; `manufacturer` is the vendor name.
    if (!run("ALTER TABLE camera ADD COLUMN channel INTEGER")) return false;
    if (!run("ALTER TABLE camera ADD COLUMN stream INTEGER")) return false;
    if (!run("ALTER TABLE camera ADD COLUMN manufacturer TEXT")) return false;
}
```

Bump `SCHEMA_VERSION` from `5` to `6`.

### 2. Model (`src/core/camera/model.h`)

Add to the IP-only section of `struct Camera`:

```cpp
std::optional<uint32_t> channel;       // IP only; NVR/DVR channel number (1-based)
std::optional<uint32_t> stream;        // IP only; 0 = main stream, 1 = sub stream
std::optional<std::string> manufacturer;  // IP only; vendor name, e.g. "Dahua"
```

These reuse the existing `bind_uint`/`col_uint` and `bind_str`/`col_str`
helpers cleanly.

### 3. Repo (`src/core/camera/repo.cpp`)

- Append `channel, stream, manufacturer` to the `COLUMNS` string (after
  `password`), keeping read order in `from_row`.
- Append the three columns + placeholders to the INSERT and UPDATE statements.
- In `bind_fields`, bind `bind_uint(c.channel)`, `bind_uint(c.stream)`,
  `bind_str(c.manufacturer)` (in COLUMNS order, after `password`).
- In `from_row`, read `c.channel = col_uint(q.value(15))`,
  `c.stream = col_uint(q.value(16))`, `c.manufacturer = col_str(q.value(17))`.

### 4. RTSP templates (`src/app/ui/camera/rtsp_templates.{h,cpp}`)

Make channel a placeholder (`%2`) and thread it through `build_rtsp`:

```cpp
// templates — %1 = IP, %2 = channel
{QStringLiteral("Dahua"),
 QStringLiteral("rtsp://%1:554/cam/realmonitor?channel=%2&subtype=0"),
 QStringLiteral("rtsp://%1:554/cam/realmonitor?channel=%2&subtype=1")},

QString build_rtsp(const RtspManufacturer& m, const QString& ip,
                   int channel, bool substream) {
    return (substream ? m.sub_template : m.main_template).arg(ip).arg(channel);
}
```

`.arg(ip)` fills `%1`; the following `.arg(channel)` fills `%2`. Update the
header comment that currently says only "%1 = IP" to document `%2 = channel`.

### 5. Add form (`src/app/ui/camera/camera_dialog.{h,cpp}`)

- New member `QSpinBox* channel_spin_ = nullptr;` (forward-declare `QSpinBox`).
- Build it after the **Stream** combo via the existing `field(...)` helper,
  labelled **"Channel"**, range **1–255**, default **1**.
- Connect `channel_spin_`'s `valueChanged` → `update_rtsp_preview()`.
- `update_rtsp_preview()` passes `channel_spin_->value()` to `build_rtsp`.
- `show_add()` resets `channel_spin_->setValue(1)`.
- `save_new_camera()` (IP branch) sets the structured fields and uses the
  channel when building the URL:

```cpp
c.manufacturer = m.name.toStdString();
c.stream = static_cast<uint32_t>(stream_combo_->currentIndex());   // 0=main,1=sub
c.channel = static_cast<uint32_t>(channel_spin_->value());
c.rtsp = build_rtsp(m, ip, channel_spin_->value(),
                    stream_combo_->currentIndex() == 1).toStdString();
```

## Testing

### Repo round-trip (`tests/test_camera_repo.cpp`)

Extend the IP-camera coverage: set `channel`, `stream`, `manufacturer` on an IP
camera, `insert`, then `get`/`all` and assert all three round-trip. Assert they
stay `nullopt` for a USB camera.

### `build_rtsp` channel injection (`tests/test_rtsp_templates.cpp`, new)

`rtsp_templates.cpp` depends only on `QString` (QtCore, already linked), not Qt
Widgets, so it can be compiled directly into the test target. In
`tests/CMakeLists.txt` add the source and the `src/app` include dir to
`denso_tests`:

```cmake
add_executable(denso_tests
    ...
    test_camera_repo.cpp
    test_rtsp_templates.cpp
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/rtsp_templates.cpp
)
target_include_directories(denso_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
```

Assert:
- `build_rtsp(Dahua, "192.168.1.20", 3, false)` →
  `rtsp://192.168.1.20:554/cam/realmonitor?channel=3&subtype=0`
- substream=true → `subtype=1`
- channel 1 still produces `channel=1` (no regression).

## Migration safety

Existing rows (USB and pre-v6 IP cameras) get `NULL` in the three new columns —
read back as `nullopt`, which is correct: their `rtsp` URL is already complete
and capture continues to use it. No data backfill needed.
