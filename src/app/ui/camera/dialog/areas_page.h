// The Camera dialog's "Areas" step: draw and reshape ROI polygons over the
// camera snapshot. Holds a working set of named areas; the controller loads the
// existing ones plus the zone numbers other cameras already own, pushes the
// oriented background frame, and persists on save_requested. The page owns no DB
// access — it just edits polygons, and validates the set before asking for a
// save so a deterministic problem (zone clash, unfinished shape, degenerate
// polygon) is named here instead of surfacing as a generic write failure.
#pragma once

#include "camera/camera.h"
#include "camera/roi_enhancement.h"

#include <QImage>
#include <QWidget>

#include <map>
#include <optional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QTimer;

namespace denso::ui {

class RoiCanvas;
class ZoneNumberEdit;

class CameraAreasPage : public QWidget {
    Q_OBJECT

public:
    explicit CameraAreasPage(QWidget* parent = nullptr);

    /// The existing set to edit, plus zone → owning-camera-name for every zone
    /// claimed by OTHER cameras (see camera::zones_owned_by_other_cameras).
    /// Taken zones are listed under the zone field and typing one raises a
    /// named duplicate warning that blocks Save, so the operator can't build a
    /// save that the repo would reject.
    void load(std::vector<camera::CameraArea> areas,
              std::map<int, std::string> zones_taken);
    /// The oriented snapshot. Stored as THE original and never overwritten by a
    /// rendered result — every preview render starts from this image, so toggling
    /// the preview or changing strength can never compound one enhancement on top
    /// of another.
    void set_background(const QImage& oriented);

    /// Seed the camera's persisted Image Enhancement bundle. This is also the
    /// baseline the dirty check compares against, so an operator who changes only
    /// a slider and then leaves is still warned.
    ///
    /// It also clears the preview toggle: that is wizard view state, never
    /// persisted, so every entry starts from the picture the camera really sends.
    void set_enhancement(const camera::ImageEnhancement& cfg);

    /// Re-baseline the dirty check against what is now on disk. Called by the
    /// controller ONLY after the save transaction has committed.
    ///
    /// It matters because a terminal action can leave the operator on this page
    /// after a successful write (finish_and_leave stays put when the camera
    /// could not be marked complete, so the button can be pressed again). Without
    /// this the page would keep reporting saved work as unsaved, and Back or Exit
    /// would offer to "discard" changes that are already persisted.
    void mark_saved();

    /// The configuration currently on the page, saved or not. The controller
    /// persists it as part of the Areas save — nothing here writes to the
    /// database.
    const camera::ImageEnhancement& enhancement() const { return working_; }

