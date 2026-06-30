// Manufacturer → RTSP URL templates. Each manufacturer maps to a main-stream
// and sub-stream URL pattern (%1 = host IP), credential-free; the username /
// password are injected at capture time. Extend the table as more vendors are
// added. (For now: Dahua.)
#pragma once

#include <QString>

#include <vector>

namespace denso::ui {

struct RtspManufacturer {
    QString name;
    QString main_template;  // credential-free, %1 = IP
    QString sub_template;   // credential-free, %1 = IP
};

/// The known manufacturers, in display order.
const std::vector<RtspManufacturer>& rtsp_manufacturers();

/// Build the credential-free RTSP URL for a manufacturer + stream + IP.
QString build_rtsp(const RtspManufacturer& manufacturer, const QString& ip, bool substream);

/// Inject `user[:password]@` after the rtsp:// scheme. Used at capture time.
QString with_credentials(const QString& rtsp, const QString& user, const QString& password);

} // namespace denso::ui
