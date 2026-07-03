#include "ui/camera/wizard_controller.h"

#include "camera/repo.h"
#include "detection/repo.h"
#include "ui/camera/dialog/areas_page.h"
#include "ui/camera/dialog/configure_page.h"
#include "ui/camera/dialog/models_page.h"
#include "ui/camera/shared/rtsp_templates.h"  // with_credentials
#include "ui/camera/shared/snapshot.h"        // grab_snapshot, apply_orientation
#include "ui/common/async_runner.h"

#include <QSize>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace denso::ui {

CameraWizardController::CameraWizardController(QSqlDatabase db, Pages pages,
                                              std::function<void(int)> show_page,
                                              QObject* parent)
    : QObject(parent), db_(std::move(db)), pages_(pages),
      show_page_(std::move(show_page)) {}

void CameraWizardController::begin_configure(const camera::Camera& cam,
                                             std::optional<int64_t> id,
                                             const QString& preview_text) {
    editing_id_ = id;
    draft_ = cam;
    last_frame_ = QImage();
    pages_.configure->populate(draft_);
    pages_.configure->set_preview_text(preview_text);
    pages_.configure->clear_error();
    show_page_(2);
    capture_snapshot();
}

void CameraWizardController::capture_snapshot() {
    pages_.configure->set_capturing(true);
    pages_.configure->set_preview_text(QStringLiteral("Capturing…"));

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
    const QSize res = pages_.configure->resolution();

    common::run_on_worker([this, index, url, res] {
        const Snapshot snap = grab_snapshot(index, url, res.width(), res.height());
        common::post_to_gui(this, [this, snap] {
            pages_.configure->set_capturing(false);
            if (snap.image.isNull()) {
                pages_.configure->set_preview_text(snap.error);
                return;
            }
            last_frame_ = snap.image;
            pages_.configure->set_frame(last_frame_);
            update_areas_background();  // refresh the ROI canvas if it's showing
        });
    });
}

void CameraWizardController::save_configured_camera() {
    pages_.configure->read_into(draft_);

    if (editing_id_.has_value()) {
        draft_.id = *editing_id_;
        if (!camera::update(db_, draft_)) {
            pages_.configure->show_error(QStringLiteral("Failed to save the camera."));
            return;
        }
    } else {
        const auto new_id = camera::insert(db_, draft_);
        if (!new_id.has_value()) {
            pages_.configure->show_error(QStringLiteral("Failed to save the camera."));
            return;
        }
        editing_id_ = *new_id;
        draft_.id = *new_id;
    }
    emit cameras_changed();
    enter_models();
}

void CameraWizardController::configure_back() {
    // Editing an existing camera has no Source step to return to.
    if (editing_id_.has_value())
        emit request_show_list();
    else
        show_page_(1);
}

void CameraWizardController::enter_models() {
    pages_.models->load_for(editing_id_.value_or(0));
    show_page_(3);
}

void CameraWizardController::save_models() {
    if (editing_id_.has_value()) {
        denso::detection::set_camera_models(
            db_, *editing_id_, pages_.models->selections(*editing_id_));
        emit cameras_changed();
    }
    enter_areas(/*direct=*/false);
}

void CameraWizardController::begin_areas_direct(const camera::Camera& cam) {
    editing_id_ = cam.id;
    draft_ = cam;
    last_frame_ = QImage();
    enter_areas(/*direct=*/true);
    capture_snapshot();
}

void CameraWizardController::enter_areas(bool direct) {
    entered_areas_directly_ = direct;
    pages_.areas->load(editing_id_.has_value()
                           ? camera::areas_for(db_, *editing_id_)
                           : std::vector<camera::CameraArea>{});
    update_areas_background();
    show_page_(4);
}

void CameraWizardController::update_areas_background() {
    if (last_frame_.isNull()) {
        pages_.areas->set_background(QImage());
        return;
    }
    pages_.areas->set_background(apply_orientation(
        last_frame_, static_cast<int>(draft_.rotation), draft_.pitch, draft_.roll));
}

void CameraWizardController::save_areas(const std::vector<camera::CameraArea>& areas) {
    if (editing_id_.has_value() &&
        !camera::replace_areas(db_, *editing_id_, areas)) {
        pages_.areas->show_save_error();
        return;
    }
    emit request_show_list();
}

void CameraWizardController::areas_back() {
    // Direct entry (per-row Areas) has no Models step to return to.
    if (entered_areas_directly_)
        emit request_show_list();
    else
        show_page_(3);
}

} // namespace denso::ui
