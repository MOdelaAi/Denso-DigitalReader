// Transport for the brazing backend: one async POST of the combined zone
// snapshot to {base_url}/api/brazing/update via QNetworkAccessManager, with a
// bounded timeout. Reports the outcome via the `done` callback — it holds NO
// retry/pending state (the BrazingReporter owns that). Lives on the GUI thread.
#pragma once

#include "brazing/brazing_transport.h"

#include <QObject>
#include <QString>

#include <functional>
#include <map>
#include <string>

class QNetworkAccessManager;

namespace denso::ui {

class BrazingClient : public QObject, public BrazingTransport {
    Q_OBJECT

public:
    explicit BrazingClient(std::string base_url, QObject* parent = nullptr);

    /// POST {"zone<n>": value, ...}. Calls done(false) immediately if base_url
    /// is empty. done(ok): ok == HTTP 2xx.
    void post(const std::map<int, int>& zones,
              std::function<void(bool)> done) override;

private:
    QString base_url_;
    QNetworkAccessManager* nam_ = nullptr;
};

} // namespace denso::ui
