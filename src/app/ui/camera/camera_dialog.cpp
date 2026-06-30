#include "ui/camera/camera_dialog.h"

#include "camera/model.h"
#include "camera/repo.h"
#include "ui/camera/camera_devices.h"
#include "ui/camera/ip_scan.h"
#include "ui/camera/rtsp_templates.h"
#include "ui/camera/snapshot.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QSize>
#include <QSpinBox>
#include <QStackedWidget>
#include <QThread>
#include <QVBoxLayout>

#include <cstdint>
#include <vector>

namespace denso::ui {
namespace {

constexpr const char* kStatusBad = "#ef4444";

struct ResPreset { const char* label; int w; int h; };
constexpr ResPreset kResPresets[] = {
    {"640 × 480", 640, 480},
    {"1280 × 720", 1280, 720},
    {"1920 × 1080", 1920, 1080},
    {"2560 × 1440", 2560, 1440},
};
constexpr int kDefaultResIndex = 1;  // 1280 × 720

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

    // USB inputs — a Scan button + a selectable results list (like Wi-Fi scan).
    usb_box_ = new QWidget;
    auto* usb_v = new QVBoxLayout(usb_box_);
    usb_v->setContentsMargins(0, 0, 0, 0);
    usb_v->setSpacing(6);
    auto* scan_row = new QHBoxLayout;
    scan_row->addWidget(dim_label(QStringLiteral("Detected cameras")), 1);
    scan_btn_ = new QPushButton(QStringLiteral("Scan"));
    scan_btn_->setProperty("flatText", true);
    connect(scan_btn_, &QPushButton::clicked, this, &CameraDialog::scan_usb);
    scan_row->addWidget(scan_btn_, 0);
    usb_v->addLayout(scan_row);
    usb_list_ = new QListWidget;
    usb_list_->setMaximumHeight(160);
    usb_v->addWidget(usb_list_);
    add_v->addWidget(usb_box_);

    // IP inputs — a subnet Scan (open RTSP port) + results list, then the URL.
    ip_box_ = new QWidget;
    auto* ip_l = new QVBoxLayout(ip_box_);
    ip_l->setContentsMargins(0, 0, 0, 0);
    ip_l->setSpacing(8);

    auto* ip_scan_row = new QHBoxLayout;
    ip_scan_row->addWidget(dim_label(QStringLiteral("Discovered hosts")), 1);
    ip_scan_btn_ = new QPushButton(QStringLiteral("Scan"));
    ip_scan_btn_->setProperty("flatText", true);
    connect(ip_scan_btn_, &QPushButton::clicked, this, &CameraDialog::scan_ip);
    ip_scan_row->addWidget(ip_scan_btn_, 0);
    ip_l->addLayout(ip_scan_row);

    ip_list_ = new QListWidget;
    ip_list_->setMaximumHeight(110);
    connect(ip_list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!(item->flags() & Qt::ItemIsSelectable)) return;
        ip_edit_->setText(item->data(Qt::UserRole).toString());
    });
    ip_l->addWidget(ip_list_);

    // A labelled field row, reused for the IP camera inputs.
    const auto field = [&](const QString& label, QWidget* w) {
        auto* row = new QHBoxLayout;
        auto* l = dim_label(label);
        l->setFixedWidth(96);
        row->addWidget(l, 0);
        row->addWidget(w, 1);
        ip_l->addLayout(row);
    };

    mfr_combo_ = new QComboBox;
    for (size_t i = 0; i < rtsp_manufacturers().size(); ++i) {
        mfr_combo_->addItem(rtsp_manufacturers()[i].name, static_cast<int>(i));
    }
    field(QStringLiteral("Manufacturer"), mfr_combo_);

    stream_combo_ = new QComboBox;
    stream_combo_->addItems({QStringLiteral("Main stream"), QStringLiteral("Sub stream")});
    field(QStringLiteral("Stream"), stream_combo_);

    channel_spin_ = new QSpinBox;
    channel_spin_->setRange(1, 255);
    channel_spin_->setValue(1);
    field(QStringLiteral("Channel"), channel_spin_);

    ip_edit_ = new QLineEdit;
    ip_edit_->setPlaceholderText(QStringLiteral("192.168.1.20"));
    field(QStringLiteral("IP address"), ip_edit_);

    user_edit_ = new QLineEdit;
    user_edit_->setPlaceholderText(QStringLiteral("admin"));
    field(QStringLiteral("Username"), user_edit_);

    pass_edit_ = new QLineEdit;
    pass_edit_->setEchoMode(QLineEdit::Password);
    field(QStringLiteral("Password"), pass_edit_);

    rtsp_preview_ = new QLabel;
    rtsp_preview_->setProperty("faint", true);
    rtsp_preview_->setWordWrap(true);
    rtsp_preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    field(QStringLiteral("RTSP URL"), rtsp_preview_);

    connect(mfr_combo_, &QComboBox::currentIndexChanged, this,
            &CameraDialog::update_rtsp_preview);
    connect(stream_combo_, &QComboBox::currentIndexChanged, this,
            &CameraDialog::update_rtsp_preview);
    connect(channel_spin_, &QSpinBox::valueChanged, this, &CameraDialog::update_rtsp_preview);
    connect(ip_edit_, &QLineEdit::textChanged, this, &CameraDialog::update_rtsp_preview);

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

    build_configure_page();

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
    ip_edit_->clear();
    user_edit_->clear();
    pass_edit_->clear();
    mfr_combo_->setCurrentIndex(0);
    stream_combo_->setCurrentIndex(0);
    channel_spin_->setValue(1);
    ip_list_->clear();  // IP scan is on-demand (slow); not run on open
    update_rtsp_preview();
    add_error_->setVisible(false);

    scan_usb();

    update_source_fields();
    stack_->setCurrentIndex(1);
}

