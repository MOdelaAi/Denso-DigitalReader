// The Camera dialog's "Areas" step: draw ROI polygons over the camera snapshot.
// Holds a working set of named areas; the coordinator loads the existing ones,
// pushes the oriented background frame, and persists on save_requested. The page
// owns no DB access — it just edits polygons.
#pragma once

#include "camera/camera.h"

#include <QImage>
#include <QWidget>

#include <vector>

class QLineEdit;
class QListWidget;
class QLabel;
class QSpinBox;

namespace denso::ui {

class RoiCanvas;

class CameraAreasPage : public QWidget {
    Q_OBJECT

public:
    explicit CameraAreasPage(QWidget* parent = nullptr);

    void load(std::vector<camera::CameraArea> areas);  // existing set to edit
    void set_background(const QImage& oriented);        // canvas backdrop
    void show_save_error();                             // persistence failed
    /// Show/hide the "re-verify after a source change" banner. When on, the
    /// camera's zone reporting is paused until these areas are saved (verified).
    void set_review_required(bool on);

signals:
    void back_requested();
    void skip_requested();
    void save_requested(const std::vector<camera::CameraArea>& areas);

private:
    void refresh_list();
    void select_area(int row);
    void commit_drawn_polygon();  // canvas closed → append a new area

    RoiCanvas* canvas_ = nullptr;
    QListWidget* list_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QSpinBox* zone_edit_ = nullptr;  // 0 = ROI-only; 1..12 = reporting zone
    QLabel* hint_ = nullptr;
    QLabel* review_banner_ = nullptr;  // "re-verify after source change" notice
    std::vector<camera::CameraArea> areas_;
};

} // namespace denso::ui
