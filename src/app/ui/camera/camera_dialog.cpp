#include "ui/camera/camera_dialog.h"

#include "camera/camera.h"
#include "ui/camera/dialog/add_page.h"
#include "ui/camera/dialog/areas_page.h"
#include "ui/camera/dialog/configure_page.h"
#include "ui/camera/dialog/list_page.h"
#include "ui/camera/dialog/models_page.h"
#include "ui/camera/dialog/wizard_stepper.h"
#include "ui/camera/wizard_controller.h"
#include "ui/common/dialog_chrome.h"

#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QShowEvent>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <optional>

namespace denso::ui {

CameraDialog::CameraDialog(QSqlDatabase db, QWidget* parent)
    : QDialog(parent), db_(std::move(db)) {
    setWindowTitle(QStringLiteral("Camera"));
    setObjectName(QStringLiteral("dialogPanel"));
    resize(760, 600);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 24, 24, 24);
    outer->setSpacing(22);
    outer->addLayout(common::dialog_header(this, QStringLiteral("Camera")));

    // Wizard step indicator — shown only during the add/edit flow (pages 1–3),
    // hidden on the list; driven by show_page().
    stepper_ = new WizardStepper(
        {QStringLiteral("Source"), QStringLiteral("Configure"),
         QStringLiteral("Models"), QStringLiteral("Areas")});
    stepper_->setVisible(false);
    outer->addWidget(stepper_, 0);

    stack_ = new QStackedWidget;

    list_page_ = new CameraListPage(db_);            // index 0
    add_page_ = new CameraAddPage;                   // index 1
    configure_page_ = new CameraConfigurePage;       // index 2
    models_page_ = new ModelsPage;                   // index 3
    areas_page_ = new CameraAreasPage;               // index 4
    models_page_->set_db(db_);
    stack_->addWidget(list_page_);
    stack_->addWidget(add_page_);
    stack_->addWidget(configure_page_);
    stack_->addWidget(models_page_);
    stack_->addWidget(areas_page_);
    outer->addWidget(stack_, 1);

    // The controller owns flow-state + persistence; the dialog owns widgets +
    // sizing. It drives page switches through this show_page callback.
    controller_ = new CameraWizardController(
        db_,
        CameraWizardController::Pages{add_page_, configure_page_, models_page_,
                                      areas_page_},
        [this](int index) { show_page(index); }, this);
    connect(controller_, &CameraWizardController::cameras_changed, this,
            &CameraDialog::cameras_changed);
    connect(controller_, &CameraWizardController::request_show_list, this,
            &CameraDialog::show_list);

    // ── List page signals ─────────────────────────────────────────────────
    connect(list_page_, &CameraListPage::add_requested, this, &CameraDialog::show_add);
    connect(list_page_, &CameraListPage::configure_requested, this,
            [this](const camera::Camera& cam) { controller_->begin_edit(cam); });
    connect(list_page_, &CameraListPage::areas_requested, this,
            [this](const camera::Camera& cam) { controller_->begin_areas_direct(cam); });
    connect(list_page_, &CameraListPage::changed, this, &CameraDialog::cameras_changed);

    // ── Add / Source page signals ─────────────────────────────────────────
    connect(add_page_, &CameraAddPage::cancel_requested, this, &CameraDialog::show_list);
    connect(add_page_, &CameraAddPage::next_requested, controller_,
            &CameraWizardController::accept_source);

    // ── Configure page signals ────────────────────────────────────────────
    connect(configure_page_, &CameraConfigurePage::back_requested, controller_,
            &CameraWizardController::configure_back);
    connect(configure_page_, &CameraConfigurePage::next_requested, controller_,
            &CameraWizardController::save_configured_camera);
    connect(configure_page_, &CameraConfigurePage::capture_requested, controller_,
            &CameraWizardController::capture_snapshot);

    // ── Models page signals ───────────────────────────────────────────────
    connect(models_page_, &ModelsPage::back_requested, this, [this] { show_page(2); });
    connect(models_page_, &ModelsPage::finish_requested, controller_,
            &CameraWizardController::save_models);

    // ── Areas page signals ────────────────────────────────────────────────
    connect(areas_page_, &CameraAreasPage::back_requested, controller_,
            &CameraWizardController::areas_back);
    connect(areas_page_, &CameraAreasPage::skip_requested, this, &CameraDialog::show_list);
    connect(areas_page_, &CameraAreasPage::refresh_preview_requested, controller_,
            &CameraWizardController::capture_snapshot);
    connect(areas_page_, &CameraAreasPage::save_requested, controller_,
            &CameraWizardController::save_areas);

    list_page_->reload();
}

void CameraDialog::showEvent(QShowEvent* e) {
    QDialog::showEvent(e);
    // The dialog is created once and reused; always reopen on the list page at
    // the compact size, even if it was closed mid-flow on the expanded Areas step.
    show_list();
}

void CameraDialog::reject() {
    // showEvent() reopens on the list, so a dismissal here silently drops any
    // in-progress ROI work — the Areas page's own Back/Exit guard never runs.
    if (stack_->currentWidget() == areas_page_ &&
        !areas_page_->confirm_discard(QStringLiteral("Close"))) {
        return;
    }
    QDialog::reject();
}

void CameraDialog::show_page(int index) {
    // The stepper belongs to the add/edit flow (pages 1–3), not the list.
    stepper_->setVisible(index >= 1);
    if (index >= 1) {
        stepper_->set_current(index - 1);  // page 1→step 0, 2→1, 3→2
    }
    // Near-fullscreen only while drawing areas; restore the compact size else.
    if (index == 4) {
        expand_for_areas();
    } else {
        restore_size();
    }
    stack_->setCurrentIndex(index);
}

void CameraDialog::expand_for_areas() {
    if (areas_expanded_) {
        return;
    }
    pre_areas_geometry_ = geometry();
    areas_expanded_ = true;
    if (QScreen* s = screen()) {
        const QRect avail = s->availableGeometry();
        const int w = static_cast<int>(avail.width() * 0.92);
        const int h = static_cast<int>(avail.height() * 0.92);
        resize(w, h);
        move(avail.center() - QPoint(w / 2, h / 2));
    }
}

void CameraDialog::restore_size() {
    if (!areas_expanded_) {
        return;
    }
    areas_expanded_ = false;
    setGeometry(pre_areas_geometry_);
}

void CameraDialog::show_list() {
    list_page_->reload();
    show_page(0);
}

void CameraDialog::show_add() {
    controller_->begin_add();  // resets the Source page + clears any edit state
}

} // namespace denso::ui
