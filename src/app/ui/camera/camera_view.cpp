#include "ui/camera/camera_view.h"

#include "camera/repo.h"
#include "mode/config.h"
#include "ui/camera/grid/camera_grid.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QString>
#include <QVBoxLayout>

#include <vector>

namespace denso::ui {

namespace {

// A credential-free one-line source summary for the retained-connection list.
// Deliberately built from the safe Camera fields only — NEVER the username,
// password, or a credential-bearing RTSP URL (spec §7.2 / credential-safe list).
QString safe_source(const camera::Camera& c) {
    if (c.camera_type == "usb") {
        return c.index ? QStringLiteral("USB camera %1").arg(*c.index)
                       : QStringLiteral("USB camera");
    }
    // IP camera: IP address plus channel / stream, following the RTSP-template
    // convention (host + channel). No credentials, no assembled RTSP URL. The IP
    // field is a free-text line edit with no user-info validation, so defensively
    // strip any "user:pass@" a pasted URL could have left in it — a credential must
    // never reach the retained list (spec: credential-safe retained-camera list).
    QString host = c.ip ? QString::fromStdString(*c.ip) : QStringLiteral("—");
    const int at = host.lastIndexOf(QLatin1Char('@'));
    if (at >= 0) host = host.mid(at + 1);
    QString s = QStringLiteral("IP %1").arg(host);
    if (c.channel) s += QStringLiteral(" · channel %1").arg(*c.channel);
    if (c.stream) {
        s += (*c.stream == 0 ? QStringLiteral(" · main stream")
                             : QStringLiteral(" · sub stream"));
    }
    return s;
}

}  // namespace

CameraView::CameraView(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                       WarmupState* warmup, QWidget* parent)
    : QWidget(parent), db_(std::move(db)), engines_(std::move(engines)),
      warmup_(warmup) {
    setObjectName(QStringLiteral("mainContent"));  // content-panel background

    auto* root = new QVBoxLayout(this);
    // Flush CCTV-wall look: the live grid runs edge-to-edge with no margin.
    root->setContentsMargins(0, 0, 0, 0);

    stack_ = new QStackedWidget;
    root->addWidget(stack_);

    // ── Page 0: empty state ────────────────────────────────────────────────
    auto* empty = new QWidget;
    auto* ev = new QVBoxLayout(empty);
    ev->addStretch(1);

    auto* col = new QVBoxLayout;
    col->setSpacing(12);

    auto* glyph = new QLabel(QStringLiteral("📷"));
    glyph->setProperty("faint", true);
    QFont gf = glyph->font();
    gf.setPointSize(48);
    glyph->setFont(gf);
    glyph->setAlignment(Qt::AlignCenter);

    auto* title = new QLabel(QStringLiteral("No cameras yet"));
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() + 4.0);
    tf.setBold(true);
    title->setFont(tf);
    title->setAlignment(Qt::AlignCenter);

    auto* subtitle = new QLabel(QStringLiteral("Add a camera to start reading"));
    subtitle->setProperty("faint", true);
    subtitle->setAlignment(Qt::AlignCenter);

    auto* add = new QPushButton(QStringLiteral("+ Add Camera"));
    add->setObjectName(QStringLiteral("addCameraButton"));
    add->setProperty("gold", true);
    connect(add, &QPushButton::clicked, this, &CameraView::add_camera_requested);

    col->addWidget(glyph);
    col->addWidget(title);
    col->addWidget(subtitle);
    col->addSpacing(8);
    auto* btn_row = new QHBoxLayout;
    btn_row->addStretch(1);
    btn_row->addWidget(add, 0);
    btn_row->addStretch(1);
    col->addLayout(btn_row);

    ev->addLayout(col);
    ev->addStretch(1);
    stack_->addWidget(empty);  // index 0

    // ── Page 1: live grid ──────────────────────────────────────────────────
    grid_ = new CameraGrid(db_, engines_, warmup_);
    stack_->addWidget(grid_);  // index 1

    // ── Page 2: retained connections / mode unavailable ────────────────────
    // Shown after a mode switch (all() non-empty, runtime() empty) so the false
    // "No cameras yet" is never displayed, and for ball_leveler as the hard
    // "setup not available in this release" state. Skeleton built once here;
    // populate_retained_page() fills the header/message/list/action per reload().
    auto* retained = new QWidget;
    auto* rv = new QVBoxLayout(retained);
    rv->addStretch(1);

    auto* rcol = new QVBoxLayout;
    rcol->setSpacing(12);

    auto* rglyph = new QLabel(QStringLiteral("📷"));
    rglyph->setProperty("faint", true);
    QFont rgf = rglyph->font();
    rgf.setPointSize(48);
    rglyph->setFont(rgf);
    rglyph->setAlignment(Qt::AlignCenter);

    retained_header_ = new QLabel;
    retained_header_->setObjectName(QStringLiteral("retainedHeader"));
    QFont rhf = retained_header_->font();
    rhf.setPointSizeF(rhf.pointSizeF() + 4.0);
    rhf.setBold(true);
    retained_header_->setFont(rhf);
    retained_header_->setAlignment(Qt::AlignCenter);

    retained_message_ = new QLabel;
    retained_message_->setObjectName(QStringLiteral("retainedMessage"));
    retained_message_->setProperty("faint", true);
    retained_message_->setAlignment(Qt::AlignCenter);
    retained_message_->setWordWrap(true);

