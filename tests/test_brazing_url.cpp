// The ONE brazing URL authority: what the operator may type, what gets
// persisted, and what a POST is actually addressed to.
//
// Two defects live here. The persisted setting is a BASE url, but the operator
// naturally pastes the complete endpoint they were given — which used to be
// stored verbatim and then had /api/brazing/update appended a second time. And a
// value that is not a base URL at all must be REFUSED with a reason, never
// quietly rewritten into something that points somewhere else.
//
// Pure string/QUrl logic — no database, no network, no widgets.
#include <catch2/catch_test_macros.hpp>

#include "brazing/url.h"

#include <string>

using denso::brazing::BaseUrlResult;
using denso::brazing::endpoint_url;
using denso::brazing::kDefaultApiPath;
using denso::brazing::normalize_base_url;

namespace {

// The confirmed PC test backend address, used verbatim so the cases read as the
// thing an operator actually types.
constexpr const char* kBase = "http://192.168.1.112:8080";

std::string canonical(const std::string& in) {
    const BaseUrlResult r = normalize_base_url(in);
    REQUIRE(r.ok);
    return r.base_url;
}

void expect_rejected(const std::string& in) {
    const BaseUrlResult r = normalize_base_url(in);
    CHECK_FALSE(r.ok);
    // A refusal the operator cannot act on is a refusal that will be retyped
    // identically, so a non-empty reason is part of the contract.
    CHECK_FALSE(r.error.empty());
    // Nothing is handed back for persistence when the input was refused.
    CHECK(r.base_url.empty());
}

} // namespace

// ── Accepted forms ───────────────────────────────────────────────────────────

TEST_CASE("a plain base URL is stored unchanged", "[brazing_url]") {
    CHECK(canonical(kBase) == kBase);
}

TEST_CASE("a trailing slash is removed", "[brazing_url]") {
    CHECK(canonical("http://192.168.1.112:8080/") == kBase);
}

TEST_CASE("a pasted complete endpoint becomes the base URL", "[brazing_url]") {
    // THE operator error this exists for: pasting the endpoint they were told to
    // POST to. Storing it verbatim produced …/update/api/brazing/update.
    CHECK(canonical("http://192.168.1.112:8080/api/brazing/update") == kBase);
}

TEST_CASE("a complete endpoint with a trailing slash becomes the base URL",
          "[brazing_url]") {
    CHECK(canonical("http://192.168.1.112:8080/api/brazing/update/") == kBase);
}

TEST_CASE("surrounding whitespace is removed", "[brazing_url]") {
    // Copy/paste from a chat message or a label routinely carries both.
    CHECK(canonical("  http://192.168.1.112:8080  ") == kBase);
    CHECK(canonical("\thttp://192.168.1.112:8080/api/brazing/update\n") == kBase);
}

TEST_CASE("an empty address is accepted as unset, not as an error", "[brazing_url]") {
    // Reporting legitimately sits disabled with no address stored — that is the
    // out-of-the-box state, and what a factory reset leaves behind.
    const BaseUrlResult r = normalize_base_url("   ");
    CHECK(r.ok);
    CHECK(r.base_url.empty());
}

TEST_CASE("https and a host name are accepted, and the scheme is canonicalized",
          "[brazing_url]") {
    CHECK(canonical("https://brazing.example.test") == "https://brazing.example.test");
    CHECK(canonical("HTTP://192.168.1.112:8080") == kBase);
}

// ── Refusals ─────────────────────────────────────────────────────────────────

TEST_CASE("an arbitrary path is rejected, never silently stripped", "[brazing_url]") {
    // The load-bearing half of the endpoint strip: only the ONE known production
    // path may be removed. Anything else may be a real reverse-proxy prefix, and
    // discarding it would post to a different resource than the operator asked
    // for — the failure mode a "just take scheme://host" normalizer would have.
    expect_rejected("http://192.168.1.112:8080/other/path");
}

TEST_CASE("the endpoint plus extra path is rejected", "[brazing_url]") {
    expect_rejected("http://192.168.1.112:8080/api/brazing/update/extra");
}

TEST_CASE("a missing scheme is rejected", "[brazing_url]") {
    expect_rejected("192.168.1.112:8080");
    expect_rejected("//192.168.1.112:8080");
    // A non-http scheme is not "close enough" either.
    expect_rejected("ftp://192.168.1.112:8080");
}

TEST_CASE("a missing host is rejected", "[brazing_url]") {
    expect_rejected("http://");
    expect_rejected("http:///api/brazing/update");
}

TEST_CASE("a query is rejected", "[brazing_url]") {
    expect_rejected("http://192.168.1.112:8080?debug=1");
    expect_rejected("http://192.168.1.112:8080/api/brazing/update?debug=1");
}

TEST_CASE("a fragment is rejected", "[brazing_url]") {
    expect_rejected("http://192.168.1.112:8080#frag");
    expect_rejected("http://192.168.1.112:8080/api/brazing/update#frag");
}

TEST_CASE("embedded credentials are rejected", "[brazing_url]") {
    // Credentials in the stored address would also end up in logs and support
    // bundles. There is no auth on this endpoint; refuse rather than carry them.
    expect_rejected("http://user:hunter2@192.168.1.112:8080");
    expect_rejected("http://user@192.168.1.112:8080/api/brazing/update");
}

TEST_CASE("a percent-encoded lookalike of the endpoint is rejected", "[brazing_url]") {
    // "/api/brazing%2Fupdate" DECODES to the endpoint but is a different path.
    // Matching on the decoded form would be this function guessing.
    expect_rejected("http://192.168.1.112:8080/api/brazing%2Fupdate");
}

