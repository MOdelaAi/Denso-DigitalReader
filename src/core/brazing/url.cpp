#include "brazing/url.h"

#include <QChar>
#include <QLatin1String>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <algorithm>

namespace denso::brazing {
namespace {

BaseUrlResult reject(const char* why) {
    BaseUrlResult out;
    out.ok = false;
    out.error = why;
    return out;
}

ApiPathResult reject_path(const char* why) {
    ApiPathResult out;
    out.ok = false;
    out.error = why;
    return out;
}

/// Remove EXACTLY one trailing "/" — the one a browser, a copied link or a text
/// field routinely adds. Never a loop: stripping an unbounded run would quietly
/// accept forms nobody types, and "tolerant about things nobody types" is how a
/// normalizer starts guessing.
QString chop_one_slash(QString s) {
    if (s.endsWith(QLatin1Char('/'))) {
        s.chop(1);
    }
    return s;
}

/// Every character RFC 3986 allows in a path segment (pchar), plus "/" and the
/// "%" of a percent-escape, which is checked separately. A whitelist, not a
/// blacklist: an address that reaches the network must be built only from
/// characters we deliberately allowed.
const QRegularExpression& path_charset() {
    static const QRegularExpression re(
        QStringLiteral("^[A-Za-z0-9._~!$&'()*+,;=:@%/-]*$"));
    return re;
}

/// "scheme:" per RFC 3986 — the marker of an absolute URI, whether or not it is
/// followed by "//".
const QRegularExpression& scheme_prefix() {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
    return re;
}

bool is_hex_digit(QChar c) {
    return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
           (c >= QLatin1Char('a') && c <= QLatin1Char('f')) ||
           (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
}

enum class EscapeCheck {
    Ok,
    Malformed,     ///< "%" not followed by two hex digits
    HidesStructure ///< a valid escape that decodes to "/" or "."
};

/// Two distinct problems with a "%" in a path.
///
/// A "%" not followed by two hex digits is a malformed escape, and Qt would
/// either drop it or re-encode it — either way the stored path would stop being
/// the path the operator typed.
///
/// A WELL-FORMED escape can be worse. The segment rules below are applied to the
/// LITERAL text, so "%2f" (a separator) and "%2e" (a dot) walk straight past
/// them: "/api/%2e%2e/admin" holds no ".." segment as written, and "/api%2Fx"
/// holds no second segment. Servers and reverse proxies routinely decode before
/// routing, so the request would resolve somewhere other than the path this
/// function validated and the Settings preview displayed. Refuse them, exactly as
/// normalize_base_url already refuses "/api/brazing%2Fupdate" rather than
/// treating it as the endpoint it decodes to.
///
/// "%5c" (backslash) is refused for BOTH reasons at once, which is why it is
/// here rather than being left to the charset check. A literal "\" is already
/// rejected — it is not in path_charset() — so accepting the encoded spelling
/// would let the same character in through the back door. And several widely
/// deployed stacks and reverse proxies alias "\" to "/" when they normalize a
/// path, which the segment rules cannot see: "/api/%5c..%5c/admin" splits into
/// the single segment "%5c..%5c", so it is neither a ".." segment nor an empty
/// one, yet on such a stack it resolves to "/admin". Refusing it costs a
/// legitimate reporting path nothing.
///
/// "%00" is the fourth, and it ends the path rather than redirecting it: a
/// backend or proxy that decodes into a C string truncates there, so
/// "/api/update%00/ignored" routes to "/api/update" while the stored value and
/// the preview both show the longer path. Same invariant as the three above —
/// what is displayed must be what is routed — reached by truncation instead of
/// by re-pointing.
///
/// The list is deliberately SHORT and CLOSED: only characters that carry path
/// STRUCTURE — the separator, the dot, the separator some stacks alias to it,
/// and the terminator. "%20" and other escapes of ordinary characters stay
/// accepted, because encoding is the correct way to write them and refusing them
/// would be a policy about spelling rather than about where the request lands.
EscapeCheck check_percent_escapes(const QString& path) {
    for (int i = 0; i < path.size(); ++i) {
        if (path.at(i) != QLatin1Char('%')) continue;
        if (i + 2 >= path.size()) return EscapeCheck::Malformed;
        const QChar hi = path.at(i + 1);
        const QChar lo = path.at(i + 2);
        if (!is_hex_digit(hi) || !is_hex_digit(lo)) return EscapeCheck::Malformed;
        const QString hex = QString(hi) + lo;
        bool parsed = false;
        // Base 16, so the hex digits' own case never matters: "%2F", "%2f",
        // "%00" and "%0O"-style typos are all decided on the decoded VALUE, not
        // on the spelling.
        const uint value = hex.toUInt(&parsed, 16);
        if (parsed && (value == '/' || value == '.' || value == '\\' ||
                       value == 0)) {
            return EscapeCheck::HidesStructure;
        }
    }
    return EscapeCheck::Ok;
}

} // namespace

ApiPathResult normalize_api_path(const std::string& input) {
    const QString trimmed = QString::fromStdString(input).trimmed();
    if (trimmed.isEmpty()) {
        // DEFAULT, not "unset" — see the header. This one rule covers both the
        // installation that has never had a brazing.api_path row and the operator
        // who clears the field, so there is no second behaviour to reason about.
        ApiPathResult out;
        out.ok = true;
        out.api_path = kDefaultApiPath;
        return out;
    }

    // Host and scheme belong to the OTHER field. Refuse rather than try to split
    // a full URL apart here: the operator would then have a server address stored
    // in two places, and the two could disagree.
    if (trimmed.contains(QLatin1String("://")) ||
        trimmed.startsWith(QLatin1String("//")) ||
        scheme_prefix().match(trimmed).hasMatch()) {
        return reject_path(
            "Enter only the path (for example /api/brazing/update). "
            "The server address belongs in Server base URL.");
    }
    // trimmed() already removed the surrounding whitespace, so anything left is
    // INSIDE the path and is either a typo or a pasted line break.
    if (trimmed.contains(QRegularExpression(QStringLiteral("\\s")))) {
        return reject_path("The path cannot contain spaces.");
    }
    if (trimmed.contains(QLatin1Char('?'))) {
        return reject_path("Remove the ? query from the path.");
    }
    if (trimmed.contains(QLatin1Char('#'))) {
        return reject_path("Remove the # fragment from the path.");
    }

    // The ONE thing this function repairs, because it is unambiguous: a path is
    // rooted at the server, so a missing leading slash has exactly one meaning.
    QString path = trimmed;
    if (!path.startsWith(QLatin1Char('/'))) {
        path.prepend(QLatin1Char('/'));
    }

    if (!path_charset().match(path).hasMatch()) {
        return reject_path(
            "The path contains characters that are not allowed in a URL.");
    }
    switch (check_percent_escapes(path)) {
        case EscapeCheck::Malformed:
            return reject_path("The path contains a malformed % escape.");
        case EscapeCheck::HidesStructure:
            return reject_path(
                "Write / and . in the path directly, not as %2F or %2E, "
                "and do not use %5C or %00.");
        case EscapeCheck::Ok:
            break;
    }
    if (path == QLatin1String("/")) {
        return reject_path(
            "Enter the path the server exposes, for example /api/brazing/update.");
    }

    // Split on "/" so each segment can be judged. segs[0] is always empty (the
    // leading slash) and is expected; an empty segment ANYWHERE ELSE is a "//"
    // run, except at the very end, where it is the single trailing slash that is
    // deliberately preserved — "/x/" and "/x" are different resources to many
    // servers and this function does not get to pick which one the customer's
    // backend exposes.
    const QStringList segs = path.split(QLatin1Char('/'));
    for (int i = 1; i < segs.size(); ++i) {
        const QString& seg = segs.at(i);
        if (seg.isEmpty() && i != segs.size() - 1) {
            return reject_path("The path cannot contain an empty // segment.");
        }
        if (seg == QLatin1String(".") || seg == QLatin1String("..")) {
            return reject_path("The path cannot contain . or .. segments.");
        }
    }

    ApiPathResult out;
    out.ok = true;
    out.api_path = path.toStdString();
    return out;
}

BaseUrlResult normalize_base_url(const std::string& input,
                                 const std::string& api_path) {
    const QString trimmed = QString::fromStdString(input).trimmed();
    if (trimmed.isEmpty()) {
        // "Unset", not "invalid": reporting can legitimately be configured off
        // with no address stored, and a mode switch leaves exactly that state.
        BaseUrlResult out;
        out.ok = true;
        return out;
    }

    // The endpoint this base is allowed to have pasted onto it is the CONFIGURED
    // one, so an operator who pastes the address they were actually given still
    // gets it stripped. A path the other normalizer refuses is reported on its
    // own field; here it just falls back to the shipped default so this function
    // still has a well-defined tolerance.
    const ApiPathResult cfg_path = normalize_api_path(api_path);
    const QString endpoint = cfg_path.ok
                                 ? QString::fromStdString(cfg_path.api_path)
                                 : QString::fromLatin1(kDefaultApiPath);

    // StrictMode: no guessing, no tolerant re-interpretation of a malformed
    // address. An operator's typo must surface as a refusal, not as a URL that
    // silently points somewhere else.
    const QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid()) {
        return reject("That is not a valid address.");
    }
    // QUrl lower-cases the scheme, so this comparison is already case-insensitive
    // over what the operator typed.
    if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) {
        return reject("Enter an address starting with http:// or https://");
    }
    if (url.host().isEmpty()) {
        return reject("The address is missing a host name or IP.");
    }
    if (!url.userInfo().isEmpty()) {
        return reject("Remove the user name / password from the address.");
    }
    if (url.hasQuery()) {
        return reject("Remove the ? query from the address.");
    }
    if (url.hasFragment()) {
        return reject("Remove the # fragment from the address.");
    }

