// Enumerate the USB/video-input cameras this machine can access, via Qt
// Multimedia (QMediaDevices). App-side so denso_core stays Qt6::Sql-only. The
// `index` is the device's position in the list — the integer cv::VideoCapture
// will later open (the capture slice revisits any ordering caveat).
#pragma once

#include <QString>

#include <vector>

namespace denso::ui {

struct UsbCamera {
    QString name;
    int index = 0;
};

/// Available USB/video-input cameras, in enumeration order.
std::vector<UsbCamera> list_usb_cameras();

} // namespace denso::ui
