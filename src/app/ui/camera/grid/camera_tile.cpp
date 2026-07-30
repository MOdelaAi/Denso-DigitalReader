#include "ui/camera/grid/camera_tile.h"

#include "camera/camera_stream.h"  // Status enum
#include "health/zone_health.h"    // ZoneCause
#include "ui/camera/shared/roi_geometry.h"   // to_widget

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>

#include <algorithm>
#include <utility>
#include <vector>

namespace denso::ui {

namespace {
const QColor kBg(20, 20, 20);
const QColor kName(229, 231, 235);
const QColor kFaint(148, 148, 148);
const QColor kRoi(250, 204, 21);  // gold — matches the ROI drawing canvas
// Zone-overlay palette. HOLD must read as visibly different from both a healthy
// value and a stopped one — a hold is not an inhibit.
const QColor kZoneOk(134, 239, 172);      // green  — accepted value
const QColor kZoneHold(250, 204, 21);     // amber  — holding the last valid
const QColor kZoneStopped(248, 113, 113); // red    — inhibited / paused / conflict

struct StatusLook {
    QColor dot;
    QString text;
};

StatusLook look_for(int status) {
    switch (static_cast<CameraStream::Status>(status)) {
        case CameraStream::Status::Live:
            return {QColor(34, 197, 94), QStringLiteral("Live")};      // green
        case CameraStream::Status::Offline:
            return {QColor(239, 68, 68), QStringLiteral("Offline")};   // red
        case CameraStream::Status::Connecting:
        default:
            return {QColor(250, 204, 21), QStringLiteral("Connecting…")};  // gold
    }
}

// The single reason to show for an inhibited camera. Multiple causes can be set;
// show the most operator-actionable one. Empty when not inhibited.
QString inhibit_banner(uint32_t causes) {
    using health::ZoneCause;
    auto has = [causes](ZoneCause c) {
        return (causes & static_cast<uint32_t>(c)) != 0;
    };
    if (has(ZoneCause::AreasNeedReview))
        return QStringLiteral("⚠ Areas need review — reporting paused");
    if (has(ZoneCause::CaptureOffline))
        return QStringLiteral("⚠ Camera offline — zones inhibited");
    if (has(ZoneCause::InferenceWorkerFailed))
        return QStringLiteral("⚠ Detection stopped — zones inhibited");
    if (has(ZoneCause::ModelUnavailable))
        return QStringLiteral("⚠ Model unavailable — zones inhibited");
    return QString();
}
}

CameraTile::CameraTile(const QString& name, QWidget* parent)
    : QWidget(parent), name_(name) {
    setMinimumSize(240, 160);
}

void CameraTile::set_frame_counter(std::shared_ptr<std::atomic<int>> counter) {
    frame_counter_ = std::move(counter);
}

void CameraTile::set_frame(const QImage& frame) {
    if (frame_counter_) {
        frame_counter_->fetch_sub(1);  // consumed one queued frame
    }
    frame_ = frame;
    meter_.tick(FpsMeter::clock::now());  // one displayed frame → update the rate
    update();
}

void CameraTile::set_inhibited(uint32_t causes) {
    causes_ = causes;
    update();
}

void CameraTile::set_preparing(bool on) {
    preparing_ = on;
    update();
}

void CameraTile::set_status(int status) {
    preparing_ = false;  // a real stream is now driving this tile
    status_ = status;
    if (static_cast<CameraStream::Status>(status) != CameraStream::Status::Live) {
        meter_.reset();  // don't carry a stale rate across an offline gap
    }
    update();
}

void CameraTile::set_areas(std::vector<camera::CameraArea> areas) {
    areas_ = std::move(areas);
    update();
}

void CameraTile::set_zone_runtime_view(std::vector<ZoneRuntimeEntry> zones) {
    zones_ = std::move(zones);
    update();
}

void CameraTile::clear_zone_runtime_view() {
    if (zones_.empty()) {
        return;  // already clear — don't schedule a needless repaint
    }
    zones_.clear();
    update();
}

void CameraTile::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), kBg);

    if (!frame_.isNull()) {
        // Stretch-to-fill — the CCTV/NVR full-bleed look: the frame is scaled to
        // the whole tile with no bands and nothing cropped (aspect may distort
        // slightly). Same rect the ROI overlay maps onto so the polygons track
        // the displayed image.
        const QRectF img(rect());
        p.drawImage(img, frame_);
        draw_areas(p, img);
    } else if (preparing_) {
        // Model still warming on the background worker — no stream yet.
        p.setPen(kFaint);
        QFont mf = p.font();
        mf.setPointSize(16);
        p.setFont(mf);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("Preparing model…"));
    } else {
        p.setPen(kFaint);
        QFont gf = p.font();
        gf.setPointSize(28);
        p.setFont(gf);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("📷"));
    }

    // Zone runtime overlay — drawn after the frame/ROIs so it stays legible, but
    // before the banner so the banner always wins the bottom strip.
    draw_zone_panel(p);

    // Inhibit banner across the bottom — persistent while this camera's zones are
    // suppressed (areas quarantined, camera offline, detection stopped, …).
    const QString banner = inhibit_banner(causes_);
    if (!banner.isEmpty()) {
        const int bh = 26;
        const QRectF bar(0, height() - bh, width(), bh);
        p.fillRect(bar, QColor(180, 83, 9, 220));  // amber
        p.setPen(QColor(255, 255, 255));
        p.drawText(bar, Qt::AlignCenter, banner);
    }

    // Overlays: name (top-left) + status dot & label (top-right). While preparing
    // there is no stream, so show a matching "Preparing…" label instead of the
    // default (Connecting) status word.
    const StatusLook look = preparing_
        ? StatusLook{QColor(250, 204, 21), QStringLiteral("Preparing…")}
        : look_for(status_);
    p.setPen(kName);
    p.drawText(rect().adjusted(10, 8, -10, 0), Qt::AlignTop | Qt::AlignLeft, name_);

    const QString status_text = look.text;
    const QRectF tr = rect().adjusted(0, 8, -10, 0);
    p.setPen(kFaint);
    p.drawText(tr, Qt::AlignTop | Qt::AlignRight, status_text);
    // Dot just left of the status text.
    const qreal text_w = p.fontMetrics().horizontalAdvance(status_text);
    p.setBrush(look.dot);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(width() - 10 - text_w - 8, 8 + p.fontMetrics().ascent() / 2.0),
                  4, 4);

    // Live fps, a line below the status (only while actually streaming).
    if (static_cast<CameraStream::Status>(status_) == CameraStream::Status::Live &&
        meter_.fps() > 0.0) {
        const QString fps_text = QStringLiteral("%1 fps").arg(meter_.fps(), 0, 'f', 1);
        p.setPen(kFaint);
        p.drawText(rect().adjusted(0, 8 + p.fontMetrics().lineSpacing(), -10, 0),
                   Qt::AlignTop | Qt::AlignRight, fps_text);
    }
}

