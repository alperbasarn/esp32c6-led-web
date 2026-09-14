// Characterization tests for main/model/net_model.cpp.
//
// The `reference` table is a FROZEN copy of the switch as it stood in
// app_main.cpp before the move. Every reason code in 0..65535 is compared, so
// a dropped, renamed or mistyped case cannot survive.

#include "model/net_model.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

namespace reference {

static const char *wifi_disconnect_reason_to_text(uint16_t reason)
{
    switch (reason) {
    case 0:   return "none";
    case 2:   return "auth-expire";
    case 3:   return "auth-leave";
    case 4:   return "assoc-expire";
    case 5:   return "assoc-too-many";
    case 6:   return "not-authenticated";
    case 7:   return "not-associated";
    case 8:   return "assoc-leave";
    case 15:  return "4way-timeout";
    case 16:  return "group-key-timeout";
    case 23:  return "802.1x-auth-failed";
    case 200: return "beacon-timeout";
    case 201: return "no-ap-found";
    case 202: return "auth-failed";
    case 203: return "assoc-failed";
    case 204: return "handshake-timeout";
    case 205: return "connection-failed";
    default:  return "unknown";
    }
}

}  // namespace reference

static long g_checks = 0;
static long g_failures = 0;

static void check(bool ok, const char *what)
{
    ++g_checks;
    if (!ok) {
        if (g_failures < 10) {
            std::fprintf(stderr, "FAIL %s\n", what);
        }
        ++g_failures;
    }
}

// Every representable reason code agrees with the frozen table.
static void test_every_code_matches_reference()
{
    for (int r = 0; r <= 65535; ++r) {
        const char *got = net_wifi_disconnect_reason_text((uint16_t) r);
        const char *want = reference::wifi_disconnect_reason_to_text((uint16_t) r);
        if (got == nullptr || std::strcmp(got, want) != 0) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "reason=%d got=%s want=%s",
                          r, got ? got : "(null)", want);
            check(false, buf);
        } else {
            ++g_checks;
        }
    }
}

// The contract says the result is never nullptr, so callers can use it raw.
static void test_never_null()
{
    for (int r = 0; r <= 65535; ++r) {
        if (net_wifi_disconnect_reason_text((uint16_t) r) == nullptr) {
            check(false, "returned nullptr");
            return;
        }
    }
    check(true, "never null");
}

// 17 mapped codes plus the fallback = 18 distinct strings. Pinned so the table
// cannot silently shrink, and so the documented count stays honest.
static void test_distinct_string_count()
{
    std::set<std::string> distinct;
    int mapped = 0;
    for (int r = 0; r <= 65535; ++r) {
        std::string s = net_wifi_disconnect_reason_text((uint16_t) r);
        distinct.insert(s);
        if (s != "unknown") {
            ++mapped;
        }
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "mapped=%d (want 17)", mapped);
    check(mapped == 17, buf);
    std::snprintf(buf, sizeof(buf), "distinct=%zu (want 18)", distinct.size());
    check(distinct.size() == 18, buf);
}

// Spot-check a few slugs literally, so a wholesale re-spelling of the table
// cannot pass just because the frozen copy was edited to match.
static void test_known_slugs()
{
    struct { uint16_t code; const char *text; } known[] = {
        {0, "none"}, {8, "assoc-leave"}, {23, "802.1x-auth-failed"},
        {201, "no-ap-found"}, {205, "connection-failed"},
        {1, "unknown"}, {9999, "unknown"},
    };
    for (const auto &k : known) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "code %u should be %s", k.code, k.text);
        check(std::strcmp(net_wifi_disconnect_reason_text(k.code), k.text) == 0, buf);
    }
}

int main()
{
    test_every_code_matches_reference();
    test_never_null();
    test_distinct_string_count();
    test_known_slugs();

    std::printf("net_model: %ld checks, %ld failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
