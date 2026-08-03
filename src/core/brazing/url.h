// The ONE authority on what a brazing server address is: the fixed reporting
// endpoint, the canonical form of the persisted base URL, and the full URL a POST
// goes to. The Settings dialog, the CameraGrid gate and the Qt-Network transport
// all read the rule from here, so the UI can never accept an address the client
// would then treat differently.
//
// The persisted setting stays a BASE url (scheme://host[:port]); the endpoint path
// is appended by the transport and is not configurable.
#pragma once

#include <string>

namespace denso::brazing {

/// The reporting endpoint appended to the base URL. Defined once — never spelled
/// out again in the UI, the client, or a second normalizer.
inline constexpr char kEndpointPath[] = "/api/brazing/update";

/// Outcome of canonicalizing an operator-entered address.
struct BaseUrlResult {
    bool        ok = false;
    std::string base_url;  ///< canonical scheme://host[:port], no trailing slash
    std::string error;     ///< operator-facing reason, when !ok
};

/// Canonicalize an address to the value that gets persisted.
///
/// Accepts, after trimming surrounding whitespace:
///   - the empty string        → ok, base_url empty ("unset"; reporting off)
///   - scheme://host[:port]    → itself
///   - the same with ONE trailing "/"
///   - the same with the COMPLETE endpoint path pasted on (with or without one
///     trailing slash) → the endpoint is stripped back to the base
///
/// Rejects (never silently rewritten): any other path — including a run of
/// several trailing slashes, which is not a form anyone types — a missing or non
/// http/https scheme, a missing host, embedded credentials (userinfo), a query,
/// and a fragment. The endpoint match is case-SENSITIVE — the production path is
/// lowercase and a differently-cased path is a different path, not a typo this
/// function is entitled to guess at.
BaseUrlResult normalize_base_url(const std::string& input);

/// The full URL one POST goes to: the canonical base plus kEndpointPath.
///
/// This is the DEFENSIVE guard at the transport boundary. It runs
/// normalize_base_url, so legacy or externally written configuration that
/// already ends in the endpoint cannot produce the doubled
/// ".../api/brazing/update/api/brazing/update", AND a value the normalizer
/// rejects produces no endpoint at all rather than a request against a path
/// nobody validated. The transport therefore agrees with the UI by construction
/// — there is one rule, not a lenient copy of it down here.
///
/// Returns an empty string when the base is unset, whitespace-only, or
/// unusable — the caller's "do not POST" signal, never a partial URL.
std::string endpoint_url(const std::string& base_url);

} // namespace denso::brazing
