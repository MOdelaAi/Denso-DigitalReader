# Camera Management Slice 3 — Configure Step Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Configure step to the Camera modal — capture a live OpenCV snapshot and set resolution / fps / rotation / pitch / roll — reachable from the add wizard and from existing rows (doubles as edit).

**Architecture:** A new OpenCV-free `snapshot.{h,cpp}` app-side helper grabs one frame (`grab_snapshot`) and rotates it for preview (`apply_rotation`); a sibling `frame_convert.h` exposes the testable `mat_to_qimage`. A third page is added to `CameraDialog`'s existing `QStackedWidget`, driven by mode state (`editing_id_` + `draft_`). OpenCV is linked into the `denso` app only — `denso_core` stays Qt6::Sql-only.

**Tech Stack:** C++/Qt6 (Widgets, Gui, Sql), OpenCV (`cv::VideoCapture`, `cv::cvtColor`), CMake + Ninja, Catch2 v3. Toolchain: MSYS2 UCRT64.

## Global Constraints

- Toolchain is **MSYS2 UCRT64**. Every build/test runs with `export PATH=/c/msys64/ucrt64/bin:$PATH` first. Reconfigure (`cmake -S . -B build -G Ninja`) only after editing a `CMakeLists.txt`; otherwise just `cmake --build build`.
- **`denso_core` stays Qt6::Sql-only.** OpenCV and the snapshot unit are app-side (mirrors how Qt Multimedia / camera_devices is app-side).
- **No schema change.** The `camera` table + `Camera` struct already carry `width`, `height`, `fps`, `pitch`, `roll`, `rotation`.
- Files are snake_case; UI code lives in namespace `denso::ui` under `src/app/ui/camera/`.
- `snapshot.h` must **not** include any OpenCV header (keeps cv out of the dialog's include graph). The cv type lives in `frame_convert.h`, included only by `snapshot.cpp` and the test.

---

## File Structure

- `CMakeLists.txt` (root) — modify: `find_package(OpenCV REQUIRED)`.
- `src/app/CMakeLists.txt` — modify: add `snapshot.cpp`, OpenCV include dirs + libs to `denso`.
- `tests/CMakeLists.txt` — modify: add `test_snapshot.cpp` + `snapshot.cpp`, link OpenCV + `Qt6::Gui`.
- `src/app/ui/camera/frame_convert.h` — create: `mat_to_qimage(const cv::Mat&)` (cv in header; cpp-side include only).
- `src/app/ui/camera/snapshot.h` — create: OpenCV-free public API (`Snapshot`, `grab_snapshot`, `apply_rotation`).
- `src/app/ui/camera/snapshot.cpp` — create: implements all three.
- `tests/test_snapshot.cpp` — create: pure tests for `mat_to_qimage` + `apply_rotation`.
- `src/app/ui/camera/camera_dialog.{h,cpp}` — modify: Configure page, controls, capture wiring, flow + edit integration.

---

### Task 1: OpenCV wiring + frame conversion + rotation (pure, TDD)

**Files:**
- Modify: `CMakeLists.txt` (root, the `find_package(Qt6 ...)` area)
- Modify: `src/app/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Create: `src/app/ui/camera/frame_convert.h`
- Create: `src/app/ui/camera/snapshot.h`
- Create: `src/app/ui/camera/snapshot.cpp`
- Test: `tests/test_snapshot.cpp`

**Interfaces:**
- Produces: `QImage denso::ui::mat_to_qimage(const cv::Mat& bgr)` (in `frame_convert.h`); `QImage denso::ui::apply_rotation(const QImage& src, int degrees)` (in `snapshot.h`).

- [ ] **Step 1: Add OpenCV to the build**

In root `CMakeLists.txt`, immediately after the existing `find_package(Qt6 ... )` line add:

```cmake
find_package(OpenCV REQUIRED)
```

In `src/app/CMakeLists.txt`, add `ui/camera/snapshot.cpp` to the `add_executable(denso ...)` source list (next to the other `ui/camera/*.cpp`), then after the existing `target_include_directories(denso ...)` / `target_link_libraries(denso ...)` lines add OpenCV:

```cmake
target_include_directories(denso PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(denso PRIVATE ${OpenCV_LIBS})
```

In `tests/CMakeLists.txt`, add to the `add_executable(denso_tests ...)` source list:

```cmake
    test_snapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/snapshot.cpp
```

and after the existing `target_link_libraries(denso_tests ...)` line add (the `src/app` include dir is already added for the rtsp_templates test — keep it):

```cmake
target_include_directories(denso_tests PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(denso_tests PRIVATE ${OpenCV_LIBS} Qt6::Gui)
```

- [ ] **Step 2: Create the headers**

`src/app/ui/camera/frame_convert.h`:

```cpp
// Convert an OpenCV BGR frame to a QImage. Isolated in its own header so the
// cv::Mat type stays out of snapshot.h (and thus out of the dialog's include
// graph); only snapshot.cpp and the test include this.
#pragma once

#include <QImage>

#include <opencv2/core.hpp>

namespace denso::ui {

/// BGR `cv::Mat` (CV_8UC3) → RGB888 QImage that owns its bytes. Empty in → null.
QImage mat_to_qimage(const cv::Mat& bgr);

} // namespace denso::ui
```

`src/app/ui/camera/snapshot.h`:

```cpp
// Grab a single frame from a camera for the Configure preview, and rotate a
// frame for display. OpenCV-free on purpose — callers (the dialog) only see
// QImage. Capture is blocking; callers run grab_snapshot off the GUI thread.
#pragma once

#include <QImage>
#include <QString>

#include <optional>

namespace denso::ui {

struct Snapshot {
    QImage  image;   // null on failure
    QString error;   // human-readable reason when image is null
};

/// Open the USB `index` OR the RTSP `url` (exactly one set), apply width/height
/// (when both > 0) + a finite open/read timeout, and grab one frame.
Snapshot grab_snapshot(std::optional<int> index, const QString& url,
                       int width, int height);

/// Rotate a frame for preview. degrees ∈ {0, 90, 180, 270}; multiples of 360
/// are identity.
QImage apply_rotation(const QImage& src, int degrees);

} // namespace denso::ui
```

- [ ] **Step 3: Write the failing tests**

`tests/test_snapshot.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "ui/camera/frame_convert.h"
#include "ui/camera/snapshot.h"

#include <QImage>
#include <opencv2/core.hpp>

using denso::ui::apply_rotation;
using denso::ui::mat_to_qimage;

TEST_CASE("mat_to_qimage swaps BGR to RGB and keeps dimensions") {
    cv::Mat bgr(2, 3, CV_8UC3, cv::Scalar(255, 0, 0));  // pure blue in BGR
    const QImage img = mat_to_qimage(bgr);
    REQUIRE(img.width() == 3);
    REQUIRE(img.height() == 2);
    const QRgb px = img.pixel(0, 0);
    REQUIRE(qBlue(px) == 255);
    REQUIRE(qRed(px) == 0);
    REQUIRE(qGreen(px) == 0);
}

TEST_CASE("mat_to_qimage returns a null image for an empty mat") {
    REQUIRE(mat_to_qimage(cv::Mat()).isNull());
}

TEST_CASE("apply_rotation by 90 swaps width and height") {
    QImage src(4, 2, QImage::Format_RGB888);
    src.fill(Qt::black);
    const QImage out = apply_rotation(src, 90);
    REQUIRE(out.width() == 2);
    REQUIRE(out.height() == 4);
}

TEST_CASE("apply_rotation by 0 is identity in size") {
    QImage src(4, 2, QImage::Format_RGB888);
    src.fill(Qt::black);
    const QImage out = apply_rotation(src, 0);
    REQUIRE(out.width() == 4);
    REQUIRE(out.height() == 2);
}
```

- [ ] **Step 4: Run the tests to verify they fail**

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cd /d/workspace/Denso-DigitalReader
cmake -S . -B build -G Ninja && cmake --build build
```

Expected: **link/compile failure** — `mat_to_qimage` / `apply_rotation` undefined (snapshot.cpp not yet implemented).

- [ ] **Step 5: Implement snapshot.cpp (pure functions only this task)**

`src/app/ui/camera/snapshot.cpp`:

```cpp
#include "ui/camera/snapshot.h"

#include "ui/camera/frame_convert.h"

#include <QTransform>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

namespace denso::ui {

QImage mat_to_qimage(const cv::Mat& bgr) {
    if (bgr.empty()) {
        return {};
    }
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    const QImage view(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step),
                      QImage::Format_RGB888);
    return view.copy();  // deep copy: rgb is local, the QImage must own its bytes
}

QImage apply_rotation(const QImage& src, int degrees) {
    if (degrees % 360 == 0) {
        return src;
    }
    QTransform t;
    t.rotate(degrees);
    return src.transformed(t);
}

Snapshot grab_snapshot(std::optional<int> index, const QString& url,
                       int width, int height) {
    return {QImage(), QStringLiteral("not implemented")};  // Task 2 implements
}

} // namespace denso::ui
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cd /d/workspace/Denso-DigitalReader
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: **all tests pass** (the 65 prior + the 4 new snapshot cases).

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/app/CMakeLists.txt tests/CMakeLists.txt \
        src/app/ui/camera/frame_convert.h src/app/ui/camera/snapshot.h \
        src/app/ui/camera/snapshot.cpp tests/test_snapshot.cpp
git commit -m "feat(camera): OpenCV wiring + mat_to_qimage + apply_rotation"
```

---

### Task 2: grab_snapshot capture (manual verify)

**Files:**
- Modify: `src/app/ui/camera/snapshot.cpp:grab_snapshot`

**Interfaces:**
- Consumes: `mat_to_qimage` (Task 1).
- Produces: working `grab_snapshot(std::optional<int> index, const QString& url, int width, int height)` → `Snapshot`.

- [ ] **Step 1: Implement grab_snapshot**

Replace the Task-1 stub body of `grab_snapshot` in `src/app/ui/camera/snapshot.cpp` with:

```cpp
Snapshot grab_snapshot(std::optional<int> index, const QString& url,
                       int width, int height) {
    cv::VideoCapture cap;
    // Fail fast instead of hanging on an unreachable RTSP host.
    const std::vector<int> params = {
        cv::CAP_PROP_OPEN_TIMEOUT_MSEC, 5000,
        cv::CAP_PROP_READ_TIMEOUT_MSEC, 5000,
    };
    if (index.has_value()) {
        cap.open(*index, cv::CAP_ANY, params);
    } else {
        cap.open(url.toStdString(), cv::CAP_ANY, params);
    }
    if (!cap.isOpened()) {
        return {QImage(), QStringLiteral("Could not open the camera.")};
    }
    if (width > 0 && height > 0) {
        cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);
    }
    cv::Mat frame;
    if (!cap.read(frame) || frame.empty()) {
        return {QImage(), QStringLiteral("No frame received from the camera.")};
    }
    return {mat_to_qimage(frame), QString()};
}
```

Ensure `#include <vector>` is present in `snapshot.cpp` (add it with the other includes).

- [ ] **Step 2: Build (no unit test — capture needs real hardware)**

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cd /d/workspace/Denso-DigitalReader
cmake --build build
```

Expected: **BUILD OK**. (`grab_snapshot` is exercised manually once the UI is wired in Task 4 — see Manual Verification.)

- [ ] **Step 3: Commit**

```bash
git add src/app/ui/camera/snapshot.cpp
git commit -m "feat(camera): implement grab_snapshot via cv::VideoCapture"
```

---

### Task 3: Configure page scaffold (controls + preset table)

**Files:**
- Modify: `src/app/ui/camera/camera_dialog.h`
- Modify: `src/app/ui/camera/camera_dialog.cpp`

**Interfaces:**
- Produces: a third stack page built by `build_configure_page()`; members `config_page_`, `preview_label_`, `capture_btn_`, `res_combo_`, `fps_spin_`, `rotation_combo_`, `pitch_spin_`, `roll_spin_`, `editing_id_`, `draft_`, `last_frame_`; helpers `populate_configure(const camera::Camera&)`, `read_configure_into_draft()`, `selected_resolution()`. Used by Tasks 4–6.

- [ ] **Step 1: Add members + helper declarations to the header**

In `src/app/ui/camera/camera_dialog.h`, add forward declarations near the others:

```cpp
class QDoubleSpinBox;
```

(`QComboBox`, `QSpinBox`, `QLabel`, `QPushButton` are already forward-declared.) Add `#include "camera/model.h"` and `#include <cstdint>` / `#include <optional>` to the header includes. In the private section add:

```cpp
    void build_configure_page();                         // construct the 3rd stack page
    void populate_configure(const camera::Camera& cam);  // fill controls from a camera
    void read_configure_into_draft();                    // controls → draft_
    void capture_snapshot();                             // threaded grab + preview (Task 4)
    void render_preview();                               // re-apply rotation to last_frame_ (Task 4)
    void save_configured_camera();                       // insert/update from draft_ (Task 5)

    // Configure page
    QWidget* config_page_ = nullptr;
    QLabel* preview_label_ = nullptr;
    QPushButton* capture_btn_ = nullptr;
    QComboBox* res_combo_ = nullptr;       // itemData = QSize
    QSpinBox* fps_spin_ = nullptr;
    QComboBox* rotation_combo_ = nullptr;  // itemData = degrees (0/90/180/270)
    QDoubleSpinBox* pitch_spin_ = nullptr;
    QDoubleSpinBox* roll_spin_ = nullptr;

    // Add/edit mode state
    std::optional<int64_t> editing_id_;    // set in edit mode; empty when adding
    camera::Camera draft_;                 // camera being added/edited
    QImage last_frame_;                    // most recent un-rotated snapshot frame
```

Add `#include <QImage>` to the header.

- [ ] **Step 2: Build the Configure page**

In `src/app/ui/camera/camera_dialog.cpp`, add includes:

```cpp
#include "ui/camera/snapshot.h"

#include <QDoubleSpinBox>
#include <QImage>
#include <QPixmap>
#include <QSize>
```

Add a file-scope preset table in the anonymous namespace (near `kStatusBad`):

```cpp
struct ResPreset { const char* label; int w; int h; };
constexpr ResPreset kResPresets[] = {
    {"640 × 480", 640, 480},
    {"1280 × 720", 1280, 720},
    {"1920 × 1080", 1920, 1080},
    {"2560 × 1440", 2560, 1440},
};
constexpr int kDefaultResIndex = 1;  // 1280 × 720
```

Implement `build_configure_page()` (call it from the constructor right after the add page is added to the stack, before `outer->addWidget(stack_, 1)`):

```cpp
void CameraDialog::build_configure_page() {
    config_page_ = new QWidget;
    auto* v = new QVBoxLayout(config_page_);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(12);

    preview_label_ = new QLabel(QStringLiteral("Click Capture to preview"));
    preview_label_->setProperty("dim", true);
    preview_label_->setAlignment(Qt::AlignCenter);
    preview_label_->setMinimumHeight(240);
    preview_label_->setObjectName(QStringLiteral("card"));
    v->addWidget(preview_label_, 1);

    auto* cap_row = new QHBoxLayout;
    cap_row->addStretch(1);
    capture_btn_ = new QPushButton(QStringLiteral("Capture"));
    capture_btn_->setProperty("flatText", true);
    connect(capture_btn_, &QPushButton::clicked, this, &CameraDialog::capture_snapshot);
    cap_row->addWidget(capture_btn_, 0);
    v->addLayout(cap_row);

    const auto field = [&](const QString& label, QWidget* w) {
        auto* row = new QHBoxLayout;
        auto* l = dim_label(label);
        l->setFixedWidth(96);
        row->addWidget(l, 0);
        row->addWidget(w, 1);
        v->addLayout(row);
    };

    res_combo_ = new QComboBox;
    for (const ResPreset& p : kResPresets) {
        res_combo_->addItem(QString::fromLatin1(p.label), QSize(p.w, p.h));
    }
    res_combo_->setCurrentIndex(kDefaultResIndex);
    field(QStringLiteral("Resolution"), res_combo_);

    fps_spin_ = new QSpinBox;
    fps_spin_->setRange(1, 60);
    fps_spin_->setValue(30);
    field(QStringLiteral("FPS"), fps_spin_);

    rotation_combo_ = new QComboBox;
    for (int deg : {0, 90, 180, 270}) {
        rotation_combo_->addItem(QStringLiteral("%1°").arg(deg), deg);
    }
    connect(rotation_combo_, &QComboBox::currentIndexChanged, this,
            &CameraDialog::render_preview);
    field(QStringLiteral("Rotation"), rotation_combo_);

    pitch_spin_ = new QDoubleSpinBox;
    pitch_spin_->setRange(-45.0, 45.0);
    pitch_spin_->setSingleStep(0.5);
    pitch_spin_->setSuffix(QStringLiteral("°"));
    field(QStringLiteral("Pitch"), pitch_spin_);

    roll_spin_ = new QDoubleSpinBox;
    roll_spin_->setRange(-45.0, 45.0);
    roll_spin_->setSingleStep(0.5);
    roll_spin_->setSuffix(QStringLiteral("°"));
    field(QStringLiteral("Roll"), roll_spin_);

    auto* footer = new QHBoxLayout;
    auto* back = new QPushButton(QStringLiteral("Back"));
    connect(back, &QPushButton::clicked, this, &CameraDialog::show_list);
    footer->addWidget(back, 0);
    footer->addStretch(1);
    auto* save = new QPushButton(QStringLiteral("Save"));
    save->setProperty("gold", true);
    connect(save, &QPushButton::clicked, this, &CameraDialog::save_configured_camera);
    footer->addWidget(save, 0);
    v->addLayout(footer);

    stack_->addWidget(config_page_);  // index 2
}
```

Add **temporary empty stubs** so the file links this task (Tasks 4–5 fill them):

```cpp
void CameraDialog::capture_snapshot() {}
void CameraDialog::render_preview() {}
void CameraDialog::save_configured_camera() {}
void CameraDialog::populate_configure(const camera::Camera&) {}
void CameraDialog::read_configure_into_draft() {}
```

- [ ] **Step 3: Build**

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cd /d/workspace/Denso-DigitalReader
cmake --build build
```

Expected: **BUILD OK**. The page exists at stack index 2 but is not yet reachable.

- [ ] **Step 4: Commit**

```bash
git add src/app/ui/camera/camera_dialog.h src/app/ui/camera/camera_dialog.cpp
git commit -m "feat(camera): scaffold Configure page (controls + presets)"
```

---

### Task 4: Capture wiring (threaded grab + rotation preview)

**Files:**
- Modify: `src/app/ui/camera/camera_dialog.cpp` (`capture_snapshot`, `render_preview`)

**Interfaces:**
- Consumes: `grab_snapshot`, `apply_rotation` (Tasks 1–2); `draft_`, `last_frame_`, the configure widgets (Task 3); `with_credentials` (`rtsp_templates.h`, already included).

- [ ] **Step 1: Implement render_preview**

Replace the `render_preview` stub in `camera_dialog.cpp`:

```cpp
void CameraDialog::render_preview() {
    if (last_frame_.isNull()) {
        return;
    }
    const int deg = rotation_combo_->currentData().toInt();
    const QImage shown = apply_rotation(last_frame_, deg);
    preview_label_->setPixmap(QPixmap::fromImage(shown).scaled(
        preview_label_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
```

- [ ] **Step 2: Implement capture_snapshot (off the GUI thread)**

Replace the `capture_snapshot` stub. It reads the source from `draft_`, the resolution from `res_combo_`, runs `grab_snapshot` on a worker thread, and posts the result back (same pattern as `scan_ip`). Add `#include <QThread>` (already present for scan_ip):

```cpp
void CameraDialog::capture_snapshot() {
    capture_btn_->setText(QStringLiteral("Capturing…"));
    capture_btn_->setEnabled(false);
    preview_label_->setText(QStringLiteral("Capturing…"));

    std::optional<int> index;
    QString url;
    if (draft_.camera_type == "usb") {
        index = draft_.index ? std::optional<int>(static_cast<int>(*draft_.index))
                             : std::optional<int>(0);
    } else {
        const QString rtsp = draft_.rtsp ? QString::fromStdString(*draft_.rtsp) : QString();
        const QString user = draft_.username ? QString::fromStdString(*draft_.username) : QString();
        const QString pass = draft_.password ? QString::fromStdString(*draft_.password) : QString();
        url = with_credentials(rtsp, user, pass);
    }
    const QSize res = res_combo_->currentData().toSize();

    auto* thread = QThread::create([this, index, url, res] {
        const Snapshot snap = grab_snapshot(index, url, res.width(), res.height());
        QMetaObject::invokeMethod(
            this,
            [this, snap] {
                capture_btn_->setText(QStringLiteral("Capture"));
                capture_btn_->setEnabled(true);
                if (snap.image.isNull()) {
                    preview_label_->setText(snap.error);
                    return;
                }
                last_frame_ = snap.image;
                render_preview();
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
```

- [ ] **Step 3: Build**

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cd /d/workspace/Denso-DigitalReader
cmake --build build
```

Expected: **BUILD OK**. (Behaviour is exercised in Task 5/6 when the page becomes reachable + Manual Verification.)

- [ ] **Step 4: Commit**

```bash
git add src/app/ui/camera/camera_dialog.cpp
git commit -m "feat(camera): threaded snapshot capture + rotation preview"
```

---

### Task 5: Add-wizard integration (source → Configure → insert)

**Files:**
- Modify: `src/app/ui/camera/camera_dialog.cpp` (rename the add-page Save button to "Configure…", repurpose its slot; implement `read_configure_into_draft`, `save_configured_camera`; reset configure defaults in `show_add`)

**Interfaces:**
- Consumes: `draft_`, `editing_id_`, configure widgets, `camera::insert` (already used by the old `save_new_camera`).
- Produces: an add flow that ends on the Configure page and inserts on Save.

- [ ] **Step 1: Repurpose the add-page footer button**

In the constructor's add-page footer, change the button label from `"Save"` to `"Configure…"` (the `save` button created near the `save_new_camera` connect). Its `clicked` connection stays pointing at `save_new_camera` for now — but `save_new_camera` is repurposed in Step 2 to **build the draft and switch to Configure** instead of inserting.

Find:

```cpp
    auto* save = new QPushButton(QStringLiteral("Save"));
    save->setProperty("gold", true);
    connect(save, &QPushButton::clicked, this, &CameraDialog::save_new_camera);
```

Replace the label only:

```cpp
    auto* save = new QPushButton(QStringLiteral("Configure…"));
    save->setProperty("gold", true);
    connect(save, &QPushButton::clicked, this, &CameraDialog::save_new_camera);
```

- [ ] **Step 2: Make save_new_camera build the draft + go to Configure**

Replace the body of `save_new_camera` so it validates the source, fills `draft_`, sets add mode, resets the Configure controls to defaults, captures, and switches to the Configure page (stack index 2). Reuse the existing validation:

```cpp
void CameraDialog::save_new_camera() {
    const auto fail = [this](const QString& msg) {
        add_error_->setText(msg);
        add_error_->setVisible(true);
    };

    camera::Camera c;
    c.active = true;
    c.name = name_edit_->text().trimmed().toStdString();

    if (usb_radio_->isChecked()) {
        QListWidgetItem* item = usb_list_->currentItem();
        if (!item || !(item->flags() & Qt::ItemIsSelectable)) {
            fail(QStringLiteral("Select a camera, or click Scan."));
            return;
        }
        c.camera_type = "usb";
        c.index = static_cast<uint32_t>(item->data(Qt::UserRole).toInt());
        if (c.name.empty()) c.name = item->text().toStdString();
    } else {
        const QString ip = ip_edit_->text().trimmed();
        if (ip.isEmpty()) {
            fail(QStringLiteral("An IP address is required."));
            return;
        }
        const RtspManufacturer& m =
            rtsp_manufacturers()[static_cast<size_t>(mfr_combo_->currentData().toInt())];
        c.camera_type = "ip";
        c.ip = ip.toStdString();
        c.manufacturer = m.name.toStdString();
        c.stream = static_cast<uint32_t>(stream_combo_->currentIndex());
        c.channel = static_cast<uint32_t>(channel_spin_->value());
        c.rtsp = build_rtsp(m, ip, channel_spin_->value(),
                            stream_combo_->currentIndex() == 1)
                     .toStdString();
        const QString user = user_edit_->text();
        const QString pass = pass_edit_->text();
        if (!user.isEmpty()) c.username = user.toStdString();
        if (!pass.isEmpty()) c.password = pass.toStdString();
        if (c.name.empty()) c.name = ip.toStdString();
    }

    editing_id_.reset();
    draft_ = c;
    populate_configure(draft_);     // defaults (draft_ has 0 dims/fps/angle)
    last_frame_ = QImage();
    preview_label_->setText(QStringLiteral("Click Capture to preview"));
    stack_->setCurrentIndex(2);
    capture_snapshot();
}
```

- [ ] **Step 3: Implement populate_configure + read_configure_into_draft**

Replace the two stubs:

```cpp
void CameraDialog::populate_configure(const camera::Camera& cam) {
    // Resolution: match a preset, else prepend a "custom" entry and select it.
    int res_idx = -1;
    for (int i = 0; i < res_combo_->count(); ++i) {
        const QSize s = res_combo_->itemData(i).toSize();
        if (s.width() == static_cast<int>(cam.width) &&
            s.height() == static_cast<int>(cam.height)) {
            res_idx = i;
            break;
        }
    }
    if (res_idx < 0) {
        if (cam.width > 0 && cam.height > 0) {
            res_combo_->insertItem(
                0, QStringLiteral("%1 × %2 (custom)").arg(cam.width).arg(cam.height),
                QSize(static_cast<int>(cam.width), static_cast<int>(cam.height)));
            res_idx = 0;
        } else {
            // No stored size (fresh add) → use the default preset.
            res_idx = res_combo_->findData(QSize(kResPresets[kDefaultResIndex].w,
                                                 kResPresets[kDefaultResIndex].h));
            if (res_idx < 0) res_idx = 0;
        }
    }
    res_combo_->setCurrentIndex(res_idx);

    fps_spin_->setValue(cam.fps > 0 ? static_cast<int>(cam.fps) : 30);

    int rot_idx = rotation_combo_->findData(static_cast<int>(cam.rotation));
    rotation_combo_->setCurrentIndex(rot_idx < 0 ? 0 : rot_idx);

    pitch_spin_->setValue(cam.pitch);
    roll_spin_->setValue(cam.roll);
}

void CameraDialog::read_configure_into_draft() {
    const QSize res = res_combo_->currentData().toSize();
    draft_.width = static_cast<uint32_t>(res.width());
    draft_.height = static_cast<uint32_t>(res.height());
    draft_.fps = static_cast<uint32_t>(fps_spin_->value());
    draft_.rotation = static_cast<uint32_t>(rotation_combo_->currentData().toInt());
    draft_.pitch = static_cast<float>(pitch_spin_->value());
    draft_.roll = static_cast<float>(roll_spin_->value());
}
```

- [ ] **Step 4: Implement save_configured_camera**

```cpp
void CameraDialog::save_configured_camera() {
    read_configure_into_draft();

    std::optional<int64_t> ok_id;
    if (editing_id_.has_value()) {
        draft_.id = *editing_id_;
        if (!camera::update(db_, draft_)) {
            add_error_->setText(QStringLiteral("Failed to save the camera."));
            add_error_->setVisible(true);
            return;
        }
    } else {
        if (!camera::insert(db_, draft_)) {
            add_error_->setText(QStringLiteral("Failed to save the camera."));
            add_error_->setVisible(true);
            return;
        }
    }
    emit cameras_changed();
    show_list();
}
```

Add `#include "camera/repo.h"` is already present. Ensure `camera::update` is declared (it is, in repo.h).

- [ ] **Step 5: Build + manual smoke**

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cd /d/workspace/Denso-DigitalReader
cmake --build build && ctest --test-dir build
```

Expected: **BUILD OK**, **ctest unchanged-green** (69 tests). Then manual: launch `build/src/app/denso.exe`, Add → pick a USB camera → Configure… → the page shows, Capture fills the preview, Save returns to the list with the camera present.

- [ ] **Step 6: Commit**

```bash
git add src/app/ui/camera/camera_dialog.cpp
git commit -m "feat(camera): add-wizard Configure step inserts the camera"
```

---

### Task 6: Edit entry from the camera list

**Files:**
- Modify: `src/app/ui/camera/camera_dialog.cpp` (`rebuild_list` — add a Configure button per row + a `show_configure_for(const camera::Camera&)` path)

**Interfaces:**
- Consumes: `populate_configure`, `capture_snapshot`, `draft_`, `editing_id_`; `camera::get` (already in repo.h).

- [ ] **Step 1: Add a Configure button to each list row**

In `rebuild_list`, inside the per-camera loop, add a Configure button **before** the Delete button:

```cpp
        auto* cfg = new QPushButton(QStringLiteral("Configure"));
        cfg->setProperty("flatText", true);
        const camera::Camera row_cam = cam;  // capture by value for the lambda
        connect(cfg, &QPushButton::clicked, this, [this, row_cam] {
            editing_id_ = row_cam.id;
            draft_ = row_cam;
            populate_configure(draft_);
            last_frame_ = QImage();
            preview_label_->setText(QStringLiteral("Capturing…"));
            add_error_->setVisible(false);
            stack_->setCurrentIndex(2);
            capture_snapshot();
        });
        rl->addWidget(cfg, 0);
```

(Place this just before the existing `auto* del = new QPushButton(...)` block.)

- [ ] **Step 2: Build + manual smoke**

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cd /d/workspace/Denso-DigitalReader
cmake --build build && ctest --test-dir build
```

Expected: **BUILD OK**, ctest green. Manual: launch the app, on an existing camera click **Configure**, change FPS/rotation, Save, reopen Configure → the changed values persisted.

- [ ] **Step 3: Commit**

```bash
git add src/app/ui/camera/camera_dialog.cpp
git commit -m "feat(camera): edit an existing camera via the Configure page"
```

---

## Manual Verification (on-device, after Task 6)

The pure functions are covered by `test_snapshot.cpp`; live capture needs real hardware, so verify by hand on the model machine:

1. **USB path:** Add a USB camera → Configure… → Capture shows a live frame. Rotate 90° → preview rotates without re-grabbing. Save → camera listed.
2. **RTSP path:** Add an IP camera (real Dahua/NVR on the LAN, correct channel/credentials) → Configure… → Capture shows the stream frame within ~5 s, or a clear error for a bad host/credential (no UI hang).
3. **Resolution:** pick 1920×1080, Capture, confirm the frame reflects the requested size (or the nearest the device supports).
4. **Edit round-trip:** Configure an existing camera, change fps + rotation + pitch, Save, reopen → values restored; a custom (non-preset) stored size shows as "W × H (custom)".

---

## Self-Review

- **Spec coverage:** build wiring (T1), `frame_convert.h`/`mat_to_qimage` (T1), `snapshot.h` API + `apply_rotation` (T1), `grab_snapshot` w/ timeout (T2), Configure page + all five controls + preset table (T3), threaded capture + rotation-only preview (T4), add-wizard flow w/ `draft_`/`editing_id_` (T5), edit entry (T6), pure tests (T1), manual capture verification (Manual Verification). Pitch/roll persist without warp preview — satisfied (controls persist in T5; no warp anywhere). ✓
- **Placeholder scan:** Task-1/Task-3 stubs are explicitly temporary and replaced in named later steps — not open-ended TODOs. No "TBD"/"handle edge cases". ✓
- **Type consistency:** `grab_snapshot(std::optional<int>, const QString&, int, int)` and `apply_rotation(const QImage&, int)` identical across T1/T2/T4; `Snapshot{image,error}` used consistently; `res_combo_` itemData is `QSize` set in T3 and read in T4/T5; `rotation_combo_` itemData is `int` degrees throughout; `draft_`/`editing_id_` types match the header. ✓
