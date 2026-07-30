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
#include "mode/config.h"            // mode::load (the COMMITTED mode)
#include "models/model_identity.h"  // load_manifest_view
#include "paths/paths.h"
#include "platform/platform_info.h"  // measured_platform_info
#include "ui/common/async_runner.h"

#include <QMessageBox>
#include <QSize>

#include <cstdint>
#include <optional>
#include <map>
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
    preview_.invalidate();  // the source may have just changed; nothing is proven
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

    // A late completion from a PREVIOUS source must not overwrite the preview for
    // the current one (which would let the operator "verify" ROIs against the
    // wrong image). Only the newest capture applies — PreviewGate owns that, and
    // owns whether the settled result was a real frame.
    const uint64_t gen = preview_.begin();

    common::run_on_worker([this, index, url, res, gen] {
        const Snapshot snap = grab_snapshot(index, url, res.width(), res.height());
        common::post_to_gui(this, [this, snap, gen, res] {
            const bool ok = !snap.image.isNull();
            if (!preview_.settle(gen, ok)) {
                return;  // superseded by a newer capture / an invalidate
            }
            pages_.configure->set_capturing(false);
            if (!ok) {
                // Keep last_frame_ on screen so the ROI canvas doesn't blank out
                // mid-edit, but the gate is now NOT live: this failure revoked
                // verification, which is the whole point.
                pages_.configure->set_preview_text(snap.error);
                return;
            }
            last_frame_ = snap.image;
            // The geometry this frame PROVES. Rotation/pitch/roll are re-applied
            // to the raw frame by update_areas_background(), so they don't stale
            // it — but an aspect change does: the frame becomes the wrong SHAPE
            // to verify normalized ROIs against. Recorded so the Areas gate can
            // tell whether the operator changed the aspect after capturing.
            captured_.width = res.width();
            captured_.height = res.height();
            pages_.configure->set_frame(last_frame_);
            update_areas_background();  // refresh the ROI canvas if it's showing
        });
    });
}

void CameraWizardController::save_configured_camera() {
    pages_.configure->read_into(draft_);

    // Nothing here proves the camera actually opens: Next was always enabled, and
    // a failed snapshot only changed a label. Wrong credentials, an unreachable
    // RTSP URL or an unplugged USB device would commit as a live production
    // camera, and the operator would only find out from the grid later — after it
    // had already been reporting nothing.
    //
    // A confirm rather than a hard block: an operator may legitimately need to
    // fix a name or re-point a camera that is temporarily down, and trapping them
    // in the wizard is its own failure. But they say so explicitly.
    if (!preview_.has_live_frame()) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            pages_.configure, QStringLiteral("No live preview"),
            QStringLiteral(
                "This camera has not produced a preview frame, so its source has "
                "not been proven to work.\n\nSave it anyway? It will be treated "
                "as a live camera and report nothing until it connects."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }

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
        // Inserted UNFINISHED. The row has to exist from here on because
        // attaching models needs a real id — but until the operator explicitly
        // finishes, camera::runtime() ignores it, so backing out at Models leaves
        // an inert draft in the list instead of a live, model-less camera that
        // silently reports nothing.
        draft_.setup_complete = false;
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
    // The COMMITTED mode from the database — never the settings page's mode combo,
    // which holds a *proposed* destination until the operator confirms the
    // destructive switch. Reading that would make models appear or vanish from
    // this dialog because someone opened an unrelated drop-down (spec §6.3).
    //
    // Same three authorization inputs the save path uses below, resolved through
    // the same shared loader and provider — so what the page OFFERS and what
    // set_camera_models ACCEPTS are two readings of one rule, and a model can never
    // be offered here and then refused on Finish.
    pages_.models->load_for(editing_id_.value_or(0), denso::mode::load(db_),
                            denso::models::load_manifest_view(denso::paths::models_dir()),
                            denso::platform::measured_platform_info());
    show_page_(3);
}

