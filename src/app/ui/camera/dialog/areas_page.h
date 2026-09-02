// The Camera dialog's "Areas" step: draw and reshape ROI polygons over the
// camera snapshot. Holds a working set of named areas; the controller loads the
// existing ones plus the zone numbers other cameras already own, pushes the
// oriented background frame, and persists on save_requested. The page owns no DB
// access — it just edits polygons, and validates the set before asking for a
// save so a deterministic problem (zone clash, unfinished shape, degenerate
// polygon) is named here instead of surfacing as a generic write failure.
#pragma once

#include "camera/camera.h"

#include <QImage>
#include <QWidget>

#include <map>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

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
    void set_background(const QImage& oriented);  // canvas backdrop
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
