#include "ui/camera/dialog/configure_page.h"

#include "ui/camera/dialog/page_util.h"
#include "ui/camera/shared/snapshot.h"  // apply_orientation

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <iterator>

namespace denso::ui {

namespace {
struct ResPreset { const char* label; int w; int h; };
constexpr ResPreset kResPresets[] = {
    {"640 × 480", 640, 480},
    {"1280 × 720", 1280, 720},
    {"1920 × 1080", 1920, 1080},
    {"2560 × 1440", 2560, 1440},
};
constexpr int kDefaultResIndex = 1;  // 1280 × 720
}

CameraConfigurePage::CameraConfigurePage(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
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
    connect(capture_btn_, &QPushButton::clicked, this,
            &CameraConfigurePage::capture_requested);
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
            &CameraConfigurePage::render_preview);
    field(QStringLiteral("Rotation"), rotation_combo_);

    pitch_spin_ = new QDoubleSpinBox;
    pitch_spin_->setRange(-45.0, 45.0);
    pitch_spin_->setSingleStep(0.5);
    pitch_spin_->setSuffix(QStringLiteral("°"));
    connect(pitch_spin_, &QDoubleSpinBox::valueChanged, this,
            &CameraConfigurePage::render_preview);
    field(QStringLiteral("Pitch"), pitch_spin_);

    roll_spin_ = new QDoubleSpinBox;
    roll_spin_->setRange(-45.0, 45.0);
    roll_spin_->setSingleStep(0.5);
    roll_spin_->setSuffix(QStringLiteral("°"));
    connect(roll_spin_, &QDoubleSpinBox::valueChanged, this,
            &CameraConfigurePage::render_preview);
    field(QStringLiteral("Roll"), roll_spin_);

    error_ = new QLabel;
    error_->setStyleSheet(QStringLiteral("color: %1;").arg(QString::fromLatin1(kStatusBad)));
    error_->setVisible(false);
    v->addWidget(error_);

    auto* footer = new QHBoxLayout;
    auto* back = new QPushButton(QStringLiteral("← Back"));
    connect(back, &QPushButton::clicked, this, &CameraConfigurePage::back_requested);
    footer->addWidget(back, 0);
    footer->addStretch(1);
    auto* next = new QPushButton(QStringLiteral("Next →"));
    next->setProperty("gold", true);
    connect(next, &QPushButton::clicked, this, &CameraConfigurePage::next_requested);
    footer->addWidget(next, 0);
    v->addLayout(footer);
}

void CameraConfigurePage::populate(const camera::Camera& cam) {
    // Drop any custom resolution entry left over from a previous open, so it
    // can't accumulate across repeated Configure opens (presets sit at the tail).
    const int preset_count = static_cast<int>(std::size(kResPresets));
    while (res_combo_->count() > preset_count) {
        res_combo_->removeItem(0);
    }

    // Resolution: match a preset, else prepend a "custom" entry and select it.
    int res_idx = -1;
    for (int i = 0; i < res_combo_->count(); ++i) {
        const QSize s = res_combo_->itemData(i).toSize();
        if (s.width() == static_cast<int>(cam.width) &&
            s.height() == static_cast<int>(cam.height)) {
            res_idx = i;
            break;
        }
    }
    if (res_idx < 0) {
        if (cam.width > 0 && cam.height > 0) {
            res_combo_->insertItem(
                0, QStringLiteral("%1 × %2 (custom)").arg(cam.width).arg(cam.height),
                QSize(static_cast<int>(cam.width), static_cast<int>(cam.height)));
            res_idx = 0;
        } else {
            res_idx = res_combo_->findData(QSize(kResPresets[kDefaultResIndex].w,
                                                 kResPresets[kDefaultResIndex].h));
            if (res_idx < 0) res_idx = 0;
        }
    }
    res_combo_->setCurrentIndex(res_idx);

    fps_spin_->setValue(cam.fps > 0 ? static_cast<int>(cam.fps) : 30);

    int rot_idx = rotation_combo_->findData(static_cast<int>(cam.rotation));
    rotation_combo_->setCurrentIndex(rot_idx < 0 ? 0 : rot_idx);

    pitch_spin_->setValue(cam.pitch);
    roll_spin_->setValue(cam.roll);
}

void CameraConfigurePage::read_into(camera::Camera& cam) const {
    const QSize res = res_combo_->currentData().toSize();
    cam.width = static_cast<uint32_t>(res.width());
    cam.height = static_cast<uint32_t>(res.height());
    cam.fps = static_cast<uint32_t>(fps_spin_->value());
    cam.rotation = static_cast<uint32_t>(rotation_combo_->currentData().toInt());
    cam.pitch = static_cast<float>(pitch_spin_->value());
    cam.roll = static_cast<float>(roll_spin_->value());
}

QSize CameraConfigurePage::resolution() const {
    return res_combo_->currentData().toSize();
}

void CameraConfigurePage::set_frame(const QImage& raw_frame) {
    raw_frame_ = raw_frame;
    render_preview();
}

void CameraConfigurePage::set_preview_text(const QString& text) {
    preview_label_->setText(text);
}

void CameraConfigurePage::set_capturing(bool capturing) {
    capture_btn_->setText(capturing ? QStringLiteral("Capturing…")
                                    : QStringLiteral("Capture"));
    capture_btn_->setEnabled(!capturing);
}

void CameraConfigurePage::show_error(const QString& msg) {
    error_->setText(msg);
    error_->setVisible(true);
}

void CameraConfigurePage::clear_error() {
    error_->setVisible(false);
}

void CameraConfigurePage::render_preview() {
    if (raw_frame_.isNull()) {
        return;
    }
    const int deg = rotation_combo_->currentData().toInt();
    const QImage shown =
        apply_orientation(raw_frame_, deg, pitch_spin_->value(), roll_spin_->value());
    preview_label_->setPixmap(QPixmap::fromImage(shown).scaled(
        preview_label_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

} // namespace denso::ui
