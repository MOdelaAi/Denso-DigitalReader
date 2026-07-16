#include "ui/camera/wizard_controller.h"

#include "camera/repo.h"
#include "camera/source_change.h"
#include "detection/repo.h"
#include "ui/camera/dialog/add_page.h"
#include "ui/camera/dialog/areas_page.h"
#include "ui/camera/dialog/configure_page.h"
#include "ui/camera/dialog/models_page.h"
#include "camera/rtsp_templates.h"  // with_credentials
#include "camera/snapshot.h"        // grab_snapshot, apply_orientation
#include "ui/common/async_runner.h"

#include <QMessageBox>
#include <QSize>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace denso::ui {

CameraWizardController::CameraWizardController(QSqlDatabase db, Pages pages,
                                              std::function<void(int)> show_page,
                                              QObject* parent)
    : QObject(parent), db_(std::move(db)), pages_(pages),
      show_page_(std::move(show_page)) {}

void CameraWizardController::push_used_sources() {
    // Sources already owned by OTHER cameras, so the Source-page scan can tag them
    // "(in use)". Exclude the camera being edited (its own IP isn't a conflict).
    std::set<std::string> ips;
    std::set<uint32_t> usb;
    for (const camera::Camera& c : camera::all(db_)) {
        if (editing_id_.has_value() && c.id == *editing_id_) continue;
        if (c.ip) ips.insert(*c.ip);
        if (c.index) usb.insert(*c.index);
    }
    pages_.add->set_used_sources(std::move(ips), std::move(usb));
}

void CameraWizardController::begin_add() {
    editing_id_ = std::nullopt;
    original_ = camera::Camera{};
    draft_ = camera::Camera{};
    push_used_sources();  // before reset() — scan_usb() reads the used set
    pages_.add->reset();
    show_page_(1);
}

void CameraWizardController::begin_edit(const camera::Camera& cam) {
    editing_id_ = cam.id;
    original_ = cam;
    draft_ = cam;
    push_used_sources();  // before populate() — scan_usb() reads the used set
    pages_.add->populate(cam);
    show_page_(1);
}

void CameraWizardController::accept_source(const camera::Camera& source) {
    if (editing_id_.has_value()) {
        // Merge only the SOURCE fields into the existing draft — keep id, active,
        // capture geometry (width/height/fps/rotation/pitch/roll) and the review
        // flag, which the Source page does not own. (Replacing the whole object
        // would reset those.)
        draft_.name = source.name;
        draft_.camera_type = source.camera_type;
        draft_.index = source.index;
        draft_.ip = source.ip;
        draft_.rtsp = source.rtsp;
        draft_.username = source.username;
        draft_.password = source.password;
        draft_.channel = source.channel;
        draft_.stream = source.stream;
        draft_.manufacturer = source.manufacturer;
    } else {
        draft_ = source;  // fresh add
    }
    open_configure(editing_id_.has_value()
                       ? QStringLiteral("Capturing…")
                       : QStringLiteral("Click Capture to preview"));
}

void CameraWizardController::open_configure(const QString& preview_text) {
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

    // Generation token: a late completion from a PREVIOUS source must not
    // overwrite the preview for the current one (which would let the operator
    // "verify" ROIs against the wrong image). Only the newest capture applies.
    const uint64_t gen = ++capture_gen_;

    common::run_on_worker([this, index, url, res, gen] {
        const Snapshot snap = grab_snapshot(index, url, res.width(), res.height());
        common::post_to_gui(this, [this, snap, gen] {
            if (gen != capture_gen_) return;  // superseded by a newer capture
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
        // Quarantine the ROIs for re-verification when this edit changed the view
        // (source or capture geometry). Set UNCONDITIONALLY on a significant
        // change — not gated on areas_for() being non-empty, which fails open on a
        // query error (empty is returned for both "no areas" and "query failed").
        // A camera with no areas simply has nothing to gate, and the next
        // areas-save clears the flag. Persisted with the update, so the reloaded
        // grid immediately excludes those areas + pauses zone reporting until the
        // operator verifies them (Areas save clears it). A cosmetic edit
        // (credentials/name) leaves any existing flag untouched.
        if (camera::requires_area_review(original_, draft_)) {
            draft_.areas_need_review = true;
        }
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
    // Source is now editable in both add and edit, so Back returns to it.
    show_page_(1);
}

void CameraWizardController::enter_models() {
    pages_.models->load_for(editing_id_.value_or(0));
    show_page_(3);
}

void CameraWizardController::save_models() {
    if (editing_id_.has_value()) {
        // Honour the transactional write result: if it failed, the camera is NOT
        // configured, so surface the error and stay on the Models page rather
        // than silently reporting success and advancing (operator would believe
        // detection is set up when nothing was saved — no readings, no reports).
        if (!denso::detection::set_camera_models(
                db_, *editing_id_, pages_.models->selections(*editing_id_))) {
            QMessageBox::warning(
                pages_.models, QStringLiteral("Save failed"),
                QStringLiteral("Could not save the detection models for this "
                               "camera. Please try again."));
            return;
        }
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
    // Zones are unique machine-wide, so the page needs to know which numbers
    // other cameras already hold — it disables them in the picker and names the
    // owner, instead of letting replace_areas reject the save with no reason.
    pages_.areas->load(editing_id_.has_value()
                           ? camera::areas_for(db_, *editing_id_)
                           : std::vector<camera::CameraArea>{},
                       camera::zones_owned_by_other_cameras(
                           db_, editing_id_.value_or(0)));
    // Prompt re-verification when this edit quarantined the ROIs; saving the areas
    // (replace_areas) clears the flag and resumes reporting.
    pages_.areas->set_review_required(editing_id_.has_value() &&
                                      draft_.areas_need_review);
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
    // Under quarantine, saving CLEARS the flag and resumes reporting — so require
    // a valid current-source preview first. Without it the operator would be
    // "verifying" ROIs against an image they can't see (offline / failed capture),
    // which could resume reporting on a misaligned view. Staying paused is safe.
    if (editing_id_.has_value() && draft_.areas_need_review && last_frame_.isNull()) {
        QMessageBox::warning(
            pages_.areas, QStringLiteral("Preview needed"),
            QStringLiteral("Capture a live preview of the new source before "
                           "verifying the areas — zone reporting stays paused "
                           "until they are checked against the current view."));
        return;
    }
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
