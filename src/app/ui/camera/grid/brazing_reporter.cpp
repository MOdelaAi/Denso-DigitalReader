#include "ui/camera/grid/brazing_reporter.h"

#include <QTimer>

#include <utility>

namespace denso::ui {

BrazingReporter::BrazingReporter(std::unique_ptr<BrazingTransport> transport,
                                 QObject* parent)
    : QObject(parent), transport_(std::move(transport)) {
    retry_timer_ = new QTimer(this);
    retry_timer_->setSingleShot(true);
    QObject::connect(retry_timer_, &QTimer::timeout, this,
                     [this] { apply(policy_.on_retry_tick()); });
}

BrazingReporter::~BrazingReporter() = default;

void BrazingReporter::submit(const std::map<int, int>& snapshot) {
    apply(policy_.submit(snapshot));
}

void BrazingReporter::apply(const RetryAction& action) {
    switch (action.kind) {
        case RetryAction::Kind::None:
            return;
        case RetryAction::Kind::Send:
            transport_->post(action.snapshot, [this](bool ok) {
                // Back on the GUI thread (BrazingClient invokes done from the
                // reply handler, which runs on the GUI thread).
                apply(policy_.on_result(ok));
            });
            return;
        case RetryAction::Kind::ArmRetry:
            retry_timer_->start(action.delay_ms);
            return;
    }
}

} // namespace denso::ui
