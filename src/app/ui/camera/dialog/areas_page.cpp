#include "ui/camera/dialog/areas_page.h"

#include "camera/area_validation.h"
#include "ui/camera/dialog/page_util.h"
#include "ui/camera/dialog/roi_canvas.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace denso::ui {

namespace {
constexpr int kMaxZone = 12;
constexpr int kSidePanelWidth = 280;

/// The list row for an area: the zone belongs here, not two clicks away in the
/// picker — verifying a camera means reading the whole mapping at once.
QString row_label(const camera::CameraArea& a) {
    const QString name = a.name.empty()
                             ? QStringLiteral("(unnamed)")
                             : QString::fromStdString(a.name);
    return a.zone ? QStringLiteral("%1  ·  Zone %2").arg(name).arg(*a.zone)
                  : QStringLiteral("%1  ·  Detection only").arg(name);
}
}  // namespace

CameraAreasPage::CameraAreasPage(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(12);

    review_banner_ = new QLabel(QStringLiteral(
        "⚠ The source changed — re-verify these areas against the preview. "
        "Zone reporting is paused until you save."));
    review_banner_->setWordWrap(true);
    review_banner_->setStyleSheet(QStringLiteral(
        "background:#b45309; color:#ffffff; border-radius:8px; padding:8px 12px;"));
    review_banner_->setVisible(false);
    v->addWidget(review_banner_);

    auto* body = new QHBoxLayout;
    body->setSpacing(12);

    // ─── Canvas + its drawing toolbar ────────────────────────────────────────
    auto* canvas_col = new QVBoxLayout;
    canvas_col->setSpacing(8);

    canvas_ = new RoiCanvas;
    connect(canvas_, &RoiCanvas::closed, this,
            &CameraAreasPage::commit_drawn_polygon);
    connect(canvas_, &RoiCanvas::polygon_edited, this,
            &CameraAreasPage::apply_edited_polygon);
    connect(canvas_, &RoiCanvas::changed, this, [this] {
        update_status();
        update_controls();
    });
    connect(canvas_, &RoiCanvas::rejected, this, [this](const QString& why) {
        status_->setText(why);
    });
    canvas_col->addWidget(canvas_, 1);

    // Every gesture also has a button: the panel may have no keyboard, and a
    // shortcut nobody can see is not a control.
    auto* tools = new QHBoxLayout;
    tools->setSpacing(8);
    status_ = dim_label(QString());
    status_->setWordWrap(true);
    tools->addWidget(status_, 1);

    undo_btn_ = new QPushButton(QStringLiteral("↶ Undo point"));
    connect(undo_btn_, &QPushButton::clicked, this, [this] {
        canvas_->undo_point();
        canvas_->setFocus();
    });
    tools->addWidget(undo_btn_, 0);

    clear_btn_ = new QPushButton(QStringLiteral("Clear"));
    clear_btn_->setProperty("flatText", true);
    connect(clear_btn_, &QPushButton::clicked, this, [this] {
        canvas_->clear();
        canvas_->setFocus();
    });
    tools->addWidget(clear_btn_, 0);

    done_btn_ = new QPushButton(QStringLiteral("✓ Done shape"));
    connect(done_btn_, &QPushButton::clicked, this, [this] {
        canvas_->close_polygon();
        canvas_->setFocus();
    });
    tools->addWidget(done_btn_, 0);
    canvas_col->addLayout(tools);

    auto* canvas_host = new QWidget;
    canvas_host->setLayout(canvas_col);
    body->addWidget(canvas_host, 1);

    // ─── Side panel: the area set + the selected area's settings ─────────────
    auto* side = new QVBoxLayout;
    side->setSpacing(8);
    side->addWidget(dim_label(QStringLiteral("Areas")));

    list_ = new QListWidget;
    connect(list_, &QListWidget::currentRowChanged, this,
            &CameraAreasPage::select_area);
    side->addWidget(list_, 1);

    auto* list_btns = new QHBoxLayout;
    list_btns->setSpacing(8);
    auto* new_btn = new QPushButton(QStringLiteral("+ New area"));
    connect(new_btn, &QPushButton::clicked, this,
            &CameraAreasPage::start_new_area);
    list_btns->addWidget(new_btn, 1);
    delete_btn_ = new QPushButton(QStringLiteral("Delete"));
    delete_btn_->setProperty("flatText", true);
    connect(delete_btn_, &QPushButton::clicked, this,
            &CameraAreasPage::delete_selected);
    list_btns->addWidget(delete_btn_, 0);
    side->addLayout(list_btns);

    auto* rule = new QFrame;
    rule->setFrameShape(QFrame::HLine);
    rule->setStyleSheet(QStringLiteral("color:#333;"));
    side->addWidget(rule);

    side->addWidget(dim_label(QStringLiteral("Selected area")));

    name_edit_ = new QLineEdit;
    name_edit_->setPlaceholderText(QStringLiteral("Area name"));
    connect(name_edit_, &QLineEdit::textEdited, this, [this](const QString& t) {
        // Typing before the shape exists used to be silently discarded; the
        // draft keeps it until the polygon closes.
        if (drafting_) {
            draft_name_ = t.toStdString();
            return;
        }
        if (camera::CameraArea* a = selected_area()) {
            a->name = t.toStdString();
            if (auto* it = list_->item(list_->currentRow())) {
                it->setText(row_label(*a));
            }
            push_context_areas();
        }
    });
    side->addWidget(name_edit_);

    side->addWidget(dim_label(QStringLiteral("Reporting zone")));
    zone_combo_ = new QComboBox;
    connect(zone_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                if (idx < 0) {
                    return;
                }
                const QVariant d = zone_combo_->itemData(idx);
                const std::optional<int> z =
                    d.isValid() ? std::optional<int>{d.toInt()}
                                : std::optional<int>{};
                if (drafting_) {
                    draft_zone_ = z;
                    return;
                }
                if (camera::CameraArea* a = selected_area()) {
                    a->zone = z;
                    if (auto* it = list_->item(list_->currentRow())) {
                        it->setText(row_label(*a));
                    }
                    push_context_areas();
                }
            });
    side->addWidget(zone_combo_);

    redraw_btn_ = new QPushButton(QStringLiteral("Redraw shape"));
    connect(redraw_btn_, &QPushButton::clicked, this,
            &CameraAreasPage::redraw_selected);
    side->addWidget(redraw_btn_);

    hint_ = dim_label(QString());
    hint_->setWordWrap(true);
    side->addWidget(hint_);
    side->addStretch(1);

    auto* side_host = new QWidget;
    side_host->setLayout(side);
    side_host->setFixedWidth(kSidePanelWidth);
    body->addWidget(side_host, 0);
    v->addLayout(body, 1);

    // ─── Footer ──────────────────────────────────────────────────────────────
    auto* footer = new QHBoxLayout;
    auto* back = new QPushButton(QStringLiteral("← Back"));
    connect(back, &QPushButton::clicked, this, [this] {
        if (confirm_discard(QStringLiteral("Go back"))) {
            emit back_requested();
        }
    });
    footer->addWidget(back, 0);

    // "Skip" said nothing about what it would do to the work on screen.
    auto* skip = new QPushButton(QStringLiteral("Exit without saving"));
    skip->setProperty("flatText", true);
    connect(skip, &QPushButton::clicked, this, [this] {
        if (confirm_discard(QStringLiteral("Exit"))) {
            emit skip_requested();
        }
    });
    footer->addWidget(skip, 0);
    footer->addStretch(1);

    auto* refresh = new QPushButton(QStringLiteral("⟳ Refresh preview"));
    connect(refresh, &QPushButton::clicked, this,
            &CameraAreasPage::refresh_preview_requested);
    footer->addWidget(refresh, 0);

    save_btn_ = new QPushButton(QStringLiteral("✓ Save areas"));
    save_btn_->setProperty("gold", true);
    connect(save_btn_, &QPushButton::clicked, this,
            &CameraAreasPage::attempt_save);
    footer->addWidget(save_btn_, 0);
    v->addLayout(footer);

    update_controls();
    update_status();
}

