#include "logging/redact.h"

namespace denso::logging {

std::string sanitize_url(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return url;  // not a URL — nothing to strip
    }
    const std::string scheme = url.substr(0, scheme_end);
    std::string rest = url.substr(scheme_end + 3);

    // Drop userinfo (user:pass@) — only the '@' that precedes the first '/'.
    const auto slash = rest.find('/');
    const auto at = rest.find('@');
    if (at != std::string::npos && (slash == std::string::npos || at < slash)) {
        rest = rest.substr(at + 1);
    }
    // Drop the query string (may carry tokens/credentials).
    const auto q = rest.find('?');
    if (q != std::string::npos) {
        rest = rest.substr(0, q);
    }
    return scheme + "://" + rest;
}

} // namespace denso::logging
