#include "ui/camera/dialog/level_calibration_page.h"

#include "ui/camera/dialog/level_canvas.h"
#include "ui/camera/dialog/page_util.h"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace denso::ui {
namespace {

/// The operator-facing explanation for each refusal the validator can return.
///
/// The VERDICT is never decided here — level::validate_calibration decides, and
/// this only translates its stable reason code. An unknown code falls through to
/// the code itself rather than to a soothing generic sentence: a refusal nobody
/// can act on is worse than an ugly one.
QString explain(const std::string& reason_code) {
    if (reason_code == "calib_rect_degenerate") {
        return QStringLiteral(
            "Drag out a measurement rectangle inside the camera image.");
    }
    if (reason_code == "calib_span_too_small") {
        return QStringLiteral(
            "Draw a taller rectangle — the 100% and 0% lines are too close "
            "together to measure between.");
    }
    if (reason_code == "calib_lines_reversed") {
        return QStringLiteral("The 100% line must sit above the 0% line.");
    }
    if (reason_code == "calib_line_outside_rect") {
        return QStringLiteral(
            "Both reference lines must lie inside the measurement rectangle.");
    }
    if (reason_code == "calib_conf_out_of_range") {
        return QStringLiteral(
            "The detection confidence must be above 0 and at most 1.");
    }
    if (reason_code == "calib_hold_invalid") {
        return QStringLiteral("The hold time cannot be negative.");
    }
    if (reason_code == "calib_not_finite") {
        return QStringLiteral("The calibration contains an invalid number.");
    }
    return QString::fromStdString(reason_code);
}

}  // namespace

LevelCalibrationPage::LevelCalibrationPage(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    auto* body = new QHBoxLayout;
    canvas_ = new LevelCanvas;
    body->addWidget(canvas_, 1);

    auto* side = new QVBoxLayout;
    side->addWidget(new QLabel(QStringLiteral("Level calibration")));
    // Stated on screen, not only in a header comment. The reference point IS the
    // detected ball's centre, so an operator who lines these up with the visible
    // liquid surface bakes a constant radius-sized error into every reading.
    auto* hint = dim_label(QStringLiteral(
        "Drag out the rectangle the ball moves within, then drag each line to "
        "where the BALL'S CENTRE sits when the tank is full (100%) and empty "
        "(0%). Line them up with the ball, not with the liquid surface."));
    hint->setWordWrap(true);
    side->addWidget(hint);

    redraw_btn_ = new QPushButton(QStringLiteral("Redraw rectangle"));
    redraw_btn_->setObjectName(QStringLiteral("levelRedraw"));
    // Every gesture has a page button: the panel may be a touchscreen with no
    // keyboard and no right button, so redrawing cannot be a hidden shortcut.
    connect(redraw_btn_, &QPushButton::clicked, this, [this] {
        canvas_->begin_draw();
        sync();
    });
    side->addWidget(redraw_btn_);

    auto* conf_row = new QHBoxLayout;
    conf_row->addWidget(new QLabel(QStringLiteral("Detection confidence")));
    conf_ = new QDoubleSpinBox;
    conf_->setObjectName(QStringLiteral("levelConf"));
    conf_->setDecimals(3);
    conf_->setRange(0.001, 1.0);
    conf_->setSingleStep(0.05);
    connect(conf_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        draft_.set_conf(v);
        sync();
    });
    conf_row->addWidget(conf_);
    side->addLayout(conf_row);

    auto* hold_row = new QHBoxLayout;
    hold_row->addWidget(new QLabel(QStringLiteral("Hold time")));
    hold_ = new QSpinBox;
    hold_->setObjectName(QStringLiteral("levelHold"));
    hold_->setRange(0, 60000);
    hold_->setSingleStep(100);
    hold_->setSuffix(QStringLiteral(" ms"));
    connect(hold_, &QSpinBox::valueChanged, this, [this](int v) {
        draft_.set_hold_ms(v);
        sync();
    });
    hold_row->addWidget(hold_);
    side->addLayout(hold_row);

    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("levelStatus"));
    status_->setWordWrap(true);
    side->addWidget(status_);
    side->addStretch(1);
    body->addLayout(side, 0);
    root->addLayout(body, 1);

    auto* footer = new QHBoxLayout;
    auto* back = new QPushButton(QStringLiteral("Back"));
    back->setObjectName(QStringLiteral("levelBack"));
    save_btn_ = new QPushButton(QStringLiteral("Save calibration & finish setup"));
    save_btn_->setObjectName(QStringLiteral("levelSave"));
    save_btn_->setProperty("gold", true);
    footer->addWidget(back);
    footer->addStretch(1);
    footer->addWidget(save_btn_);
    root->addLayout(footer);
    connect(back, &QPushButton::clicked, this, &LevelCalibrationPage::back_requested);
    connect(save_btn_, &QPushButton::clicked, this,
            [this] { attempt_save(); });

    // The canvas requests; the DRAFT decides. Nothing between these lambdas and
    // the draft may clamp, reorder or refuse — that would be a second copy of a
    // rule that already exists, and the two would eventually disagree.
    connect(canvas_, &LevelCanvas::rect_drawn, this,
            [this](double x, double y, double w, double h) {
                draft_.set_rect(x, y, w, h);
                if (draft_.has_rect()) canvas_->begin_edit();
                sync();
            });
    connect(canvas_, &LevelCanvas::y_100_moved, this, [this](double y) {
        draft_.set_y_100(y);
        sync();
    });
    connect(canvas_, &LevelCanvas::y_0_moved, this, [this](double y) {
        draft_.set_y_0(y);
        sync();
    });
    connect(canvas_, &LevelCanvas::rejected, this, [this](const QString& why) {
        status_->setText(why);
    });

    load(std::nullopt);
}

