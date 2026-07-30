// Zone Runtime Overlay — rendered CONTENT and offscreen GEOMETRY.
//
// Two kinds of assertion, deliberately separated:
//   • content — `zone_row_text()` is pure, so what each state actually shows is
//     asserted exactly, not inferred from pixels;
//   • geometry — the tile is rendered offscreen (QWidget::grab, no window
//     manager, no camera) at real tile sizes to prove the panel stays inside the
//     tile, does not blanket the frame, and clears when the overlay is dropped.
//
// Runs in denso_integration_tests: the single offscreen QApplication comes from
// integration_main.cpp. Nothing here opens a device, a model or a socket.
#include <catch2/catch_test_macros.hpp>

#include "brazing/zone_runtime.h"
#include "ui/camera/grid/camera_tile.h"

#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QString>

#include <optional>
#include <vector>

using denso::ui::CameraTile;
using denso::ui::ZoneDisplayState;
using denso::ui::ZoneRuntimeEntry;
using denso::ui::zone_row_text;

namespace {

ZoneRuntimeEntry entry(int zone_no, ZoneDisplayState state,
                       std::optional<int> value = std::nullopt) {
    ZoneRuntimeEntry e;
    e.camera_id = 1;
    e.zone_no = zone_no;
    e.state = state;
    e.value = value;
    return e;
}

// Render a tile offscreen at `w` x `h` with the given overlay rows.
QImage render(int w, int h, const std::vector<ZoneRuntimeEntry>& zones) {
    CameraTile tile(QStringLiteral("cam"));
    tile.resize(w, h);
    if (!zones.empty()) {
        tile.set_zone_runtime_view(zones);
    }
    return tile.grab().toImage();
}

// How many pixels differ between two same-sized renders.
int differing_pixels(const QImage& a, const QImage& b) {
    REQUIRE(a.size() == b.size());
    int n = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y)) ++n;
        }
    }
    return n;
}

// True if the exact colour appears among the pixels the OVERLAY changed.
//
// Scanning the whole image would be useless here: the tile's "Connecting…"
// status dot and its ROI gold are both QColor(250, 204, 21) — the very amber
// kZoneHold uses — so an unrestricted probe finds amber in every render. Diffing
// against the bare tile first isolates what the zone panel actually painted.
// Glyph cores are drawn in the unblended pen colour, so an exact match holds.
bool overlay_painted_colour(const QImage& base, const QImage& over, QRgb want) {
    REQUIRE(base.size() == over.size());
    for (int y = 0; y < over.height(); ++y) {
        for (int x = 0; x < over.width(); ++x) {
            const QRgb p = over.pixel(x, y);
            if (base.pixel(x, y) == p) continue;   // untouched by the overlay
            if (qRgb(qRed(p), qGreen(p), qBlue(p)) == want) return true;
        }
    }
    return false;
}

// True if any pixel outside the given inset differs from the baseline — used to
// prove the panel does not spill past the tile edges.
bool differs_in_border(const QImage& base, const QImage& over, int inset) {
    for (int y = 0; y < base.height(); ++y) {
        for (int x = 0; x < base.width(); ++x) {
            const bool in_border = x < inset || y < inset ||
                                   x >= base.width() - inset ||
                                   y >= base.height() - inset;
            if (in_border && base.pixel(x, y) != over.pixel(x, y)) return true;
        }
    }
    return false;
}

} // namespace

// ── Rendered content ─────────────────────────────────────────────────────────

// MUTATION: "hide the accepted value while Healthy" must die.
TEST_CASE("Healthy row shows its accepted value", "[zone_overlay][render]") {
    CHECK(zone_row_text(entry(1, ZoneDisplayState::Healthy, 128))
              .contains(QStringLiteral("128")));
    CHECK(zone_row_text(entry(1, ZoneDisplayState::Healthy, 128))
              .contains(QStringLiteral("OK")));
}

// MUTATION: "render Hold as Inhibited", and "hide the last valid value during
// hold", must both die. A hold still has a trustworthy number to show.
TEST_CASE("Hold row shows the last valid value and reads as HOLD",
          "[zone_overlay][render]") {
    const QString hold = zone_row_text(entry(2, ZoneDisplayState::HoldingLastValid, 95));
    CHECK(hold.contains(QStringLiteral("95")));
    CHECK(hold.contains(QStringLiteral("HOLD")));
    CHECK_FALSE(hold.contains(QStringLiteral("INHIBITED")));
}

// MUTATION: "render a numeric value while Inhibited / Paused / Conflict" must
// die. None of these states carries a reading anyone should act on.
TEST_CASE("states without a trusted reading show no number", "[zone_overlay][render]") {
    const ZoneDisplayState mute[] = {
        ZoneDisplayState::Acquiring, ZoneDisplayState::Inhibited,
        ZoneDisplayState::Paused,    ZoneDisplayState::Conflict};
    for (const ZoneDisplayState s : mute) {
        const QString row = zone_row_text(entry(3, s));
        INFO("row: " << row.toStdString());
        CHECK(row.contains(QStringLiteral("--")));
        // "Z3" is the only digit allowed anywhere in the row.
        const QString after_zone = row.mid(2);
        for (const QChar c : after_zone) {
            CHECK_FALSE(c.isDigit());
        }
    }
}

