#include "ui/camera/camera_dialog.h"

#include "camera/camera.h"
#include "camera/repo.h"
#include "ui/camera/dialog/add_page.h"
#include "ui/camera/dialog/areas_page.h"
#include "ui/camera/dialog/configure_page.h"
#include "ui/camera/dialog/list_page.h"
#include "ui/camera/shared/rtsp_templates.h"  // with_credentials
#include "ui/camera/shared/snapshot.h"
#include "ui/camera/dialog/wizard_stepper.h"

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QSize>
#include <QStackedWidget>
#include <QThread>
#include <QVBoxLayout>

#include <cstdint>
#include <optional>
#include <vector>

namespace denso::ui {
namespace {

/// Header chrome: "Camera" title + close glyph + gold rule.
QVBoxLayout* header(QDialog* dlg) {
    auto* h = new QVBoxLayout;
    h->setSpacing(10);
    auto* row = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("Camera"));
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() + 6.0);
    title->setFont(tf);
    auto* close_glyph = new QPushButton(QStringLiteral("✕"));
    close_glyph->setProperty("flatText", true);
    close_glyph->setFixedSize(28, 28);
    QObject::connect(close_glyph, &QPushButton::clicked, dlg, &QDialog::reject);
    row->addWidget(title, 1);
    row->addWidget(close_glyph, 0);
    h->addLayout(row);
    auto* underline = new QFrame;
    underline->setObjectName(QStringLiteral("goldUnderline"));
    underline->setFixedSize(48, 3);
    h->addWidget(underline, 0, Qt::AlignLeft);
    return h;
}

} // namespace

CameraDialog::CameraDialog(QSqlDatabase db, QWidget* parent)
    : QDialog(parent), db_(std::move(db)) {
    setWindowTitle(QStringLiteral("Camera"));
    setObjectName(QStringLiteral("dialogPanel"));
    resize(760, 600);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 24, 24, 24);
    outer->setSpacing(22);
    outer->addLayout(header(this));

    // Wizard step indicator — shown only during the add/edit flow (pages 1–3),
    // hidden on the list; driven by show_page().
    stepper_ = new WizardStepper(
        {QStringLiteral("Source"), QStringLiteral("Configure"),
         QStringLiteral("Areas")});
    stepper_->setVisible(false);
    outer->addWidget(stepper_, 0);

    stack_ = new QStackedWidget;

    // ── List page (index 0) ───────────────────────────────────────────────
    list_page_ = new CameraListPage(db_);
    connect(list_page_, &CameraListPage::add_requested, this, &CameraDialog::show_add);
    connect(list_page_, &CameraListPage::configure_requested, this,
            [this](const camera::Camera& cam) {
                begin_configure(cam, cam.id, QStringLiteral("Capturing…"));
            });
    connect(list_page_, &CameraListPage::areas_requested, this,
            [this](const camera::Camera& cam) {
                editing_id_ = cam.id;
                draft_ = cam;
                last_frame_ = QImage();
                enter_areas(/*direct=*/true);
                capture_snapshot();
            });
    connect(list_page_, &CameraListPage::changed, this, &CameraDialog::cameras_changed);
    stack_->addWidget(list_page_);

    // ── Add / Source page (index 1) ───────────────────────────────────────
    add_page_ = new CameraAddPage;
    connect(add_page_, &CameraAddPage::cancel_requested, this, &CameraDialog::show_list);
    connect(add_page_, &CameraAddPage::next_requested, this,
            [this](const camera::Camera& draft) {
                begin_configure(draft, std::nullopt,
                                QStringLiteral("Click Capture to preview"));
            });
    stack_->addWidget(add_page_);

    // ── Configure page (index 2) ──────────────────────────────────────────
    configure_page_ = new CameraConfigurePage;
    connect(configure_page_, &CameraConfigurePage::back_requested, this, [this] {
        // Editing an existing camera has no Source step to return to.
        if (editing_id_.has_value()) show_list();
        else show_page(1);
    });
    connect(configure_page_, &CameraConfigurePage::next_requested, this,
            &CameraDialog::save_configured_camera);
    connect(configure_page_, &CameraConfigurePage::capture_requested, this,
            &CameraDialog::capture_snapshot);
    stack_->addWidget(configure_page_);

    // ── Areas page (index 3) ──────────────────────────────────────────────
    areas_page_ = new CameraAreasPage;
    connect(areas_page_, &CameraAreasPage::back_requested, this, [this] {
        // Direct entry (per-row Areas) has no Configure step to return to.
        if (entered_areas_directly_) show_list();
        else show_page(2);
    });
    connect(areas_page_, &CameraAreasPage::skip_requested, this, &CameraDialog::show_list);
    connect(areas_page_, &CameraAreasPage::save_requested, this, &CameraDialog::save_areas);
    stack_->addWidget(areas_page_);

    outer->addWidget(stack_, 1);

    list_page_->reload();
}

