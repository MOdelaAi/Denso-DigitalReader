# Ensemble detection + class-centric Models page Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Merge same-class detections across ensembled models into one box (highest-confidence wins) and replace the flat per-model×per-class Models wizard page with a class-centric one.

**Architecture:** A new pure OpenCV helper (`merge_detections`) does class-aware cross-model NMS by class **name**; `DetectionProcessor::process` pools all models' kept detections, merges, then draws. `ModelsPage` is reworked to ensemble-model checkboxes + a searchable union class list, fanning selections back to the unchanged per-model DB schema by class name.

**Tech Stack:** C++17, Qt6 Widgets, OpenCV, Catch2 v3, CMake + Ninja (MSYS2 UCRT64).

## Global Constraints

- `denso_core` must not link `Qt6::Widgets`/OpenCV/ORT — this work touches only the `denso` app target + tests; no `src/core/` changes.
- No DB schema change / no migration — reuse `model`, `camera_model`, `camera_model_class` and `detection/repo` as-is.
- Class identity for merging and the UI is by class **name**, never class_id (indices differ across models).
- Confidence is **per class name** (global), not per-model-per-class.
- Domain types (`src/core/detection/detection.h`) are unchanged.
- Follow existing patterns: pure helpers under `shared/detection/` compiled into both the app target and `denso_tests`; pages own controls + emit signals, the coordinator drives DB writes.
- Build: `cmake --build build`. Tests run directly (CTest discovery has a pre-existing DLL-PATH issue): `PATH="/c/msys64/ucrt64/bin:$PATH" ./build/tests/denso_tests.exe`.

---

## Task 1: Pure cross-model NMS helper (`merge_detections`)

**Files:**
- Create: `src/app/ui/camera/shared/detection/merge_detections.h`
- Create: `src/app/ui/camera/shared/detection/merge_detections.cpp`
- Test: `tests/test_merge_detections.cpp`
- Modify: `src/app/CMakeLists.txt:31` (add source to app target)
- Modify: `tests/CMakeLists.txt` (add test + compile the helper into `denso_tests`)

**Interfaces:**
- Produces:
  - `struct denso::ui::NamedDetection { cv::Rect box; float conf; std::string name; };`
  - `std::vector<denso::ui::NamedDetection> denso::ui::merge_detections(std::vector<NamedDetection> dets, float iou_thresh);`
    — class-aware greedy NMS: sort by `conf` desc; keep a box unless it overlaps an already-kept box **of the same `name`** by IoU `> iou_thresh`. Different names never suppress each other.

- [ ] **Step 1: Write the header**

Create `src/app/ui/camera/shared/detection/merge_detections.h`:

```cpp
// Cross-model class-aware NMS: pool detections from several models (each tagged
// with its class *name*) and keep, per name, only the highest-confidence box
// among overlapping ones. Merging by name (not class id) is what lets the same
// class from different models dedup, since ids differ across models. Different
// names never suppress each other. Pure (OpenCV only) — unit-tested.
#pragma once

#include <opencv2/core.hpp>

#include <string>
#include <vector>

namespace denso::ui {

struct NamedDetection {
    cv::Rect box;
    float conf = 0.0f;
    std::string name;  // class name — the merge identity
};

// Greedy NMS within each class name. Input need not be sorted. Output is in
// descending-confidence order. A box is dropped when it overlaps an already-kept
// box of the same name by IoU strictly greater than iou_thresh.
std::vector<NamedDetection> merge_detections(std::vector<NamedDetection> dets,
                                             float iou_thresh);

} // namespace denso::ui
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_merge_detections.cpp`:

