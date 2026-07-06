// The dialogs' shared "run blocking OS work off the GUI thread, post the result
// back" idiom, extracted from SettingsDialog::run_async and the inline
// QThread::create in CameraDialog::capture_snapshot. Leaf: depends only on Qt.
#pragma once

#include <functional>

class QObject;
class QThread;

namespace denso::ui::common {

/// Run `work` on a throwaway worker QThread (auto-deleted when it finishes). A
/// real QThread is used (not QtConcurrent) so QProcess inside `work` has an
/// event dispatcher — the Qt analog of std::thread + upgrade_in_event_loop.
/// Returns the QThread* so the caller can track it and wait() on teardown; do
/// not delete it (it self-deletes via deleteLater on finish).
QThread* run_on_worker(std::function<void()> work);

/// Post `fn` to `ctx`'s thread (the GUI thread) as a queued call. Use from
/// inside worker `work` to marshal results back to widgets.
void post_to_gui(QObject* ctx, std::function<void()> fn);

} // namespace denso::ui::common
