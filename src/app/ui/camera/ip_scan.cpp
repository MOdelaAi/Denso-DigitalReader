#include "ui/camera/ip_scan.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QList>
#include <QNetworkInterface>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>

namespace denso::ui {

std::vector<QString> scan_rtsp_subnet(uint16_t port, int deadline_ms) {
    // Candidate /24 network bases from the machine's non-loopback IPv4 addresses.
    QList<quint32> bases;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            const quint32 base = ip.toIPv4Address() & 0xFFFFFF00u;
            if (!bases.contains(base)) {
                bases.append(base);
            }
        }
    }
    if (bases.isEmpty()) {
        return {};
    }

    // Fire all probes concurrently; a shared deadline bounds total wall time, so
    // silent hosts cost nothing beyond the deadline. Open/refused ports answer
    // fast. Runs on the caller's (worker) thread via a local event loop.
    std::vector<quint32> found;
    QList<QTcpSocket*> sockets;
    QEventLoop loop;
    int pending = 0;

    for (const quint32 base : bases) {
        for (quint32 host = 1; host <= 254; ++host) {
            const quint32 addr = base | host;
            auto* sock = new QTcpSocket;
            sockets.append(sock);
            ++pending;
            QObject::connect(sock, &QTcpSocket::connected, &loop, [&, addr, sock]() {
                found.push_back(addr);
                sock->disconnect();  // prevent the error handler from also firing
                if (--pending == 0) loop.quit();
            });
            QObject::connect(sock, &QTcpSocket::errorOccurred, &loop,
                             [&, sock](QAbstractSocket::SocketError) {
                                 sock->disconnect();
                                 if (--pending == 0) loop.quit();
                             });
            sock->connectToHost(QHostAddress(addr), port);
        }
    }

    QTimer::singleShot(deadline_ms, &loop, &QEventLoop::quit);
    loop.exec();
    qDeleteAll(sockets);

    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());

    std::vector<QString> out;
    out.reserve(found.size());
    for (const quint32 addr : found) {
        out.push_back(QHostAddress(addr).toString());
    }
    return out;
}

} // namespace denso::ui
