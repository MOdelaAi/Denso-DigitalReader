// Splitting the persisted base URL into the three controls an operator edits, and
// composing them back.
//
// The single free-text "Server base URL" field was too technical for the panel
// operator, so the Settings page now edits protocol / server address / port. The
// DATABASE did not change: `brazing.base_url` is still one canonical string, and
// these two functions are the only bridge between it and the form. No migration,
// no new column, no second place a server address lives.
//
// The load-bearing property is the ROUND TRIP: every base URL the authority
// accepts must survive split -> compose byte-for-byte, or the decomposed editor
// would silently rewrite configurations that were working.
//
// Pure string/QUrl logic — no database, no network, no widgets.
#include <catch2/catch_test_macros.hpp>

#include "brazing/url.h"

#include <string>
#include <vector>

using denso::brazing::BaseUrlParts;
using denso::brazing::BaseUrlResult;
using denso::brazing::compose_base_url;
using denso::brazing::normalize_base_url;
using denso::brazing::split_base_url;

namespace {

BaseUrlParts parts_of(bool https, const std::string& host, const std::string& port) {
    BaseUrlParts p;
    p.https = https;
    p.host = host;
    p.port = port;
    return p;
}

std::string composed(const BaseUrlParts& p) {
    const BaseUrlResult r = compose_base_url(p);
    REQUIRE(r.ok);
    return r.base_url;
}

void expect_compose_rejected(const BaseUrlParts& p) {
    const BaseUrlResult r = compose_base_url(p);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
    CHECK(r.base_url.empty());
}

} // namespace

// ── The backward-compatibility contract ──────────────────────────────────────

TEST_CASE("every accepted base URL survives split then compose",
          "[base_url_parts]") {
    // THE guarantee that makes the decomposed editor safe to ship against
    // installations already in the field: opening Settings and pressing Save must
    // not change a stored address. Run over the shapes real configurations take.
    const std::vector<std::string> stored = {
        "http://192.168.1.112:8080",
        "http://192.168.1.112",              // no port — the protocol default
        "https://192.168.1.112:8443",
        "https://server.example.com",        // host name, no port
        "https://server.example.com:9443",
        "http://brazing.example.test:80",    // an explicitly written default port
        "http://[::1]:8080",                 // IPv6, which needs its brackets back
        "https://[2001:db8::1]:8443",
        "http://192.168.1.112:1",            // the port bounds themselves
        "http://192.168.1.112:65535",
    };
    for (const std::string& v : stored) {
        INFO("stored value: " << v);
        // Precondition: the authority really does accept it, so a typo in this
        // table cannot make the case pass vacuously.
        const BaseUrlResult canonical = normalize_base_url(v);
        REQUIRE(canonical.ok);
        REQUIRE(canonical.base_url == v);
        CHECK(composed(split_base_url(v)) == v);
    }
}

TEST_CASE("a legacy row holding the full endpoint splits by its base",
          "[base_url_parts]") {
    // The paste tolerance moved: the form can no longer take a whole endpoint,
    // but a value ALREADY stored may be one. It must split by the base it
    // denotes, so the operator sees an address and a port rather than a refusal.
    const BaseUrlParts p =
        split_base_url("http://192.168.1.112:8080/api/brazing/update");
    CHECK_FALSE(p.https);
    CHECK(p.host == "192.168.1.112");
    CHECK(p.port == "8080");
    CHECK(composed(p) == "http://192.168.1.112:8080");
}

TEST_CASE("an unset base URL splits to an empty form, and back", "[base_url_parts]") {
    // The out-of-the-box state, and what a factory reset leaves behind. Blank is
    // "unset", not "invalid" — reporting legitimately sits off with no address.
    const BaseUrlParts p = split_base_url("");
    CHECK_FALSE(p.https);        // HTTP is the default selection
    CHECK(p.host.empty());
    CHECK(p.port.empty());
    const BaseUrlResult r = compose_base_url(p);
    CHECK(r.ok);
    CHECK(r.base_url.empty());
}

TEST_CASE("an unusable stored value is shown, not discarded", "[base_url_parts]") {
    // A row the normalizer refuses (hand-edited, or a restored backup) still has
    // a readable host. Blanking the form would hide the operator's configuration
    // from them; showing it lets them see and correct it, while the endpoint
    // preview says why nothing is being sent.
    const BaseUrlParts p = split_base_url("http://192.168.1.112:8080/other/path");
    CHECK(p.host == "192.168.1.112");
    CHECK(p.port == "8080");
}

// ── Composing ────────────────────────────────────────────────────────────────

