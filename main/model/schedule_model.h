// Schedule and sleep/wake-timer arithmetic — pure model, no ESP-IDF.
//
// Part of main/model/: compiled both into the firmware and by the host suite
// with plain g++, so it may not include an esp_* header or touch a global.
//
// Time comes in as a PARAMETER, never from a clock. The caller reads
// esp_timer_get_time() (monotonic, for the relative timer) or localtime_r()
// (wall clock, for fixed schedules) and passes the values in. That is what
// makes midnight wrap, day-of-week masks and the fired-once-per-minute rule
// testable on the host in microseconds instead of by waiting for a real clock.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One fixed wall-clock schedule slot. Owned here rather than in app_main.cpp
// so the firing predicate can take it directly and stay pure.
typedef struct {
    uint8_t enabled;
    uint8_t hour;
    uint8_t minute;
    uint8_t days;    // bitmask, bit0=Sunday .. bit6=Saturday (0x7F = every day)
    uint8_t action;  // 0=off, 1=on
} schedule_entry_t;

// --- relative (sleep/wake) one-shot timer -----------------------------------
//
// deadline_us is a monotonic timestamp in the same domain as now_us, or 0 when
// no timer is pending. Both functions are total: they never divide by zero and
// never return a negative.

// Whole seconds remaining until the deadline, clamped at 0. Returns 0 for an
// inactive timer.
//
// This is the single projection shared by GET /api/state and GET /api/schedule,
// which each carried their own copy of the same expression. The two documents
// keep their different SHAPES (flat relative_* keys vs. a nested object); what
// is unified is the arithmetic behind them.
int schedule_relative_remaining_s(int64_t deadline_us, int64_t now_us);

// True when a pending timer has reached its deadline and should fire now.
bool schedule_relative_due(int64_t deadline_us, int64_t now_us);

// --- fixed wall-clock schedules ---------------------------------------------

// True when this entry should fire at the given local time.
//
// wday is 0=Sunday..6=Saturday (tm_wday). now_epoch_min is the current epoch
// minute, and last_fired_epoch_min the value recorded the last time this slot
// fired (-1 if never) — comparing them is what stops a schedule firing
// repeatedly during the minute it matches.
bool schedule_entry_due(const schedule_entry_t *entry,
                        int wday, int hour, int minute,
                        int32_t now_epoch_min, int32_t last_fired_epoch_min);

#ifdef __cplusplus
}
#endif