void CameraDialog::showEvent(QShowEvent* e) {
    QDialog::showEvent(e);
    // The dialog is created once and reused; always reopen on the list page at
    // the compact size, even if it was closed mid-flow on the expanded Areas
    // step.
    show_list();
}

void CameraDialog::show_page(int index) {
    // The stepper belongs to the add/edit flow (pages 1–3), not the list.
    stepper_->setVisible(index >= 1);
    if (index >= 1) {
        stepper_->set_current(index - 1);  // page 1→step 0, 2→1, 3→2
    }
    // Near-fullscreen only while drawing areas; restore the compact size else.
    if (index == 3) {
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
    add_page_->reset();
    show_page(1);
}

void CameraDialog::begin_configure(const camera::Camera& cam,
                                   std::optional<int64_t> id,
                                   const QString& preview_text) {
    editing_id_ = id;
    draft_ = cam;
    last_frame_ = QImage();
    configure_page_->populate(draft_);
    configure_page_->set_preview_text(preview_text);
    configure_page_->clear_error();
    show_page(2);
    capture_snapshot();
}

void CameraDialog::capture_snapshot() {
    configure_page_->set_capturing(true);
    configure_page_->set_preview_text(QStringLiteral("Capturing…"));

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
    const QSize res = configure_page_->resolution();

    auto* thread = QThread::create([this, index, url, res] {
        const Snapshot snap = grab_snapshot(index, url, res.width(), res.height());
        QMetaObject::invokeMethod(
            this,
            [this, snap] {
                configure_page_->set_capturing(false);
                if (snap.image.isNull()) {
                    configure_page_->set_preview_text(snap.error);
                    return;
                }
                last_frame_ = snap.image;
                configure_page_->set_frame(last_frame_);
                update_areas_background();  // refresh the ROI canvas if it's showing
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void CameraDialog::save_configured_camera() {
    configure_page_->read_into(draft_);

    if (editing_id_.has_value()) {
        draft_.id = *editing_id_;
        if (!camera::update(db_, draft_)) {
            configure_page_->show_error(QStringLiteral("Failed to save the camera."));
            return;
        }
    } else {
        const auto new_id = camera::insert(db_, draft_);
        if (!new_id.has_value()) {
            configure_page_->show_error(QStringLiteral("Failed to save the camera."));
            return;
        }
        // Adopt the assigned id so the (optional) Areas step that follows can
        // attach ROIs to the just-inserted camera.
        editing_id_ = *new_id;
        draft_.id = *new_id;
    }
    emit cameras_changed();
    // Advance to the optional Draw-ROI step; the snapshot is already captured,
    // so reuse it. Reached via Next, so Back from Areas returns to Configure.
    enter_areas(/*direct=*/false);
}

void CameraDialog::enter_areas(bool direct) {
    entered_areas_directly_ = direct;
    areas_page_->load(editing_id_.has_value()
                          ? camera::areas_for(db_, *editing_id_)
                          : std::vector<camera::CameraArea>{});
    update_areas_background();
    show_page(3);
}

void CameraDialog::update_areas_background() {
    if (last_frame_.isNull()) {
        areas_page_->set_background(QImage());
        return;
    }
    areas_page_->set_background(apply_orientation(
        last_frame_, static_cast<int>(draft_.rotation), draft_.pitch, draft_.roll));
}

void CameraDialog::save_areas(const std::vector<camera::CameraArea>& areas) {
    if (editing_id_.has_value() &&
        !camera::replace_areas(db_, *editing_id_, areas)) {
        areas_page_->show_save_error();
        return;
    }
    show_list();
}

} // namespace denso::ui
