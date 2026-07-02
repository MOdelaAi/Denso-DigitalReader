# Startup Screen with Threaded Warm-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show a startup screen immediately on launch that reports warm-up progress and stays animated while the minutes-long TensorRT engine build runs on a background thread, then swap in the main window when warm-up finishes.

**Architecture:** A new `ui/startup` launch helper owns a shared `EngineRegistry`, shows a frameless `StartupScreen`, and runs `EngineRegistry::warm_up()` on a `QThread` via a `WarmupWorker`. Progress is reported through queued signals to the splash; on completion the helper builds `MainWindow` (injected with the pre-warmed registry) and closes the splash. `main.cpp` stays a thin orchestrator, ending with `return denso::ui::launch(app, conn, state);`.

**Tech Stack:** C++20, Qt6 Widgets (`QThread`, `QProgressBar`, resources via AUTORCC), CMake + Ninja, MSYS2 UCRT64 toolchain, ONNX Runtime.

## Global Constraints

- `main.cpp` stays a thin orchestrator — no business logic (`CLAUDE.md`).
- `denso_core` must not link `Qt6::Widgets`; this feature is entirely in `src/app/` (app-only), never `src/core/`.
- Each target dir is its own include root, so includes read `ui/...`, `settings/...` (no relative `../`).
- Build: `cmake --build build`. Reconfigure after CMake/qrc/source-list changes: `cmake -S . -B build -G Ninja`.
- Tests: `export PATH="/c/msys64/ucrt64/bin:$PATH"; ctest --test-dir build` — must stay **132/132**.
- This feature has no pure seam covered by the Catch2 `denso_tests` suite (it's UI + ORT-bound). Verification is: green build, 132/132 tests, and a visual smoke of the widget + a manual app launch. This is stated plainly, not padded with token tests.
- End commit messages with the `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` trailer.

---

### Task 1: Add a progress callback to `EngineRegistry::warm_up`

**Files:**
- Modify: `src/app/ui/camera/shared/detection/engine_registry.h:30`
- Modify: `src/app/ui/camera/shared/detection/engine_registry.cpp:24-62`

**Interfaces:**
- Produces: `void EngineRegistry::warm_up(std::function<void(const std::string&)> on_model = {})` — invokes `on_model(filename)` (if set) before each model's load/infer. Default `{}` keeps existing callers working.

- [ ] **Step 1: Add `<functional>` and change the declaration**

In `engine_registry.h`, add the include near the other standard headers (after `#include <string>`):

```cpp
#include <functional>
```

Replace the `warm_up` declaration (line 30) and its doc comment tail so it reads:

```cpp
    /// Load AND warm (one blank inference) every *.onnx in models_dir. Call once
    /// at startup, before the capture threads run, so the first real frame
    /// doesn't stall on CUDA kernel init / allocation. Engines are cached, so
    /// cameras reuse the already-warm sessions. Blocking; logs per model.
    /// `on_model`, if set, is called with each model's filename just before it is
    /// prepared — used to drive a startup progress display.
    void warm_up(std::function<void(const std::string&)> on_model = {});
```

- [ ] **Step 2: Update the definition to fire the callback**

In `engine_registry.cpp`, change the signature and add the callback call. Replace:

```cpp
void EngineRegistry::warm_up() {
```

with:

```cpp
void EngineRegistry::warm_up(std::function<void(const std::string&)> on_model) {
```

Then, inside the loop, immediately after `const QString name = QString::fromStdString(filename);` and before the `qInfo().noquote() << "[warmup] preparing"` line, insert:

```cpp
        if (on_model) {
            on_model(filename);
        }
```

- [ ] **Step 3: Build to verify it compiles (default arg keeps `camera_grid.cpp` working)**

Run: `cmake --build build`
Expected: links `denso.exe` with no errors (existing `engines_->warm_up()` call in `camera_grid.cpp:63` still compiles via the default argument).

- [ ] **Step 4: Run tests**

Run: `export PATH="/c/msys64/ucrt64/bin:$PATH"; ctest --test-dir build`
Expected: `100% tests passed ... 132`.

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/camera/shared/detection/engine_registry.h src/app/ui/camera/shared/detection/engine_registry.cpp
git commit -m "feat(detection): warm_up progress callback

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Embed the app icon as a Qt resource

**Files:**
- Create: `src/app/resources.qrc`
- Modify: `CMakeLists.txt:11-12` (enable AUTORCC)
- Modify: `src/app/CMakeLists.txt:10` (add the qrc to the target sources)

**Interfaces:**
- Produces: the pixmap resource path `:/icon.png`, loadable via `QPixmap(QStringLiteral(":/icon.png"))`.

- [ ] **Step 1: Create the qrc file**

Create `src/app/resources.qrc` (the path is relative to the qrc's own directory, `src/app/`, so `../../assets/icon.png` resolves to the repo-root `assets/icon.png`):

```xml
<!DOCTYPE RCC>
<RCC version="1.0">
  <qresource prefix="/">
    <file alias="icon.png">../../assets/icon.png</file>
  </qresource>
</RCC>
```

- [ ] **Step 2: Enable AUTORCC**

In the top-level `CMakeLists.txt`, right after `set(CMAKE_AUTOMOC ON)` (line 11), add:

```cmake
set(CMAKE_AUTORCC ON)
```

- [ ] **Step 3: Add the qrc to the denso target sources**

In `src/app/CMakeLists.txt`, in the `add_executable(denso WIN32 ...)` list, add `resources.qrc` right after `main.cpp` (line 10):

```cmake
add_executable(denso WIN32
    main.cpp
    resources.qrc
    # app shell
```

- [ ] **Step 4: Reconfigure and build**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: configures and links with no errors; the generated `qrc_resources.cpp` compiles.

- [ ] **Step 5: Commit**

```bash
git add src/app/resources.qrc CMakeLists.txt src/app/CMakeLists.txt
git commit -m "build: embed app icon as a Qt resource (:/icon.png)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: `StartupScreen` widget

**Files:**
- Create: `src/app/ui/startup_screen.h`
- Create: `src/app/ui/startup_screen.cpp`
- Modify: `src/app/CMakeLists.txt` (add `ui/startup_screen.cpp`)
- Test (visual smoke, scratch harness): `<scratchpad>/startup_repro.cpp`

**Interfaces:**
- Consumes: `denso::ui::palette(bool)` and the `Palette` fields `panel_2`, `txt`, `txt_faint`, `panel_3`, `gold` (from `ui/theme.h`); the `:/icon.png` resource from Task 2.
- Produces:
  - `denso::ui::StartupScreen(bool dark, QWidget* parent = nullptr)` — frameless, centered, dark/light-themed splash.
  - slot `void StartupScreen::set_status(const QString& msg)`.

- [ ] **Step 1: Create the header**

Create `src/app/ui/startup_screen.h`:

```cpp
// A frameless, centered startup splash shown while detection engines warm up
// (see ui/startup.{h,cpp}). Self-styled from the theme palette because the
// app-wide stylesheet (qApp->setStyleSheet) is only applied later, in
// MainWindow::apply_startup. set_status() updates the progress line; an
// indeterminate progress bar animates on the main event loop as a liveness cue.
#pragma once

#include <QWidget>

class QLabel;

namespace denso::ui {

class StartupScreen : public QWidget {
    Q_OBJECT

public:
    explicit StartupScreen(bool dark, QWidget* parent = nullptr);

public slots:
    void set_status(const QString& msg);

private:
    QLabel* status_ = nullptr;
};

} // namespace denso::ui
```

- [ ] **Step 2: Create the implementation**

Create `src/app/ui/startup_screen.cpp`:

```cpp
#include "ui/startup_screen.h"

#include "ui/theme.h"

#include <QGuiApplication>
#include <QLabel>
#include <QPixmap>
#include <QProgressBar>
#include <QScreen>
#include <QVBoxLayout>

namespace denso::ui {

StartupScreen::StartupScreen(bool dark, QWidget* parent) : QWidget(parent) {
    setWindowFlag(Qt::FramelessWindowHint);
    setWindowTitle(QStringLiteral("Denso DigitalReader"));

    const Palette p = palette(dark);
    setStyleSheet(
        QStringLiteral(
            "QWidget { background: %1; color: %2; }"
            "QLabel#title { font-size: 20px; font-weight: 600; }"
            "QLabel#status { color: %3; }"
            "QProgressBar { border: none; background: %4;"
            " border-radius: 4px; height: 6px; }"
            "QProgressBar::chunk { background: %5; border-radius: 4px; }")
            .arg(p.panel_2.name(), p.txt.name(), p.txt_faint.name(),
                 p.panel_3.name(), p.gold.name()));

    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(48, 40, 48, 40);
    col->setSpacing(16);
    col->setAlignment(Qt::AlignCenter);

    auto* logo = new QLabel;
    const QPixmap pix(QStringLiteral(":/icon.png"));
    if (!pix.isNull()) {
        logo->setPixmap(pix.scaled(96, 96, Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation));
    }
    logo->setAlignment(Qt::AlignCenter);

    auto* title = new QLabel(QStringLiteral("Denso DigitalReader"));
    title->setObjectName(QStringLiteral("title"));
    title->setAlignment(Qt::AlignCenter);

    status_ = new QLabel(QStringLiteral("Starting…"));
    status_->setObjectName(QStringLiteral("status"));
    status_->setAlignment(Qt::AlignCenter);

    auto* bar = new QProgressBar;
    bar->setRange(0, 0);  // indeterminate — animates on the event loop
    bar->setTextVisible(false);
    bar->setFixedWidth(240);

    col->addWidget(logo);
    col->addWidget(title);
    col->addWidget(status_);
    col->addWidget(bar, 0, Qt::AlignHCenter);

    setFixedSize(360, 300);
    if (QScreen* s = QGuiApplication::primaryScreen()) {
        const QRect g = s->geometry();
        move(g.center() - rect().center());
    }
}

void StartupScreen::set_status(const QString& msg) { status_->setText(msg); }

} // namespace denso::ui
```

- [ ] **Step 3: Add to the build**

In `src/app/CMakeLists.txt`, under the `# app shell` group (after `ui/mainwindow.cpp`, line 13), add:

```cmake
    ui/startup_screen.cpp
```

- [ ] **Step 4: Build**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: compiles and links with no errors.

- [ ] **Step 5: Visual smoke test (scratch harness)**

Create `<scratchpad>/startup_repro.cpp` (replace `<scratchpad>` with the session scratchpad dir), which instantiates the real widget and screenshots it:

```cpp
#include "ui/startup_screen.h"
#include <QApplication>
#include <QTimer>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    denso::ui::StartupScreen s(true);
    s.set_status(QStringLiteral("Preparing model denso.onnx…"));
    s.show();
    QTimer::singleShot(600, [&] { s.grab().save(argv[1]); app.quit(); });
    return app.exec();
}
```

Compile it against the built objects and Qt (run from the repo root):

```bash
export PATH="/c/msys64/ucrt64/bin:$PATH"
SP="<scratchpad>"
g++ -std=c++20 -fPIC \
  -I src/app -I "$(pwd)/build/src/app/denso_autogen/include" \
  "$SP/startup_repro.cpp" \
  build/src/app/CMakeFiles/denso.dir/ui/startup_screen.cpp.obj \
  build/src/app/CMakeFiles/denso.dir/denso_autogen/mocs_compilation.cpp.obj \
  -o "$SP/startup_repro.exe" \
  $(pkg-config --cflags --libs Qt6Widgets)
"$SP/startup_repro.exe" "$SP/startup.png"
```

If linking the moc object is troublesome, instead link the whole app object set is unnecessary — as a fallback, temporarily add a `main()` guarded by `#ifdef STARTUP_SMOKE` is NOT needed; the simplest reliable path is to visually confirm during the Task 5 manual launch. Either way:
Expected: `startup.png` shows the icon, "Denso DigitalReader", the status line "Preparing model denso.onnx…", and an indeterminate progress bar, on a dark panel. Read it to confirm.

- [ ] **Step 6: Commit**

```bash
git add src/app/ui/startup_screen.h src/app/ui/startup_screen.cpp src/app/CMakeLists.txt
git commit -m "feat(ui): StartupScreen splash widget

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: `WarmupWorker`

**Files:**
- Create: `src/app/ui/warmup_worker.h`
- Create: `src/app/ui/warmup_worker.cpp`
- Modify: `src/app/CMakeLists.txt` (add `ui/warmup_worker.cpp`)

**Interfaces:**
- Consumes: `EngineRegistry::warm_up(std::function<void(const std::string&)>)` (Task 1).
- Produces:
  - `denso::ui::WarmupWorker(std::shared_ptr<EngineRegistry> engines, QObject* parent = nullptr)`.
  - slot `void run()`; signals `void progress(const QString& model)` and `void finished()`.

- [ ] **Step 1: Create the header**

Create `src/app/ui/warmup_worker.h`:

```cpp
// Runs EngineRegistry::warm_up() off the main thread (moveToThread'd onto a
// QThread by ui/startup) so the StartupScreen keeps animating during the
// minutes-long TensorRT build. Emits progress per model and finished() at the
// end; both cross to the main thread via queued connections.
#pragma once

#include "ui/camera/shared/detection/engine_registry.h"

#include <QObject>
#include <QString>

#include <memory>

namespace denso::ui {

class WarmupWorker : public QObject {
    Q_OBJECT

public:
    explicit WarmupWorker(std::shared_ptr<EngineRegistry> engines,
                          QObject* parent = nullptr);

public slots:
    void run();

signals:
    void progress(const QString& model);
    void finished();

private:
    std::shared_ptr<EngineRegistry> engines_;
};

} // namespace denso::ui
```

- [ ] **Step 2: Create the implementation**

Create `src/app/ui/warmup_worker.cpp`:

```cpp
#include "ui/warmup_worker.h"

namespace denso::ui {

WarmupWorker::WarmupWorker(std::shared_ptr<EngineRegistry> engines,
                           QObject* parent)
    : QObject(parent), engines_(std::move(engines)) {}

void WarmupWorker::run() {
    if (engines_) {
        engines_->warm_up([this](const std::string& name) {
            emit progress(QStringLiteral("Preparing model %1…")
                              .arg(QString::fromStdString(name)));
        });
    }
    emit finished();
}

} // namespace denso::ui
```

- [ ] **Step 3: Add to the build**

In `src/app/CMakeLists.txt`, under the `# app shell` group (after `ui/startup_screen.cpp` from Task 3), add:

```cmake
    ui/warmup_worker.cpp
```

- [ ] **Step 4: Build**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: compiles and links with no errors.

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/warmup_worker.h src/app/ui/warmup_worker.cpp src/app/CMakeLists.txt
git commit -m "feat(ui): WarmupWorker to run warm_up off the main thread

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Integration — `launch()`, registry injection, `main.cpp`, docs

This is the atomic switch to the new flow: it creates the shared `EngineRegistry`, injects it through `MainWindow → CameraView → CameraGrid`, removes the lazy create+warm from `CameraGrid::reload()`, and points `main.cpp` at `launch()`. All parts must land together so the app both compiles and still warms up.

**Files:**
- Create: `src/app/ui/startup.h`
- Create: `src/app/ui/startup.cpp`
- Modify: `src/app/ui/camera/grid/camera_grid.h:27,41`
- Modify: `src/app/ui/camera/grid/camera_grid.cpp` (ctor + `reload()` lines 53-64)
- Modify: `src/app/ui/camera/camera_view.h:8,21,32-36`
- Modify: `src/app/ui/camera/camera_view.cpp:70` (and ctor)
- Modify: `src/app/ui/mainwindow.h:20-27`
- Modify: `src/app/ui/mainwindow.cpp:48`
- Modify: `src/app/main.cpp:11,95-99`
- Modify: `src/app/CMakeLists.txt` (add `ui/startup.cpp`)
- Modify: `CLAUDE.md` (warm-up thread wording)
- Modify: `docs/ARCHITECTURE.md` (warm-up thread wording)

**Interfaces:**
- Consumes: `StartupScreen` (Task 3), `WarmupWorker` (Task 4), `EngineRegistry` (Task 1), `MainWindow`.
- Produces: `int denso::ui::launch(QApplication& app, QSqlDatabase db, std::shared_ptr<settings::Settings> state)`.
- Changed constructors:
  - `CameraGrid(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines, QWidget* parent = nullptr)`
  - `CameraView(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines, QWidget* parent = nullptr)`
  - `MainWindow(QSqlDatabase db, std::shared_ptr<settings::Settings> state, std::shared_ptr<EngineRegistry> engines, QWidget* parent = nullptr)`

- [ ] **Step 1: Inject the registry into `CameraGrid` (header)**

In `src/app/ui/camera/grid/camera_grid.h`, change the constructor (line 27) to:

```cpp
    explicit CameraGrid(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                        QWidget* parent = nullptr);
```

and change the member (line 41) from `std::unique_ptr<EngineRegistry> engines_;` to:

```cpp
    std::shared_ptr<EngineRegistry> engines_;
```

- [ ] **Step 2: Inject the registry into `CameraGrid` (source) and drop lazy warm-up**

In `src/app/ui/camera/grid/camera_grid.cpp`, update the constructor to store the injected registry. Find the existing constructor definition (it currently takes `(QSqlDatabase db, QWidget* parent)`) and change its signature + member init to:

```cpp
CameraGrid::CameraGrid(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                       QWidget* parent)
    : QWidget(parent), db_(std::move(db)), engines_(std::move(engines)) {
```

(Keep the rest of the constructor body unchanged — only the signature line and the `engines_(...)` member initializer are added. If the existing initializer list differs, preserve the existing members and just add `engines_(std::move(engines))`.)

Then in `reload()` (lines 56-64), **delete** the entire lazy-create-and-warm block:

```cpp
    if (!engines_) {
        const std::string dir = QCoreApplication::applicationDirPath().toStdString();
        engines_ = std::make_unique<EngineRegistry>(dir + "/models",
                                                     dir + "/models/trt_cache");
        // Load + warm every model once, up front (this is the first reload, run
        // during startup before any capture thread), so the first detected frame
        // doesn't stall on CUDA init. Cameras reuse these cached, warm engines.
        engines_->warm_up();
    }
```

The rest of `reload()` (from `std::vector<camera::Camera> cams = camera::all(db_);` onward) is unchanged and uses `engines_` as before. If `QCoreApplication` is now unused in this file, leave its include — it may be used elsewhere; do not remove includes speculatively.

- [ ] **Step 3: Inject the registry into `CameraView`**

In `src/app/ui/camera/camera_view.h`, add the registry include near the top (after `#include <QWidget>`, line 9):

```cpp
#include "ui/camera/shared/detection/engine_registry.h"

#include <memory>
```

Change the constructor (line 21) to:

```cpp
    explicit CameraView(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                        QWidget* parent = nullptr);
```

and add a member alongside the others (after line 35, `CameraGrid* grid_ = nullptr;`):

```cpp
    std::shared_ptr<EngineRegistry> engines_;
```

In `src/app/ui/camera/camera_view.cpp`, update the constructor definition signature to match and store the registry (add `engines_(std::move(engines))` to its member-init list; preserve existing members). Then change the grid construction (line 70) from `grid_ = new CameraGrid(db_);` to:

```cpp
    grid_ = new CameraGrid(db_, engines_);
```

- [ ] **Step 4: Inject the registry into `MainWindow`**

In `src/app/ui/mainwindow.h`, add a forward declaration in the `denso::ui` namespace (near the other forward decls, after `class CameraView;`, line 20):

```cpp
class EngineRegistry;
```

Change the constructor (lines 26-27) to:

```cpp
    MainWindow(QSqlDatabase db, std::shared_ptr<settings::Settings> state,
               std::shared_ptr<EngineRegistry> engines, QWidget* parent = nullptr);
```

In `src/app/ui/mainwindow.cpp`, update the constructor definition signature to match, and pass the registry when building the view (line 48) — change `camera_view_ = new CameraView(db_);` to:

```cpp
    camera_view_ = new CameraView(db_, engines);
```

(`engines` is forwarded, not stored on `MainWindow`. `engine_registry.h` is pulled in transitively via `camera_view.h`, so no new include is needed in `mainwindow.cpp`.)

- [ ] **Step 5: Create the `launch` header**

Create `src/app/ui/startup.h`:

```cpp
// Startup orchestration: show the StartupScreen, warm the detection engines on
// a background thread, then build and show MainWindow with the pre-warmed,
// shared EngineRegistry injected. Keeps main.cpp a thin orchestrator.
#pragma once

#include "settings/settings.h"

#include <QSqlDatabase>

#include <memory>

class QApplication;

namespace denso::ui {

int launch(QApplication& app, QSqlDatabase db,
           std::shared_ptr<settings::Settings> state);

} // namespace denso::ui
```

- [ ] **Step 6: Create the `launch` implementation**

Create `src/app/ui/startup.cpp`:

```cpp
#include "ui/startup.h"

#include "ui/camera/shared/detection/engine_registry.h"
#include "ui/mainwindow.h"
#include "ui/startup_screen.h"
#include "ui/warmup_worker.h"

#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include <memory>
#include <string>

namespace denso::ui {

int launch(QApplication& app, QSqlDatabase db,
           std::shared_ptr<settings::Settings> state) {
    const std::string dir = QCoreApplication::applicationDirPath().toStdString();
    auto engines = std::make_shared<EngineRegistry>(dir + "/models",
                                                    dir + "/models/trt_cache");

    auto splash = std::make_unique<StartupScreen>(state->dark);
    splash->show();

    auto* thread = new QThread;
    auto* worker = new WarmupWorker(engines);
    worker->moveToThread(thread);

    // Built on the main thread once warm-up finishes; must outlive app.exec(),
    // so it lives in this scope and is populated by the finished handler.
    std::unique_ptr<MainWindow> window;

    QObject::connect(thread, &QThread::started, worker, &WarmupWorker::run);
    QObject::connect(worker, &WarmupWorker::progress, splash.get(),
                     &StartupScreen::set_status);
    QObject::connect(worker, &WarmupWorker::finished, &app,
                     [&window, &splash, thread, worker, db, state, engines]() {
                         thread->quit();
                         thread->wait();  // warm-up done before we build the grid
                         delete worker;
                         delete thread;

                         window = std::make_unique<MainWindow>(db, state, engines);
                         window->apply_startup();
                         window->show();
                         splash->close();
                         splash.reset();
                     });

    thread->start();
    return app.exec();
}

} // namespace denso::ui
```

- [ ] **Step 7: Make `main.cpp` call `launch`**

In `src/app/main.cpp`, replace the include of `ui/mainwindow.h` (line 11) with:

```cpp
#include "ui/startup.h"
```

Then replace the window construction + run (lines 95-99):

```cpp
    denso::ui::MainWindow window(conn, state);
    window.apply_startup();
    window.show();

    return app.exec();
```

with:

```cpp
    return denso::ui::launch(app, conn, state);
```

- [ ] **Step 8: Add `startup.cpp` to the build**

In `src/app/CMakeLists.txt`, under the `# app shell` group (after `ui/warmup_worker.cpp` from Task 4), add:

```cmake
    ui/startup.cpp
```

- [ ] **Step 9: Reconfigure and build**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: configures and links `denso.exe` with no errors.

- [ ] **Step 10: Run tests**

Run: `export PATH="/c/msys64/ucrt64/bin:$PATH"; ctest --test-dir build`
Expected: `100% tests passed ... 132`.

- [ ] **Step 11: Manual launch verification**

Run the app from the build's exe dir (path varies by generator; typically `build/src/app/denso.exe`):

```bash
export PATH="/c/msys64/ucrt64/bin:$PATH"
./build/src/app/denso.exe
```

Expected, in order:
1. The startup screen appears **immediately** (icon + "Denso DigitalReader" + status + animated indeterminate bar).
2. During warm-up the status line updates to "Preparing model &lt;name&gt;…" per `models/*.onnx`, and the bar keeps animating (no "Not Responding").
3. When warm-up finishes, the main window appears and the splash closes.
4. Cameras stream and (for cameras with attached models) detection works — confirming the injected registry is the same warm one (no second warm-up occurs when navigating to the camera view or after closing the camera modal).

If no GPU/models are present, the splash may flash briefly before the window — that is expected.

- [ ] **Step 12: Update the docs to match the worker-thread warm-up**

In `CLAUDE.md`, in the Detection / ONNX Runtime section, update the TensorRT warm-up bullet so it no longer says warm-up runs on the main thread. Change the phrase describing where `EngineRegistry::warm_up()` runs from "(main thread, before any capture thread exists)" to:

> (on a dedicated startup worker thread driven by `ui/startup`, while the `StartupScreen` splash animates — completing **before the window shows and before any capture thread exists**)

In `docs/ARCHITECTURE.md`, update the two warm-up references (around lines 257 and 327) the same way: warm-up is triggered from `EngineRegistry::warm_up()` on a **startup worker thread** (not the main thread), still before any capture thread exists, with the splash providing feedback during the minutes-long, non-interruptible build. Keep the existing rationale about never building lazily on a capture thread.

- [ ] **Step 13: Commit**

```bash
git add src/app/ui/startup.h src/app/ui/startup.cpp src/app/main.cpp \
        src/app/ui/mainwindow.h src/app/ui/mainwindow.cpp \
        src/app/ui/camera/camera_view.h src/app/ui/camera/camera_view.cpp \
        src/app/ui/camera/grid/camera_grid.h src/app/ui/camera/grid/camera_grid.cpp \
        src/app/CMakeLists.txt CLAUDE.md docs/ARCHITECTURE.md
git commit -m "feat(ui): startup screen with threaded engine warm-up

Show StartupScreen immediately, warm detection engines on a background
thread with per-model progress, then build MainWindow with a shared,
pre-warmed EngineRegistry injected through the view/grid. main.cpp now
delegates to ui/startup::launch. Docs updated: warm-up runs on a startup
worker thread (not the main thread), before the window and any capture
thread.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:**
- Startup screen (logo + live status + animated bar) → Task 3. ✅
- Threaded warm-up → Tasks 1 (callback) + 4 (worker) + 5 (thread wiring). ✅
- `ui/startup` launch helper owning `EngineRegistry` → Task 5. ✅
- `EngineRegistry` injected as `shared_ptr` through `MainWindow`→`CameraView`→`CameraGrid`; lazy warm removed → Task 5 steps 1-4. ✅
- `main.cpp` stays thin → Task 5 step 7. ✅
- Icon asset → Task 2. ✅
- Thread joined before grid built (no `get()` race) → Task 5 step 6 (`thread->wait()` before `new MainWindow`). ✅
- Docs updated → Task 5 step 12. ✅
- Testing = build + 132 tests + visual smoke + manual launch → stated in Global Constraints and Task 5 step 11. ✅

**Placeholder scan:** No "TBD"/"handle edge cases"/"similar to Task N"; every code step shows full code. The Task 3 smoke note offers a fallback but the primary path is concrete. ✅

**Type consistency:** `warm_up(std::function<void(const std::string&)>)` used identically in Tasks 1/4. `std::shared_ptr<EngineRegistry>` used identically across `CameraGrid`/`CameraView`/`MainWindow`/`launch`. `set_status(const QString&)` and `progress(const QString&)`/`finished()` names match between Tasks 3/4/5. `launch(QApplication&, QSqlDatabase, std::shared_ptr<settings::Settings>)` matches `main.cpp`'s call. ✅
