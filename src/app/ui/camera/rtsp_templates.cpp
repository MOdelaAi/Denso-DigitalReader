#include "ui/camera/rtsp_templates.h"

namespace denso::ui {

const std::vector<RtspManufacturer>& rtsp_manufacturers() {
    static const std::vector<RtspManufacturer> list = {
        {QStringLiteral("Dahua"),
         QStringLiteral("rtsp://%1:554/cam/realmonitor?channel=1&subtype=0"),
         QStringLiteral("rtsp://%1:554/cam/realmonitor?channel=1&subtype=1")},
    };
    return list;
}

QString build_rtsp(const RtspManufacturer& manufacturer, const QString& ip, bool substream) {
    return (substream ? manufacturer.sub_template : manufacturer.main_template).arg(ip);
}

QString with_credentials(const QString& rtsp, const QString& user, const QString& password) {
    const QString scheme = QStringLiteral("rtsp://");
    if (user.isEmpty() || !rtsp.startsWith(scheme)) {
        return rtsp;
    }
    QString creds = user;
    if (!password.isEmpty()) {
        creds += QStringLiteral(":") + password;
    }
    return scheme + creds + QStringLiteral("@") + rtsp.mid(scheme.size());
}

} // namespace denso::ui
