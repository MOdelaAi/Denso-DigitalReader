// The application palette + theme-driven stylesheet, ported 1:1 from the Slint
// `theme.slint` global. Every colour token from the reference is reproduced;
// `palette(dark)` returns the dark or light variant, and `style_sheet()` builds
// the Qt stylesheet that dresses the widget tree in those colours (the analog
// of Slint binding `Theme.*` and `Palette.color-scheme` across the views).
#pragma once

#include <QColor>
#include <QString>

namespace denso::ui {

/// The full colour palette for one theme variant. Field names mirror the Slint
/// `Theme` global so the mapping reads 1:1.
struct Palette {
    bool dark = true;

    // backgrounds / panels
    QColor bg_grad_1;
    QColor panel;
    QColor panel_1;
    QColor panel_2;
    QColor panel_3;

    // accent (gold)
    QColor gold;
    QColor gold_300;
    QColor gold_400;
    QColor gold_500;

    // text
    QColor txt;
    QColor txt_dim;
    QColor txt_border;
    QColor txt_faint;
    QColor txt_secondary;
    QColor txt_mid;

    // status (identical in both themes)
    QColor status_ok;
    QColor status_ok_dark;
    QColor status_ok_light;
    QColor status_bad;
    QColor status_bad_dark;
    QColor status_bad_light;
    QColor status_warning;
    QColor status_warning_dark;
    QColor status_warning_light;
    QColor status_info;
    QColor status_info_dark;
    QColor status_info_light;
    QColor status_neutral;
    QColor status_neutral_dark;
    QColor status_neutral_light;

    // neutrals (identical in both themes)
    QColor neutral_dark;
    QColor neutral_medium;

    // modal scrim
    QColor overlay;
};

/// The dark (default) or light palette.
Palette palette(bool dark);

/// The application-wide stylesheet for the given palette — backgrounds, text,
/// inputs, buttons. Widgets with role colours (status lines, gold buttons) are
/// tagged by object/property and styled here.
QString style_sheet(const Palette& p);

} // namespace denso::ui