```cpp
#include "ui/camera/shared/detection/merge_detections.h"

#include <catch2/catch_test_macros.hpp>

using denso::ui::merge_detections;
using denso::ui::NamedDetection;

TEST_CASE("overlapping same-name boxes collapse to the highest confidence") {
    std::vector<NamedDetection> in{
        {cv::Rect(10, 10, 100, 100), 0.6f, "person"},
        {cv::Rect(14, 12, 100, 100), 0.9f, "person"},  // ~same box, higher conf
    };
    const auto out = merge_detections(in, 0.5f);
    REQUIRE(out.size() == 1);
    CHECK(out[0].conf == 0.9f);
}

TEST_CASE("non-overlapping same-name boxes both survive") {
    std::vector<NamedDetection> in{
        {cv::Rect(0, 0, 50, 50), 0.8f, "person"},
        {cv::Rect(500, 500, 50, 50), 0.7f, "person"},
    };
    const auto out = merge_detections(in, 0.5f);
    REQUIRE(out.size() == 2);
}

TEST_CASE("overlapping boxes of different names are both kept") {
    std::vector<NamedDetection> in{
        {cv::Rect(10, 10, 100, 100), 0.8f, "person"},
        {cv::Rect(12, 12, 100, 100), 0.7f, "car"},
    };
    const auto out = merge_detections(in, 0.5f);
    REQUIRE(out.size() == 2);
}

TEST_CASE("output is sorted by confidence descending") {
    std::vector<NamedDetection> in{
        {cv::Rect(0, 0, 10, 10), 0.3f, "a"},
        {cv::Rect(100, 0, 10, 10), 0.9f, "b"},
        {cv::Rect(200, 0, 10, 10), 0.6f, "c"},
    };
    const auto out = merge_detections(in, 0.5f);
    REQUIRE(out.size() == 3);
    CHECK(out[0].conf == 0.9f);
    CHECK(out[1].conf == 0.6f);
    CHECK(out[2].conf == 0.3f);
}
```

- [ ] **Step 3: Wire the helper into both build targets**

In `src/app/CMakeLists.txt`, add the source after line 31 (`ui/camera/shared/detection/yolo_decode.cpp`):

```cmake
    ui/camera/shared/detection/merge_detections.cpp
```

In `tests/CMakeLists.txt`, add to the `add_executable(denso_tests ...)` list, next to the other pure-OpenCV detection helpers (after the `test_yolo_decode.cpp` block):

```cmake
    test_merge_detections.cpp
    # merge_detections is GUI-target code but pure OpenCV (no Qt), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/detection/merge_detections.cpp
```

- [ ] **Step 4: Run the test to verify it fails**

Run:
```bash
cmake --build build
PATH="/c/msys64/ucrt64/bin:$PATH" ./build/tests/denso_tests.exe "[merge]" 2>/dev/null || \
PATH="/c/msys64/ucrt64/bin:$PATH" ./build/tests/denso_tests.exe "overlapping same-name*"
```
Expected: FAIL — link error `undefined reference to ... merge_detections` (implementation not written yet).

- [ ] **Step 5: Write the implementation**

Create `src/app/ui/camera/shared/detection/merge_detections.cpp`:

```cpp
#include "ui/camera/shared/detection/merge_detections.h"

#include <algorithm>

namespace denso::ui {
namespace {

float iou(const cv::Rect& a, const cv::Rect& b) {
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.width, b.x + b.width);
    const int y2 = std::min(a.y + a.height, b.y + b.height);
    const int iw = std::max(0, x2 - x1);
    const int ih = std::max(0, y2 - y1);
    const float inter = static_cast<float>(iw) * static_cast<float>(ih);
    const float uni = static_cast<float>(a.area() + b.area()) - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

} // namespace

std::vector<NamedDetection> merge_detections(std::vector<NamedDetection> dets,
                                             float iou_thresh) {
    std::sort(dets.begin(), dets.end(),
              [](const NamedDetection& a, const NamedDetection& b) {
                  return a.conf > b.conf;
              });
    std::vector<NamedDetection> kept;
    kept.reserve(dets.size());
    for (const NamedDetection& d : dets) {
        bool suppressed = false;
        for (const NamedDetection& k : kept) {
            if (k.name == d.name && iou(k.box, d.box) > iou_thresh) {
                suppressed = true;
                break;
            }
        }
        if (!suppressed) kept.push_back(d);
    }
    return kept;
}

} // namespace denso::ui
```

- [ ] **Step 6: Run the tests to verify they pass**

Run:
```bash
cmake --build build
PATH="/c/msys64/ucrt64/bin:$PATH" ./build/tests/denso_tests.exe "overlapping same-name boxes collapse to the highest confidence" "non-overlapping same-name boxes both survive" "overlapping boxes of different names are both kept" "output is sorted by confidence descending"
```
Expected: PASS — `All tests passed (… assertions in 4 test cases)`.

