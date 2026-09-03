// The reporting API path half of the ONE brazing URL authority: what an operator
// may type into "Reporting API path", what gets persisted, and what must be
// refused because it belongs in the other field.
//
// The path used to be a compile-time constant, so different customer backends
// could not be reached without a rebuild. Making it configurable creates exactly
// two new ways to break reporting, and both are pinned here: a value that is not
// a path at all (a whole server URL) must be REFUSED rather than concatenated
// onto the base, and an installation that has never configured a path must keep
// the endpoint it always had.
//
// Pure string/QUrl logic — no database, no network, no widgets.
#include <catch2/catch_test_macros.hpp>

#include "brazing/url.h"

#include <string>

using denso::brazing::ApiPathResult;
using denso::brazing::endpoint_url;
using denso::brazing::kDefaultApiPath;
using denso::brazing::normalize_api_path;

namespace {

constexpr const char* kBase = "http://192.168.1.112:8080";

std::string canonical_path(const std::string& in) {
    const ApiPathResult r = normalize_api_path(in);
    REQUIRE(r.ok);
    // An accepted path is always usable: never empty, always rooted.
    REQUIRE_FALSE(r.api_path.empty());
    REQUIRE(r.api_path.front() == '/');
    return r.api_path;
}

void expect_path_rejected(const std::string& in) {
    const ApiPathResult r = normalize_api_path(in);
    CHECK_FALSE(r.ok);
    // A refusal the operator cannot act on is a refusal that will be retyped
    // identically, so a non-empty reason is part of the contract.
    CHECK_FALSE(r.error.empty());
    // Nothing is handed back for persistence when the input was refused — in
    // particular NOT the default, which would silently report to the shipped
    // endpoint after the operator asked for a different one.
    CHECK(r.api_path.empty());
}

} // namespace

// ── The backward-compatibility contract ──────────────────────────────────────

TEST_CASE("the default reporting API path is exactly /api/brazing/update",
          "[brazing_api_path]") {
    // Pinned as a literal. Every installation that predates this setting has no
    // brazing.api_path row, resolves to this value, and must go on posting to the
    // endpoint it always did — so a refactor may not drift it.
    CHECK(std::string(kDefaultApiPath) == "/api/brazing/update");
}

TEST_CASE("an unset path resolves to the default, not to an error",
          "[brazing_api_path]") {
    // The ONE rule behind both "this installation has no such row" and "the
    // operator cleared the field". There is no such thing as reporting to no path
    // at all, so blank cannot mean "unset" the way an empty base URL does.
    CHECK(canonical_path("") == kDefaultApiPath);
    CHECK(canonical_path("   ") == kDefaultApiPath);
    CHECK(canonical_path("\t\n") == kDefaultApiPath);
}

TEST_CASE("a default configuration composes the exact historical endpoint",
          "[brazing_api_path]") {
    // The whole point of the defaulting above, stated as the endpoint an
    // un-migrated appliance will POST to.
    CHECK(endpoint_url(kBase) == "http://192.168.1.112:8080/api/brazing/update");
    CHECK(endpoint_url(kBase, "") == "http://192.168.1.112:8080/api/brazing/update");
    CHECK(endpoint_url(kBase, kDefaultApiPath) ==
          "http://192.168.1.112:8080/api/brazing/update");
}

// ── Accepted forms ───────────────────────────────────────────────────────────

TEST_CASE("a rooted path is stored unchanged", "[brazing_api_path]") {
    CHECK(canonical_path("/api/brazing/update") == "/api/brazing/update");
    CHECK(canonical_path("/api/denso/update") == "/api/denso/update");
    CHECK(canonical_path("/api/v1/zones/update") == "/api/v1/zones/update");
}

TEST_CASE("a missing leading slash is added", "[brazing_api_path]") {
    // The ONE thing this normalizer repairs, because it has exactly one meaning:
    // a path is rooted at the server.
    CHECK(canonical_path("api/denso/update") == "/api/denso/update");
    CHECK(canonical_path("update") == "/update");
}

TEST_CASE("surrounding whitespace is removed", "[brazing_api_path]") {
    CHECK(canonical_path("  /api/denso/update  ") == "/api/denso/update");
    CHECK(canonical_path("\t/api/denso/update\n") == "/api/denso/update");
    CHECK(canonical_path("  api/denso/update ") == "/api/denso/update");
}

TEST_CASE("a single trailing slash is PRESERVED, not trimmed",
          "[brazing_api_path]") {
    // Unlike the base URL, where a trailing slash is noise. "/x/" and "/x" are
    // different resources to plenty of servers (a Django APPEND_SLASH backend
    // wants one), and this function does not get to pick which one the customer
    // exposes. It is carried through into the endpoint verbatim.
    CHECK(canonical_path("/api/denso/update/") == "/api/denso/update/");
    CHECK(endpoint_url(kBase, "/api/denso/update/") ==
          "http://192.168.1.112:8080/api/denso/update/");
}

TEST_CASE("a path may carry the characters URLs allow", "[brazing_api_path]") {
    CHECK(canonical_path("/api/v2.1/zone-update_1") == "/api/v2.1/zone-update_1");
    CHECK(canonical_path("/api/denso%20update") == "/api/denso%20update");
}

