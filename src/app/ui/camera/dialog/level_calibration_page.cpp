#include "ui/camera/dialog/level_calibration_page.h"

#include "camera/camera.h"   // camera::kMaxZone — the ONE zone-number range
#include "ui/camera/dialog/level_canvas.h"
#include "ui/camera/dialog/page_util.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <utility>

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
    if (reason_code == "level_zone_taken") {
        return QStringLiteral(
            "That reporting zone is already used by another camera.");
    }
    if (reason_code == "level_zone_duplicate") {
        return QStringLiteral(
            "Two zones on this camera report the same number.");
    }
    if (reason_code == "level_zone_count") {
        return QStringLiteral(
            "A camera needs between one and four measurement zones.");
    }
    return QString::fromStdString(reason_code);
}

/// The list row for a zone: the reporting number belongs here, not two clicks
/// away in the picker — verifying a camera means reading the whole mapping at
/// once. Mirrors the Areas list exactly.
QString row_label(const denso::level::LevelZone& z, bool ready) {
    return QStringLiteral("Zone %1%2")
        .arg(z.zone_no)
        .arg(ready ? QString() : QStringLiteral("  ·  not drawn"));
}

}  // namespace

LevelCalibrationPage::LevelCalibrationPage(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    auto* body = new QHBoxLayout;
    canvas_ = new LevelCanvas;
    body->addWidget(canvas_, 1);

    auto* side = new QVBoxLayout;
    side->addWidget(new QLabel(QStringLiteral("Level zones")));
    // Stated on screen, not only in a header comment. The reference point IS the
    // detected ball's centre, so an operator who lines these up with the visible
    // liquid surface bakes a constant radius-sized error into every reading.
    auto* hint = dim_label(QStringLiteral(
        "Drag out the rectangle the ball moves within, then drag each line to "
        "where the BALL'S CENTRE sits when the tank is full (100%) and empty "
        "(0%). Line them up with the ball, not with the liquid surface."));
    hint->setWordWrap(true);
    side->addWidget(hint);

    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("levelZoneList"));
    connect(list_, &QListWidget::currentRowChanged, this,
            [this](int row) { select_zone(row); });
    side->addWidget(list_, 1);

    auto* zone_row = new QHBoxLayout;
    add_btn_ = new QPushButton(QStringLiteral("Add zone"));
    add_btn_->setObjectName(QStringLiteral("levelAddZone"));
    connect(add_btn_, &QPushButton::clicked, this, [this] { add_zone(); });
    delete_btn_ = new QPushButton(QStringLiteral("Delete zone"));
    delete_btn_->setObjectName(QStringLiteral("levelDeleteZone"));
    connect(delete_btn_, &QPushButton::clicked, this, [this] { delete_selected(); });
    zone_row->addWidget(add_btn_);
    zone_row->addWidget(delete_btn_);
    side->addLayout(zone_row);

    side->addWidget(dim_label(QStringLiteral("Reporting zone")));
    zone_combo_ = new QComboBox;
    zone_combo_->setObjectName(QStringLiteral("levelZoneNo"));
    connect(zone_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
                if (idx < 0 || selected_ < 0 ||
                    selected_ >= static_cast<int>(zones_.size())) {
                    return;
                }
                const QVariant d = zone_combo_->itemData(idx);
                if (!d.isValid()) return;
                zones_[static_cast<size_t>(selected_)].zone_no = d.toInt();
                refresh_list();
                rebuild_zone_choices();
                sync();
            });
    side->addWidget(zone_combo_);

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

    // Added as a LAYOUT, not as a fixed-width host widget: wrapping it in a
    // widget re-runs the layout and resizes the canvas, which silently moves
    // every normalized coordinate the operator just drew against.
    body->addLayout(side, 0);
    root->addLayout(body, 1);

    auto* footer = new QHBoxLayout;
    auto* back = new QPushButton(QStringLiteral("Back"));
    back->setObjectName(QStringLiteral("levelBack"));
    save_btn_ = new QPushButton(QStringLiteral("Save zones & finish setup"));
    save_btn_->setObjectName(QStringLiteral("levelSave"));
    save_btn_->setProperty("gold", true);
    footer->addWidget(back);
    footer->addStretch(1);
    footer->addWidget(save_btn_);
    root->addLayout(footer);
    connect(back, &QPushButton::clicked, this, [this] {
        // Same safety the Areas step has: leaving with unsaved geometry must ask
        // first. Without it a mis-tapped Back silently discards work the operator
        // has just spent time placing.
        if (confirm_discard(QStringLiteral("Go back"))) {
            emit back_requested();
        }
    });
    connect(save_btn_, &QPushButton::clicked, this, [this] { attempt_save(); });

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
    connect(canvas_, &LevelCanvas::rejected, this,
            [this](const QString& why) { status_->setText(why); });

    load({}, {});
}