- [ ] **Step 7: Commit**

```bash
git add src/app/ui/camera/shared/detection/merge_detections.h \
        src/app/ui/camera/shared/detection/merge_detections.cpp \
        tests/test_merge_detections.cpp \
        src/app/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(detection): pure cross-model class-aware NMS helper

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Rewire `DetectionProcessor::process` to pool → merge → draw

**Files:**
- Modify: `src/app/ui/camera/grid/frame_processor.cpp:39-74` (the `process` body + includes/const)

**Interfaces:**
- Consumes: `denso::ui::merge_detections`, `denso::ui::NamedDetection` (Task 1); existing `selected_conf` (file-local), `run.engine->infer`, `apply_orientation`, `qimage_to_mat`/`mat_to_qimage`, `inside_any_area`.
- Produces: no new public symbols — same `DetectionProcessor::process(const QImage&)` signature and drawing style (rect + `"<name> NN%"`, colour `cv::Scalar(0,215,255)`).

- [ ] **Step 1: Add the include and merge constant**

In `src/app/ui/camera/grid/frame_processor.cpp`, add the include after line 6 (`#include "ui/camera/shared/snapshot.h"`):

```cpp
#include "ui/camera/shared/detection/merge_detections.h"  // merge_detections
```

In the anonymous namespace (after the `selected_conf` helper, before its closing `}`), add:

```cpp
constexpr float kMergeIoU = 0.5f;  // cross-model boxes of a class over this merge
```

- [ ] **Step 2: Replace the `process` body**

Replace the whole `QImage DetectionProcessor::process(const QImage& frame) { ... }` (currently lines 39-74) with:

```cpp
QImage DetectionProcessor::process(const QImage& frame) {
    const QImage oriented = apply_orientation(frame, degrees_, pitch_, roll_);
    cv::Mat bgr = qimage_to_mat(oriented);
    if (bgr.empty()) {
        return oriented;
    }
    const float w = static_cast<float>(bgr.cols);
    const float h = static_cast<float>(bgr.rows);

    // Pool every model's kept detections, each tagged with its class *name*, so
    // the same class from different models can merge despite differing ids.
    std::vector<NamedDetection> pool;
    for (const ModelRun& run : models_) {
        if (!run.engine) continue;
        for (const Detection& d : run.engine->infer(bgr)) {
            const auto conf = selected_conf(run.classes, d.class_id);
            if (!conf || d.conf < *conf) continue;  // not selected / below thr
            // Confine to ROI: keep only boxes whose center is inside an area.
            // Areas are normalized [0,1] to this oriented frame. Empty → no
            // confinement.
            if (!areas_.empty() && w > 0.0f && h > 0.0f) {
                const denso::camera::Point center{
                    (d.box.x + d.box.width * 0.5f) / w,
                    (d.box.y + d.box.height * 0.5f) / h};
                if (!denso::camera::inside_any_area(areas_, center)) continue;
            }
            std::string name =
                (d.class_id < static_cast<int>(run.class_names.size())
                     ? run.class_names[d.class_id]
                     : std::to_string(d.class_id));
            pool.push_back({d.box, d.conf, std::move(name)});
        }
    }

    // Cross-model NMS: within each class name, keep the highest-confidence box.
    const std::vector<NamedDetection> kept =
        merge_detections(std::move(pool), kMergeIoU);

    for (const NamedDetection& d : kept) {
        cv::rectangle(bgr, d.box, cv::Scalar(0, 215, 255), 2);
        std::string label = d.name;
        char buf[16];
        std::snprintf(buf, sizeof(buf), " %.0f%%", d.conf * 100.0f);
        label += buf;
        cv::putText(bgr, label, cv::Point(d.box.x, std::max(0, d.box.y - 4)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 215, 255), 1);
    }
    return mat_to_qimage(bgr);
}
```

- [ ] **Step 3: Build to verify it compiles and links**

