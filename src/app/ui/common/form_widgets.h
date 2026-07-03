// Small shared label/row factories used by the app's dialogs. Extracted from
// the copies that lived in settings_dialog.cpp's anon namespace and camera's
// page_util, so there is a single definition. Leaf: depends only on Qt.
#pragma once

#include <QString>

class QFrame;
class QLabel;
class QWidget;

namespace denso::ui::common {

/// Bold, letter-spaced, faint section header (the "APPEARANCE"/"NETWORK" caps).
QLabel* eyebrow(const QString& text);

/// A dimmed secondary label (property "dim").
QLabel* dim_label(const QString& text);

/// A "label … value" row; the value QLabel is returned via value_out for later
/// text updates.
QWidget* spec_row(const QString& label, QLabel** value_out);

/// A 1px horizontal divider.
QFrame* hline();

} // namespace denso::ui::common
