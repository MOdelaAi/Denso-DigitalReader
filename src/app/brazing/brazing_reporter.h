// GUI-thread coordinator: turns a stream of full zone snapshots into reliable
// delivery. Owns a BrazingRetryPolicy (decides what to do), a BrazingTransport
// (does the POST), and a single-shot retry QTimer. submit() is called via
// common::post_to_gui from the ZoneReporter, so everything here runs on the GUI
// thread — no locking. Mirrors ZoneReporter as a thin shell (no unit test;
// covered by the integration smoke). Retry state is in-memory only.
#pragma once

#include "brazing/brazing_retry_policy.h"
#include "brazing/brazing_transport.h"

#include <QObject>

#include <map>
#include <memory>

class QTimer;

namespace denso::ui {

class BrazingReporter : public QObject {
    Q_OBJECT

public:
    explicit BrazingReporter(std::unique_ptr<BrazingTransport> transport,
                             QObject* parent = nullptr);
    ~BrazingReporter() override;

    /// Hand in the latest full zone snapshot to (eventually) deliver.
    void submit(const std::map<int, ZoneValue>& snapshot);

signals:
    /// The outcome of ONE delivery attempt, reported from the same
    /// QPointer-guarded result callback the retry policy is driven from — so a
    /// listener sees exactly what the policy saw, and a reply landing after this
    /// reporter is destroyed emits nothing.
    ///
    /// These exist for the top-bar status indicator. They carry no payload: a
    /// reading value must never travel to a widget that only needs to know
    /// whether the last POST worked, and the failure detail is already in the
    /// bounded log. Retry scheduling needs no separate signal — a failure is what
    /// arms the retry, so `delivery_failed` already marks "failing or retrying".
    void delivery_succeeded();
    void delivery_failed();

private:
    void apply(const RetryAction& action);  // execute one policy instruction

    std::unique_ptr<BrazingTransport> transport_;
    BrazingRetryPolicy policy_;
    QTimer* retry_timer_ = nullptr;  // single-shot; owned via QObject parent
};

} // namespace denso::ui
