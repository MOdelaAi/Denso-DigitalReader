#include "ui/camera_dialog.h"

#include "camera/model.h"
#include "camera/repo.h"
#include "ui/camera_devices.h"

#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <cstdint>
#include <vector>

namespace denso::ui {
namespace {

constexpr const char* kStatusBad = "#ef4444";

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

QLabel* dim_label(const QString& text) {
    auto* l = new QLabel(text);
    l->setProperty("dim", true);
    return l;
}

} // namespace

CameraDialog::CameraDialog(QSqlDatabase db, QWidget* parent)
    : QDialog(parent), db_(std::move(db)) {
    setWindowTitle(QStringLiteral("Camera"));
    setObjectName(QStringLiteral("dialogPanel"));
    resize(720, 560);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 24, 24, 24);
    outer->setSpacing(22);
    outer->addLayout(header(this));

    stack_ = new QStackedWidget;

    // ── List page ─────────────────────────────────────────────────────────
    auto* list_page = new QWidget;
    auto* list_v = new QVBoxLayout(list_page);
    list_v->setContentsMargins(0, 0, 0, 0);
    list_v->setSpacing(10);

    empty_label_ = dim_label(QStringLiteral("No cameras — add one to get started."));
    empty_label_->setAlignment(Qt::AlignCenter);
    list_v->addWidget(empty_label_);

    auto* rows_host = new QWidget;
    rows_box_ = new QVBoxLayout(rows_host);
    rows_box_->setContentsMargins(0, 0, 0, 0);
    rows_box_->setSpacing(8);
    list_v->addWidget(rows_host);
    list_v->addStretch(1);

    auto* list_footer = new QHBoxLayout;
    list_footer->addStretch(1);
    auto* add_btn = new QPushButton(QStringLiteral("+ Add Camera"));
    add_btn->setProperty("gold", true);
    connect(add_btn, &QPushButton::clicked, this, &CameraDialog::show_add);
    list_footer->addWidget(add_btn, 0);
    list_v->addLayout(list_footer);

    stack_->addWidget(list_page);

    // ── Add page ──────────────────────────────────────────────────────────
    auto* add_page = new QWidget;
    auto* add_v = new QVBoxLayout(add_page);
    add_v->setContentsMargins(0, 0, 0, 0);
    add_v->setSpacing(12);

    auto* type_row = new QHBoxLayout;
    usb_radio_ = new QRadioButton(QStringLiteral("USB"));
    ip_radio_ = new QRadioButton(QStringLiteral("IP / RTSP"));
    usb_radio_->setChecked(true);
    connect(usb_radio_, &QRadioButton::toggled, this, &CameraDialog::update_source_fields);
    type_row->addWidget(usb_radio_, 0);
    type_row->addWidget(ip_radio_, 0);
    type_row->addStretch(1);
    add_v->addLayout(type_row);

    auto* name_row = new QHBoxLayout;
    name_row->addWidget(dim_label(QStringLiteral("Name")), 0);
    name_edit_ = new QLineEdit;
    name_edit_->setPlaceholderText(QStringLiteral("Camera name"));
    name_row->addWidget(name_edit_, 1);
    add_v->addLayout(name_row);

    // USB inputs
    usb_box_ = new QWidget;
    auto* usb_l = new QHBoxLayout(usb_box_);
    usb_l->setContentsMargins(0, 0, 0, 0);
    usb_l->addWidget(dim_label(QStringLiteral("Device")), 0);
    usb_combo_ = new QComboBox;
    usb_l->addWidget(usb_combo_, 1);
    add_v->addWidget(usb_box_);

    // IP inputs
    ip_box_ = new QWidget;
    auto* ip_l = new QVBoxLayout(ip_box_);
    ip_l->setContentsMargins(0, 0, 0, 0);
    ip_l->setSpacing(8);
    auto* rtsp_row = new QHBoxLayout;
    rtsp_row->addWidget(dim_label(QStringLiteral("RTSP URL")), 0);
    rtsp_edit_ = new QLineEdit;
    rtsp_edit_->setPlaceholderText(QStringLiteral("rtsp://192.168.1.20:554/stream"));
    rtsp_row->addWidget(rtsp_edit_, 1);
    ip_l->addLayout(rtsp_row);
    auto* user_row = new QHBoxLayout;
    user_row->addWidget(dim_label(QStringLiteral("Username")), 0);
    user_edit_ = new QLineEdit;
    user_edit_->setPlaceholderText(QStringLiteral("optional"));
    user_row->addWidget(user_edit_, 1);
    ip_l->addLayout(user_row);
    add_v->addWidget(ip_box_);