void LevelCalibrationPage::load(std::vector<denso::level::LevelZone> saved,
                                std::map<int, std::string> zones_taken) {
    zones_ = std::move(saved);
    zones_taken_ = std::move(zones_taken);
    // A fresh configuration starts with ONE zone rather than none: zero zones is
    // a state the write chokepoint refuses, so offering it as the starting point
    // would hand the operator an empty page whose only exit is an error.
    if (zones_.empty()) {
        denso::level::LevelZone z;
        z.zone_no = first_free_zone_no();
        if (z.zone_no == 0) z.zone_no = 1;   // nothing free: let the save refuse it
        zones_.push_back(z);
    }
    // Snapshot for the dirty check, taken from what the operator will be shown.
    loaded_ = zones_;
    selected_ = -1;
    refresh_list();
    select_zone(0);
    list_->setCurrentRow(0);
}

void LevelCalibrationPage::set_background(const QImage& oriented) {
    canvas_->set_frame(oriented);
}

std::vector<denso::level::LevelZone> LevelCalibrationPage::zones() const {
    std::vector<denso::level::LevelZone> out = zones_;
    // The selected zone's authoritative geometry lives in the live draft, not in
    // the vector — folding it in HERE means every reader (save, dirty check,
    // status) sees the same set, rather than each remembering to do it.
    if (selected_ >= 0 && selected_ < static_cast<int>(out.size())) {
        out[static_cast<size_t>(selected_)].calibration = draft_.draft();
    }
    return out;
}

void LevelCalibrationPage::commit_selected() {
    if (selected_ >= 0 && selected_ < static_cast<int>(zones_.size())) {
        zones_[static_cast<size_t>(selected_)].calibration = draft_.draft();
    }
}

void LevelCalibrationPage::select_zone(int row) {
    // Fold the outgoing zone's live geometry back before switching, or every
    // selection change would silently discard the edits just made to it.
    commit_selected();
    if (row < 0 || row >= static_cast<int>(zones_.size())) {
        selected_ = -1;
        draft_ = denso::level::CalibrationDraft();
        sync();
        return;
    }
    selected_ = row;
    const denso::level::LevelZone& z = zones_[static_cast<size_t>(row)];
    // from_calibration adopts the stored values WHOLE. Routing a reload through
    // the clamping mutators would nudge them, so merely selecting a zone would
    // rewrite the operator's calibration.
    draft_ = denso::level::CalibrationDraft::from_calibration(z.calibration);
    // Resuming lands in Editing so the first thing the operator can do is adjust;
    // a zone with no rectangle yet lands in Drawing because there is nothing to
    // adjust.
    if (draft_.has_rect()) {
        canvas_->begin_edit();
    } else {
        canvas_->begin_draw();
    }
    rebuild_zone_choices();
    sync_zone_combo(z.zone_no);
    sync();
}

int LevelCalibrationPage::first_free_zone_no() const {
    std::set<int> used;
    for (const auto& [zone, owner] : zones_taken_) used.insert(zone);
    for (const denso::level::LevelZone& z : zones_) used.insert(z.zone_no);
    for (int z = 1; z <= denso::camera::kMaxZone; ++z) {
        if (!used.count(z)) return z;
    }
    return 0;
}

void LevelCalibrationPage::add_zone() {
    // The cap is the Ball rule (level::kMaxBallZones), enforced here as an
    // affordance and again in the write chokepoint as the actual gate.
    if (static_cast<int>(zones_.size()) >= denso::level::kMaxBallZones) {
        return;
    }
    const int zone_no = first_free_zone_no();
    if (zone_no == 0) {
        status_->setText(QStringLiteral(
            "No reporting zone numbers are free. Free one on another camera "
            "first."));
        return;
    }
    commit_selected();
    denso::level::LevelZone z;
    z.zone_no = zone_no;
    zones_.push_back(z);
    refresh_list();
    const int row = static_cast<int>(zones_.size()) - 1;
    list_->setCurrentRow(row);
    select_zone(row);
}