void CameraDialog::scan_usb() {
    usb_list_->clear();
    const std::vector<UsbCamera> cams = list_usb_cameras();
    for (const UsbCamera& cam : cams) {
        auto* item = new QListWidgetItem(cam.name, usb_list_);
        item->setData(Qt::UserRole, cam.index);
    }
    if (cams.empty()) {
        auto* none = new QListWidgetItem(QStringLiteral("No USB cameras found"), usb_list_);
        none->setFlags(Qt::NoItemFlags);  // not selectable
    } else {
        usb_list_->setCurrentRow(0);  // preselect the first
    }
}

void CameraDialog::scan_ip() {
    ip_scan_btn_->setText(QStringLiteral("Scanning…"));
    ip_scan_btn_->setEnabled(false);
    ip_list_->clear();

    // The subnet probe blocks for a few seconds, so run it off the GUI thread
    // (like the Wi-Fi scan) and post the results back via a queued call.
    auto* thread = QThread::create([this] {
        const std::vector<QString> hosts = scan_rtsp_subnet();
        QMetaObject::invokeMethod(
            this,
            [this, hosts] {
                ip_list_->clear();
                for (const QString& ip : hosts) {
                    auto* item = new QListWidgetItem(
                        QStringLiteral("%1 : 554 open").arg(ip), ip_list_);
                    item->setData(Qt::UserRole, ip);
                }
                if (hosts.empty()) {
                    auto* none = new QListWidgetItem(
                        QStringLiteral("No RTSP hosts found"), ip_list_);
                    none->setFlags(Qt::NoItemFlags);
                }
                ip_scan_btn_->setText(QStringLiteral("Scan"));
                ip_scan_btn_->setEnabled(true);
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void CameraDialog::update_rtsp_preview() {
    if (rtsp_manufacturers().empty()) return;
    const RtspManufacturer& m =
        rtsp_manufacturers()[static_cast<size_t>(mfr_combo_->currentData().toInt())];
    const QString ip = ip_edit_->text().trimmed();
    rtsp_preview_->setText(
        ip.isEmpty() ? QStringLiteral("—")
                     : build_rtsp(m, ip, channel_spin_->value(),
                                  stream_combo_->currentIndex() == 1));
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
        QListWidgetItem* item = usb_list_->currentItem();
        if (!item || !(item->flags() & Qt::ItemIsSelectable)) {
            fail(QStringLiteral("Select a camera, or click Scan."));
            return;
        }
        c.camera_type = "usb";
        c.index = static_cast<uint32_t>(item->data(Qt::UserRole).toInt());
        if (c.name.empty()) c.name = item->text().toStdString();
    } else {
        const QString ip = ip_edit_->text().trimmed();
        if (ip.isEmpty()) {
            fail(QStringLiteral("An IP address is required."));
            return;
        }
        const RtspManufacturer& m =
            rtsp_manufacturers()[static_cast<size_t>(mfr_combo_->currentData().toInt())];
        c.camera_type = "ip";
        c.ip = ip.toStdString();
        c.manufacturer = m.name.toStdString();
        c.stream = static_cast<uint32_t>(stream_combo_->currentIndex());
        c.channel = static_cast<uint32_t>(channel_spin_->value());
        c.rtsp = build_rtsp(m, ip, channel_spin_->value(),
                            stream_combo_->currentIndex() == 1)
                     .toStdString();
        const QString user = user_edit_->text();
        const QString pass = pass_edit_->text();
        if (!user.isEmpty()) c.username = user.toStdString();
        if (!pass.isEmpty()) c.password = pass.toStdString();
        if (c.name.empty()) c.name = ip.toStdString();
    }

    if (!camera::insert(db_, c)) {
        fail(QStringLiteral("Failed to save the camera."));
        return;
    }
    emit cameras_changed();
    show_list();
}

void CameraDialog::build_configure_page() {
    config_page_ = new QWidget;
    auto* v = new QVBoxLayout(config_page_);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(12);

    preview_label_ = new QLabel(QStringLiteral("Click Capture to preview"));
    preview_label_->setProperty("dim", true);
    preview_label_->setAlignment(Qt::AlignCenter);
    preview_label_->setMinimumHeight(240);
    preview_label_->setObjectName(QStringLiteral("card"));
    v->addWidget(preview_label_, 1);

    auto* cap_row = new QHBoxLayout;
    cap_row->addStretch(1);
    capture_btn_ = new QPushButton(QStringLiteral("Capture"));
    capture_btn_->setProperty("flatText", true);
    connect(capture_btn_, &QPushButton::clicked, this, &CameraDialog::capture_snapshot);
    cap_row->addWidget(capture_btn_, 0);
    v->addLayout(cap_row);

    const auto field = [&](const QString& label, QWidget* w) {
        auto* row = new QHBoxLayout;
        auto* l = dim_label(label);
        l->setFixedWidth(96);
        row->addWidget(l, 0);
        row->addWidget(w, 1);
        v->addLayout(row);
    };

    res_combo_ = new QComboBox;
    for (const ResPreset& p : kResPresets) {
        res_combo_->addItem(QString::fromUtf8(p.label), QSize(p.w, p.h));
    }
    res_combo_->setCurrentIndex(kDefaultResIndex);
    field(QStringLiteral("Resolution"), res_combo_);

    fps_spin_ = new QSpinBox;
    fps_spin_->setRange(1, 60);
    fps_spin_->setValue(30);
    field(QStringLiteral("FPS"), fps_spin_);

    rotation_combo_ = new QComboBox;
    for (int deg : {0, 90, 180, 270}) {
        rotation_combo_->addItem(QStringLiteral("%1°").arg(deg), deg);
    }
    connect(rotation_combo_, &QComboBox::currentIndexChanged, this,
            &CameraDialog::render_preview);
    field(QStringLiteral("Rotation"), rotation_combo_);

    pitch_spin_ = new QDoubleSpinBox;
    pitch_spin_->setRange(-45.0, 45.0);
    pitch_spin_->setSingleStep(0.5);
    pitch_spin_->setSuffix(QStringLiteral("°"));
    field(QStringLiteral("Pitch"), pitch_spin_);

    roll_spin_ = new QDoubleSpinBox;
    roll_spin_->setRange(-45.0, 45.0);
    roll_spin_->setSingleStep(0.5);
    roll_spin_->setSuffix(QStringLiteral("°"));
    field(QStringLiteral("Roll"), roll_spin_);

    auto* footer = new QHBoxLayout;
    auto* back = new QPushButton(QStringLiteral("Back"));
    connect(back, &QPushButton::clicked, this, &CameraDialog::show_list);
    footer->addWidget(back, 0);
    footer->addStretch(1);
    auto* save = new QPushButton(QStringLiteral("Save"));
    save->setProperty("gold", true);
    connect(save, &QPushButton::clicked, this, &CameraDialog::save_configured_camera);
    footer->addWidget(save, 0);
    v->addLayout(footer);

    stack_->addWidget(config_page_);  // index 2
}

void CameraDialog::capture_snapshot() {
    capture_btn_->setText(QStringLiteral("Capturing…"));
    capture_btn_->setEnabled(false);
    preview_label_->setText(QStringLiteral("Capturing…"));

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
    const QSize res = res_combo_->currentData().toSize();

    auto* thread = QThread::create([this, index, url, res] {
        const Snapshot snap = grab_snapshot(index, url, res.width(), res.height());
        QMetaObject::invokeMethod(
            this,
            [this, snap] {
                capture_btn_->setText(QStringLiteral("Capture"));
                capture_btn_->setEnabled(true);
                if (snap.image.isNull()) {
                    preview_label_->setText(snap.error);
                    return;
                }
                last_frame_ = snap.image;
                render_preview();
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void CameraDialog::render_preview() {
    if (last_frame_.isNull()) {
        return;
    }
    const int deg = rotation_combo_->currentData().toInt();
    const QImage shown = apply_rotation(last_frame_, deg);
    preview_label_->setPixmap(QPixmap::fromImage(shown).scaled(
        preview_label_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
void CameraDialog::save_configured_camera() {}
void CameraDialog::populate_configure(const camera::Camera&) {}
void CameraDialog::read_configure_into_draft() {}

} // namespace denso::ui