    /// The editing canvas, so a test can drive real draw gestures (the same
    /// accessor LevelCalibrationPage exposes for the same reason).
    RoiCanvas* canvas() const { return canvas_; }
    void show_save_error();                       // persistence failed
    /// Show/hide the "re-verify after a source change" banner. When on, the
    /// camera's zone reporting is paused until these areas are saved (verified).
    void set_review_required(bool on);
    /// The camera's add wizard is not finished: saving here completes it, and
    /// leaving does not start it. Relabels the terminal actions to say so.
    void set_unfinished(bool on);

signals:
    void back_requested();
    void skip_requested();
    void save_requested(const std::vector<camera::CameraArea>& areas);
    void refresh_preview_requested();

private:
    void refresh_list();
    /// Repaint the canvas backdrop from `original_background_`: the raw snapshot
    /// when the preview is off or the level is Off, otherwise that snapshot put
    /// through the SHARED enhancement authority (ui::enhance_preview) with the
    /// working polygons. Always from the original — never from what is on screen.
    void render_background();
    /// Coalesced render request. A vertex drag emits changes at mouse-move rate;
    /// enhancing a 1080p snapshot per motion event would make dragging crawl, so
    /// requests collapse into one render on a short timer.
    void request_preview_refresh();
    /// The polygons a save WOULD write, plus any shape still being drawn — so the
    /// preview mask follows unsaved geometry the moment it exists.
    std::vector<camera::CameraArea> working_areas() const;
    bool preview_enabled() const;
    void update_enhance_controls();
    /// Push `working_` into the widgets without re-emitting their signals, then
    /// refresh the value readouts. Used by seeding and by Reset.
    void sync_enhance_widgets();
    /// One control moved: adopt the new values and refresh the readouts, then
    /// re-render.
    ///
    /// `immediate` separates the two kinds of control. A combo pick, the master
    /// switch and Reset are DISCRETE operator actions and must show their result
    /// at once. A slider is CONTINUOUS: a drag emits a change per pixel of travel,
    /// and enhancing a 1080p snapshot on each one would make the control unusable,
    /// so those go through the same 80 ms coalescer a vertex drag uses.
    void on_enhance_edited(bool immediate);
    /// Reset the WORKING state to disabled-and-neutral. No database write — the
    /// operator still has to press Save, and Discard still restores what is
    /// stored.
    void reset_enhancement();
    void select_area(int row);
    void commit_drawn_polygon();  // canvas closed → append the drafted area
    void start_new_area();
    void delete_selected();
    void redraw_selected();
    void apply_edited_polygon(const std::vector<camera::Point>& pts);
    void rebuild_zone_choices();
    void sync_zone_editor(std::optional<int> zone);
    // Takes no argument on purpose: it reads whichever of the selected area or
    // the in-progress draft is current, so no call site can pass the wrong one.
    void sync_format_combo();
    void update_format_preview();
    void push_context_areas();  // the non-selected areas, for canvas context
    void update_status();
    void update_controls();
    void attempt_save();
    void cancel_active_draw();
    camera::CameraArea* selected_area();
    QString suggested_name() const;

public:
    /// True while a new area is being drawn or an existing one redrawn — even
    /// with no points placed yet. While this holds, the area list and the step's
    /// other entry points are locked: the operator finishes the shape or
    /// cancels it, so a half-drawn polygon can't evaporate on a stray tap.
    bool has_active_draw() const;
    /// True when leaving now would lose work — an edited set, or a draw in
    /// progress. The dialog asks this before closing so the window's X and
    /// Escape can't bypass the guard the Back/Exit buttons apply.
    bool is_dirty() const;
    bool confirm_discard(const QString& action);

private:
    RoiCanvas* canvas_ = nullptr;
    QListWidget* list_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    ZoneNumberEdit* zone_edit_ = nullptr;
    QComboBox* format_combo_ = nullptr;
    QPushButton* new_btn_ = nullptr;
    QPushButton* redraw_btn_ = nullptr;
    QPushButton* delete_btn_ = nullptr;
    QPushButton* remove_vertex_btn_ = nullptr;
    QPushButton* undo_btn_ = nullptr;
    QPushButton* clear_btn_ = nullptr;
    QPushButton* cancel_btn_ = nullptr;
    QPushButton* done_btn_ = nullptr;
    QPushButton* save_btn_ = nullptr;
    QPushButton* skip_btn_ = nullptr;
    bool unfinished_ = false;
    QLabel* status_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* format_preview_ = nullptr;
    QLabel* review_banner_ = nullptr;  // "re-verify after source change" notice

    // ── Digital ROI image enhancement (per CAMERA, edited here) ──────────────
    // It lives on this page rather than on Configure because this is the only
    // place the operator can see the ROI polygons and judge the result. It is
    // still ONE value for the whole camera: there is deliberately no per-row
    // control in the area list.
    QCheckBox* enable_check_ = nullptr;
    QComboBox* enhance_combo_ = nullptr;          // local contrast
    QSlider* brightness_slider_ = nullptr;
    QSlider* contrast_slider_ = nullptr;
    QSlider* gamma_slider_ = nullptr;             // hundredths
    QSlider* saturation_slider_ = nullptr;
    QLabel* brightness_value_ = nullptr;
    QLabel* contrast_value_ = nullptr;
    QLabel* gamma_value_ = nullptr;
    QLabel* saturation_value_ = nullptr;
    QPushButton* reset_btn_ = nullptr;
    QCheckBox* preview_check_ = nullptr;
    QLabel* enhance_hint_ = nullptr;
    QTimer* preview_timer_ = nullptr;
    /// The unsaved working configuration, and the baseline it is dirty against.
    camera::ImageEnhancement working_{};
    camera::ImageEnhancement loaded_enhancement_{};
    /// The snapshot as captured+oriented. The single source every render reads.
    QImage original_background_;

    std::vector<camera::CameraArea> areas_;        // the working set
    std::vector<camera::CameraArea> loaded_;       // as loaded, for dirty checks
    std::map<int, std::string> zones_taken_;       // zone → other camera's name
    bool drafting_ = false;        // a new area is being drawn but not yet added
    std::string draft_name_;       // its name, typed BEFORE the shape exists
    std::optional<int> draft_zone_;
    int draft_decimal_places_ = 0;   // new zones default to 0000
    int redrawing_row_ = -1;  // row whose shape is being replaced, or -1
    bool review_required_ = false;
};

} // namespace denso::ui
