// Small shared helpers for the Camera dialog's page widgets: the dim label
// factory and the error-text colour. Kept tiny and dependency-light so each
// page can include it without pulling in the others.
#pragma once

#include <QString>

class QLabel;

namespace denso::ui {

inline constexpr const char* kStatusBad = "#ef4444";

/// A QLabel tagged with the `dim` style property (muted secondary text).
QLabel* dim_label(const QString& text);

} // namespace denso::ui
