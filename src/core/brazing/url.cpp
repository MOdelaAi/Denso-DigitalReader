#include "brazing/url.h"

#include <QChar>
#include <QLatin1String>
#include <QString>
#include <QUrl>

namespace denso::brazing {
namespace {

const QLatin1String kPath(kEndpointPath);

BaseUrlResult reject(const char* why) {
    BaseUrlResult out;
    out.ok = false;
    out.error = why;
    return out;
}

} // namespace

BaseUrlResult normalize_base_url(const std::string& input) {
    const QString trimmed = QString::fromStdString(input).trimmed();
    if (trimmed.isEmpty()) {
        // "Unset", not "invalid": reporting can legitimately be configured off
        // with no address stored, and a mode switch leaves exactly that state.
        BaseUrlResult out;
        out.ok = true;
        return out;
    }

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
    QString path = url.path(QUrl::FullyEncoded);
    // Exactly ONE optional trailing slash is tolerated — the one a browser, a
    // copied link or a text field routinely adds. NOT a loop: stripping an
    // unbounded run would quietly accept "…:8080////" and "…/update////", which
    // are not among the forms this function documents, and "tolerant about
    // things nobody types" is how a normalizer starts guessing.
    if (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    // Exactly the production endpoint, or nothing. Deliberately case-sensitive:
    // the shipped path is lowercase, so a differently-cased path is a different
    // path — see the header.
    if (!path.isEmpty() && path != kPath) {
        return reject(
            "Enter only the server base URL (for example http://192.168.1.112:8080). "
            "The application adds /api/brazing/update itself.");
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

std::string endpoint_url(const std::string& base_url) {
    // EXACTLY the rule the dialog validates with and the grid gates on — one
    // authority, no second opinion at the transport. A value the normalizer
    // refuses yields NO endpoint, so an arbitrary path is neither accepted nor
    // rewritten into a request; the client reports the failure instead of posting
    // somewhere the operator did not ask for.
    const BaseUrlResult res = normalize_base_url(base_url);
    if (!res.ok || res.base_url.empty()) {
        return {};   // unusable or unset — the caller's signal not to POST at all
    }
    // The canonical base already had a pasted endpoint stripped, which is the
    // defensive half: a legacy or externally written row ending in kEndpointPath
    // cannot produce ".../update/api/brazing/update".
    return res.base_url + kEndpointPath;
}

} // namespace denso::brazing
