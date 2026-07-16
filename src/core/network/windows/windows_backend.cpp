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
#include <QElapsedTimer>
#include <QFile>
#include <QIODevice>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace denso::network {
namespace {

constexpr int kNetCmdTimeoutMs = 15000;  // cap a stuck netsh/ipconfig
constexpr int kNetCmdGraceMs = 2000;     // grace after kill() before giving up

/// Run one command, returning its stdout (UTF-8, lossy). Empty on spawn
/// failure — mirrors Rust `run` (`Command::output().map(...).unwrap_or_default`).
std::string run(const QString& cmd, const QStringList& args) {
    QProcess p;
    p.start(cmd, args);
    if (!p.waitForStarted()) return {};
    if (!p.waitForFinished(kNetCmdTimeoutMs)) {
        p.kill();
        p.waitForFinished(kNetCmdGraceMs);
        return {};  // timed out — treat as no output (mirrors spawn failure)
    }
    return QString::fromUtf8(p.readAllStandardOutput()).toStdString();
}

/// Run several small read-only commands concurrently, returning each stdout in
/// order. All are started before any is awaited, so wall time ≈ the slowest
/// single command instead of the sum — `netsh wlan` (seconds) no longer stacks
/// on top of `ipconfig`. Bounded: the await phase shares one `kNetCmdTimeoutMs`
/// deadline, then every still-running child is killed up front and reaped
/// together under one `kNetCmdGraceMs` budget — so the batch normally settles
/// within ~timeout + grace no matter how many hang (not N× either term). A timed-out
/// command reads as empty output, exactly like run(). File-local by design: it
/// assumes small output (no pipe-backpressure handling), true for the ipconfig/
/// netsh status commands it serves.
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

/// Run one `netsh` invocation, treating a non-zero exit — or netsh's stdout
/// error text — as failure (thrown as `std::runtime_error`, mirroring the Rust
/// `Result::Err`).
void run_checked(const QStringList& args) {
    QProcess p;
    p.start("netsh", args);
    if (!p.waitForStarted()) {
        throw std::runtime_error("failed to spawn netsh: " + p.errorString().toStdString());
    }
    if (!p.waitForFinished(kNetCmdTimeoutMs)) {
        p.kill();
        p.waitForFinished(kNetCmdGraceMs);
        throw std::runtime_error("netsh " + args.join(' ').toStdString() +
                                 ": timed out after 15s");
    }
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
        // ipconfig and netsh-wlan are independent; run them concurrently so the
        // slow WLAN query doesn't serialize behind ipconfig.
        const std::vector<std::string> out = run_concurrent({
            {"ipconfig", {}},
            {"netsh", {"wlan", "show", "interfaces"}},
        });
        return parse::build_snapshot(out[0], out[1]);
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