// ── Refusals: the server address belongs in the OTHER field ──────────────────

TEST_CASE("a full server URL is rejected from the path field",
          "[brazing_api_path]") {
    // THE misuse this field invites. Accepting it would put a host in two
    // settings at once, and the two could then disagree about where readings go.
    expect_path_rejected("http://another-server/api/update");
    expect_path_rejected("https://another-server/api/update");
    expect_path_rejected("http://192.168.1.113:9090/api/brazing/update");
    expect_path_rejected("HTTP://another-server/api/update");
}

TEST_CASE("a protocol-relative or scheme-prefixed value is rejected",
          "[brazing_api_path]") {
    expect_path_rejected("//another-server/api/update");
    expect_path_rejected("ftp://another-server/api/update");
    // A bare "scheme:" with no "//" is still an absolute URI, not a path.
    expect_path_rejected("mailto:ops@example.test");
}

TEST_CASE("a query or a fragment is rejected", "[brazing_api_path]") {
    // Same rule the base URL already applies: the reporting contract is a plain
    // POST to a path, and neither the UI nor the transport interprets these.
    expect_path_rejected("/api/denso/update?debug=1");
    expect_path_rejected("/api/denso/update#frag");
}

TEST_CASE("whitespace inside the path is rejected", "[brazing_api_path]") {
    // trim() already removed the surrounding whitespace, so anything left is a
    // typo or a pasted line break — never a path the server exposes.
    expect_path_rejected("/api/denso update");
    expect_path_rejected("/api/denso\tupdate");
    expect_path_rejected("/api/denso\nupdate");
}

TEST_CASE("an empty // segment is rejected", "[brazing_api_path]") {
    // The doubled-slash defect the base URL normalizer exists to prevent, seen
    // from the other side: "//" INSIDE the path would reach the network verbatim.
    expect_path_rejected("/api//denso/update");
    expect_path_rejected("/api/denso/update//");
}

TEST_CASE("relative . and .. segments are rejected", "[brazing_api_path]") {
    // A path that walks upward is a path whose destination depends on how the
    // server resolves it. Refuse rather than post somewhere nobody validated.
    expect_path_rejected("/api/../admin");
    expect_path_rejected("/api/./update");
    expect_path_rejected("../api/update");
}

TEST_CASE("a bare slash is rejected", "[brazing_api_path]") {
    // Legal as a URL, but it is what a half-cleared field looks like, and posting
    // readings at a server's root is not something an operator asks for on
    // purpose. Blank means the default; "/" means they were still typing.
    expect_path_rejected("/");
}

TEST_CASE("characters that are not legal in a URL path are rejected",
          "[brazing_api_path]") {
    expect_path_rejected("/api/denso/update\"");
    expect_path_rejected("/api/<denso>/update");
    expect_path_rejected("/api/denso\\update");
    // A "%" that is not a valid escape would be re-encoded or dropped by Qt, so
    // the stored path would stop being the path that was typed.
    expect_path_rejected("/api/denso%zz/update");
    expect_path_rejected("/api/denso%2");
}

TEST_CASE("a percent-encoded / or . is rejected", "[brazing_api_path]") {
    // WELL-FORMED escapes that smuggle structure past the segment rules, which
    // are applied to the literal text. Servers and reverse proxies commonly
    // decode before routing, so accepting these would let the request resolve
    // somewhere other than the path this function validated and the Settings
    // preview displayed — the stored path and the effective path would differ.
    // The base URL normalizer already refuses "/api/brazing%2Fupdate" on exactly
    // this reasoning; the two halves of the authority now agree.
    expect_path_rejected("/api/%2e%2e/admin");     // ".." in disguise
    expect_path_rejected("/api/%2E%2E/admin");     // …and upper case
    expect_path_rejected("/api%2Fdenso/update");   // a separator in disguise
    expect_path_rejected("/api/denso%2f%2fupdate");// an empty segment in disguise
    expect_path_rejected("/api/denso%2Eupdate");   // a dot in disguise
    // A backslash is rejected for BOTH reasons at once: the literal "\" is not
    // in the path charset, so the encoded spelling must not be a back door; and
    // several stacks and reverse proxies alias "\" to "/" when normalizing, which
    // the segment rules cannot see — "%5c..%5c" is one segment, neither ".." nor
    // empty, yet it resolves to a parent directory on such a server.
    expect_path_rejected("/api/%5c..%5c/admin");
    expect_path_rejected("/api/%5C..%5C/admin");
    expect_path_rejected("/api%5cdenso/update");
    // A NUL ends the path rather than re-pointing it: a backend or proxy that
    // decodes into a C string truncates there, so the request routes to a SHORTER
    // path than the one stored and previewed. Same invariant, reached by
    // truncation instead of by redirection.
    expect_path_rejected("/api/update%00");
    expect_path_rejected("/api/update%00/ignored");
    expect_path_rejected("/api/%00update");
    // …and an escape of anything else is still fine, so the rule did not become
    // "no percent-encoding at all".
    CHECK(canonical_path("/api/denso%20update") == "/api/denso%20update");
}

