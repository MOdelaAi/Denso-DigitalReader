// Test-side access to the Settings → Server address controls.
//
// The server address used to be ONE free-text QLineEdit ("brazingUrl") holding a
// whole URL. It is now three controls — protocol, address, port — because the
// single URL field was too technical for the panel operator. The DATABASE did not
// change: those three still compose to the one canonical `brazing.base_url`.
//
// Tests that only need "put this server into the form" say so through
// set_brazing_base() and stay readable as URLs, which is how they were written and
// how the stored value still looks. Tests that are ABOUT the decomposition drive
// the individual controls directly.
#pragma once

#include "brazing/url.h"

#include <catch2/catch_test_macros.hpp>

#include <QComboBox>
#include <QLatin1String>
#include <QLineEdit>
#include <QString>
#include <QStringLiteral>
#include <QWidget>

namespace denso::testing {

inline QComboBox* brazing_scheme(QWidget& dlg) {
    auto* w = dlg.findChild<QComboBox*>(QStringLiteral("brazingScheme"));
    REQUIRE(w != nullptr);
    return w;
}

inline QLineEdit* brazing_host(QWidget& dlg) {
    auto* w = dlg.findChild<QLineEdit*>(QStringLiteral("brazingHost"));
    REQUIRE(w != nullptr);
    return w;
}

inline QLineEdit* brazing_port(QWidget& dlg) {
    auto* w = dlg.findChild<QLineEdit*>(QStringLiteral("brazingPort"));
    REQUIRE(w != nullptr);
    return w;
}

/// Drive the three address controls from ONE base URL, as an operator would after
/// being told "the server is at http://…:8080". Splits through the same authority
/// the dialog seeds with, so a test setting a URL and the dialog loading that URL
/// put the form in the identical state.
///
/// Only for values brazing::normalize_base_url ACCEPTS — a test that wants to
/// prove a refusal must type into the offending control directly, because that is
/// what an operator can actually do now.
inline void set_brazing_base(QWidget& dlg, const QString& base_url) {
    const brazing::BaseUrlParts parts =
        brazing::split_base_url(base_url.toStdString());
    auto* scheme = brazing_scheme(dlg);
    const int row = scheme->findData(parts.https ? QStringLiteral("https")
                                                 : QStringLiteral("http"));
    REQUIRE(row >= 0);
    scheme->setCurrentIndex(row);
    brazing_host(dlg)->setText(QString::fromStdString(parts.host));
    brazing_port(dlg)->setText(QString::fromStdString(parts.port));
}

/// The three controls read back as one canonical base URL — the value the dialog
/// would persist from the form as it stands. Empty when the form does not compose
/// to a usable address.
inline QString brazing_base_text(QWidget& dlg) {
    brazing::BaseUrlParts parts;
    parts.https = brazing_scheme(dlg)->currentData().toString() ==
                  QStringLiteral("https");
    parts.host = brazing_host(dlg)->text().toStdString();
    parts.port = brazing_port(dlg)->text().toStdString();
    const brazing::BaseUrlResult r = brazing::compose_base_url(parts);
    return r.ok ? QString::fromStdString(r.base_url) : QString();
}

} // namespace denso::testing