TEST_CASE("zone rows render in the order the grid supplies", "[zone_overlay][render]") {
    const std::vector<ZoneRuntimeEntry> zones{
        entry(1, ZoneDisplayState::Healthy, 128),
        entry(2, ZoneDisplayState::HoldingLastValid, 95),
        entry(3, ZoneDisplayState::Acquiring),
        entry(4, ZoneDisplayState::Inhibited),
    };
    QStringList rows;
    for (const auto& z : zones) rows << zone_row_text(z);
    CHECK(rows.at(0).startsWith(QStringLiteral("Z1")));
    CHECK(rows.at(1).startsWith(QStringLiteral("Z2")));
    CHECK(rows.at(2).startsWith(QStringLiteral("Z3")));
    CHECK(rows.at(3).startsWith(QStringLiteral("Z4")));
    // Right-aligned numeric column: a 3-digit and a 2-digit reading END at the
    // same offset, so the units digits line up down the panel.
    CHECK(rows.at(0).indexOf(QStringLiteral("128")) + 3 ==
          rows.at(1).indexOf(QStringLiteral("95")) + 2);
}

// ── Offscreen geometry ───────────────────────────────────────────────────────

TEST_CASE("overlay renders at the smallest supported tile and stays inside it",
          "[zone_overlay][render]") {
    // 240x160 is the documented minimum tile (spec §9) — a 2x2 wall on a small
    // panel. Four zones is the most one camera realistically carries.
    const std::vector<ZoneRuntimeEntry> zones{
        entry(1, ZoneDisplayState::Healthy, 128),
        entry(2, ZoneDisplayState::HoldingLastValid, 95),
        entry(3, ZoneDisplayState::Acquiring),
        entry(4, ZoneDisplayState::Inhibited),
    };
    const QImage base = render(240, 160, {});
    const QImage over = render(240, 160, zones);

    REQUIRE(base.size() == over.size());
    const int changed = differing_pixels(base, over);
    CHECK(changed > 0);                                   // something was drawn
    // ...and it is a compact panel, not a blanket over the feed.
    CHECK(changed < base.width() * base.height() * 40 / 100);
    // Nothing bleeds into the outer 4 px frame of the tile.
    CHECK_FALSE(differs_in_border(base, over, 4));
}

TEST_CASE("overlay renders at a full-screen single-tile size",
          "[zone_overlay][render]") {
    const std::vector<ZoneRuntimeEntry> zones{
        entry(1, ZoneDisplayState::Healthy, 1234),
        entry(2, ZoneDisplayState::Conflict),
    };
    const QImage base = render(1920, 1080, {});
    const QImage over = render(1920, 1080, zones);
    const int changed = differing_pixels(base, over);
    CHECK(changed > 0);
    // The panel is a fixed point size, so on a big tile it covers very little.
    CHECK(changed < base.width() * base.height() * 10 / 100);
    CHECK_FALSE(differs_in_border(base, over, 4));
}

// MUTATION: "render Hold identically to Inhibited" must die at the pixel level
// too — the two states carry different COLOURS, not just different words.
// A plain image diff is not enough: the row texts already differ, so the images
// differ even when both are painted in the stopped colour. Probe the palette.
TEST_CASE("Hold is visually distinct from Inhibited", "[zone_overlay][render]") {
    const QImage bare = render(320, 240, {});
    const QImage hold = render(320, 240, {entry(1, ZoneDisplayState::HoldingLastValid, 95)});
    const QImage inh  = render(320, 240, {entry(1, ZoneDisplayState::Inhibited)});
    CHECK(differing_pixels(hold, inh) > 0);

    // kZoneHold (amber) must be painted by the hold panel and by no part of the
    // inhibited one, which uses kZoneStopped (red).
    const QRgb amber = qRgb(250, 204, 21);
    CHECK(overlay_painted_colour(bare, hold, amber));
    CHECK_FALSE(overlay_painted_colour(bare, inh, amber));
}

TEST_CASE("a different accepted value renders differently", "[zone_overlay][render]") {
    const QImage a = render(320, 240, {entry(1, ZoneDisplayState::Healthy, 128)});
    const QImage b = render(320, 240, {entry(1, ZoneDisplayState::Healthy, 999)});
    CHECK(differing_pixels(a, b) > 0);
}

// MUTATION: "retain a stale camera/zone overlay" must die. Dropping the view has
// to restore the bare tile exactly — a leftover panel would show numbers for a
// camera that no longer reports.
TEST_CASE("clearing the overlay leaves no stale pixels", "[zone_overlay][render]") {
    CameraTile tile(QStringLiteral("cam"));
    tile.resize(320, 240);
    const QImage bare = tile.grab().toImage();

    tile.set_zone_runtime_view({entry(1, ZoneDisplayState::Healthy, 128)});
    const QImage shown = tile.grab().toImage();
    REQUIRE(differing_pixels(bare, shown) > 0);

    tile.clear_zone_runtime_view();
    CHECK(differing_pixels(bare, tile.grab().toImage()) == 0);
}
