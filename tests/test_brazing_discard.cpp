// Slice 7 / Part A — the R4 pending-snapshot discard log (spec §6.6, §11-R4).
//
// A mode switch tears the grid down, and CameraGrid::clear() destroys the
// BrazingReporter. If the server never acked the last snapshot, that snapshot is
// dropped — correct (the OLD mode's readings must never be posted after a switch)
// but previously SILENT. ~BrazingReporter now logs the discard.
//
// The contract this proves is deliberately narrow, and both halves matter:
//   • the log names the zone COUNT and the zone NUMBERS, so an operator can tell
//     which zones' last reading never reached the server;
//   • the log NEVER names a reading VALUE. Values are process data; the log is a
//     bounded 24/7 file that leaves the appliance in support bundles.
//
// Runs in denso_integration_tests because BrazingReporter lives in denso_app (it
// owns a QTimer + the transport seam), not in the pure denso_brazing lib. The pure
// accessor it logs from (BrazingRetryPolicy::pending_zone_numbers) is unit-tested
// in denso_tests/test_brazing_retry_policy.cpp.
#include <catch2/catch_test_macros.hpp>

#include "brazing/brazing_reporter.h"
#include "brazing/brazing_transport.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <functional>
#include <map>
#include <memory>
#include <utility>

using denso::ui::BrazingReporter;
using denso::ui::BrazingTransport;

namespace {

// Captures qDebug/qInfo/qWarning output for the scope and ALWAYS restores the
// previous handler — including when a Catch2 assertion aborts the test mid-scope.
// The message handler is process-global state shared with every other case in this
// binary, so leaking ours would silently swallow (or misattribute) their logging.
class ScopedLogCapture {
public:
    ScopedLogCapture() {
        active_ = this;
        prev_ = qInstallMessageHandler(&ScopedLogCapture::handle);
    }
    ~ScopedLogCapture() {
        qInstallMessageHandler(prev_);
        active_ = nullptr;
    }
    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

    QString text() const { return lines_.join(QLatin1Char('\n')); }

private:
    static void handle(QtMsgType, const QMessageLogContext&, const QString& msg) {
        if (active_) active_->lines_ << msg;  // swallowed: nothing reaches the console
    }

    QtMessageHandler prev_ = nullptr;
    QStringList lines_;
    static ScopedLogCapture* active_;
};

ScopedLogCapture* ScopedLogCapture::active_ = nullptr;

// A transport that accepts the POST and NEVER invokes `done`, so the snapshot
// stays in flight forever and is therefore still undelivered at destruction —
// exactly the state a mode switch interrupts (a downed/hanging server). It
// deliberately drops the callback rather than storing it: retaining it would keep
// a QPointer to the reporter alive past the point this test destroys it.
class NeverAcksTransport : public BrazingTransport {
public:
    void post(const std::map<int, int>& zones, std::function<void(bool)> done) override {
        ++posts;
        last = zones;
        (void)done;  // never invoked — no ack ever arrives
    }

    int posts = 0;
    std::map<int, int> last;
};

}  // namespace

TEST_CASE("destroying a reporter with an undelivered snapshot logs the zones, never the values",
          "[brazing_discard]") {
    ScopedLogCapture log;

    {
        auto transport = std::make_unique<NeverAcksTransport>();
        NeverAcksTransport* raw = transport.get();
        BrazingReporter reporter(std::move(transport));

        // Zone 3 reads 120, zone 4 reads 35. The POST is issued and never acked.
        reporter.submit(std::map<int, int>{{3, 120}, {4, 35}});
        REQUIRE(raw->posts == 1);
        REQUIRE(raw->last == std::map<int, int>({{3, 120}, {4, 35}}));
    }  // ~BrazingReporter — the discard is logged here.

    const QString out = log.text();

    // The count and both zone numbers are named, so the operator knows exactly
    // which zones' last reading never reached the server. Asserted as DELIMITED
    // fragments, not bare digits: a bare contains("2")/contains("3") would also
    // pass on a wrong line like "4 zones: 23", or on a digit borrowed from some
    // unrelated captured message.
    CHECK(out.contains(QStringLiteral("for 2 zones:")));
    CHECK(out.contains(QStringLiteral("3, 4")));
    // …and it is recognisably the discard line, not some incidental output.
    CHECK(out.contains(QStringLiteral("discard"), Qt::CaseInsensitive));

    // The READING VALUES must never appear. This is the load-bearing half of the
    // assertion: the zone numbers 3/4 are short and could match by accident, but
    // "120" and "35" can only come from the payload.
    CHECK_FALSE(out.contains(QStringLiteral("120")));
    CHECK_FALSE(out.contains(QStringLiteral("35")));
}

TEST_CASE("destroying a reporter whose snapshot was acked logs nothing", "[brazing_discard]") {
    // Mirrors the case above with ONE difference — the server acks — so nothing is
    // pending at teardown and the destructor must stay silent. Without this, a
    // destructor that logged unconditionally would still pass the test above while
    // spamming the 24/7 log on every ordinary grid rebuild.
    ScopedLogCapture log;

    {
        // A transport that acks immediately (synchronously, on this thread — the
        // reporter's own contract is that done() runs on the GUI thread).
        class AcksTransport : public BrazingTransport {
        public:
            void post(const std::map<int, int>&, std::function<void(bool)> done) override {
                done(true);
            }
        };
        BrazingReporter reporter(std::make_unique<AcksTransport>());
        reporter.submit(std::map<int, int>{{3, 120}, {4, 35}});
    }  // ~BrazingReporter — nothing pending, so nothing logged.

    CHECK(log.text().isEmpty());
}

TEST_CASE("destroying a reporter that never received a snapshot logs nothing",
          "[brazing_discard]") {
    // The overwhelmingly common teardown: reporting is enabled but no zone ever
    // changed, so the policy is idle. Must be silent.
    ScopedLogCapture log;
    { BrazingReporter reporter(std::make_unique<NeverAcksTransport>()); }
    CHECK(log.text().isEmpty());
}
