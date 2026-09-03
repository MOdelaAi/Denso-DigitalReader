#include "brazing/brazing_client.h"

#include "logging/redact.h"

#include "brazing/brazing_payload.h"
#include "brazing/url.h"

#include <QByteArray>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QVariant>

namespace denso::ui {

namespace {
constexpr int kBrazingTimeoutMs = 5000;  // abort a stuck POST (soak-safe)
}

BrazingClient::BrazingClient(std::string base_url, std::string api_path,
                             QObject* parent)
    : QObject(parent),
      // ONE composition site, and the ONE place the base and the configured path
      // are joined. brazing::endpoint_url() owns both the trailing-slash trim
      // this used to do inline and the guard against a stored base that already
      // ends in the endpoint (which would otherwise post to …/update/api/…).
      endpoint_(QString::fromStdString(
          denso::brazing::endpoint_url(base_url, api_path))),
      nam_(new QNetworkAccessManager(this)) {
    // A non-empty address that yields no endpoint was REFUSED by the shared
    // normalizer — either half of it can be the reason. The grid already declines
    // to build a sender for such a configuration, so reaching here means
    // something constructed a client directly — say it once rather than sit
    // silent for the life of the process.
    if (endpoint_.isEmpty() && !base_url.empty()) {
        qWarning().noquote() << "[brazing] unusable server address or reporting"
                             << "API path; nothing will be sent:"
                             << QString::fromStdString(
                                    logging::sanitize_url(base_url))
                             << QString::fromStdString(api_path);
    }
}

void BrazingClient::post(const std::map<int, ZoneValue>& zones,
                         std::function<void(bool)> done) {
    if (endpoint_.isEmpty()) {
        if (done) done(false);
        return;
    }
    const QUrl url(endpoint_);
    if (!url.isValid()) {
        qWarning().noquote() << "[brazing] invalid base URL:"
                             << QString::fromStdString(
                                    logging::sanitize_url(endpoint_.toStdString()));
        if (done) done(false);
        return;
    }
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QByteArrayLiteral("application/json"));
    req.setTransferTimeout(kBrazingTimeoutMs);

    const QByteArray body =
        QByteArray::fromStdString(build_brazing_payload(zones));
    QNetworkReply* reply = nam_->post(req, body);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done] {
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 &&
                        status < 300;
        if (!ok) {
            qWarning().noquote()
                << "[brazing] POST failed (will retry):" << reply->errorString();
        }
        reply->deleteLater();
        if (done) done(ok);
    });
}

} // namespace denso::ui
