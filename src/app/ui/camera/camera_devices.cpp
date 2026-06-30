#include "ui/camera/camera_devices.h"

#include <QCameraDevice>
#include <QList>
#include <QMediaDevices>

namespace denso::ui {

std::vector<UsbCamera> list_usb_cameras() {
    std::vector<UsbCamera> out;
    const QList<QCameraDevice> devices = QMediaDevices::videoInputs();
    int index = 0;
    for (const QCameraDevice& device : devices) {
        const QString desc = device.description();
        out.push_back(UsbCamera{
            desc.isEmpty() ? QStringLiteral("Camera %1").arg(index) : desc,
            index,
        });
        ++index;
    }
    return out;
}

} // namespace denso::ui
