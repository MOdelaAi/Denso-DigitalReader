// The shared modal header: a bold title, a ✕ close-glyph wired to the dialog's
// reject(), and the gold underline rule. Extracted from the duplicated header
// blocks in settings_dialog.cpp and camera_dialog.cpp. Leaf: depends only on Qt.
#pragma once

#include <QString>

class QDialog;
class QVBoxLayout;

namespace denso::ui::common {

/// Build the header layout for a modal: `title` + close glyph (→ dlg->reject())
/// + gold underline. The caller adds the returned layout to its outer layout.
QVBoxLayout* dialog_header(QDialog* dlg, const QString& title);

} // namespace denso::ui::common