    // FullyEncoded, not the decoded form: "/api/brazing%2Fupdate" decodes to the
    // endpoint but is a DIFFERENT path, and this function is not entitled to
    // guess that the operator meant the endpoint.
    //
    // One optional trailing slash is tolerated on BOTH sides of the comparison,
    // so a configured path that itself ends in "/" still matches the pasted
    // endpoint whether or not the paste kept the slash.
    const QString path = chop_one_slash(url.path(QUrl::FullyEncoded));
    const QString expected = chop_one_slash(endpoint);
    // Exactly the configured endpoint, or nothing. Deliberately case-sensitive:
    // a differently-cased path is a different path — see the header.
    if (!path.isEmpty() && (expected.isEmpty() || path != expected)) {
        return reject(
            "Enter only the server base URL (for example http://192.168.1.112:8080). "
            "The application adds the reporting API path itself.");
    }

    // Rebuild from the PARSED url rather than by string surgery, so the host
    // form (IPv6 brackets, IDN) and the port survive exactly as Qt read them.
    QUrl canonical = url;
    canonical.setPath(QString());
    BaseUrlResult out;
    out.ok = true;
    out.base_url = canonical.toString(QUrl::FullyEncoded).toStdString();
    return out;
}

BaseUrlParts split_base_url(const std::string& base_url) {
    BaseUrlParts out;   // http, no host, no port — the out-of-the-box state

    // Prefer the CANONICAL form, so a row holding the full endpoint (legacy, or
    // externally written) splits by the base it denotes. When the normalizer
    // refuses the value there is still something worth showing: parse it
    // best-effort so the operator sees the part that was readable instead of a
    // blank form that silently discarded their configuration.
    const BaseUrlResult canonical = normalize_base_url(base_url);
    const QString source = (canonical.ok && !canonical.base_url.empty())
                               ? QString::fromStdString(canonical.base_url)
                               : QString::fromStdString(base_url).trimmed();
    if (source.isEmpty()) {
        return out;
    }

    const QUrl url(source, QUrl::StrictMode);
    out.https = url.scheme() == QLatin1String("https");
    // host() is the DECODED host without IPv6 brackets — exactly what belongs in
    // a text field, and exactly what setHost() takes back in compose.
    out.host = url.host().toStdString();
    if (url.port() != -1) {
        out.port = QString::number(url.port()).toStdString();
    }
    return out;
}