bool CameraWizardController::save_models_only() {
    if (!editing_id_.has_value()) {
        return true;
    }

    // ── Hidden-attachment guard ──────────────────────────────────────────────
    // A model this camera is ATTACHED to but that the policy no longer allows is
    // ABSENT from the Models page (spec §6.2 — absent, not greyed, not annotated),
    // so selections() cannot carry it and this save would DETACH it. That is the
    // one outcome this whole design exists to prevent: the camera would stop being
    // a diagnosable, inhibited ModelCompatibilityRejected and become an
    // apparently-healthy camera with no model that silently reads nothing.
    //
    // It cannot simply be preserved either — set_camera_models refuses any set
    // containing a rejected model (spec §7.1), and rightly so. So the operator is
    // told exactly what will be removed and decides. Recomputed here from the same
    // three inputs the page was given, rather than trusting page state.
    const denso::mode::TargetMode mode = denso::mode::load(db_);
    const denso::models::ManifestView view =
        denso::models::load_manifest_view(denso::paths::models_dir());
    const denso::models::PlatformInfo platform =
        denso::platform::measured_platform_info();

    std::set<int64_t> offered;
    for (const auto& s : denso::detection::selectable_models(db_, mode, view, platform)) {
        offered.insert(s.row.id);
    }
    std::map<int64_t, std::string> catalog_names;
    for (const auto& m : denso::detection::list_models(db_)) {
        catalog_names[m.id] = denso::models::diagnostic_filename(m.filename);
    }
    QStringList hidden;
    for (const auto& cm : denso::detection::models_for(db_, *editing_id_)) {
        if (offered.count(cm.model_id) == 0) {
            const auto it = catalog_names.find(cm.model_id);
            hidden << QStringLiteral("%1 (catalog #%2)")
                          .arg(it == catalog_names.end()
                                   ? QStringLiteral("<unknown>")
                                   : QString::fromStdString(it->second))
                          .arg(cm.model_id);
        }
    }
    if (!hidden.isEmpty()) {
        const auto answer = QMessageBox::question(
            pages_.models, QStringLiteral("Remove incompatible model?"),
            QStringLiteral(
                "This camera is attached to %1 that the current operating mode "
                "does not allow, so it is not shown above:\n\n  %2\n\nSaving now "
                "REMOVES that attachment. The camera will then have no detection "
                "model and will report nothing — it will no longer be flagged as "
                "incompatible, because nothing will be attached.\n\nRemove it?")
                .arg(hidden.size() == 1 ? QStringLiteral("a model")
                                        : QStringLiteral("models"),
                     hidden.join(QStringLiteral("\n  "))),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return false;   // stay on the Models step; nothing written
        }
    }

    // Honour the transactional write result: if it failed, the camera is NOT
    // configured, so surface the error and stay on the Models page rather
    // than silently reporting success and advancing (operator would believe
    // detection is set up when nothing was saved — no readings, no reports).
    //
    // The mode passed here is the COMMITTED one, read from the database — never
    // the settings-page selector widget, which may hold a choice the operator has
    // not applied (spec §6.3). Attaching against an unconfirmed mode would let the
    // wizard authorize a model the appliance is not actually running.
    denso::detection::AttachRefusal refusal;
    if (!denso::detection::set_camera_models(
            db_, *editing_id_, pages_.models->selections(*editing_id_),
            denso::mode::load(db_),
            denso::models::load_manifest_view(denso::paths::models_dir()),
            denso::platform::measured_platform_info(), &refusal)) {
        // Name the real cause. A compatibility refusal is a decision, not a
        // malfunction: "please try again" would be actively misleading, because
        // trying again does exactly the same thing.
        if (!refusal.policy_reason.empty()) {
            // Wrong-mode is only ONE of the seven ways the policy can refuse.
            // Blaming the operating mode for an undeclared, mis-hashed or
            // wrong-classes model would send the operator to switch a mode that
            // was never the problem — the same reasoning that named the issue kind
            // ModelCompatibilityRejected rather than ModelModeIncompatible.
            const bool wrong_mode = refusal.policy_reason == "model_mode_incompatible";
            const QString lead =
                wrong_mode
                    ? QStringLiteral("\"%1\" cannot be used in the current "
                                     "operating mode and was not attached.")
                    : QStringLiteral("\"%1\" was refused by the model "
                                     "compatibility check and was not attached.");
            QMessageBox::warning(
                pages_.models, QStringLiteral("Model not compatible"),
                (lead + QStringLiteral("\n\nModel: %2 (catalog #%4)\nReason: %3\n\n"
                                       "No change was made to this camera's "
                                       "detection models."))
                    .arg(QString::fromStdString(refusal.filename),
                         QString::fromStdString(refusal.canonical_id),
                         QString::fromStdString(refusal.policy_reason))
                    .arg(refusal.model_id));
        } else {
            QMessageBox::warning(
                pages_.models, QStringLiteral("Save failed"),
                QStringLiteral("Could not save the detection models for this "
                               "camera. Please try again."));
        }
        return false;
    }
    emit cameras_changed();
    return true;
}

