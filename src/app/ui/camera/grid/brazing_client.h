// Pushes the combined zone snapshot to the brazing backend. Lives on the GUI
// thread; send() fires an async POST to {base_url}/api/brazing/update via
// QNetworkAccessManager with a bounded timeout. Best-effort: a failed/slow/
// unreachable POST is logged (throttled) and dropped — no queue, no retry (the
// next zone change re-sends the full snapshot). The ZoneReporter marshals
// snapshots here with common::post_to_gui.
#pragma once

#include <QObject>
#include <QString>

#include <map>
#include <string>

class QNetworkAccessManager;

namespace denso::ui {

class BrazingClient : public QObject {
    Q_OBJECT

public:
    explicit BrazingClient(std::string base_url, QObject* parent = nullptr);

    /// POST {"zone<n>": value, ...}. No-op if base_url is empty.
    void send(const std::map<int, int>& zones);

private:
    QString base_url_;
    QNetworkAccessManager* nam_ = nullptr;
};

} // namespace denso::ui
