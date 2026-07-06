#include "ui/camera/grid/brazing_client.h"

#include "ui/camera/grid/brazing_payload.h"

#include <QByteArray>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

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

void BrazingClient::send(const std::map<int, int>& zones) {
    if (base_url_.isEmpty()) {
        return;
    }
    const QUrl url(base_url_ + QStringLiteral("/api/brazing/update"));
    if (!url.isValid()) {
        qWarning().noquote() << "[brazing] invalid base URL:" << base_url_;
        return;
    }
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QByteArrayLiteral("application/json"));
    req.setTransferTimeout(kBrazingTimeoutMs);

    const QByteArray body =
        QByteArray::fromStdString(build_brazing_payload(zones));
    QNetworkReply* reply = nam_->post(req, body);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply] {
        if (reply->error() != QNetworkReply::NoError) {
            // Best-effort: log and drop. Next change re-sends the full snapshot.
            qWarning().noquote() << "[brazing] POST failed:" << reply->errorString();
        }
        reply->deleteLater();
    });
}

} // namespace denso::ui
