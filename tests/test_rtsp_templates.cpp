#include <catch2/catch_test_macros.hpp>

#include "ui/camera/shared/rtsp_templates.h"

using denso::ui::build_rtsp;
using denso::ui::rtsp_manufacturers;
using denso::ui::RtspManufacturer;

namespace {

const RtspManufacturer& dahua() {
    return rtsp_manufacturers().front();  // Dahua is the first (and only) entry
}

} // namespace

TEST_CASE("build_rtsp injects the channel into the main-stream URL") {
    const QString url = build_rtsp(dahua(), QStringLiteral("192.168.1.20"), 3, false);
    REQUIRE(url == QStringLiteral("rtsp://192.168.1.20:554/cam/realmonitor?channel=3&subtype=0"));
}

TEST_CASE("build_rtsp picks the sub-stream template when substream is true") {
    const QString url = build_rtsp(dahua(), QStringLiteral("192.168.1.20"), 3, true);
    REQUIRE(url == QStringLiteral("rtsp://192.168.1.20:554/cam/realmonitor?channel=3&subtype=1"));
}

TEST_CASE("build_rtsp with channel 1 matches the legacy default") {
    const QString url = build_rtsp(dahua(), QStringLiteral("10.0.0.5"), 1, false);
    REQUIRE(url == QStringLiteral("rtsp://10.0.0.5:554/cam/realmonitor?channel=1&subtype=0"));
}