TEST_CASE("the three controls compose the canonical base URL", "[base_url_parts]") {
    CHECK(composed(parts_of(false, "192.168.1.112", "8080")) ==
          "http://192.168.1.112:8080");
    CHECK(composed(parts_of(true, "server.example.com", "8443")) ==
          "https://server.example.com:8443");
    // A host name is lower-cased, which is what host names mean; an operator
    // typing it in capitals gets a working address, not a refusal.
    CHECK(composed(parts_of(false, "EXAMPLE.com", "8080")) ==
          "http://example.com:8080");
    // Surrounding whitespace from a copy/paste is removed on both controls.
    CHECK(composed(parts_of(false, "  192.168.1.112 ", " 8080 ")) ==
          "http://192.168.1.112:8080");
}

TEST_CASE("an empty port means the protocol's default and is omitted",
          "[base_url_parts]") {
    // NOT a convenience: a customer behind a reverse proxy on 80/443 has no port
    // to type, and a stored "https://server.example.com" has to give itself back
    // unchanged. Requiring a port here would break both.
    CHECK(composed(parts_of(false, "server.example.com", "")) ==
          "http://server.example.com");
    CHECK(composed(parts_of(true, "server.example.com", "  ")) ==
          "https://server.example.com");
}

TEST_CASE("an IPv6 address is composed with its brackets", "[base_url_parts]") {
    // The operator types the address as they read it, without brackets; the URL
    // needs them. Composing by string concatenation would produce
    // "http://::1:8080", which is not parseable as an address plus a port.
    CHECK(composed(parts_of(false, "::1", "8080")) == "http://[::1]:8080");
    CHECK(split_base_url("http://[::1]:8080").host == "::1");
}

// ── Refusals ─────────────────────────────────────────────────────────────────

TEST_CASE("a port outside 1..65535 is rejected", "[base_url_parts]") {
    expect_compose_rejected(parts_of(false, "192.168.1.112", "0"));
    expect_compose_rejected(parts_of(false, "192.168.1.112", "65536"));
    expect_compose_rejected(parts_of(false, "192.168.1.112", "99999"));
    expect_compose_rejected(parts_of(false, "192.168.1.112", "-1"));
}

TEST_CASE("a port that is not a plain number is rejected", "[base_url_parts]") {
    // toInt() alone would take "+80" and some locale spellings, and the stored
    // value would then differ from what was typed.
    expect_compose_rejected(parts_of(false, "192.168.1.112", "80a"));
    expect_compose_rejected(parts_of(false, "192.168.1.112", "+80"));
    expect_compose_rejected(parts_of(false, "192.168.1.112", "8 0"));
    expect_compose_rejected(parts_of(false, "192.168.1.112", "80.5"));
}

TEST_CASE("a whole URL typed into the address control is rejected",
          "[base_url_parts]") {
    // THE misuse the decomposition invites: pasting the address they were given
    // into the first box that looks right. Each part has its own control now, so
    // this is refused rather than silently reinterpreted.
    //
    // MEASURED, not assumed: QUrl::setHost stores an EMPTY host for these and
    // leaves isValid() TRUE, so a check on isValid() alone would compose
    // "http://:8080" and post to nowhere.
    expect_compose_rejected(parts_of(false, "http://192.168.1.112", "8080"));
    expect_compose_rejected(parts_of(false, "192.168.1.112:8080", ""));
    expect_compose_rejected(parts_of(false, "192.168.1.112/api", "8080"));
    expect_compose_rejected(parts_of(false, "192.168.1.112 8080", ""));
}

TEST_CASE("a port with no address is rejected, but a wholly empty form is not",
          "[base_url_parts]") {
    // A half-filled form is a mistake worth naming; an empty one is the
    // legitimate "reporting off" state.
    expect_compose_rejected(parts_of(false, "", "8080"));
    CHECK(compose_base_url(parts_of(false, "", "")).ok);
    CHECK(compose_base_url(parts_of(true, "  ", "")).ok);
}

TEST_CASE("composing goes through the base URL authority", "[base_url_parts]") {
    // compose_base_url ends in normalize_base_url, so the decomposed editor
    // cannot accept an address the free-text field would have refused, and what
    // it hands over for persistence is canonical by exactly the same rule.
    for (const auto& p : {parts_of(false, "192.168.1.112", "8080"),
                          parts_of(true, "server.example.com", ""),
                          parts_of(false, "::1", "8080")}) {
        const std::string out = composed(p);
        INFO("composed: " << out);
        const BaseUrlResult again = normalize_base_url(out);
        CHECK(again.ok);
        CHECK(again.base_url == out);   // already canonical — a fixed point
    }
}
