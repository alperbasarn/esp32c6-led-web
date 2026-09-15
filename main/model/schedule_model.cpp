// Schedule and timer arithmetic. See schedule_model.h for the contract.
//
// Extracted from app_main.cpp: the relative-timer projection previously
// appeared twice, once in send_state_json and once in schedule_get_handler,
// and the firing predicates were inline in schedule_task. The expressions are
// preserved exactly, including the integer truncation in the seconds
// conversion and the >= (not >) comparison on the deadline.

#include "schedule_model.h"

int schedule_relative_remaining_s(int64_t deadline_us, int64_t now_us)
{
    if (deadline_us == 0) {
        return 0;
    }
    const int64_t rem_us = deadline_us - now_us;
    return rem_us > 0 ? static_cast<int>(rem_us / 1000000) : 0;
}

bool schedule_relative_due(int64_t deadline_us, int64_t now_us)
{
    return deadline_us != 0 && now_us >= deadline_us;
}

bool schedule_entry_due(const schedule_entry_t *entry,
                        int wday, int hour, int minute,
                        int32_t now_epoch_min, int32_t last_fired_epoch_min)
{
    if (!entry || !entry->enabled) {
        return false;
    }
    if (wday < 0 || wday > 6) {
        return false;
    }
    if ((entry->days & (1u << wday)) == 0) {
        return false;
    }
    if (entry->hour != hour || entry->minute != minute) {
        return false;
    }
    // Fire at most once per epoch minute, so a 1 Hz tick does not re-fire the
    // same schedule up to sixty times while its minute is current.
    return last_fired_epoch_min != now_epoch_min;
}
