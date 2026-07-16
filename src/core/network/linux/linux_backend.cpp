// Linux network backend. Reads live status through `nmcli` via QProcess; the
// pure parse helpers it calls live in nmcli.{h,cpp}. Config apply and Wi-Fi
// scan/join are device-only work, reported as unimplemented so boot-reassert
// surfaces them non-fatally — exactly as the Rust stub does. Ported 1:1 from
// Rust `network::linux` (the runner half).
#include "network/backend.h"
#include "network/linux/nmcli.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace denso::network {
namespace {

constexpr int kNetCmdTimeoutMs = 15000;  // cap a stuck netsh/ipconfig
constexpr int kNetCmdGraceMs = 2000;     // grace after kill() before giving up

/// Run one command, returning its stdout (UTF-8, lossy). Empty on spawn failure.
std::string run(const QString& cmd, const QStringList& args) {
    QProcess p;
    p.start(cmd, args);
    if (!p.waitForStarted()) return {};
    if (!p.waitForFinished(kNetCmdTimeoutMs)) {
        p.kill();
        p.waitForFinished(kNetCmdGraceMs);
        return {};  // timed out — treat as no output
    }
    return QString::fromUtf8(p.readAllStandardOutput()).toStdString();
}

/// Run several small read-only commands concurrently, returning each stdout in
/// order. All are started before any is awaited, so wall time ≈ the slowest
/// single command instead of the sum — the two `device show` queries and the
/// Wi-Fi listing (which can trigger a scan) overlap. Bounded: the await phase
/// shares one `kNetCmdTimeoutMs` deadline, then every still-running child is
/// killed up front and reaped together under one `kNetCmdGraceMs` budget — so
/// the batch normally settles within ~timeout + grace no matter how many hang
/// (not N× either term). A timed-out command reads as empty output, exactly like
/// run(). File-local by design: it assumes small output (no pipe-backpressure
/// handling), true for the nmcli status commands it serves.
std::vector<std::string> run_concurrent(
    const std::vector<std::pair<QString, QStringList>>& cmds) {
    std::vector<std::unique_ptr<QProcess>> procs;
    procs.reserve(cmds.size());
    QElapsedTimer clock;
    clock.start();  // bound covers launch + await
    for (const auto& [cmd, args] : cmds) {
        auto p = std::make_unique<QProcess>();
        p->start(cmd, args);  // fire all off before waiting on any
        procs.push_back(std::move(p));
    }

    std::vector<std::string> out(cmds.size());
    std::vector<bool> done(cmds.size(), false);
    for (std::size_t i = 0; i < procs.size(); ++i) {
        const int remaining =
            std::max(0, kNetCmdTimeoutMs - static_cast<int>(clock.elapsed()));
        if (procs[i]->waitForFinished(remaining)) {  // false on spawn failure/timeout
            out[i] = QString::fromUtf8(procs[i]->readAllStandardOutput()).toStdString();
            done[i] = true;
        }
    }
    // Anything still running blew the shared deadline: kill them all up front,
    // then reap together under one grace budget so a stuck child can't block the
    // destructor and N stragglers don't cost N× grace. Their output stays empty.
    for (const auto& p : procs)
        if (p->state() != QProcess::NotRunning) p->kill();
    QElapsedTimer grace;
    grace.start();
    for (std::size_t i = 0; i < procs.size(); ++i) {
        if (done[i]) continue;
        const int remaining =
            std::max(0, kNetCmdGraceMs - static_cast<int>(grace.elapsed()));
        procs[i]->waitForFinished(remaining);  // reap; output intentionally dropped
    }
    return out;
}

/// Build interface status from an `nmcli device show` stdout (pure).
InterfaceStatus status_from_show(const std::string& out) {
    const auto [ip, gateway] = nmcli::parse_device_show(out);
    InterfaceStatus s;
    s.connected = !ip.empty();
    s.ip = ip;
    s.gateway = gateway;
    return s;
}

QStringList device_show_args(const std::string& dev) {
    return {"-t", "-f", "IP4.ADDRESS,IP4.GATEWAY", "device", "show",
            QString::fromStdString(dev)};
}

class LinuxBackend : public NetworkBackend {
public:
    NetworkSnapshot snapshot() const override {
        const std::string dev = run("nmcli", {"-t", "-f", "DEVICE,TYPE,STATE", "device"});
        const auto [eth_dev, wifi_dev] = nmcli::pick_devices(dev);

        // The per-device IP queries and the Wi-Fi listing (which can trigger a
        // scan) are independent — run them concurrently and map stdout back by
        // slot. The device list above is a genuine dependency and stays serial.
        std::vector<std::pair<QString, QStringList>> cmds;
        int eth_i = -1, wifi_i = -1, wifi_list_i = -1;
        if (eth_dev) {
            eth_i = static_cast<int>(cmds.size());
            cmds.push_back({"nmcli", device_show_args(*eth_dev)});
        }
        if (wifi_dev) {
            wifi_i = static_cast<int>(cmds.size());
            cmds.push_back({"nmcli", device_show_args(*wifi_dev)});
            wifi_list_i = static_cast<int>(cmds.size());
            cmds.push_back({"nmcli", {"-t", "-f", "ACTIVE,SSID,SIGNAL", "device", "wifi"}});
        }
        const std::vector<std::string> out = run_concurrent(cmds);

        InterfaceStatus ethernet;
        if (eth_i >= 0) ethernet = status_from_show(out[eth_i]);

        InterfaceStatus wifi;
        if (wifi_i >= 0) {
            wifi = status_from_show(out[wifi_i]);
            const auto [ssid, signal] = nmcli::parse_wifi(out[wifi_list_i]);
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