Run:
```bash
cmake --build build
```
Expected: `denso.exe` links cleanly. (The `denso_tests.exe` CatchAddTests discovery line may still FAIL — that is the pre-existing DLL-PATH issue, unrelated to this change; compilation/linking of both targets succeeding is the success signal.)

- [ ] **Step 4: Run the existing test suite to confirm no regressions**

Run:
```bash
PATH="/c/msys64/ucrt64/bin:$PATH" ./build/tests/denso_tests.exe
```
Expected: PASS — `All tests passed`, including the four `merge_detections` cases.

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/camera/grid/frame_processor.cpp
git commit -m "feat(detection): merge same-class boxes across ensembled models

Pool every attached model's kept detections and run cross-model
class-aware NMS (highest-confidence wins) before drawing, so two models
that both detect a class no longer draw duplicate boxes.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Class-centric `ModelsPage`

**Files:**
- Modify: `src/app/ui/camera/dialog/models_page.h` (replace private members)
- Modify: `src/app/ui/camera/dialog/models_page.cpp` (rebuild the layout + round-trip)

**Interfaces:**
- Consumes: `denso::detection::list_models`, `denso::detection::models_for` (from `detection/repo.h`); domain types `DetectionModel`, `CameraModel`, `ModelClassSelection`.
- Produces: unchanged public surface — `set_db`, `load_for(int64_t camera_id)`, `selections(int64_t camera_id) const`, signals `back_requested()` / `finish_requested()`. The coordinator (`camera_dialog.cpp`) is untouched.

- [ ] **Step 1: Replace the header's private section**

In `src/app/ui/camera/dialog/models_page.h`, replace the whole file with:

```cpp
// The wizard's "Models" step, class-centric: pick which models form the
// detection ensemble, then pick classes *once* (union of the chosen models'
// classes, by name) with one confidence each. Selections fan back out to the
// per-model schema by class name in selections(). Reads catalog + current
// attachments from detection::repo; the coordinator saves via
// detection::set_camera_models on Finish. Pure widget — owns its controls,
// emits requests, holds no business logic.
#pragma once

#include "detection/detection.h"

#include <QSqlDatabase>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

class QVBoxLayout;
class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;

namespace denso::ui {

class ModelsPage : public QWidget {
    Q_OBJECT
public:
    explicit ModelsPage(QWidget* parent = nullptr);
    ~ModelsPage() override;

    void set_db(QSqlDatabase db) { db_ = std::move(db); }
    void load_for(int64_t camera_id);
    std::vector<denso::detection::CameraModel> selections(int64_t camera_id) const;

signals:
    void back_requested();
    void finish_requested();

private:
    void rebuild_class_list();  // union of the checked models' class names
    void apply_filter();        // show/hide class rows by the search text

    QSqlDatabase db_;
    QVBoxLayout* models_layout_ = nullptr;  // ensemble model checkboxes
    QLineEdit* search_ = nullptr;
    QVBoxLayout* class_layout_ = nullptr;   // class rows

    std::vector<denso::detection::DetectionModel> catalog_;  // cached for rebuilds

    struct ModelCheck {
        int64_t model_id = 0;
        QCheckBox* on = nullptr;
    };
    std::vector<ModelCheck> model_checks_;

    struct ClassRow {
        QString name;
        QCheckBox* on = nullptr;
        QDoubleSpinBox* conf = nullptr;
        QWidget* row = nullptr;
    };
    std::vector<ClassRow> class_rows_;

    // Remembered class selections (name → {selected, conf}) so toggling a model
    // in/out of the ensemble preserves what the user set. Seeded from the DB in
    // load_for, folded from the live widgets on every rebuild.
    std::map<QString, std::pair<bool, double>> selected_state_;
};

} // namespace denso::ui
```

- [ ] **Step 2: Replace the source**

Replace the whole `src/app/ui/camera/dialog/models_page.cpp` with:

```cpp
#include "ui/camera/dialog/models_page.h"

#include "detection/repo.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace denso::ui {

ModelsPage::ModelsPage(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    // ── Ensemble models ──
    root->addWidget(new QLabel(QStringLiteral("Models in ensemble")));
    auto* models_holder = new QWidget;
    models_layout_ = new QVBoxLayout(models_holder);
    models_layout_->setContentsMargins(0, 0, 0, 0);
    root->addWidget(models_holder);

    // ── Classes to detect ──
    root->addWidget(new QLabel(QStringLiteral("Classes to detect")));
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("Filter classes…"));
    root->addWidget(search_);
    connect(search_, &QLineEdit::textChanged, this, [this] { apply_filter(); });

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* holder = new QWidget;
    class_layout_ = new QVBoxLayout(holder);
    scroll->setWidget(holder);
    root->addWidget(scroll, 1);

    // ── Footer ──
    auto* footer = new QHBoxLayout;
    auto* back = new QPushButton(QStringLiteral("Back"));
    // Models is a middle step (Areas follows); the primary button advances.
    auto* finish = new QPushButton(QStringLiteral("Next"));
    footer->addWidget(back);
    footer->addStretch(1);
    footer->addWidget(finish);
    root->addLayout(footer);
    connect(back, &QPushButton::clicked, this, &ModelsPage::back_requested);
    connect(finish, &QPushButton::clicked, this, &ModelsPage::finish_requested);
}

ModelsPage::~ModelsPage() = default;

void ModelsPage::load_for(int64_t camera_id) {
    catalog_ = denso::detection::list_models(db_);
    const auto attached = denso::detection::models_for(db_, camera_id);

    // Seed remembered selections from the DB (name → {selected, conf}), first
    // conf seen wins if two models disagree on a shared name.
    selected_state_.clear();
    for (const auto& cm : attached) {
        const denso::detection::DetectionModel* dm = nullptr;
        for (const auto& m : catalog_) {
            if (m.id == cm.model_id) {
                dm = &m;
                break;
            }
        }
        if (!dm) continue;
        for (const auto& s : cm.classes) {
            if (s.class_id < 0 ||
                s.class_id >= static_cast<int>(dm->class_names.size())) {
                continue;
            }
            const QString name = QString::fromStdString(dm->class_names[s.class_id]);
            if (selected_state_.find(name) == selected_state_.end()) {
                selected_state_[name] = {true, static_cast<double>(s.conf)};
            }
        }
    }

    // Build the ensemble model checkboxes (checked == currently attached).
    QLayoutItem* it = nullptr;
    while ((it = models_layout_->takeAt(0)) != nullptr) {
        delete it->widget();
        delete it;
    }
    model_checks_.clear();
    for (const auto& m : catalog_) {
        auto* cb = new QCheckBox(QString::fromStdString(m.name));
        const bool is_attached =
            std::any_of(attached.begin(), attached.end(),
                        [&](const denso::detection::CameraModel& a) {
                            return a.model_id == m.id;
                        });
        cb->setChecked(is_attached);
        connect(cb, &QCheckBox::toggled, this, [this] { rebuild_class_list(); });
        models_layout_->addWidget(cb);
        model_checks_.push_back({m.id, cb});
    }

    rebuild_class_list();
}

void ModelsPage::rebuild_class_list() {
    // Fold the current widget values back into the remembered map so a model
    // toggle keeps whatever the user already set.
    for (const ClassRow& r : class_rows_) {
        selected_state_[r.name] = {r.on->isChecked(), r.conf->value()};
    }

    // Clear existing rows.
    class_rows_.clear();
    QLayoutItem* it = nullptr;
    while ((it = class_layout_->takeAt(0)) != nullptr) {
        delete it->widget();  // null for the trailing stretch — delete(nullptr) is ok
        delete it;
    }

    // Which models are currently in the ensemble.
    std::set<int64_t> checked;
    for (const ModelCheck& mc : model_checks_) {
        if (mc.on->isChecked()) checked.insert(mc.model_id);
    }

    // Union of the checked models' class names (std::set → unique + sorted).
    std::set<QString> names;
    for (const auto& m : catalog_) {
        if (checked.find(m.id) == checked.end()) continue;
        for (const auto& n : m.class_names) {
            names.insert(QString::fromStdString(n));
        }
    }

    for (const QString& name : names) {
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto* on = new QCheckBox(name);
        auto* conf = new QDoubleSpinBox;
        conf->setRange(0.0, 1.0);
        conf->setSingleStep(0.05);
        conf->setValue(0.5);
        const auto prev = selected_state_.find(name);
        if (prev != selected_state_.end()) {
            on->setChecked(prev->second.first);
            conf->setValue(prev->second.second);
        }
        h->addWidget(on, 1);
        h->addWidget(conf);
        class_layout_->addWidget(row);
        class_rows_.push_back({name, on, conf, row});
    }
    class_layout_->addStretch(1);
    apply_filter();
}

void ModelsPage::apply_filter() {
    const QString q = search_->text().trimmed();
    for (const ClassRow& r : class_rows_) {
        const bool show = q.isEmpty() || r.name.contains(q, Qt::CaseInsensitive);
        r.row->setVisible(show);
    }
}

std::vector<denso::detection::CameraModel> ModelsPage::selections(
    int64_t camera_id) const {
    // Current checked class names → conf (one global value per name).
    std::map<QString, double> chosen;
    for (const ClassRow& r : class_rows_) {
        if (r.on->isChecked()) chosen[r.name] = r.conf->value();
    }

    // Fan out to per-model selections: each checked model contributes the
    // chosen classes it actually has, mapped to its own class_id.
    std::vector<denso::detection::CameraModel> out;
    for (const ModelCheck& mc : model_checks_) {
        if (!mc.on->isChecked()) continue;
        const denso::detection::DetectionModel* dm = nullptr;
        for (const auto& m : catalog_) {
            if (m.id == mc.model_id) {
                dm = &m;
                break;
            }
        }
        if (!dm) continue;

        denso::detection::CameraModel cm;
        cm.camera_id = camera_id;
        cm.model_id = mc.model_id;
        for (size_t k = 0; k < dm->class_names.size(); ++k) {
            const QString name = QString::fromStdString(dm->class_names[k]);
            const auto sel = chosen.find(name);
            if (sel == chosen.end()) continue;
            cm.classes.push_back(denso::detection::ModelClassSelection{
                static_cast<int>(k), static_cast<float>(sel->second)});
        }
        out.push_back(std::move(cm));
    }
    return out;
}

} // namespace denso::ui
```

