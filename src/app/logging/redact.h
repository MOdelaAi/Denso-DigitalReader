// Credential-safe logging helper — no Qt, unit-tested. RTSP/HTTP URLs can carry
// `user:pass@host` and query strings; strip them before anything reaches a log.
#pragma once

#include <string>

namespace denso::logging {

/// Return a log-safe form of a URL: keep scheme://host[:port]/path, DROP the
/// `user:pass@` userinfo and any `?query`. A non-URL string is returned as-is.
std::string sanitize_url(const std::string& url);

} // namespace denso::logging