// ─── Loading ─────────────────────────────────────────────────────────────────

void CameraAreasPage::load(std::vector<camera::CameraArea> areas,
                           std::map<int, std::string> zones_taken) {
    areas_ = std::move(areas);
    loaded_ = areas_;
    zones_taken_ = std::move(zones_taken);
    drafting_ = false;
    draft_name_.clear();
    draft_zone_.reset();
    redrawing_row_ = -1;
    rebuild_zone_choices();
    refresh_list();
    name_edit_->clear();
    sync_zone_combo(std::nullopt);
    canvas_->go_idle();
    push_context_areas();
    if (!areas_.empty()) {
        list_->setCurrentRow(0);  // → select_area: show something immediately
    }
    update_controls();
    update_status();
    canvas_->setFocus();
}

void CameraAreasPage::set_background(const QImage& oriented) {
    canvas_->set_frame(oriented);
    update_controls();
    update_status();
}

void CameraAreasPage::show_save_error() {
    // The predictable causes are caught in attempt_save(); reaching here means
    // a genuine write fault, so say that rather than blaming the operator.
    QMessageBox::critical(
        this, QStringLiteral("Could not save"),
        QStringLiteral("The areas could not be written to the database. Your "
                       "changes are still on screen — try Save again."));
}

void CameraAreasPage::set_review_required(bool on) {
    review_required_ = on;
    review_banner_->setVisible(on);
    save_btn_->setText(on ? QStringLiteral("✓ Verify & save")
                          : QStringLiteral("✓ Save areas"));
}

