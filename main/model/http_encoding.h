// Pure HTTP content-coding negotiation. No ESP-IDF, no allocation, no globals
// -- see tools/check_pure.sh.
//
// This exists because the web page is embedded gzip-compressed and ONLY
// gzip-compressed: storing both representations would cost more flash than the
// uncompressed literal it replaced, which is the entire point of compressing
// it. So whether a client accepts gzip stops being an optimisation and becomes
// the difference between serving the page and serving nothing, which is reason
// enough to get the parsing right and to pin it with tests.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Largest Accept-Encoding value worth reading off the wire. Real requests are
// well under this (browsers send ~20-40 bytes); the cap exists so the adapter
// can use a fixed stack buffer. See http_accepts_gzip_truncated() for what the
// caller should do when a header does not fit.
#define HTTP_ACCEPT_ENCODING_MAX 160

// True if a response may be sent with `Content-Encoding: gzip`.
//
// Follows RFC 9110 section 12.5.3:
//   - a NULL pointer means the request carried no Accept-Encoding field at all,
//     and "any content coding is considered acceptable"  -> true
//   - an empty or whitespace-only value advertises no codings, so only identity
//     is acceptable                                      -> false
//   - `gzip` (or the deprecated `x-gzip`) with a non-zero qvalue -> true
//   - `*` with a non-zero qvalue                                -> true,
//     unless gzip is separately pinned to q=0, which is more specific and wins
//   - anything else                                              -> false
//
// Coding names are matched case-insensitively. Malformed qvalues are treated as
// absent (q=1), matching the lenient reading servers need in practice.
bool http_accepts_gzip(const char *accept_encoding);

// What to answer when the Accept-Encoding value was too long for the caller's
// buffer and had to be cut short. Parsing a truncated value risks reading a
// half-written token and refusing gzip to a client that does, in fact, accept
// it -- which would serve a 406 to a normal browser. Since every real client
// that sends a long Accept-Encoding sends one containing gzip, the safe answer
// is yes. Kept as a named function rather than a bare `true` at the call site
// so the reasoning stays attached to the decision.
bool http_accepts_gzip_truncated(void);

#ifdef __cplusplus
}
#endif