void LevelCalibrationPage::delete_selected() {
    if (selected_ < 0 || selected_ >= static_cast<int>(zones_.size())) {
        return;
    }
    // The last zone cannot be deleted: zero zones is a state the write
    // chokepoint refuses, so allowing it would leave the page in a shape whose
    // only exit is a refusal. Removing the camera's Ball configuration entirely
    // is a different action.
    if (zones_.size() == 1) {
        status_->setText(QStringLiteral(
            "A camera needs at least one measurement zone. Redraw this one "
            "instead of deleting it."));
        return;
    }
    const int zone_no = zones_[static_cast<size_t>(selected_)].zone_no;
    if (QMessageBox::question(
            this, QStringLiteral("Delete zone?"),
            QStringLiteral("Delete zone %1? It will stop reporting.").arg(zone_no),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    zones_.erase(zones_.begin() + selected_);
    const int row = std::min(selected_, static_cast<int>(zones_.size()) - 1);
    selected_ = -1;   // the index it referred to no longer means the same zone
    refresh_list();
    list_->setCurrentRow(row);
    select_zone(row);
}

void LevelCalibrationPage::refresh_list() {
    const QSignalBlocker block(list_);
    const int keep = list_->currentRow();
    list_->clear();
    const std::vector<denso::level::LevelZone> view = zones();
    for (const denso::level::LevelZone& z : view) {
        list_->addItem(row_label(z, denso::level::validate_calibration(z.calibration).ok));
    }
    if (keep >= 0 && keep < list_->count()) {
        list_->setCurrentRow(keep);
    }
}

void LevelCalibrationPage::rebuild_zone_choices() {
    // Every zone number the operator may NOT pick for the current target, and who
    // holds it. Mirrors the Areas picker, including the reason text — the two
    // read from the SAME ownership query, so they cannot disagree.
    std::map<int, QString> unavailable;
    for (const auto& [zone, owner] : zones_taken_) {
        unavailable.emplace(zone, QString::fromStdString(owner));
    }
    for (int i = 0; i < static_cast<int>(zones_.size()); ++i) {
        if (i == selected_) continue;   // a zone doesn't block its own number
        unavailable.emplace(zones_[static_cast<size_t>(i)].zone_no,
                            QStringLiteral("this camera"));
    }

    const QSignalBlocker block(zone_combo_);
    zone_combo_->clear();
    auto* model = qobject_cast<QStandardItemModel*>(zone_combo_->model());
    for (int z = 1; z <= denso::camera::kMaxZone; ++z) {
        const auto it = unavailable.find(z);
        const bool taken = it != unavailable.end();
        zone_combo_->addItem(taken ? QStringLiteral("Zone %1 — used by %2")
                                         .arg(z)
                                         .arg(it->second)
                                   : QStringLiteral("Zone %1").arg(z),
                             QVariant(z));
        if (taken && model) {
            // Shown but disabled, and NAMED: hiding it would leave the operator
            // wondering where the number went.
            if (auto* item = model->item(zone_combo_->count() - 1)) {
                item->setEnabled(false);
            }
        }
    }
}

void LevelCalibrationPage::sync_zone_combo(int zone_no) {
    const QSignalBlocker block(zone_combo_);
    const int idx = zone_combo_->findData(QVariant(zone_no));
    zone_combo_->setCurrentIndex(idx >= 0 ? idx : 0);
}

std::optional<int> LevelCalibrationPage::first_invalid_zone() const {
    for (const denso::level::LevelZone& z : zones()) {
        if (!denso::level::validate_calibration(z.calibration).ok) {
            return z.zone_no;
        }
    }
    return std::nullopt;
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

    const bool has_sel = selected_ >= 0;
    zone_combo_->setEnabled(has_sel);
    redraw_btn_->setEnabled(has_sel);
    conf_->setEnabled(has_sel);
    hold_->setEnabled(has_sel);
    add_btn_->setEnabled(static_cast<int>(zones_.size()) < denso::level::kMaxBallZones);
    delete_btn_->setEnabled(has_sel && zones_.size() > 1);
    refresh_list();

    if (has_sel && !draft_.has_rect()) {
        status_->setText(QStringLiteral(
            "Drag out the rectangle the ball moves within to begin."));
        save_btn_->setEnabled(false);
        return;
    }
    if (has_sel) {
        const auto check = draft_.check();
        if (!check.ok) {
            status_->setText(explain(check.reason_code));
            save_btn_->setEnabled(false);
            return;
        }
    }
    // Save covers the WHOLE set, so it is gated on the whole set. Offering it
    // because the SELECTED zone is ready would let the operator submit a
    // configuration the chokepoint rolls back entirely — and lose every zone's
    // work to a fault in one of them.
    if (const auto bad = first_invalid_zone()) {
        status_->setText(
            QStringLiteral("Zone %1 is not finished. Select it and draw its "
                           "rectangle and lines.")
                .arg(*bad));
        save_btn_->setEnabled(false);
        return;
    }
    const auto& c = draft_.draft();
    status_->setText(QStringLiteral("%1 zone%2 ready to save. Selected zone: 100% "
                                    "at %3%, 0% at %4% down the frame.")
                         .arg(zones_.size())
                         .arg(zones_.size() == 1 ? QString() : QStringLiteral("s"))
                         .arg(c.y_100 * 100.0, 0, 'f', 1)
                         .arg(c.y_0 * 100.0, 0, 'f', 1));
    save_btn_->setEnabled(true);
}

void LevelCalibrationPage::attempt_save() {
    // Re-checked here, not merely gated by the button: a disabled button is an
    // affordance, and this page is driven by signals a future caller could invoke
    // directly. The verdict is the draft's, which is the write path's.
    commit_selected();
    if (zones_.empty() || first_invalid_zone().has_value()) {
        sync();
        return;
    }
    emit save_requested(zones());
}

void LevelCalibrationPage::show_save_error() {
    QMessageBox::warning(
        this, QStringLiteral("Save failed"),
        QStringLiteral("Could not save this camera's level zones. "
                       "Nothing was changed. Please try again."));
}

void LevelCalibrationPage::show_refusal(const QString& reason_code, int zone_no) {
    const QString where =
        zone_no > 0 ? QStringLiteral("Zone %1: ").arg(zone_no) : QString();
    QMessageBox::warning(
        this, QStringLiteral("Zones not saved"),
        QStringLiteral("These zones were refused and nothing was changed.\n\n"
                       "%1%2")
            .arg(where, explain(reason_code.toStdString())));
}

bool LevelCalibrationPage::is_dirty() const {
    const std::vector<denso::level::LevelZone> now = zones();
    if (now.size() != loaded_.size()) {
        return true;   // a zone was added or deleted
    }
    for (size_t i = 0; i < now.size(); ++i) {
        const denso::level::LevelZone& a = now[i];
        const denso::level::LevelZone& b = loaded_[i];
        if (a.zone_no != b.zone_no) return true;
        // Field-by-field. Exact comparison is right here: the draft holds the
        // stored doubles VERBATIM (from_calibration assigns whole, precisely so
        // opening the page cannot nudge them), so an untouched resume compares
        // equal bit for bit.
        const denso::level::LevelCalibration& c = a.calibration;
        const denso::level::LevelCalibration& l = b.calibration;
        if (c.rect_x != l.rect_x || c.rect_y != l.rect_y || c.rect_w != l.rect_w ||
            c.rect_h != l.rect_h || c.y_100 != l.y_100 || c.y_0 != l.y_0 ||
            c.conf != l.conf || c.hold_ms != l.hold_ms) {
            return true;
        }
    }
    return false;
}

bool LevelCalibrationPage::confirm_discard(const QString& action) {
    if (!is_dirty()) {
        return true;
    }
    return QMessageBox::question(
               this, QStringLiteral("Discard changes?"),
               QStringLiteral("%1 without saving? The level zone changes "
                              "on this page will be lost.")
                   .arg(action),
               QMessageBox::Discard | QMessageBox::Cancel,
               QMessageBox::Cancel) == QMessageBox::Discard;
}

} // namespace denso::ui
