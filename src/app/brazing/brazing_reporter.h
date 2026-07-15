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
    void submit(const std::map<int, int>& snapshot);

private:
    void apply(const RetryAction& action);  // execute one policy instruction

    std::unique_ptr<BrazingTransport> transport_;
    BrazingRetryPolicy policy_;
    QTimer* retry_timer_ = nullptr;  // single-shot; owned via QObject parent
};

} // namespace denso::ui
