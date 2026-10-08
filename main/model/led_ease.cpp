#include "led_ease.h"

#include <algorithm>
#include <cmath>

double led_ease_alpha(double dt_ms, double tau_ms)
{
    if (tau_ms <= 0.0) {
        return 1.0;
    }
    return 1.0 - std::exp(-dt_ms / tau_ms);
}

double led_ease_step(double cur, double tgt, double alpha)
{
    return cur + (tgt - cur) * alpha;
}

double led_ease_clamp_dt(double dt_ms)
{
    return std::clamp(dt_ms, kLedEaseMinDtMs, kLedEaseMaxDtMs);
}

bool led_ease_advance(led_ease_rgb_t *current, const led_ease_rgb_t *target,
                      double dt_ms, double tau_ms)
{
    if (!current || !target) {
        // Nothing to advance. Reporting "settled" would tell the caller it may
        // block forever; reporting "in flight" only costs one more frame.
        return false;
    }

    double alpha = led_ease_alpha(led_ease_clamp_dt(dt_ms), tau_ms);
    current->brightness = led_ease_step(current->brightness, target->brightness, alpha);
    current->red = led_ease_step(current->red, target->red, alpha);
    current->green = led_ease_step(current->green, target->green, alpha);
    current->blue = led_ease_step(current->blue, target->blue, alpha);

    bool settled =
        std::fabs(current->brightness - target->brightness) < kLedEaseEpsilon &&
        std::fabs(current->red - target->red) < kLedEaseEpsilon &&
        std::fabs(current->green - target->green) < kLedEaseEpsilon &&
        std::fabs(current->blue - target->blue) < kLedEaseEpsilon;
    if (settled) {
        current->brightness = target->brightness;
        current->red = target->red;
        current->green = target->green;
        current->blue = target->blue;
    }
    return settled;
}

bool led_ease_power_from_brightness(uint8_t displayed_brightness)
{
    return displayed_brightness >= 1;
}