    // The read-only retained-camera list (name + credential-free source).
    auto* list_host = new QWidget;
    list_host->setObjectName(QStringLiteral("retainedList"));
    retained_list_box_ = new QVBoxLayout(list_host);
    retained_list_box_->setContentsMargins(0, 0, 0, 0);
    retained_list_box_->setSpacing(4);

    retained_setup_btn_ = new QPushButton(QStringLiteral("Set up cameras"));
    retained_setup_btn_->setObjectName(QStringLiteral("setupCamerasButton"));
    retained_setup_btn_->setProperty("gold", true);
    // Re-enters service through the EXISTING wizard only — no new completion path.
    connect(retained_setup_btn_, &QPushButton::clicked, this,
            &CameraView::add_camera_requested);

    rcol->addWidget(rglyph);
    rcol->addWidget(retained_header_);
    rcol->addWidget(retained_message_);
    rcol->addSpacing(8);
    auto* list_row = new QHBoxLayout;
    list_row->addStretch(1);
    list_row->addWidget(list_host, 0);
    list_row->addStretch(1);
    rcol->addLayout(list_row);
    rcol->addSpacing(8);
    auto* setup_row = new QHBoxLayout;
    setup_row->addStretch(1);
    setup_row->addWidget(retained_setup_btn_, 0);
    setup_row->addStretch(1);
    rcol->addLayout(setup_row);

    rv->addLayout(rcol);
    rv->addStretch(1);
    stack_->addWidget(retained);  // index 2

    reload();
}

void CameraView::reload() {
    // Mode is a HARD admission gate — read it FIRST (spec §3.5, §12.14). A
    // committed ball_leveler NEVER builds the digit-reader pipeline, regardless of
    // any DB camera flags (even a hand-restored completed+active row).
    if (denso::mode::load(db_) == denso::mode::TargetMode::BallLeveler) {
        // Tear the grid down via the ONE authoritative primitive (no reload(), so
        // no CameraStream / processor / reporter / ZoneHealth is constructed and
        // reload_invocations() does not advance), publish the idle status (real
        // verdict + mode fields), and show the unavailable page.
        grid_->teardown();
        grid_->publish_idle_status();
        populate_retained_page(/*ball_leveler*/ true);
        stack_->setCurrentIndex(2);
        return;
    }

    // digit_reader. Must match what CameraGrid will actually show: a database
    // holding only unfinished drafts has nothing live.
    const std::vector<camera::Camera> all = camera::all(db_);
    const int n_runtime = static_cast<int>(camera::runtime(db_).size());
    grid_->reload();  // rebuild + start streams (clears to nothing when runtime is empty)
    if (n_runtime > 0) {
        stack_->setCurrentIndex(1);  // live grid
    } else if (all.empty()) {
        stack_->setCurrentIndex(0);  // "No cameras yet" + Add
    } else {
        // Retained connections but none runnable → setup required, not empty.
        populate_retained_page(/*ball_leveler*/ false);
        stack_->setCurrentIndex(2);
    }
}

void CameraView::populate_retained_page(bool ball_leveler) {
    // Rebuild the list from every retained row (all(), not runtime()). Delete the
    // old labels SYNCHRONOUSLY (they carry no pending signals/events): deleteLater()
    // would leave a stale label rendered over the rebuilt list until the event loop
    // ran, if page 2 is already the shown page when reload() repopulates it.
    while (QLayoutItem* item = retained_list_box_->takeAt(0)) {
        delete item->widget();  // delete nullptr is a no-op for spacer items
        delete item;
    }
    const std::vector<camera::Camera> cams = camera::all(db_);
    const int n = static_cast<int>(cams.size());
    const QString kept = n == 1 ? QStringLiteral("1 camera connection kept")
                                : QStringLiteral("%1 camera connections kept").arg(n);

    if (ball_leveler) {
        // Unavailable state: read-only list, no action. The count header only
        // appears when there is something to list.
        retained_header_->setText(kept);
        retained_header_->setVisible(n > 0);
        retained_message_->setText(QStringLiteral(
            "Floating Ball Leveler setup is not available in this release."));
    } else {
        retained_header_->setText(kept + QStringLiteral(" — processing setup required"));
        retained_header_->setVisible(true);
        retained_message_->setText(
            QStringLiteral("Set up each camera to resume reading."));
    }

    for (const camera::Camera& cam : cams) {
        auto* l = new QLabel(QStringLiteral("%1  —  %2")
                                 .arg(QString::fromStdString(cam.name), safe_source(cam)));
        l->setAlignment(Qt::AlignCenter);
        retained_list_box_->addWidget(l);
    }

    // The setup action re-enters the existing wizard; ball_leveler exposes none.
    retained_setup_btn_->setVisible(!ball_leveler);
}

void CameraView::release_streams() {
    grid_->release_streams();
}

void CameraView::teardown_for_switch() {
    // Authoritative teardown of the live pipeline (== CameraGrid::clear()), then
    // show the neutral empty page (index 0) so no stale grid lingers during the
    // multi-second reset transaction. NOT reload() / runtime(): nothing may restart
    // the old-mode cameras before the switch commits (spec §6.2).
    grid_->teardown();
    stack_->setCurrentIndex(0);
}

int CameraView::current_page_index() const { return stack_->currentIndex(); }

bool CameraView::grid_has_live_streams() const {
    return grid_->has_live_streams();
}

uint64_t CameraView::grid_reload_invocations() const {
    return grid_->reload_invocations();
}

} // namespace denso::ui
