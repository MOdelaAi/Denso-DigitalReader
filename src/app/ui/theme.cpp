#include "ui/theme.h"

namespace denso::ui {
namespace {

/// A QColor as a CSS `rgba(...)` literal usable in a Qt stylesheet (covers the
/// alpha-bearing scrim as well as the opaque tokens).
QString css(const QColor& c) {
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(c.alpha());
}

} // namespace

Palette palette(bool dark) {
    Palette p;
    p.dark = dark;

    // backgrounds / panels
    p.bg_grad_1 = dark ? QColor("#232323") : QColor("#e8e8e8");
    p.panel = dark ? QColor("#202020") : QColor("#f5f5f5");
    p.panel_1 = dark ? QColor("#202020") : QColor("#f5f5f5");
    p.panel_2 = dark ? QColor("#343434") : QColor("#e2e2e2");
    p.panel_3 = dark ? QColor("#373737") : QColor("#dcdcdc");

    // accent (gold)
    p.gold = dark ? QColor("#ffc646") : QColor("#e69822");
    p.gold_300 = dark ? QColor("#ffeb3b") : QColor("#d4b400");
    p.gold_400 = dark ? QColor("#face48") : QColor("#d9a520");
    p.gold_500 = dark ? QColor("#f8cc47") : QColor("#d7a31f");

    // text
    p.txt = dark ? QColor("#ffffff") : QColor("#1a1a1a");
    p.txt_dim = dark ? QColor("#e9e9e9") : QColor("#3a3a3a");
    p.txt_border = dark ? QColor("#e69822") : QColor("#b8761a");
    p.txt_faint = dark ? QColor("#9ca3af") : QColor("#6b7280");
    p.txt_secondary = dark ? QColor("#e9e9e9") : QColor("#3a3a3a");
    p.txt_mid = dark ? QColor("#cccccc") : QColor("#555555");

    // status (identical in both themes)
    p.status_ok = QColor("#22c55e");
    p.status_ok_dark = QColor("#16a34a");
    p.status_ok_light = QColor("#4ade80");
    p.status_bad = QColor("#ef4444");
    p.status_bad_dark = QColor("#b91c1c");
    p.status_bad_light = QColor("#f87171");
    p.status_warning = QColor("#f59e0b");
    p.status_warning_dark = QColor("#d97706");
    p.status_warning_light = QColor("#fbbf24");
    p.status_info = QColor("#8b5cf6");
    p.status_info_dark = QColor("#7c3aed");
    p.status_info_light = QColor("#a78bfa");
    p.status_neutral = QColor("#6b7280");
    p.status_neutral_dark = QColor("#4b5563");
    p.status_neutral_light = QColor("#9ca3af");

    // neutrals (identical in both themes)
    p.neutral_dark = QColor("#4b5563");
    p.neutral_medium = QColor("#6b7280");

    // modal scrim (#00000099)
    p.overlay = QColor(0, 0, 0, 0x99);

    return p;
}

QString style_sheet(const Palette& p) {
    // Mirrors how the Slint views bind Theme.* and keep std-widgets in step
    // with Palette.color-scheme: one sheet drives the whole tree.
    return QStringLiteral(R"(
        QWidget { color: %(txt); background: transparent; }
        QMainWindow, QDialog { background: %(panel2); }
        #mainContent { background: %(panel); }
        #topBar { background: %(panel2); }

        QLabel { background: transparent; color: %(txt); }
        QLabel[dim="true"] { color: %(txtDim); }
        QLabel[faint="true"] { color: %(txtFaint); }
        #goldUnderline { background: %(gold); border-radius: 2px; }

        QPushButton {
            background: %(panel3); color: %(txt);
            border: none; border-radius: 8px; padding: 6px 14px;
        }
        QPushButton:hover { background: %(panel2); }
        QPushButton[gold="true"] {
            background: %(gold); color: #202020; font-weight: 600;
        }
        QPushButton[gold="true"]:hover { background: %(gold300); }
        QPushButton[gold="true"]:pressed { background: %(gold500); }
        QPushButton[flatText="true"] {
            background: transparent; color: %(txtFaint); padding: 6px 10px;
        }
        QPushButton[flatText="true"]:hover { background: %(panel3); color: %(txt); }

        QLineEdit {
            background: %(panel); color: %(txt);
            border: 1px solid %(panel3); border-radius: 6px; padding: 4px 6px;
            selection-background-color: %(gold);
        }
        QComboBox {
            background: %(panel); color: %(txt);
            border: 1px solid %(panel3); border-radius: 6px; padding: 4px 6px;
        }
        QComboBox QAbstractItemView {
            background: %(panel2); color: %(txt);
            selection-background-color: %(panel3);
        }
        QCheckBox { color: %(txt); background: transparent; }

        #navList { background: transparent; border: none; }
        #navList::item {
            color: %(txtDim); padding: 8px 10px; border-radius: 6px; margin: 2px 0px;
        }
        #navList::item:selected { background: %(panel3); color: %(gold); }
        #navList::item:hover { background: %(panel3); }

        #card { background: %(panel3); border-radius: 10px; }
        #dialogPanel {
            background: %(panel2);
            border: 1px solid %(panel3); border-radius: 12px;
        }
        QScrollArea { border: none; background: transparent; }
    )")
        .replace(QStringLiteral("%(txt)"), css(p.txt))
        .replace(QStringLiteral("%(txtDim)"), css(p.txt_dim))
        .replace(QStringLiteral("%(txtFaint)"), css(p.txt_faint))
        .replace(QStringLiteral("%(panel2)"), css(p.panel_2))
        .replace(QStringLiteral("%(panel3)"), css(p.panel_3))
        .replace(QStringLiteral("%(panel)"), css(p.panel))
        .replace(QStringLiteral("%(gold300)"), css(p.gold_300))
        .replace(QStringLiteral("%(gold500)"), css(p.gold_500))
        .replace(QStringLiteral("%(gold)"), css(p.gold));
}

} // namespace denso::ui