// ─── Selection + the working set ─────────────────────────────────────────────

camera::CameraArea* CameraAreasPage::selected_area() {
    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(areas_.size())) {
        return nullptr;
    }
    return &areas_[static_cast<size_t>(row)];
}

void CameraAreasPage::refresh_list() {
    const QSignalBlocker block(list_);  // repopulating must not fire selection
    list_->clear();
    for (const camera::CameraArea& a : areas_) {
        list_->addItem(row_label(a));
    }
}

void CameraAreasPage::select_area(int row) {
    if (row < 0 || row >= static_cast<int>(areas_.size())) {
        name_edit_->clear();
        sync_zone_combo(std::nullopt);
        canvas_->go_idle();
        push_context_areas();
        update_controls();
        update_status();
        return;
    }
    drafting_ = false;
    redrawing_row_ = -1;
    const camera::CameraArea& a = areas_[static_cast<size_t>(row)];
    name_edit_->setText(QString::fromStdString(a.name));
    sync_zone_combo(a.zone);
    push_context_areas();
    canvas_->edit_polygon(a.points);
    update_controls();
    update_status();
}

void CameraAreasPage::push_context_areas() {
    // Everything except the one being edited — the active polygon is drawn on
    // top by the canvas itself.
    const int row = list_->currentRow();
    std::vector<camera::CameraArea> others;
    for (size_t i = 0; i < areas_.size(); ++i) {
        if (static_cast<int>(i) != row || drafting_) {
            others.push_back(areas_[i]);
        }
    }
    canvas_->set_context_areas(others);
}

QString CameraAreasPage::suggested_name() const {
    // Count-based names collide after a delete; probe for a free one.
    for (int n = static_cast<int>(areas_.size()) + 1;; ++n) {
        const std::string candidate = QStringLiteral("Area %1").arg(n).toStdString();
        bool taken = false;
        for (const camera::CameraArea& a : areas_) {
            if (a.name == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return QString::fromStdString(candidate);
        }
    }
}

void CameraAreasPage::start_new_area() {
    if (!canvas_->has_frame()) {
        status_->setText(QStringLiteral(
            "No preview to draw on — press “Refresh preview” first."));
        return;
    }
    list_->setCurrentRow(-1);
    drafting_ = true;
    redrawing_row_ = -1;
    draft_name_ = suggested_name().toStdString();
    draft_zone_.reset();
    name_edit_->setText(QString::fromStdString(draft_name_));
    sync_zone_combo(std::nullopt);
    push_context_areas();
    canvas_->begin_draw();
    canvas_->setFocus();
    update_controls();
    update_status();
}

void CameraAreasPage::commit_drawn_polygon() {
    if (!canvas_->is_valid()) {
        return;
    }
    if (camera::polygon_is_degenerate(canvas_->polygon())) {
        QMessageBox::warning(
            this, QStringLiteral("Area too small"),
            QStringLiteral("That shape encloses almost no area, so nothing "
                           "would ever be detected in it. Draw it again a "
                           "little larger."));
        canvas_->begin_draw();
        update_controls();
        update_status();
        return;
    }

    if (redrawing_row_ >= 0 && redrawing_row_ < static_cast<int>(areas_.size())) {
        // A redraw keeps the area's identity — that's the whole point of it.
        const int row = redrawing_row_;
        areas_[static_cast<size_t>(row)].points = canvas_->polygon();
        redrawing_row_ = -1;
        refresh_list();
        list_->setCurrentRow(row);  // → select_area
        return;
    }

    camera::CameraArea a;
    // camera_id is assigned by the repo's replace_areas (it ignores this field).
    a.name = draft_name_.empty() ? suggested_name().toStdString() : draft_name_;
    a.zone = draft_zone_;
    a.points = canvas_->polygon();
    areas_.push_back(std::move(a));
    drafting_ = false;
    draft_name_.clear();
    draft_zone_.reset();
    refresh_list();
    list_->setCurrentRow(static_cast<int>(areas_.size()) - 1);  // → select_area
}

void CameraAreasPage::apply_edited_polygon(
    const std::vector<camera::Point>& pts) {
    if (camera::CameraArea* a = selected_area()) {
        a->points = pts;
    }
}

void CameraAreasPage::redraw_selected() {
    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(areas_.size())) {
        return;
    }
    if (!canvas_->has_frame()) {
        status_->setText(QStringLiteral(
            "No preview to draw on — press “Refresh preview” first."));
        return;
    }
    redrawing_row_ = row;
    drafting_ = false;
    canvas_->begin_draw();
    canvas_->setFocus();
    update_controls();
    update_status();
}

