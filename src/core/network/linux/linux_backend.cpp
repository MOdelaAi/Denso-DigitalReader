// Linux network backend. Reads live status through `nmcli` via QProcess; the
// pure parse helpers it calls live in nmcli.{h,cpp}. Config apply and Wi-Fi
// scan/join are device-only work, reported as unimplemented so boot-reassert
// surfaces them non-fatally — exactly as the Rust stub does. Ported 1:1 from
// Rust `network::linux` (the runner half).
#include "network/backend.h"
#include "network/linux/nmcli.h"

#include <QProcess>
#include <QString>
#include <QStringList>

#include <stdexcept>

namespace denso::network {
namespace {

/// Run one command, returning its stdout (UTF-8, lossy). Empty on spawn failure.
std::string run(const QString& cmd, const QStringList& args) {
    QProcess p;
    p.start(cmd, args);
    if (!p.waitForStarted()) return {};
    p.waitForFinished(-1);
    return QString::fromUtf8(p.readAllStandardOutput()).toStdString();
}

InterfaceStatus device_ip(const std::string& dev) {
    const std::string out = run(
        "nmcli", {"-t", "-f", "IP4.ADDRESS,IP4.GATEWAY", "device", "show", QString::fromStdString(dev)});
    const auto [ip, gateway] = nmcli::parse_device_show(out);
    InterfaceStatus s;
    s.connected = !ip.empty();
    s.ip = ip;
    s.gateway = gateway;
    return s;
}

class LinuxBackend : public NetworkBackend {
public:
    NetworkSnapshot snapshot() const override {
        const std::string dev = run("nmcli", {"-t", "-f", "DEVICE,TYPE,STATE", "device"});
        const auto [eth_dev, wifi_dev] = nmcli::pick_devices(dev);

        InterfaceStatus ethernet;
        if (eth_dev) ethernet = device_ip(*eth_dev);

        InterfaceStatus wifi;
        if (wifi_dev) {
            wifi = device_ip(*wifi_dev);
            const std::string w =
                run("nmcli", {"-t", "-f", "ACTIVE,SSID,SIGNAL", "device", "wifi"});
            const auto [ssid, signal] = nmcli::parse_wifi(w);
            wifi.ssid = ssid;
            wifi.signal = signal;
        }
        return NetworkSnapshot{ethernet, wifi};
    }

    void apply_config(const NetConfig&) const override {
        // TODO(device): nmcli `con mod`/`con up` (or netplan apply). Privileged
        // and verified on the Jetson/Pi target, not this dev path. Report
        // unimplemented so boot-reassert surfaces it non-fatally.
        throw std::runtime_error("network apply not yet implemented for Linux");
    }

    std::vector<WifiNetwork> scan_wifi() const override {
        // TODO(device): nmcli -t -f SSID,SIGNAL,SECURITY device wifi list.
        throw std::runtime_error("wifi scan not yet implemented for Linux");
    }

    void connect_wifi(const std::string&, const std::optional<std::string>&) const override {
        // TODO(device): nmcli device wifi connect <ssid> [password <pw>].
        throw std::runtime_error("wifi connect not yet implemented for Linux");
    }
};

} // namespace

std::unique_ptr<NetworkBackend> make_linux_backend() {
    return std::make_unique<LinuxBackend>();
}

} // namespace denso::network