void CameraWizardController::save_models() {
    // "Next: Detection areas" — persist, then advance. NOT a finish: the camera
    // stays unfinished until Areas is saved or whole-frame is chosen.
    if (!save_models_only()) {
        return;
    }
    enter_areas(/*direct=*/false);
}

void CameraWizardController::begin_areas_direct(const camera::Camera& cam) {
    // Defense in depth, not just a hidden button: this shortcut skips the Models
    // step, so letting an unfinished camera in would let save_areas() finish a
    // camera that never chose a model. The list hides the entry point; the
    // invariant lives here so a future caller can't reintroduce it.
    if (!cam.setup_complete) {
        begin_edit(cam);  // resume the wizard from the start instead
        return;
    }
    editing_id_ = cam.id;
    draft_ = cam;
    last_frame_ = QImage();
    preview_.invalidate();
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
    // Saving here is what finishes an in-progress camera — the page relabels its
    // terminal actions so neither one lies about that.
    pages_.areas->set_unfinished(editing_id_.has_value() && !draft_.setup_complete);
    update_areas_background();
    show_page_(4);
}

bool CameraWizardController::preview_verifies_draft() const {
    // Liveness alone is not enough. Capture at 1920×1080, then switch the
    // resolution preset to 640×480 and press Next: the aspect change quarantines
    // the ROIs (requires_area_review), the Areas page still shows the retained
    // 16:9 frame, and the gate is still live — so the operator would confirm
    // normalized polygons against a frame of the wrong shape and resume
    // reporting. A same-aspect resolution change is safe (normalized ROIs don't
    // move, which is why requires_area_review ignores it), as are
    // rotation/pitch/roll (re-applied to the raw frame).
    return preview_.has_live_frame() && !camera::aspect_changed(captured_, draft_);
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
    // a LIVE current-source preview first. Without it the operator would be
    // "verifying" ROIs against an image that isn't the camera, which could resume
    // reporting on a misaligned view. Staying paused is safe.
    //
    // Gate on the PreviewGate, not on last_frame_.isNull(): a refresh that failed
    // leaves the previous success on screen, so "not null" was true even though
    // nothing had been verified against the current view — the safety mechanism
    // let itself be bypassed through its own Refresh button.
    if (editing_id_.has_value() && draft_.areas_need_review &&
        !preview_verifies_draft()) {
        QMessageBox::warning(
            pages_.areas, QStringLiteral("Live preview needed"),
            QStringLiteral("Refresh the preview and get a live frame at this "
                           "camera's current resolution before verifying the "
                           "areas — zone reporting stays paused until they are "
                           "checked against the current view."));
        return;
    }
    if (editing_id_.has_value() &&
        !camera::replace_areas(db_, *editing_id_, areas)) {
        pages_.areas->show_save_error();
        return;
    }
    // Only now — the areas write landed, so the setup this finishes is real.
    finish_and_leave(pages_.areas);
}