TEST_CASE("a run of trailing slashes is rejected", "[brazing_url]") {
    // ONE trailing slash is what a browser or a copied link adds, and it is
    // accepted. A run of them is not a form anyone types, and stripping it would
    // be the normalizer starting to guess — the exact habit that lets a genuinely
    // wrong address through later.
    expect_rejected("http://192.168.1.112:8080//");
    expect_rejected("http://192.168.1.112:8080////");
    expect_rejected("http://192.168.1.112:8080/api/brazing/update//");
    // …and the single-slash forms still pass, so the tightening did not cost the
    // case it exists to allow.
    CHECK(canonical("http://192.168.1.112:8080/") == kBase);
    CHECK(canonical("http://192.168.1.112:8080/api/brazing/update/") == kBase);
}

TEST_CASE("a differently-cased endpoint path is rejected", "[brazing_url]") {
    // The shipped path is lowercase; a differently-cased path is a different
    // resource, not a typo this function is entitled to correct.
    expect_rejected("http://192.168.1.112:8080/API/Brazing/Update");
}

// ── The composed endpoint (the transport boundary) ───────────────────────────

TEST_CASE("the DEFAULT endpoint path is exactly /api/brazing/update",
          "[brazing_url]") {
    // Pinned as a literal: this is the confirmed production contract, and it is
    // now also the backward-compatibility contract — an installation with no
    // configured reporting API path composes exactly this. No refactor may drift
    // it. The configurable half is covered in test_brazing_api_path.cpp.
    CHECK(std::string(kDefaultApiPath) == "/api/brazing/update");
}

TEST_CASE("endpoint_url appends the endpoint to a base URL", "[brazing_url]") {
    CHECK(endpoint_url(kBase) == "http://192.168.1.112:8080/api/brazing/update");
    CHECK(endpoint_url("http://192.168.1.112:8080/") ==
          "http://192.168.1.112:8080/api/brazing/update");
}

TEST_CASE("endpoint_url cannot produce a doubled path", "[brazing_url]") {
    // The defensive guard: a legacy or externally written config row already
    // holding the full endpoint must NOT become …/update/api/brazing/update.
    CHECK(endpoint_url("http://192.168.1.112:8080/api/brazing/update") ==
          "http://192.168.1.112:8080/api/brazing/update");
    CHECK(endpoint_url("http://192.168.1.112:8080/api/brazing/update/") ==
          "http://192.168.1.112:8080/api/brazing/update");
}

TEST_CASE("endpoint_url refuses anything the normalizer refuses", "[brazing_url]") {
    // The transport applies the SAME rule as the dialog and the grid gate, so an
    // address the UI would reject cannot become a request through some other
    // construction path. It is neither accepted nor rewritten — there is simply
    // no endpoint, and the client reports the failure instead of posting.
    CHECK(endpoint_url("http://192.168.1.112:8080/other/path").empty());
    CHECK(endpoint_url("http://192.168.1.112:8080/api/brazing/update/extra").empty());
    CHECK(endpoint_url("http://user@192.168.1.112:8080/api/brazing/update").empty());
    CHECK(endpoint_url("192.168.1.112:8080").empty());
    CHECK(endpoint_url("http://192.168.1.112:8080?debug=1").empty());
}

TEST_CASE("endpoint_url of an unset address is empty", "[brazing_url]") {
    // Empty is the caller's "do not POST at all" signal, never a bare path.
    CHECK(endpoint_url("").empty());
    CHECK(endpoint_url("   ").empty());
    CHECK(endpoint_url("/").empty());
}

// ── The base normalizer against a CONFIGURED reporting API path ──────────────

TEST_CASE("the paste tolerance follows the configured path", "[brazing_url]") {
    // normalize_base_url strips the endpoint the operator was actually given.
    // Keyed to the shipped default instead, it would refuse a perfectly correct
    // paste with a message naming a path the customer's server does not expose.
    const BaseUrlResult r =
        normalize_base_url("http://192.168.1.112:8080/api/denso/update",
                           "/api/denso/update");
    REQUIRE(r.ok);
    CHECK(r.base_url == kBase);
    // …and one trailing slash on the paste is tolerated exactly as before.
    const BaseUrlResult with_slash =
        normalize_base_url("http://192.168.1.112:8080/api/denso/update/",
                           "/api/denso/update");
    REQUIRE(with_slash.ok);
    CHECK(with_slash.base_url == kBase);
}

TEST_CASE("a path that is not the configured endpoint is still rejected",
          "[brazing_url]") {
    // The tolerance MOVES with the configuration; it does not widen. Once
    // /api/denso/update is configured, the old default is just another arbitrary
    // path on the base, and stripping it would post to a resource the operator
    // never named.
    const BaseUrlResult r =
        normalize_base_url("http://192.168.1.112:8080/api/brazing/update",
                           "/api/denso/update");
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
    CHECK(r.base_url.empty());
}

TEST_CASE("an unusable configured path leaves the base rule well-defined",
          "[brazing_url]") {
    // The path is reported on its own field; this function must not become
    // undefined because of it. It falls back to the shipped default's tolerance,
    // and a plain base URL — the overwhelmingly common case — still passes.
    CHECK(normalize_base_url(kBase, "http://elsewhere/api").ok);
    CHECK(normalize_base_url(kBase, "http://elsewhere/api").base_url == kBase);
}
