#include "brazing/brazing_reporter.h"

#include <QDebug>
#include <QPointer>
#include <QString>
#include <QStringList>
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

BrazingReporter::~BrazingReporter() {
    // The reporter is destroyed by CameraGrid::teardown() — the pre-transaction
    // teardown of a MODE SWITCH — and when a Settings save retires it in favour of
    // a differently-configured one. An ordinary camera rebuild (the wizard
    // closing, Refresh Cameras) deliberately does NOT destroy it, precisely so a
    // pending snapshot survives that. If the server never acked the last
    // snapshot, that snapshot dies here. Dropping it is CORRECT on the paths that
    // do reach here (the old mode's readings must not be posted after a switch,
    // spec §6.6; the old server's must not be posted to the new one) but must not
    // be silent, so record what was lost.
    //
    // Zone NUMBERS and their count only — never a reading value (spec §11-R4).
    // This is a log line, not a report: it neither retries nor re-sends anything.
    //
    // Wrapped because a destructor is implicitly noexcept: every step here
    // allocates (the vector, the QStrings, the join, Qt's own formatting), so a
    // std::bad_alloc escaping would turn a teardown into std::terminate. A
    // best-effort diagnostic must never be able to kill a 24/7 appliance — losing
    // the line is the correct trade against losing the process.
    try {
        if (const auto zones = policy_.pending_zone_numbers()) {
            QStringList names;
            names.reserve(static_cast<qsizetype>(zones->size()));
            for (const int zone_no : *zones) names << QString::number(zone_no);
            qInfo().noquote()
                << "[brazing] discarding undelivered snapshot for" << zones->size()
                << "zones:" << names.join(QStringLiteral(", "));
        }
    } catch (...) {
        // Deliberately swallowed — see above.
    }
}

void BrazingReporter::submit(const std::map<int, ZoneValue>& snapshot) {
    apply(policy_.submit(snapshot));
}

void BrazingReporter::apply(const RetryAction& action) {
    switch (action.kind) {
        case RetryAction::Kind::None:
            return;
        case RetryAction::Kind::Send: {
            // done() runs later on the GUI thread (BrazingClient invokes it from
            // the reply handler). Guard with a QPointer so a POST completing after
            // this reporter is torn down can't call into a destroyed object —
            // don't rely on transitive QNAM/reply ownership for lifetime safety.
            QPointer<BrazingReporter> self(this);
            transport_->post(action.snapshot, [self](bool ok) {
                if (!self) return;
                // Announce BEFORE driving the policy, so the observable outcome
                // of this attempt cannot be pre-empted by whatever the policy
                // decides to do next (an immediate re-send would otherwise
                // report its own result first).
                if (ok) {
                    emit self->delivery_succeeded();
                } else {
                    emit self->delivery_failed();
                }
                self->apply(self->policy_.on_result(ok));
            });
            return;
        }
        case RetryAction::Kind::ArmRetry:
            retry_timer_->start(action.delay_ms);
            return;
    }
}

} // namespace denso::ui
