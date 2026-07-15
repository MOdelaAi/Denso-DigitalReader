#include "brazing/brazing_client.h"

#include "logging/redact.h"

#include "brazing/brazing_payload.h"

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

BrazingClient::BrazingClient(std::string base_url, QObject* parent)
    : QObject(parent),
      base_url_(QString::fromStdString(base_url)),
      nam_(new QNetworkAccessManager(this)) {
    // Trim a trailing slash so base_url + path doesn't double up.
    while (base_url_.endsWith('/')) {
        base_url_.chop(1);
    }
}

void BrazingClient::post(const std::map<int, int>& zones,
                         std::function<void(bool)> done) {
    if (base_url_.isEmpty()) {
        if (done) done(false);
        return;
    }
    const QUrl url(base_url_ + QStringLiteral("/api/brazing/update"));
    if (!url.isValid()) {
        qWarning().noquote() << "[brazing] invalid base URL:"
                             << QString::fromStdString(
                                    logging::sanitize_url(base_url_.toStdString()));
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