bool CameraWizardController::finish_setup(QWidget* parent) {
    // Ordering is the contract: only ever called AFTER the models/areas write
    // this completes has itself succeeded. Completing first would let a failed
    // write leave a live camera whose setup never actually landed — the exact
    // bug this slice exists to remove, just later in the flow.
    if (!editing_id_.has_value()) {
        return true;  // nothing was inserted; nothing to finish
    }
    if (draft_.setup_complete) {
        return true;  // editing an already-finished camera
    }
    if (!camera::mark_setup_complete(db_, *editing_id_)) {
        QMessageBox::warning(
            parent, QStringLiteral("Setup not finished"),
            QStringLiteral("The camera's settings were saved, but it could not be "
                           "marked as finished, so it will not start yet. Press "
                           "Finish again to retry."));
        return false;
    }
    draft_.setup_complete = true;
    return true;
}

void CameraWizardController::finish_whole_frame() {
    // The Models step's terminal action. ROI areas are optional — none means
    // detect on the whole frame — so this is a real finish, not an "exit".
    if (!save_models_only()) {
        return;
    }
    if (!editing_id_.has_value()) {
        return;
    }
    // "Use whole frame" has to MEAN it. Marking setup complete while the row
    // still holds areas would leave the camera confined to those ROIs — the
    // button would be lying. So the empty set is persisted, which means it can
    // destroy hand-drawn work: name that consequence rather than doing it
    // silently.
    const std::vector<camera::CameraArea> existing =
        camera::areas_for(db_, *editing_id_);
    if (!existing.empty()) {
        QStringList zones;
        for (const camera::CameraArea& a : existing) {
            if (a.zone) zones << QString::number(*a.zone);
        }
        QString msg = QStringLiteral(
                          "Detect on the whole frame?\n\nThis camera's %1 "
                          "detection area(s) will be deleted.")
                          .arg(existing.size());
        if (!zones.isEmpty()) {
            msg += QStringLiteral(" Zone %1 will stop being reported.")
                       .arg(zones.join(QStringLiteral(", ")));
        }
        if (QMessageBox::question(pages_.models, QStringLiteral("Use whole frame"),
                                  msg, QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
    }
    // replace_areas also clears areas_need_review, which is right here: the
    // quarantine exists to stop ROIs being trusted against an unverified view,
    // and a camera with no ROIs has nothing to misalign and reports no zones.
    if (!camera::replace_areas(db_, *editing_id_, {})) {
        QMessageBox::warning(pages_.models, QStringLiteral("Save failed"),
                             QStringLiteral("Could not clear this camera's "
                                            "detection areas. Please try again."));
        return;
    }
    draft_.areas_need_review = false;
    finish_and_leave(pages_.models);
}

void CameraWizardController::finish_and_leave(QWidget* parent) {
    // The ONE owner of "a terminal action succeeded": complete, tell the app, and
    // return to the list ONLY if the completion landed. Both terminal actions
    // ("Finish — use whole frame", "Save areas & finish setup") had their own
    // copy of this sequence and both got it wrong the same way — each warned that
    // setup had NOT finished and then went back to the list anyway, turning an
    // explicit Finish into a silent "leave it unfinished" and taking away the
    // page where the operator could retry. Call this AFTER your write succeeds.
    const bool completed = finish_setup(parent);
    emit cameras_changed();  // the write landed either way
    if (completed) {
        emit request_show_list();
    }
    // else: stay put. The data is saved, so pressing the button again retries.
}

void CameraWizardController::areas_back() {
    // Direct entry (per-row Areas) has no Models step to return to.
    if (entered_areas_directly_) {
        emit request_show_list();
        return;
    }
    // RE-ENTER, never just show_page_(3). The Models page is created once and
    // reused for the whole application lifetime, so raising page 3 without
    // reloading renders whatever the last load_for() left behind — a list filtered
    // for a mode that may no longer be committed, against a manifest that may have
    // been reseeded since. enter_models() re-reads all three authorization inputs
    // (committed mode, manifest view, measured platform) and rebuilds the model
    // cards AND the class rows from them.
    //
    // Lossless: save_models() persists the selections through save_models_only()
    // before enter_areas() ever runs, so reseeding from the database restores
    // exactly what was saved.
    enter_models();
}

} // namespace denso::ui
