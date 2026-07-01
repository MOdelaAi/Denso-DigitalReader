#include "ui/camera/dialog/add_page.h"

#include "ui/camera/dialog/camera_devices.h"
#include "ui/camera/dialog/page_util.h"
#include "ui/camera/dialog/ip_scan.h"
#include "ui/camera/shared/rtsp_templates.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>

#include <cstdint>
#include <vector>

namespace denso::ui {

CameraAddPage::CameraAddPage(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(12);

    auto* type_row = new QHBoxLayout;
    usb_radio_ = new QRadioButton(QStringLiteral("USB"));
    ip_radio_ = new QRadioButton(QStringLiteral("IP / RTSP"));
    usb_radio_->setChecked(true);
    connect(usb_radio_, &QRadioButton::toggled, this,
            &CameraAddPage::update_source_fields);
    type_row->addWidget(usb_radio_, 0);
    type_row->addWidget(ip_radio_, 0);
    type_row->addStretch(1);
    v->addLayout(type_row);

    auto* name_row = new QHBoxLayout;
    name_row->addWidget(dim_label(QStringLiteral("Name")), 0);
    name_edit_ = new QLineEdit;
    name_edit_->setPlaceholderText(QStringLiteral("Camera name"));
    name_row->addWidget(name_edit_, 1);
    v->addLayout(name_row);

    // USB inputs — a Scan button + a selectable results list (like Wi-Fi scan).
    usb_box_ = new QWidget;
    auto* usb_v = new QVBoxLayout(usb_box_);
    usb_v->setContentsMargins(0, 0, 0, 0);
    usb_v->setSpacing(6);
    auto* scan_row = new QHBoxLayout;
    scan_row->addWidget(dim_label(QStringLiteral("Detected cameras")), 1);
    auto* scan_btn = new QPushButton(QStringLiteral("Scan"));
    scan_btn->setProperty("flatText", true);
    connect(scan_btn, &QPushButton::clicked, this, &CameraAddPage::scan_usb);
    scan_row->addWidget(scan_btn, 0);
    usb_v->addLayout(scan_row);
    usb_list_ = new QListWidget;
    usb_list_->setMaximumHeight(160);
    usb_v->addWidget(usb_list_);
    v->addWidget(usb_box_);

    // IP inputs — a subnet Scan (open RTSP port) + results list, then the URL.
    ip_box_ = new QWidget;
    auto* ip_l = new QVBoxLayout(ip_box_);
    ip_l->setContentsMargins(0, 0, 0, 0);
    ip_l->setSpacing(8);

    auto* ip_scan_row = new QHBoxLayout;
    ip_scan_row->addWidget(dim_label(QStringLiteral("Discovered hosts")), 1);
    ip_scan_btn_ = new QPushButton(QStringLiteral("Scan"));
    ip_scan_btn_->setProperty("flatText", true);
    connect(ip_scan_btn_, &QPushButton::clicked, this, &CameraAddPage::scan_ip);
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
            &CameraAddPage::update_rtsp_preview);
    connect(stream_combo_, &QComboBox::currentIndexChanged, this,
            &CameraAddPage::update_rtsp_preview);
    connect(channel_spin_, &QSpinBox::valueChanged, this,
            &CameraAddPage::update_rtsp_preview);
    connect(ip_edit_, &QLineEdit::textChanged, this,
            &CameraAddPage::update_rtsp_preview);

    v->addWidget(ip_box_);

    add_error_ = new QLabel;
    add_error_->setStyleSheet(QStringLiteral("color: %1;").arg(QString::fromLatin1(kStatusBad)));
    add_error_->setVisible(false);
    v->addWidget(add_error_);
    v->addStretch(1);

    auto* footer = new QHBoxLayout;
    auto* cancel = new QPushButton(QStringLiteral("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &CameraAddPage::cancel_requested);
    footer->addWidget(cancel, 0);
    footer->addStretch(1);
    auto* next = new QPushButton(QStringLiteral("Next →"));
    next->setProperty("gold", true);
    connect(next, &QPushButton::clicked, this, &CameraAddPage::validate_and_emit);
    footer->addWidget(next, 0);
    v->addLayout(footer);
}

void CameraAddPage::reset() {
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
}

void CameraAddPage::scan_usb() {
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

void CameraAddPage::scan_ip() {
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

void CameraAddPage::update_rtsp_preview() {
    if (rtsp_manufacturers().empty()) return;
    const RtspManufacturer& m =
        rtsp_manufacturers()[static_cast<size_t>(mfr_combo_->currentData().toInt())];
    const QString ip = ip_edit_->text().trimmed();
    rtsp_preview_->setText(
        ip.isEmpty() ? QStringLiteral("—")
                     : build_rtsp(m, ip, channel_spin_->value(),
                                  stream_combo_->currentIndex() == 1));
}

void CameraAddPage::update_source_fields() {
    const bool usb = usb_radio_->isChecked();
    usb_box_->setVisible(usb);
    ip_box_->setVisible(!usb);
}

void CameraAddPage::validate_and_emit() {
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

    add_error_->setVisible(false);
    emit next_requested(c);
}

} // namespace denso::ui