BaseUrlResult compose_base_url(const BaseUrlParts& parts,
                               const std::string& api_path) {
    const QString host = QString::fromStdString(parts.host).trimmed();
    const QString port_text = QString::fromStdString(parts.port).trimmed();

    if (host.isEmpty()) {
        if (!port_text.isEmpty()) {
            // A port with nowhere to send it is a half-filled form, not the
            // "unset" state — say so rather than silently storing nothing.
            return reject("Enter the server address.");
        }
        // "Unset", not "invalid": the same legitimate reporting-off state the
        // free-text field expressed with an empty box.
        BaseUrlResult out;
        out.ok = true;
        return out;
    }

    // An empty port is the protocol's default and is simply omitted. That is not
    // a convenience: a stored "https://server.example.com" carries no port, and
    // this editor has to give it back unchanged.
    int port = -1;
    if (!port_text.isEmpty()) {
        bool numeric = false;
        // toInt() alone would accept "+80" and locale oddities; require plain
        // digits so what is stored is what was typed.
        const bool digits_only =
            !port_text.isEmpty() &&
            std::all_of(port_text.cbegin(), port_text.cend(),
                        [](QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); });
        const int value = port_text.toInt(&numeric);
        if (!digits_only || !numeric || value < 1 || value > 65535) {
            return reject("Enter a port number between 1 and 65535.");
        }
        port = value;
    }

    QUrl url;
    url.setScheme(parts.https ? QStringLiteral("https") : QStringLiteral("http"));
    url.setHost(host, QUrl::StrictMode);
    // MEASURED, not assumed: QUrl::setHost silently stores an EMPTY host for a
    // value it cannot parse and leaves isValid() TRUE, so "192.168.1.112:8080"
    // would compose to "http://:8080". The emptiness of host() is the only
    // reliable signal here.
    if (url.host().isEmpty()) {
        return reject(
            "Enter only the server name or IP address here — the protocol, the "
            "port and the API path each have their own field.");
    }
    if (port != -1) {
        url.setPort(port);
    }

    // Back through the ONE authority, so the decomposed editor cannot accept an
    // address the free-text field would have refused, and the value persisted is
    // canonical by exactly the same rule as before.
    return normalize_base_url(url.toString(QUrl::FullyEncoded).toStdString(),
                              api_path);
}

std::string endpoint_url(const std::string& base_url, const std::string& api_path) {
    // EXACTLY the rule the dialog validates with, previews with, and the grid
    // gates on — one authority, no second opinion at the transport. A value
    // either normalizer refuses yields NO endpoint, so an arbitrary path is
    // neither accepted nor rewritten into a request; the client reports the
    // failure instead of posting somewhere the operator did not ask for.
    const ApiPathResult path = normalize_api_path(api_path);
    if (!path.ok) {
        return {};   // unusable path — do not fall back and post somewhere else
    }
    const BaseUrlResult base = normalize_base_url(base_url, path.api_path);
    if (!base.ok || base.base_url.empty()) {
        return {};   // unusable or unset — the caller's signal not to POST at all
    }
    // The canonical base already had a pasted endpoint stripped, which is the
    // defensive half: a legacy or externally written row ending in the endpoint
    // cannot produce ".../update/api/brazing/update".
    return base.base_url + path.api_path;
}

} // namespace denso::brazing
