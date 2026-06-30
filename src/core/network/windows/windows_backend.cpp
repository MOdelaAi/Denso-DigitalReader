// Windows network backend. Drives the OS through `ipconfig` / `netsh` CLIs via
// QProcess; the pure helpers it calls (build_netsh_commands, build_snapshot,
// parse_wifi_networks, build_profile_xml) live in the cross-platform logic
// core. Ported 1:1 from Rust `network::windows` (the runner half).
#include "network/backend.h"
#include "network/windows/netsh.h"
#include "network/windows/parse.h"
#include "network/windows/wifi.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <exception>
#include <stdexcept>

namespace denso::network {
namespace {

/// Run one command, returning its stdout (UTF-8, lossy). Empty on spawn
/// failure — mirrors Rust `run` (`Command::output().map(...).unwrap_or_default`).
std::string run(const QString& cmd, const QStringList& args) {
    QProcess p;
    p.start(cmd, args);
    if (!p.waitForStarted()) return {};
    p.waitForFinished(-1);
    return QString::fromUtf8(p.readAllStandardOutput()).toStdString();
}

/// Run one `netsh` invocation, treating a non-zero exit — or netsh's stdout
/// error text — as failure (thrown as `std::runtime_error`, mirroring the Rust
/// `Result::Err`).
void run_checked(const QStringList& args) {
    QProcess p;
    p.start("netsh", args);
    if (!p.waitForStarted()) {
        throw std::runtime_error("failed to spawn netsh: " + p.errorString().toStdString());
    }
    p.waitForFinished(-1);
    if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) return;

    const QString err = QString::fromUtf8(p.readAllStandardError());
    const QString detail =
        err.trimmed().isEmpty() ? QString::fromUtf8(p.readAllStandardOutput()) : err;
    throw std::runtime_error("netsh " + args.join(' ').toStdString() + ": " +
                             detail.trimmed().toStdString());
}

QStringList to_qargs(const std::vector<std::string>& args) {
    QStringList q;
    for (const auto& a : args) q << QString::fromStdString(a);
    return q;
}

class WindowsBackend : public NetworkBackend {
public:
    NetworkSnapshot snapshot() const override {
        const std::string ipcfg = run("ipconfig", {});
        const std::string wlan = run("netsh", {"wlan", "show", "interfaces"});
        return parse::build_snapshot(ipcfg, wlan);
    }

    void apply_config(const NetConfig& config) const override {
        for (const auto& args : netsh::build_netsh_commands(config)) {
            run_checked(to_qargs(args));
        }
    }

    std::vector<WifiNetwork> scan_wifi() const override {
        const std::string out = run("netsh", {"wlan", "show", "networks", "mode=bssid"});
        return wifi::parse_wifi_networks(out);
    }

    void connect_wifi(const std::string& ssid,
                      const std::optional<std::string>& password) const override {
        // Hand the network (and PSK, if any) to the OS as a WLAN profile, then
        // connect. The key lives in the Windows credential store, not our DB.
        const std::string xml = wifi::build_profile_xml(ssid, password);
        const QString path = QDir(QDir::tempPath()).filePath("denso_wlan_profile.xml");

        // Rust's `fs::write` reports open, write, AND flush failures under the
        // one "write profile:" prefix — check each so a short write or flush
        // error surfaces the same way rather than as a later netsh failure.
        QFile f(path);
        const QByteArray bytes = QByteArray::fromStdString(xml);
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.flush()) {
            throw std::runtime_error("write profile: " + f.errorString().toStdString());
        }
        f.close();

        // Remove the temp file whether or not `add profile` succeeds, then
        // propagate the add error (mirrors the Rust order: run, remove, `add?`).
        std::exception_ptr add_err;
        try {
            run_checked({"wlan", "add", "profile", "filename=" + path});
        } catch (...) {
            add_err = std::current_exception();
        }
        QFile::remove(path);
        if (add_err) std::rethrow_exception(add_err);

        const QString qssid = QString::fromStdString(ssid);
        run_checked({"wlan", "connect", "name=" + qssid, "ssid=" + qssid});
    }
};

} // namespace

std::unique_ptr<NetworkBackend> make_windows_backend() {
    return std::make_unique<WindowsBackend>();
}

} // namespace denso::network
