// The main content area's camera view. With no cameras it shows the "no cameras
// yet" empty state + an Add button; with one or more it shows the live 1–4
// streaming grid. reload() re-reads the camera list and switches/refreshes
// accordingly. release_streams() stops capture (e.g. while the Camera modal is
// open, so it can grab the same USB device); the next reload() restarts.
#pragma once

#include "brazing/brazing_status.h"   // BrazingStatus
#include "detection/engine_registry.h"

#include <QSqlDatabase>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace denso::ui {

class CameraGrid;
class WarmupState;

class CameraView : public QWidget {
    Q_OBJECT

public:
    explicit CameraView(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                        WarmupState* warmup, QWidget* parent = nullptr);

    /// Re-read the camera list, switch empty-state vs grid, and (re)start streams.
    void reload();

    /// Stop capture without tearing the view down (frees devices for the modal).
    void release_streams();

    /// Pre-mode-switch teardown: tear the live grid down via CameraGrid's ONE
    /// authoritative primitive and park on the neutral, non-live page for the
    /// duration of the reset transaction. Deliberately does NOT call reload() or
    /// query camera::runtime() — nothing must restart the old-mode pipeline before
    /// the reset commits (spec §6.2). The rebuild happens later, after the switch.
    void teardown_for_switch();

    /// Adopt a NEW inference session after a committed mode switch. Forwards to
    /// CameraGrid, which refuses while anything is still streaming - see
    /// CameraGrid::set_engines.
    void set_engines(std::shared_ptr<EngineRegistry> engines, WarmupState* warmup);

    /// Forwards to CameraGrid::settle_pending_after_warmup() - see there.
    void settle_pending_after_warmup();

    /// Forwards to CameraGrid::apply_brazing_config() - see there. Deliberately
    /// a pass-through and NOT a reload(): a Backend settings change must not
    /// re-read the camera list, restart capture or reload a model.
    void apply_brazing_config();

    /// The grid's backend reporting status, forwarded verbatim. The view adds no
    /// state of its own — it is a pass-through so the window need not reach
    /// through it into the grid.
    BrazingStatus brazing_status() const;
    /// The canonical base URL the live sender was built with ("" when none), from
    /// the grid. It can carry no credentials because normalize_base_url refuses
    /// userinfo.
    std::string active_brazing_base_url() const;
    /// The FULL endpoint the live sender posts to ("" when none), from the grid.
    /// Feeds the Backend indicator's tooltip: with the reporting API path
    /// configurable, the base alone no longer tells an operator where readings
    /// are going. Same credential guarantee as above.
    std::string active_brazing_endpoint() const;
    /// How many camera runtimes the grid holds — see CameraGrid::stream_count().
    size_t grid_stream_count() const;
    /// How many cameras the grid admitted — see CameraGrid::admitted_count().
    size_t grid_admitted_count() const;

    // Test-only observers of the view's non-live state after teardown_for_switch().
    int current_page_index() const;      ///< 0 = empty, 1 = live grid, 2 = retained/unavailable
    bool grid_has_live_streams() const;  ///< whether the grid still holds a stream
    uint64_t grid_reload_invocations() const;  ///< delegate to CameraGrid::reload_invocations()
    uint64_t grid_generation() const;     ///< delegate to CameraGrid::generation()

signals:
    void add_camera_requested();
    /// Re-emitted from CameraGrid, unchanged, so the window connects to the view
    /// it already owns instead of to a grid it would have to reach through.
    void brazing_status_changed(BrazingStatus status);

private:
    // (Re)build page 2's retained-connection list + mode-specific header/message/
    // action from the current DB rows. `ball_leveler` selects the unavailable
    // variant (read-only list, no setup action); digit_reader shows the
    // setup-required variant with a "Set up cameras" action.
    void populate_retained_page(bool ball_leveler);

    QSqlDatabase db_;
    QStackedWidget* stack_ = nullptr;
    CameraGrid* grid_ = nullptr;
    std::shared_ptr<EngineRegistry> engines_;
    WarmupState* warmup_ = nullptr;

    // Page 2 (retained connections / mode unavailable): skeleton built once in the
    // ctor, repopulated per reload().
    QLabel* retained_header_ = nullptr;
    QLabel* retained_message_ = nullptr;
    QVBoxLayout* retained_list_box_ = nullptr;
    QPushButton* retained_setup_btn_ = nullptr;
};

} // namespace denso::ui
