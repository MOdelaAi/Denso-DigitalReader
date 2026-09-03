// The ONE authority on where a zone-reading report is sent: the canonical form of
// the persisted server base URL, the canonical form of the persisted reporting
// API path, and the full URL a POST goes to. The Settings dialog, the CameraGrid
// gate and the Qt-Network transport all read the rule from here, so the UI can
// never accept an address the client would then treat differently — and the
// dialog's live preview shows the SAME string the transport will build.
//
// The two persisted halves are kept apart on purpose: scheme + host + port belong
// to the base URL, and only the path belongs to the API path. Neither field is
// allowed to carry the other's part.
#pragma once

#include <string>

namespace denso::brazing {

/// The reporting API path used when nothing else is configured — the path this
/// application shipped with before the setting existed, so an installation that
/// has no `brazing.api_path` row keeps posting to exactly the same endpoint.
/// Defined once — never spelled out again in the UI, the client, or a second
/// normalizer.
inline constexpr char kDefaultApiPath[] = "/api/brazing/update";

/// Outcome of canonicalizing an operator-entered address.
struct BaseUrlResult {
    bool        ok = false;
    std::string base_url;  ///< canonical scheme://host[:port], no trailing slash
    std::string error;     ///< operator-facing reason, when !ok
};

/// Outcome of canonicalizing an operator-entered reporting API path.
struct ApiPathResult {
    bool        ok = false;
    std::string api_path;  ///< canonical path, always leading "/"; never empty when ok
    std::string error;     ///< operator-facing reason, when !ok
};

/// Canonicalize the reporting API path that gets persisted.
///
/// Accepts, after trimming surrounding whitespace:
///   - the empty string              -> kDefaultApiPath (see below)
///   - "/api/denso/update"           -> itself
///   - "api/denso/update"            -> "/api/denso/update"  (leading slash added)
///   - one trailing slash is kept as typed, because "/x/" and "/x" are genuinely
///     different resources to many servers and this function does not get to
///     decide which one the customer's backend exposes
///
/// EMPTY MEANS DEFAULT, not "unset". Unlike the base URL — where empty is the
/// legitimate "no server configured, reporting off" state — there is no such
/// thing as reporting to no path at all. Resolving blank to kDefaultApiPath is
/// the SAME rule that makes a missing `brazing.api_path` row keep working, so
/// there is one behaviour to reason about rather than two. The caller is expected
/// to show the resolved value (the dialog re-seeds the field and the preview
/// updates as it is typed), so nothing about this is silent.
///
/// Rejects (never silently rewritten): anything carrying scheme or host — a full
/// "http(s)://server/path", a protocol-relative "//server/path", any "scheme:"
/// prefix — because those belong in the Server base URL; a query or a fragment;
/// embedded whitespace; an empty path segment ("//" anywhere); a "." or ".."
/// segment; a bare "/"; any character that is not legal in a URL path; a
/// malformed "%" escape; and a WELL-FORMED escape of "/", ".", "\" or NUL
/// ("%2F", "%2E", "%5C", "%00"), which would carry structure past the segment
/// rules above and let a decoding server route somewhere other than the path
/// shown in the preview — by re-pointing it, or, for NUL, by truncating it. That
/// list is closed: escapes of ordinary characters ("%20") stay accepted, because
/// encoding is how they are correctly written.
ApiPathResult normalize_api_path(const std::string& input);

/// Canonicalize an address to the value that gets persisted.
///
/// Accepts, after trimming surrounding whitespace:
///   - the empty string        -> ok, base_url empty ("unset"; reporting off)
///   - scheme://host[:port]    -> itself
///   - the same with ONE trailing "/"
///   - the same with the COMPLETE endpoint path pasted on (with or without one
///     trailing slash) -> the endpoint is stripped back to the base
///
/// `api_path` is the CONFIGURED reporting path, and is what the paste tolerance
/// above strips. It is a parameter rather than a constant so that an operator who
/// pastes the endpoint they were actually given — which, with a customer-specific
/// path, is not kDefaultApiPath — still gets it stripped instead of a refusal
/// naming a path their server does not have. A value that normalize_api_path
/// refuses falls back to kDefaultApiPath for the purposes of this strip; the
/// caller reports the path error on its own field.
///
/// Rejects (never silently rewritten): any other path — including a run of
/// several trailing slashes, which is not a form anyone types — a missing or non
/// http/https scheme, a missing host, embedded credentials (userinfo), a query,
/// and a fragment. The endpoint match is case-SENSITIVE — a differently-cased
/// path is a different path, not a typo this function is entitled to guess at.
BaseUrlResult normalize_base_url(const std::string& input,
                                 const std::string& api_path = kDefaultApiPath);

/// The persisted base URL as the three things an operator actually knows, which
/// is how the Settings page edits it. This is a VIEW of `brazing.base_url`, not a
/// second place it is stored: the row stays one canonical "scheme://host[:port]"
/// string, split for display and composed back on save.
struct BaseUrlParts {
    bool        https = false;  ///< false = http
    std::string host;           ///< IPv4, IPv6 (no brackets) or host name; empty = unset
    std::string port;           ///< decimal as typed; empty = the protocol's default
};

/// Split a persisted base URL into the three editor fields.
///
/// Runs normalize_base_url first, so a legacy or externally written row holding
/// the full endpoint splits by the base it denotes rather than by its literal
/// text. A value the normalizer refuses is parsed best-effort instead of being
/// discarded — the operator can then see and correct whichever part was readable,
/// and the endpoint preview says why it is unusable meanwhile.
///
/// Round-trip guarantee: for EVERY base URL normalize_base_url accepts,
/// compose_base_url(split_base_url(v)) yields v's canonical form exactly. That is
/// what makes the decomposed editor backward-compatible with every stored value.
BaseUrlParts split_base_url(const std::string& base_url);

/// Compose the three editor fields back into the canonical base URL that gets
/// persisted — through normalize_base_url, so the decomposed editor cannot accept
/// an address the free-text field would have refused. `api_path` is only the
/// paste-tolerance argument that normalize_base_url documents.
///
/// Accepts: an empty host with an empty port → ok with an empty base_url, the
/// legitimate "no server configured, reporting off" state. An empty port means
/// the protocol's default and is simply omitted, which is required for
/// backward compatibility — a stored "https://server.example.com" carries no port
/// and must round-trip unchanged.
///
/// Rejects: a port that is not a decimal number in 1..65535; a port typed with no
/// host (a half-filled form, not an "unset" one); and a host QUrl will not accept
/// — which includes an address carrying a scheme, a port or a path, because each
/// of those has its own control now.
BaseUrlResult compose_base_url(const BaseUrlParts& parts,
                               const std::string& api_path = kDefaultApiPath);

/// The full URL one POST goes to: the canonical base plus the canonical path.
/// This is the ONE composition site — the runtime transport, the CameraGrid log
/// line and the Settings preview all call it, so the preview cannot show one
/// endpoint while the client posts to another.
///
/// It is also the DEFENSIVE guard at the transport boundary. It runs both
/// normalizers, so legacy or externally written configuration that already ends
/// in the endpoint cannot produce a doubled path, AND a value either normalizer
/// rejects produces no endpoint at all rather than a request against something
/// nobody validated. The transport therefore agrees with the UI by construction
/// — there is one rule, not a lenient copy of it down here.
///
/// Returns an empty string when the base is unset, whitespace-only, or unusable,
/// or when the path is unusable — the caller's "do not POST" signal, never a
/// partial URL.
std::string endpoint_url(const std::string& base_url,
                         const std::string& api_path = kDefaultApiPath);

} // namespace denso::brazing