- [ ] **Step 3: Build to verify it compiles**

Run:
```bash
cmake --build build
```
Expected: `denso.exe` links cleanly (no changes to the coordinator were needed — the public surface is unchanged).

- [ ] **Step 4: Manual smoke test (widget logic has no unit harness in this project)**

Run the app:
```bash
PATH="/c/msys64/ucrt64/bin:$PATH" ./build/src/app/denso
```
Verify, in the Camera dialog's Models step:
1. **Ensemble list** shows one checkbox per catalog model.
2. Checking two general models (e.g. `yolov8n` + `yolo11n`) shows each class name **once** (e.g. a single `person` row), not once per model.
3. The **Filter classes** box narrows the list as you type.
4. Selecting `person` @ 0.60, Finish, reopen the dialog for the same camera → `person` still checked @ 0.60 and both models still checked (round-trip through `models_for`).
5. On a camera with two general models attached and both selecting `person`, a person in frame draws a **single** box (Task 2 merge), labelled `person NN%`.

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/camera/dialog/models_page.h src/app/ui/camera/dialog/models_page.cpp
git commit -m "feat(ui): class-centric Models wizard page

Replace the flat per-model x per-class layout with ensemble-model
checkboxes plus a searchable union class list (each class listed once,
one confidence). Selections fan back out to the unchanged per-model
schema by class name.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-Review notes

- **Spec coverage:** Change 1 (runtime merge) → Tasks 1+2. Change 2 (class-centric page) → Task 3. Name-based identity → Task 1 `NamedDetection.name` + Task 3 name↔class_id fan-out. Schema unchanged → no migration task (verified: only app + tests touched). Testing → Task 1 unit tests + Task 2/3 build & manual smoke, matching the spec's testability note.
- **Numeric/unnamed classes (denso `0–9`):** handled implicitly — they flow through as their own names (the digit strings), never merging with COCO names; no special-casing needed.
- **Type consistency:** `merge_detections` / `NamedDetection` signatures match between Task 1 (definition) and Task 2 (use); `load_for`/`selections` signatures unchanged from the current header so the coordinator needs no edits.
