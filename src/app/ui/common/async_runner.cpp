#include "ui/common/async_runner.h"

#include <QMetaObject>
#include <QObject>
#include <QThread>

#include <utility>

namespace denso::ui::common {

void run_on_worker(std::function<void()> work) {
    auto* thread = QThread::create(std::move(work));
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void post_to_gui(QObject* ctx, std::function<void()> fn) {
    QMetaObject::invokeMethod(ctx, std::move(fn), Qt::QueuedConnection);
}

} // namespace denso::ui::common