void LevelCalibrationPage::load(
    const std::optional<denso::level::LevelCalibration>& saved) {
    draft_ = saved.has_value()
                 ? denso::level::CalibrationDraft::from_calibration(*saved)
                 : denso::level::CalibrationDraft();
    // Resuming lands in Editing so the first thing the operator can do is adjust;
    // a fresh configuration lands in Drawing because there is nothing to adjust.
    if (draft_.has_rect()) {
        canvas_->begin_edit();
    } else {
        canvas_->begin_draw();
    }
    sync();
}

void LevelCalibrationPage::set_background(const QImage& oriented) {
    canvas_->set_frame(oriented);
}

void LevelCalibrationPage::sync() {
    canvas_->set_calibration(draft_.draft(), draft_.has_rect());

    // Blocked: these setters exist to DISPLAY the draft. Letting them write back
    // would round a stored value through the widget's step/decimals, so merely
    // opening the page could alter a saved calibration.
    {
        const QSignalBlocker b1(conf_);
        const QSignalBlocker b2(hold_);
        conf_->setValue(draft_.draft().conf);
        hold_->setValue(draft_.draft().hold_ms);
    }

    if (!draft_.has_rect()) {
        status_->setText(QStringLiteral(
            "Drag out the rectangle the ball moves within to begin."));
        save_btn_->setEnabled(false);
        return;
    }
    const auto check = draft_.check();
    if (!check.ok) {
        status_->setText(explain(check.reason_code));
        save_btn_->setEnabled(false);
        return;
    }
    const auto& c = draft_.draft();
    status_->setText(
        QStringLiteral("Ready to save. 100% at %1%, 0% at %2% down the frame.")
            .arg(c.y_100 * 100.0, 0, 'f', 1)
            .arg(c.y_0 * 100.0, 0, 'f', 1));
    save_btn_->setEnabled(true);
}

void LevelCalibrationPage::attempt_save() {
    // Re-checked here, not merely gated by the button: a disabled button is an
    // affordance, and this page is driven by signals a future caller could invoke
    // directly. The verdict is the draft's, which is the write path's.
    if (!draft_.has_rect() || !draft_.check().ok) {
        sync();
        return;
    }
    emit save_requested(draft_.draft());
}

void LevelCalibrationPage::show_save_error() {
    QMessageBox::warning(
        this, QStringLiteral("Save failed"),
        QStringLiteral("Could not save this camera's level calibration. "
                       "Nothing was changed. Please try again."));
}

void LevelCalibrationPage::show_refusal(const QString& reason_code) {
    QMessageBox::warning(
        this, QStringLiteral("Calibration not saved"),
        QStringLiteral("This calibration was refused and nothing was changed.\n\n"
                       "Reason: %1")
            .arg(reason_code));
}

} // namespace denso::ui