// Compact zone panel, bottom-left, sitting ABOVE the inhibit banner. Deliberately
// small and semi-transparent: this is a verification aid over the very display
// the operator is inspecting, so it must never become the thing in the way.
// Renders only what the projection says — no state is inferred here.
namespace {

// The state's label and colour. Split from the text so the row CONTENT stays a
// pure, directly-assertable function while the palette stays here with the rest
// of the painting.
QString zone_state_label(ZoneDisplayState s) {
    switch (s) {
        case ZoneDisplayState::Healthy:          return QStringLiteral("OK");
        case ZoneDisplayState::HoldingLastValid: return QStringLiteral("HOLD");
        case ZoneDisplayState::Acquiring:        return QStringLiteral("ACQUIRING");
        case ZoneDisplayState::Inhibited:        return QStringLiteral("INHIBITED");
        case ZoneDisplayState::Paused:           return QStringLiteral("PAUSED");
        case ZoneDisplayState::Conflict:         return QStringLiteral("CONFLICT");
    }
    return QStringLiteral("ACQUIRING");
}

QColor zone_state_colour(ZoneDisplayState s) {
    switch (s) {
        case ZoneDisplayState::Healthy:          return kZoneOk;
        case ZoneDisplayState::HoldingLastValid: return kZoneHold;
        case ZoneDisplayState::Acquiring:        return kFaint;
        // A hold is NOT an inhibit: HOLD keeps its own colour above so an
        // operator can tell "still reading, value retained" from "stopped".
        case ZoneDisplayState::Inhibited:
        case ZoneDisplayState::Paused:
        case ZoneDisplayState::Conflict:         return kZoneStopped;
    }
    return kFaint;
}

} // namespace

QString zone_row_text(const ZoneRuntimeEntry& z) {
    // A number is shown ONLY where the projection carries one — Acquiring,
    // Inhibited, Paused and Conflict all render "--" by construction, because
    // `value` is nullopt for every one of them.
    const QString num = z.value ? QString::number(*z.value) : QStringLiteral("--");
    return QStringLiteral("Z%1 %2 %3")
        .arg(z.zone_no)
        .arg(num, 5)      // right-aligned so digits line up
        .arg(zone_state_label(z.state));
}

void CameraTile::draw_zone_panel(QPainter& p) const {
    if (zones_.empty()) {
        return;
    }
    struct Row { QString text; QColor colour; };
    std::vector<Row> rows;
    rows.reserve(zones_.size());
    for (const ZoneRuntimeEntry& z : zones_) {
        rows.push_back({zone_row_text(z), zone_state_colour(z.state)});
    }

    p.save();
    QFont f = p.font();
    f.setPointSize(9);
    f.setFamily(QStringLiteral("monospace"));  // stable column alignment
    p.setFont(f);
    const QFontMetrics fm(f);

    const int line_h = fm.lineSpacing();
    int text_w = 0;
    for (const Row& r : rows) {
        text_w = std::max(text_w, fm.horizontalAdvance(r.text));
    }
    const int pad = 6;
    const int panel_h = static_cast<int>(rows.size()) * line_h + pad * 2;
    const int panel_w = text_w + pad * 2;
    // Sit above the inhibit banner when one is showing, so the two never overlap.
    const int banner_h = inhibit_banner(causes_).isEmpty() ? 0 : 26;
    const int top = height() - banner_h - panel_h - 8;
    const QRectF panel(8, top, panel_w, panel_h);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 130));  // translucent, not an opaque block
    p.drawRoundedRect(panel, 4, 4);

    int y = top + pad + fm.ascent();
    for (const Row& r : rows) {
        p.setPen(r.colour);
        p.drawText(QPointF(8 + pad, y), r.text);
        y += line_h;
    }
    p.restore();
}

void CameraTile::draw_areas(QPainter& p, const QRectF& image_rect) const {
    if (areas_.empty()) {
        return;  // no ROIs → raw frame, nothing overlaid
    }
    p.save();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kRoi, 2.0));
    for (const camera::CameraArea& area : areas_) {
        if (area.points.size() < 3) {
            continue;  // not a polygon
        }
        QPolygonF poly;
        for (const camera::Point& pt : area.points) {
            poly << to_widget(QPointF(pt.x, pt.y), image_rect);
        }
        p.drawPolygon(poly);  // closed outline
        if (!area.name.empty()) {
            p.drawText(poly.first() + QPointF(4, -4),
                       QString::fromStdString(area.name));
        }
    }
    p.restore();
}

} // namespace denso::ui
