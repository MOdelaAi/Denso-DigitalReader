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
    /// `base_url` is the persisted SERVER BASE (scheme://host[:port]). The full
    /// endpoint is composed once here through brazing::endpoint_url(), which is
    /// also the defensive guard against a legacy or externally written value that
    /// already ends in /api/brazing/update — appending the path to such a value
    /// blindly is what produced the doubled path.
    explicit BrazingClient(std::string base_url, QObject* parent = nullptr);

    /// POST {"zone<n>": value, ...}. Calls done(false) immediately if base_url
    /// is empty. done(ok): ok == HTTP 2xx.
    void post(const std::map<int, ZoneValue>& zones,
              std::function<void(bool)> done) override;

    /// The URL this client posts to; empty when no address is configured.
    /// Exposed so a test can prove the composed endpoint WITHOUT a network.
    const QString& endpoint() const { return endpoint_; }

private:
    QString endpoint_;   // {canonical base}/api/brazing/update, or empty
    QNetworkAccessManager* nam_ = nullptr;
};

} // namespace denso::ui
