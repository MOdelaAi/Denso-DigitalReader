// What the top bar tells the operator about backend reporting. A RUNTIME fact,
// produced by the object that owns the sender (CameraGrid) and by nobody else —
// the indicator must never re-derive it from a second read of the database, or
// the bar and the pipeline could disagree.
//
// Deliberately NOT a connectivity report. The backend exposes only
// POST /api/brazing/update: there is no health endpoint and no persistent
// connection, so "the reporting stack is running" is the strongest true claim
// available. That is why On must never be rendered as "Connected".
#pragma once

#include <QMetaType>

namespace denso::ui {

enum class BrazingStatus {
    Off,    ///< no sender exists: reporting disabled, or no usable base URL
    On,     ///< a sender exists and nothing has failed since it was built
    Error,  ///< a sender exists and the last delivery attempt failed
};

} // namespace denso::ui

// Registered so the status can cross a queued connection unchanged if one is
// ever introduced; every connection today is direct, on the GUI thread.
Q_DECLARE_METATYPE(denso::ui::BrazingStatus)