TEST_CASE("the structural-escape rule is decided on the value, not the spelling",
          "[brazing_api_path]") {
    // The hex digits are parsed base 16, so a mixed- or upper-case spelling of
    // the same character cannot slip past a rule written for the lower-case one.
    // Pinned because "%2f but not %2F" is exactly the kind of half-rule a
    // hand-written character comparison produces.
    for (const char* v : {"/api/%2f/update", "/api/%2F/update",
                          "/api/%2e%2E/update", "/api/%5c/update",
                          "/api/%5C/update", "/api/update%00",
                          "/api/update%0" "0"}) {
        INFO("value: " << v);
        expect_path_rejected(v);
    }
}

TEST_CASE("the structural-escape list is CLOSED", "[brazing_api_path]") {
    // The four rejected characters are the ones that change WHERE the request
    // lands. Everything else an operator might legitimately encode stays
    // accepted, so this never became a blanket ban on percent-encoding.
    CHECK(canonical_path("/api/denso%20update") == "/api/denso%20update");
    CHECK(canonical_path("/api/denso%2Dupdate") == "/api/denso%2Dupdate");  // "-"
    CHECK(canonical_path("/api/denso%5Fupdate") == "/api/denso%5Fupdate");  // "_"
    CHECK(canonical_path("/api/%41%42") == "/api/%41%42");                  // "AB"
    CHECK(canonical_path("/api/denso%7Eupdate") == "/api/denso%7Eupdate");  // "~"
}

// ── The composed endpoint ────────────────────────────────────────────────────

TEST_CASE("a custom path composes onto the base with exactly one slash",
          "[brazing_api_path]") {
    // The two malformations this whole authority exists to make impossible.
    CHECK(endpoint_url(kBase, "/api/denso/update") ==
          "http://192.168.1.112:8080/api/denso/update");
    // A base carrying a trailing slash must not double it…
    CHECK(endpoint_url("http://192.168.1.112:8080/", "/api/denso/update") ==
          "http://192.168.1.112:8080/api/denso/update");
    // …and a path missing its leading slash must not weld onto the host.
    CHECK(endpoint_url(kBase, "api/denso/update") ==
          "http://192.168.1.112:8080/api/denso/update");
    CHECK(endpoint_url("http://192.168.1.112:8080/", "api/denso/update") ==
          "http://192.168.1.112:8080/api/denso/update");
}

TEST_CASE("endpoint_url refuses anything the path normalizer refuses",
          "[brazing_api_path]") {
    // The transport applies the SAME rule as the dialog and the grid gate, so a
    // path the UI would reject cannot become a request through some other
    // construction path. Empty is the caller's "do not POST at all" signal —
    // never a fallback to the default, which would report to an endpoint the
    // operator did not choose.
    CHECK(endpoint_url(kBase, "http://another-server/api/update").empty());
    CHECK(endpoint_url(kBase, "/api/denso/update?debug=1").empty());
    CHECK(endpoint_url(kBase, "/api//denso/update").empty());
    CHECK(endpoint_url(kBase, "/").empty());
}

TEST_CASE("a pasted endpoint is stripped against the CONFIGURED path",
          "[brazing_api_path]") {
    // The paste tolerance follows the configured path, not the shipped default.
    // An operator given "http://host:8080/api/denso/update" pastes exactly that;
    // a normalizer keyed to the default would refuse it with a message naming a
    // path their server does not even expose.
    CHECK(endpoint_url("http://192.168.1.112:8080/api/denso/update",
                       "/api/denso/update") ==
          "http://192.168.1.112:8080/api/denso/update");
    CHECK(endpoint_url("http://192.168.1.112:8080/api/denso/update/",
                       "/api/denso/update") ==
          "http://192.168.1.112:8080/api/denso/update");
    // And it stays exact: the OLD default is no longer a path this base may carry
    // once a different one is configured.
    CHECK(endpoint_url("http://192.168.1.112:8080/api/brazing/update",
                       "/api/denso/update")
              .empty());
}

TEST_CASE("the paste tolerance is one trailing slash in EITHER direction",
          "[brazing_api_path]") {
    // The tolerance is symmetric: the pasted endpoint may carry a trailing slash
    // the configured path lacks, AND may lack one the configured path has. The
    // second direction is the one worth stating out loud, because the path
    // setting deliberately treats "/x/" and "/x" as different resources — so what
    // is accepted here is the PASTE, not a redefinition of the endpoint. The
    // endpoint stays whatever was configured, and the preview shows it
    // immediately, so the operator is never left guessing which form won.
    CHECK(endpoint_url("http://192.168.1.112:8080/api/denso/update",
                       "/api/denso/update/") ==
          "http://192.168.1.112:8080/api/denso/update/");
    CHECK(endpoint_url("http://192.168.1.112:8080/api/denso/update/",
                       "/api/denso/update") ==
          "http://192.168.1.112:8080/api/denso/update");
    // Two slashes is still not a form anyone types, in either direction.
    CHECK(endpoint_url("http://192.168.1.112:8080/api/denso/update//",
                       "/api/denso/update/")
              .empty());
}