void CameraAreasPage::delete_selected() {
    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(areas_.size())) {
        return;
    }
    const camera::CameraArea& a = areas_[static_cast<size_t>(row)];
    const QString name = a.name.empty() ? QStringLiteral("this area")
                                        : QString::fromStdString(a.name);
    const QString zone_note =
        a.zone ? QStringLiteral(" It reports zone %1, which will stop.").arg(*a.zone)
               : QString();
    if (QMessageBox::question(
            this, QStringLiteral("Delete area?"),
            QStringLiteral("Delete “%1”?%2").arg(name, zone_note),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    areas_.erase(areas_.begin() + row);
    redrawing_row_ = -1;
    drafting_ = false;
    refresh_list();
    if (areas_.empty()) {
        select_area(-1);
    } else {
        list_->setCurrentRow(std::min(row, static_cast<int>(areas_.size()) - 1));
    }
    update_controls();
    update_status();
}

// ─── Zone picker ─────────────────────────────────────────────────────────────

void CameraAreasPage::rebuild_zone_choices() {
    const QSignalBlocker block(zone_combo_);
    zone_combo_->clear();
    // No "0 = none" sentinel to decode — the default choice says what it does.
    zone_combo_->addItem(QStringLiteral("Detection only — do not report"),
                         QVariant());
    for (int z = 1; z <= kMaxZone; ++z) {
        const auto owner = zones_taken_.find(z);
        const bool taken = owner != zones_taken_.end();
        zone_combo_->addItem(
            taken ? QStringLiteral("Zone %1 — used by %2")
                        .arg(z)
                        .arg(QString::fromStdString(owner->second))
                  : QStringLiteral("Zone %1").arg(z),
            QVariant(z));
        if (taken) {
            // Visible but unpickable: the operator learns the number is spoken
            // for and by whom, instead of discovering it via a failed save.
            if (auto* model =
                    qobject_cast<QStandardItemModel*>(zone_combo_->model())) {
                if (auto* item = model->item(zone_combo_->count() - 1)) {
                    item->setEnabled(false);
                }
            }
        }
    }
}

void CameraAreasPage::sync_zone_combo(std::optional<int> zone) {
    const QSignalBlocker block(zone_combo_);
    const int idx = zone ? zone_combo_->findData(QVariant(*zone)) : 0;
    zone_combo_->setCurrentIndex(idx >= 0 ? idx : 0);
}

// ─── Live state ──────────────────────────────────────────────────────────────

void CameraAreasPage::update_status() {
    if (!canvas_->has_frame()) {
        status_->setText(QStringLiteral(
            "Waiting for a camera preview — press “Refresh preview”."));
        return;
    }
    switch (canvas_->mode()) {
        case RoiCanvas::Mode::Drawing: {
            const int n = canvas_->point_count();
            const QString what = redrawing_row_ >= 0
                                     ? QStringLiteral("Redrawing")
                                     : QStringLiteral("Drawing");
            if (n == 0) {
                status_->setText(QStringLiteral("%1 — tap the camera image to "
                                                "place the first corner.")
                                     .arg(what));
            } else if (n < 3) {
                status_->setText(
                    QStringLiteral("%1 — %2 of at least 3 corners placed.")
                        .arg(what)
                        .arg(n));
            } else {
                status_->setText(
                    QStringLiteral("%1 — %2 corners. Tap the ringed first "
                                   "corner or press “Done shape”.")
                        .arg(what)
                        .arg(n));
            }
            break;
        }
        case RoiCanvas::Mode::Editing:
            status_->setText(QStringLiteral(
                "Drag a corner to move it · tap an edge to add one · "
                "right-click a corner to remove it."));
            break;
        case RoiCanvas::Mode::Idle:
            status_->setText(
                areas_.empty()
                    ? QStringLiteral("No areas yet. Press “+ New area” to draw "
                                     "one, or save with none to watch the "
                                     "whole frame.")
                    : QStringLiteral("Select an area to check or adjust it."));
            break;
    }
}

void CameraAreasPage::update_controls() {
    const bool drawing = canvas_->mode() == RoiCanvas::Mode::Drawing;
    const bool has_sel = list_->currentRow() >= 0;
    const bool has_frame = canvas_->has_frame();

    undo_btn_->setEnabled(drawing && canvas_->point_count() > 0);
    clear_btn_->setEnabled(drawing && canvas_->point_count() > 0);
    done_btn_->setEnabled(drawing && canvas_->is_valid());

    delete_btn_->setEnabled(has_sel);
    redraw_btn_->setEnabled(has_sel && has_frame && !drawing);
    // Metadata is only meaningful for a real target — either the selected area
    // or the draft. Enabled-but-ignored controls are how the old page lost the
    // operator's typing.
    name_edit_->setEnabled(has_sel || drafting_);
    zone_combo_->setEnabled(has_sel || drafting_);

    hint_->setText(
        drawing ? QStringLiteral("Name and zone are kept and applied when you "
                                 "finish the shape.")
                : QStringLiteral("Areas limit where this camera looks. An area "
                                 "with a zone also reports its reading."));
}

// ─── Leaving + saving ────────────────────────────────────────────────────────

bool CameraAreasPage::is_dirty() const {
    return !camera::areas_equal(areas_, loaded_) || canvas_->has_unfinished_draw();
}

bool CameraAreasPage::confirm_discard(const QString& action) {
    if (!is_dirty()) {
        return true;
    }
    return QMessageBox::question(
               this, QStringLiteral("Discard changes?"),
               QStringLiteral("Your changes to the areas have not been saved. "
                              "%1 and lose them?")
                   .arg(action),
               QMessageBox::Discard | QMessageBox::Cancel,
               QMessageBox::Cancel) == QMessageBox::Discard;
}

void CameraAreasPage::attempt_save() {
    // A part-drawn polygon is on screen but not in the working set — saving
    // would have quietly thrown it away.
    if (canvas_->has_unfinished_draw()) {
        QMessageBox::warning(
            this, QStringLiteral("Finish the area first"),
            QStringLiteral("You are still drawing an area. Finish the shape "
                           "(“Done shape”) or clear it, then save."));
        return;
    }

    for (const camera::CameraArea& a : areas_) {
        if (camera::polygon_is_degenerate(a.points)) {
            QMessageBox::warning(
                this, QStringLiteral("Area too small"),
                QStringLiteral("“%1” encloses almost no area, so nothing would "
                               "be detected in it. Redraw or delete it.")
                    .arg(QString::fromStdString(a.name)));
            return;
        }
    }

    // Named here rather than surfacing as a generic write failure from the repo.
    if (const auto c = camera::find_zone_conflict(areas_, zones_taken_)) {
        QMessageBox::warning(
            this, QStringLiteral("Zone already in use"),
            c->owner.empty()
                ? QStringLiteral("Two areas on this camera both report zone "
                                 "%1. Give “%2” a different zone.")
                      .arg(c->zone)
                      .arg(QString::fromStdString(c->area_name))
                : QStringLiteral("Zone %1 is already reported by “%2”. Give "
                                 "“%3” a different zone.")
                      .arg(c->zone)
                      .arg(QString::fromStdString(c->owner))
                      .arg(QString::fromStdString(c->area_name)));
        return;
    }

    // Saving nothing is legal — it means "watch the whole frame" — but it also
    // deletes every stored area, and under quarantine resumes reporting with no
    // ROI at all. Too consequential to happen on a stray click.
    if (areas_.empty() && !loaded_.empty()) {
        if (QMessageBox::question(
                this, QStringLiteral("Remove all areas?"),
                QStringLiteral(
                    "This camera has %1 saved area(s) and you are about to "
                    "save none. Detection will cover the whole frame and any "
                    "zone reporting from this camera will stop. Continue?")
                    .arg(loaded_.size()),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
    }

    emit save_requested(areas_);
}

} // namespace denso::ui
