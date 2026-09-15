// Tests for main/model/schedule_model.cpp.
//
// Unlike the colour and Wi-Fi models, this is not purely a characterization
// test: the two duplicated relative-timer projections were COLLAPSED into one
// function, so the point is to prove the single implementation reproduces what
// BOTH original copies computed, and then to cover the cases that were
// previously untestable because the logic was welded to a 1 Hz task and a real
// clock — midnight wrap, day-of-week masks, and the fire-once-per-minute rule.

#include "model/schedule_model.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace reference {

// Frozen: the expression as it appeared in BOTH send_state_json and
// schedule_get_handler, which each carried their own identical copy.
static int relative_remaining_s(int64_t rel_deadline, int64_t now_us)
{
    const bool rel_active = rel_deadline != 0;
    int rel_remaining_s = 0;
    if (rel_active) {
        int64_t rem_us = rel_deadline - now_us;
        rel_remaining_s = rem_us > 0 ? static_cast<int>(rem_us / 1000000) : 0;
    }
    return rel_remaining_s;
}

// Frozen: the predicate inline in schedule_task.
static bool relative_due(int64_t deadline, int64_t now_us)
{
    return deadline != 0 && now_us >= deadline;
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

#define CHECKF(cond, fmt, ...)                                  \
    do {                                                        \
        char buf[192];                                          \
        std::snprintf(buf, sizeof(buf), (fmt), __VA_ARGS__);    \
        check((cond), buf);                                     \
    } while (0)

static const int64_t kSec = 1000000;

// The collapsed projection must equal what both original copies produced,
// across a wide sweep including the inactive, expired and exact-boundary cases.
static void test_remaining_matches_both_originals()
{
    const int64_t nows[] = {0, 1, kSec, 1234567, 86400 * kSec, 9000000000LL};
    for (int64_t now : nows) {
        for (int64_t off = -5 * kSec; off <= 5 * kSec; off += 250000) {
            const int64_t deadline = now + off;
            if (deadline == 0) {
                continue;  // 0 means inactive, covered separately
            }
            const int got = schedule_relative_remaining_s(deadline, now);
            const int want = reference::relative_remaining_s(deadline, now);
            CHECKF(got == want, "remaining now=%lld deadline=%lld got=%d want=%d",
                   (long long) now, (long long) deadline, got, want);
        }
    }
}

static void test_remaining_edges()
{
    check(schedule_relative_remaining_s(0, 12345) == 0, "inactive timer reads 0");
    check(schedule_relative_remaining_s(0, 0) == 0, "inactive at t=0 reads 0");
    // Already past: clamps at 0 rather than going negative.
    check(schedule_relative_remaining_s(100, 500) == 0, "expired clamps to 0");
    // Truncation, not rounding: 1.9 s remaining reads as 1.
    check(schedule_relative_remaining_s(1900000, 0) == 1, "truncates toward zero");
    check(schedule_relative_remaining_s(2000000, 0) == 2, "exact 2 s reads 2");
    // Sub-second remaining reads 0 while the timer is still pending — the
    // countdown showing 0 does not mean it has fired.
    check(schedule_relative_remaining_s(999999, 0) == 0, "sub-second reads 0");
}

static void test_due_matches_original()
{
    const int64_t nows[] = {0, 1, kSec, 86400 * kSec};
    for (int64_t now : nows) {
        for (int64_t off = -3; off <= 3; ++off) {
            const int64_t deadline = now + off;
            const bool got = schedule_relative_due(deadline, now);
            const bool want = reference::relative_due(deadline, now);
            CHECKF(got == want, "due now=%lld deadline=%lld got=%d want=%d",
                   (long long) now, (long long) deadline, (int) got, (int) want);
        }
    }
    check(!schedule_relative_due(0, 999999999), "inactive never due");
    check(schedule_relative_due(100, 100), "due exactly at the deadline");
    check(!schedule_relative_due(101, 100), "not due one tick early");
}

// --- fixed schedules --------------------------------------------------------

static schedule_entry_t entry(uint8_t enabled, uint8_t hour, uint8_t minute,
                              uint8_t days, uint8_t action)
{
    schedule_entry_t e{};
    e.enabled = enabled;
    e.hour = hour;
    e.minute = minute;
    e.days = days;
    e.action = action;
    return e;
}

static void test_entry_basic()
{
    const uint8_t every_day = 0x7F;
    schedule_entry_t e = entry(1, 7, 30, every_day, 1);

    check(schedule_entry_due(&e, 3, 7, 30, 100, -1), "fires at its time");
    check(!schedule_entry_due(&e, 3, 7, 31, 100, -1), "wrong minute");
    check(!schedule_entry_due(&e, 3, 8, 30, 100, -1), "wrong hour");

    schedule_entry_t off = entry(0, 7, 30, every_day, 1);
    check(!schedule_entry_due(&off, 3, 7, 30, 100, -1), "disabled never fires");

    check(!schedule_entry_due(nullptr, 3, 7, 30, 100, -1), "null entry is not due");
}

// The rule that stops a 1 Hz tick firing the same slot up to sixty times.
static void test_entry_fires_once_per_minute()
{
    schedule_entry_t e = entry(1, 7, 30, 0x7F, 1);
    check(schedule_entry_due(&e, 3, 7, 30, 500, -1), "first tick of the minute fires");
    check(!schedule_entry_due(&e, 3, 7, 30, 500, 500), "already fired this minute");
    check(schedule_entry_due(&e, 3, 7, 30, 501, 500), "a later minute fires again");
}

// Each day bit selects exactly its own weekday, bit0=Sunday.
static void test_entry_day_mask()
{
    for (int day = 0; day <= 6; ++day) {
        schedule_entry_t e = entry(1, 12, 0, (uint8_t) (1u << day), 1);
        for (int wday = 0; wday <= 6; ++wday) {
            const bool got = schedule_entry_due(&e, wday, 12, 0, 10, -1);
            CHECKF(got == (wday == day), "day mask bit%d on wday%d got=%d",
                   day, wday, (int) got);
        }
    }
    schedule_entry_t none = entry(1, 12, 0, 0x00, 1);
    for (int wday = 0; wday <= 6; ++wday) {
        CHECKF(!schedule_entry_due(&none, wday, 12, 0, 10, -1),
               "empty day mask never fires (wday%d)", wday);
    }
    // Out-of-range weekday must not shift by an undefined amount.
    schedule_entry_t every = entry(1, 12, 0, 0x7F, 1);
    check(!schedule_entry_due(&every, -1, 12, 0, 10, -1), "wday -1 rejected");
    check(!schedule_entry_due(&every, 7, 12, 0, 10, -1), "wday 7 rejected");
}

// Midnight is a real boundary: 00:00 must fire, and the epoch-minute dedupe
// has to keep working as the day rolls over.
static void test_entry_midnight_wrap()
{
    schedule_entry_t e = entry(1, 0, 0, 0x7F, 0);
    check(schedule_entry_due(&e, 0, 0, 0, 1000, -1), "fires at 00:00");
    check(!schedule_entry_due(&e, 0, 23, 59, 999, -1), "does not fire at 23:59");
    check(!schedule_entry_due(&e, 0, 0, 0, 1000, 1000), "no double fire at 00:00");
    // Saturday 23:59 -> Sunday 00:00: the weekday the caller passes changes.
    schedule_entry_t sunday_only = entry(1, 0, 0, 0x01, 1);
    check(!schedule_entry_due(&sunday_only, 6, 0, 0, 2000, -1), "Saturday 00:00 skipped");
    check(schedule_entry_due(&sunday_only, 0, 0, 0, 2000, -1), "Sunday 00:00 fires");
}

int main()
{
    test_remaining_matches_both_originals();
    test_remaining_edges();
    test_due_matches_original();
    test_entry_basic();
    test_entry_fires_once_per_minute();
    test_entry_day_mask();
    test_entry_midnight_wrap();

    std::printf("schedule_model: %ld checks, %ld failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