    add_error_ = new QLabel;
    add_error_->setStyleSheet(QStringLiteral("color: %1;").arg(QString::fromLatin1(kStatusBad)));
    add_error_->setVisible(false);
    add_v->addWidget(add_error_);
    add_v->addStretch(1);

    auto* add_footer = new QHBoxLayout;
    auto* cancel = new QPushButton(QStringLiteral("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &CameraDialog::show_list);
    add_footer->addWidget(cancel, 0);
    add_footer->addStretch(1);
    auto* save = new QPushButton(QStringLiteral("Save"));
    save->setProperty("gold", true);
    connect(save, &QPushButton::clicked, this, &CameraDialog::save_new_camera);
    add_footer->addWidget(save, 0);
    add_v->addLayout(add_footer);

    stack_->addWidget(add_page);

    outer->addWidget(stack_, 1);

    rebuild_list();
}

void CameraDialog::show_list() {
    rebuild_list();
    stack_->setCurrentIndex(0);
}

void CameraDialog::show_add() {
    // Reset the form and refresh the detected-device list.
    usb_radio_->setChecked(true);
    name_edit_->clear();
    rtsp_edit_->clear();
    user_edit_->clear();
    add_error_->setVisible(false);

    usb_combo_->clear();
    for (const UsbCamera& cam : list_usb_cameras()) {
        usb_combo_->addItem(cam.name, cam.index);
    }
    if (usb_combo_->count() == 0) {
        usb_combo_->addItem(QStringLiteral("No USB cameras found"), -1);
    }

    update_source_fields();
    stack_->setCurrentIndex(1);
}

void CameraDialog::update_source_fields() {
    const bool usb = usb_radio_->isChecked();
    usb_box_->setVisible(usb);
    ip_box_->setVisible(!usb);
}

void CameraDialog::rebuild_list() {
    while (QLayoutItem* item = rows_box_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    const std::vector<camera::Camera> cams = camera::all(db_);
    empty_label_->setVisible(cams.empty());

    for (const camera::Camera& cam : cams) {
        auto* row = new QWidget;
        row->setObjectName(QStringLiteral("card"));
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(12, 8, 12, 8);
        rl->setSpacing(8);

        rl->addWidget(new QLabel(QString::fromStdString(cam.name)), 1);

        auto* badge = dim_label(cam.camera_type == "ip" ? QStringLiteral("IP")
                                                         : QStringLiteral("USB"));
        rl->addWidget(badge, 0);

        auto* del = new QPushButton(QStringLiteral("Delete"));
        del->setProperty("flatText", true);
        const int64_t id = cam.id;
        connect(del, &QPushButton::clicked, this, [this, id] {
            camera::remove(db_, id);
            emit cameras_changed();
            rebuild_list();
        });
        rl->addWidget(del, 0);

        rows_box_->addWidget(row);
    }
}

void CameraDialog::save_new_camera() {
    const auto fail = [this](const QString& msg) {
        add_error_->setText(msg);
        add_error_->setVisible(true);
    };

    camera::Camera c;
    c.active = true;
    c.name = name_edit_->text().trimmed().toStdString();

    if (usb_radio_->isChecked()) {
        const int index = usb_combo_->currentData().toInt();
        if (index < 0) {
            fail(QStringLiteral("No USB camera selected."));
            return;
        }
        c.camera_type = "usb";
        c.index = static_cast<uint32_t>(index);
        if (c.name.empty()) c.name = usb_combo_->currentText().toStdString();
    } else {
        const QString rtsp = rtsp_edit_->text().trimmed();
        if (rtsp.isEmpty()) {
            fail(QStringLiteral("An RTSP URL is required."));
            return;
        }
        c.camera_type = "ip";
        c.rtsp = rtsp.toStdString();
        const QString user = user_edit_->text().trimmed();
        if (!user.isEmpty()) c.username = user.toStdString();
        if (c.name.empty()) c.name = rtsp.toStdString();
    }

    if (!camera::insert(db_, c)) {
        fail(QStringLiteral("Failed to save the camera."));
        return;
    }
    emit cameras_changed();
    show_list();
}

} // namespace denso::ui
