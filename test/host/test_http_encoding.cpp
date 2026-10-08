// Host tests for main/model/http_encoding.cpp.
//
// Why this is worth exhaustive tests for a 100-line parser: the web page is
// embedded gzip-ONLY, so a wrong answer here is not a slower response, it is a
// 406 instead of the control UI. The captive portal is the only way to
// configure a device that has no Wi-Fi credentials yet, so "the page did not
// load" can mean "the device cannot be set up".

#include "../../main/model/http_encoding.h"

#include <cstdio>
#include <cstring>

static int g_checks = 0;
static int g_failures = 0;

static void expect(const char *header, bool want, const char *why)
{
    ++g_checks;
    bool got = http_accepts_gzip(header);
    if (got != want) {
        ++g_failures;
        std::printf("FAIL  %-44s want=%s got=%s  (%s)\n",
                    header ? header : "<no header>",
                    want ? "true" : "false", got ? "true" : "false", why);
    }
}

int main()
{
    // --- the cases that actually occur in the field ---------------------------
    expect(nullptr, true, "RFC 9110: no field means any coding is acceptable");
    expect("gzip", true, "bare gzip");
    expect("gzip, deflate", true, "curl --compressed");
    expect("gzip, deflate, br", true, "Chrome / Edge");
    expect("gzip, deflate, br, zstd", true, "recent Chrome");
    expect("gzip, deflate", true, "Android WebView captive-portal login");
    expect("gzip, deflate, br", true, "Apple CNA (WebKit)");

    // --- a client that genuinely refuses gzip --------------------------------
    expect("", false, "empty value advertises no coding; identity only");
    expect("   ", false, "whitespace-only value");
    expect("identity", false, "identity alone");
    expect("deflate", false, "named a coding, but not gzip, and no wildcard");
    expect("br, zstd", false, "no gzip, no wildcard");
    expect("gzip;q=0", false, "gzip explicitly forbidden");
    expect("gzip;q=0.0", false, "gzip forbidden, one decimal");
    expect("gzip;q=0.000", false, "gzip forbidden, three decimals");
    expect("identity;q=1, gzip;q=0", false, "identity preferred, gzip forbidden");

    // --- the wildcard, and gzip's precedence over it -------------------------
    expect("*", true, "wildcard accepts everything");
    expect("*;q=1", true, "wildcard, explicit q=1");
    expect("*;q=0.5", true, "wildcard, fractional but non-zero");
    expect("*;q=0", false, "wildcard forbids everything");
    expect("*;q=0, gzip", true, "explicit gzip beats a forbidding wildcard");
    expect("gzip;q=0, *", false, "explicit gzip q=0 beats a permitting wildcard");
    expect("gzip;q=0, *;q=1", false, "specific entry wins regardless of order");
    expect("deflate, *;q=0", false, "wildcard q=0 with no gzip entry");

    // --- qvalue arithmetic ---------------------------------------------------
    expect("gzip;q=1", true, "q=1");
    expect("gzip;q=1.0", true, "q=1.0");
    expect("gzip;q=1.000", true, "q=1.000");
    expect("gzip;q=0.001", true, "smallest representable non-zero q");
    expect("gzip;q=0.5", true, "mid q");
    expect("gzip;q=0.01", true, "two decimals, non-zero");
    expect("gzip;q=0.0001", false, "fourth decimal is below resolution, rounds to 0");

    // --- whitespace and case tolerance --------------------------------------
    expect("GZIP", true, "coding names are case-insensitive");
    expect("GzIp", true, "mixed case");
    expect("x-gzip", true, "deprecated alias still means gzip");
    expect("X-GZIP", true, "deprecated alias, upper case");
    expect("  gzip  ", true, "surrounding whitespace");
    expect("deflate,gzip", true, "no space after comma");
    expect("deflate ,  gzip  ,  br", true, "irregular spacing");
    expect("gzip ; q=0", false, "whitespace around the parameter separator");
    expect("gzip;  q  =  0", false, "whitespace inside the parameter");
    expect("gzip;Q=0", false, "parameter name is case-insensitive");

    // --- malformed input must not crash, and must not lock the user out ------
    expect(",", false, "a lone comma advertises nothing");
    expect(",,,", false, "only empty elements");
    expect("gzip,", true, "trailing comma");
    expect(",gzip", true, "leading comma");
    expect("gzip;", true, "parameter separator with no parameter");
    expect("gzip;q", true, "parameter name with no value -> q absent -> q=1");
    expect("gzip;q=", true, "empty qvalue is malformed -> treated as absent");
    expect("gzip;q=abc", true, "unparsable qvalue -> treated as absent");
    expect("gzip;q=2", true, "out-of-range q clamps to 1, not to 0");
    expect("gzip;charset=utf-8", true, "an unrelated parameter is not a qvalue");
    expect("gzipper", false, "must not match a coding that merely starts with gzip");
    expect("notgzip", false, "must not match a coding that merely contains gzip");
    expect("gzip2", false, "must not prefix-match");
    // The mirror of the above: a coding name that is a strict PREFIX of "gzip"
    // must not match either. Found by mutation testing -- weakening token_is()
    // to stop checking that the literal was fully consumed left every case
    // above still passing.
    expect("gz", false, "must not match a strict prefix of gzip");
    expect("g", false, "single character must not match");
    expect("gzi", false, "one character short must not match");
    expect("x-gz", false, "strict prefix of the deprecated alias");
    expect("x-", false, "alias prefix alone must not match");
    expect("gz;q=1", false, "a prefix with a qvalue is still not gzip");

    // --- duplicate entries: the permissive one wins --------------------------
    expect("gzip;q=0, gzip;q=1", true, "duplicate gzip, one permits");
    expect("gzip;q=1, gzip;q=0", true, "duplicate gzip, order must not matter");

    // --- a long header is never silently refused -----------------------------
    // The adapter reads at most HTTP_ACCEPT_ENCODING_MAX bytes. Anything longer
    // is answered by http_accepts_gzip_truncated() instead of by parsing a
    // half-token, because refusing a browser is worse than compressing for a
    // client that did not ask.
    ++g_checks;
    if (!http_accepts_gzip_truncated()) {
        ++g_failures;
        std::printf("FAIL  truncated header must default to accepting gzip\n");
    }

    ++g_checks;
    if (HTTP_ACCEPT_ENCODING_MAX < 64) {
        ++g_failures;
        std::printf("FAIL  HTTP_ACCEPT_ENCODING_MAX=%d is too small for real headers\n",
                    HTTP_ACCEPT_ENCODING_MAX);
    }

    // A value at the buffer limit, all of it plausible, still parses.
    {
        char longhdr[HTTP_ACCEPT_ENCODING_MAX];
        std::memset(longhdr, 0, sizeof longhdr);
        std::snprintf(longhdr, sizeof longhdr, "%s",
                      "deflate, br, zstd, compress, identity, x-compress, "
                      "bzip2, lzma, xz, snappy, lz4, gzip");
        expect(longhdr, true, "gzip last in a long but complete list");
    }

    std::printf("http_encoding: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
