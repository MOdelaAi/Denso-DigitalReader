// Transport for the brazing backend: one async POST of the combined zone
// snapshot to {base_url}{api_path} via QNetworkAccessManager, with a bounded
// timeout. Reports the outcome via the `done` callback — it holds NO
// retry/pending state (the BrazingReporter owns that). Lives on the GUI thread.
#pragma once

#include "brazing/brazing_transport.h"
#include "brazing/url.h"   // kDefaultApiPath — the ONE definition of the default

#include <QObject>
#include <QString>

#include <cstddef>   // std::nullptr_t
#include <functional>
#include <map>
#include <string>

class QNetworkAccessManager;

namespace denso::ui {

class BrazingClient : public QObject, public BrazingTransport {
    Q_OBJECT

public:
    /// `base_url` is the persisted SERVER BASE (scheme://host[:port]) and
    /// `api_path` the persisted reporting API path (defaulting to the shipped
    /// brazing::kDefaultApiPath, so a caller that has no configured path behaves
    /// exactly as this client always did). The full endpoint is composed once
    /// here through brazing::endpoint_url(), which is also the defensive guard
    /// against a legacy or externally written base that already ends in the
    /// endpoint — appending the path to such a value blindly is what produced the
    /// doubled path.
    explicit BrazingClient(std::string base_url,
                           std::string api_path = denso::brazing::kDefaultApiPath,
                           QObject* parent = nullptr);

    /// `BrazingClient(base, nullptr)` used to mean "no parent". `api_path` now
    /// occupies that position, and `nullptr` converts to std::string through
    /// const char* — so the call would still COMPILE and then construct a string
    /// from a null pointer at run time. Every other spelling of a parent is
    /// already a compile error (QObject* does not convert to std::string); this
    /// deletion makes the one silent case an error too, naming this function.
    BrazingClient(std::string base_url, std::nullptr_t,
                  QObject* parent = nullptr) = delete;

    /// POST {"zone<n>": value, ...}. Calls done(false) immediately if base_url
    /// is empty. done(ok): ok == HTTP 2xx.
    void post(const std::map<int, ZoneValue>& zones,
              std::function<void(bool)> done) override;

    /// The URL this client posts to; empty when no address is configured.
    /// Exposed so a test can prove the composed endpoint WITHOUT a network.
    const QString& endpoint() const { return endpoint_; }

private:
    QString endpoint_;   // {canonical base}{canonical api path}, or empty
    QNetworkAccessManager* nam_ = nullptr;
};

} // namespace denso::ui
