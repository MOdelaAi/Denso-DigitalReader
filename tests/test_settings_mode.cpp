// Slice 5 — the Target Mode selector in Settings emits an INTENT only, and never
// for the currently-active mode. This is the task-sanctioned "test-only signal
// assertion": it drives the real SettingsDialog widgets offscreen and asserts the
// switch_mode_requested contract WITHOUT any MainWindow orchestration (that is
// Slice 7). It writes nothing and tears nothing down.
//
// Runs in denso_integration_tests (Qt Widgets); the offscreen QApplication is
// provided once by integration_main.cpp.
#include <catch2/catch_test_macros.hpp>

#include "db/db.h"
#include "mode/mode.h"
#include "ui/settings/settings_dialog.h"

#include <QComboBox>
#include <QObject>
#include <QPushButton>

using denso::mode::TargetMode;

TEST_CASE("Settings mode selector emits an intent only, never for the active mode",
          "[settings_mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));

    denso::ui::SettingsDialog dlg(db->handle());
    auto* combo = dlg.findChild<QComboBox*>(QStringLiteral("modeSelect"));
    auto* btn = dlg.findChild<QPushButton*>(QStringLiteral("switchAndResetButton"));
    REQUIRE(combo != nullptr);
    REQUIRE(btn != nullptr);

    int emit_count = 0;
    int last_target = -1;
    QObject::connect(&dlg, &denso::ui::SettingsDialog::switch_mode_requested,
                     [&](int t) { ++emit_count; last_target = t; });

    // Seeding the current mode must NOT emit, and must disable the button because
    // the selected mode now equals the current mode.
    dlg.set_current_mode(TargetMode::DigitReader);
    CHECK(emit_count == 0);
    CHECK(combo->currentData().toInt() == static_cast<int>(TargetMode::DigitReader));
    CHECK_FALSE(btn->isEnabled());

    // Selecting the OTHER mode enables the button but is a bare change — no intent.
    const int ball_row = combo->findData(static_cast<int>(TargetMode::BallLeveler));
    REQUIRE(ball_row >= 0);
    combo->setCurrentIndex(ball_row);
    CHECK(emit_count == 0);
    CHECK(btn->isEnabled());

    // Clicking emits exactly one intent carrying the selected target.
    btn->click();
    CHECK(emit_count == 1);
    CHECK(last_target == static_cast<int>(TargetMode::BallLeveler));

    // Re-seeding to the now-selected mode disables the button again and never emits.
    dlg.set_current_mode(TargetMode::BallLeveler);
    CHECK(emit_count == 1);  // unchanged — seeding is not an intent
    CHECK_FALSE(btn->isEnabled());
}
