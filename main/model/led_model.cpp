#include "led_model.h"

#include <algorithm>
#include <cstring>

// The parameter catalogue. Order matches led_effect_t, and the UI renders the
// labels verbatim, so both the names and the min/max/default triples are part
// of the external contract rather than internal tuning knobs.
static const effect_spec_t kEffectSpecs[LED_EFFECT_COUNT] = {
    {0, {{"", 0, 0, 0}, {"", 0, 0, 0}, {"", 0, 0, 0}, {"", 0, 0, 0}, {"", 0, 0, 0}}},
    {3, {{"Pulse Speed", 1, 255, 140}, {"Glow Floor", 0, 255, 72}, {"Pulse Depth", 0, 255, 180}, {"", 0, 0, 0}, {"", 0, 0, 0}}},
    {5, {{"Drift Speed", 1, 255, 120}, {"Rainbow Length", 1, 255, 96}, {"Color Blend", 0, 255, 220}, {"Start Offset", 0, 255, 0}, {"Contrast", 0, 255, 96}}},
    {3, {{"Chase Speed", 1, 255, 175}, {"Tail Length", 1, 255, 90}, {"Tail Sharpness", 0, 255, 170}, {"", 0, 0, 0}, {"", 0, 0, 0}}},
    {3, {{"Spark Density", 1, 255, 180}, {"Base Glow", 0, 255, 60}, {"Twinkle Speed", 1, 255, 170}, {"", 0, 0, 0}, {"", 0, 0, 0}}},
    {3, {{"Wave Speed", 1, 255, 110}, {"Wavelength", 1, 255, 110}, {"Wave Depth", 0, 255, 190}, {"", 0, 0, 0}, {"", 0, 0, 0}}},
    {5, {{"Cooling", 0, 255, 90}, {"Sparking", 0, 255, 120}, {"Flame Speed", 1, 255, 150}, {"Flame Height", 1, 255, 120}, {"Warmth", 0, 255, 160}}},
    {5, {{"Drift Speed", 1, 255, 70}, {"Color Scale", 1, 255, 110}, {"Saturation", 0, 255, 210}, {"Hue Center", 0, 255, 150}, {"Hue Spread", 0, 255, 90}}},
};

uint8_t led_clamp_u8(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

uint16_t led_clamp_u16(int value, int min_value, int max_value)
{
    return static_cast<uint16_t>(std::clamp(value, min_value, max_value));
}

uint8_t led_effect_from_index(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, static_cast<int>(LED_EFFECT_COUNT) - 1));
}

const effect_spec_t *led_effect_spec(uint8_t effect)
{
    return &kEffectSpecs[led_effect_from_index(effect)];
}

const char *led_effect_to_name(uint8_t effect)
{
    switch (effect) {
    case LED_EFFECT_GLOW:
        return "glow";
    case LED_EFFECT_RAINBOW:
        return "rainbow";
    case LED_EFFECT_CHASE:
        return "chase";
    case LED_EFFECT_SPARKLE:
        return "sparkle";
    case LED_EFFECT_WAVE:
        return "wave";
    case LED_EFFECT_FIRE:
        return "fire";
    case LED_EFFECT_AURORA:
        return "aurora";
    case LED_EFFECT_SOLID:
    default:
        return "solid";
    }
}

uint8_t led_effect_from_name(const char *effect_name)
{
    if (!effect_name) {
        return LED_EFFECT_SOLID;
    }
    if (std::strcmp(effect_name, "glow") == 0) {
        return LED_EFFECT_GLOW;
    }
    if (std::strcmp(effect_name, "rainbow") == 0) {
        return LED_EFFECT_RAINBOW;
    }
    if (std::strcmp(effect_name, "chase") == 0) {
        return LED_EFFECT_CHASE;
    }
    if (std::strcmp(effect_name, "sparkle") == 0) {
        return LED_EFFECT_SPARKLE;
    }
    if (std::strcmp(effect_name, "wave") == 0) {
        return LED_EFFECT_WAVE;
    }
    if (std::strcmp(effect_name, "fire") == 0) {
        return LED_EFFECT_FIRE;
    }
    if (std::strcmp(effect_name, "aurora") == 0) {
        return LED_EFFECT_AURORA;
    }
    return LED_EFFECT_SOLID;
}

uint8_t led_clamp_param(const effect_param_spec_t *spec, uint8_t value)
{
    if (!spec) {
        return value;
    }
    // An inverted declaration (max below min) would make std::clamp undefined,
    // so min wins. No catalogue entry is inverted; this keeps a future typo
    // from being undefined behaviour on the render path.
    if (spec->max_value < spec->min_value) {
        return spec->min_value;
    }
    return static_cast<uint8_t>(std::clamp(static_cast<int>(value),
                                           static_cast<int>(spec->min_value),
                                           static_cast<int>(spec->max_value)));
}

void led_clamp_effect_profile(uint8_t effect, effect_params_t *profile)
{
    if (!profile) {
        return;
    }

    const effect_spec_t *spec = led_effect_spec(effect);
    for (size_t index = 0; index < kEffectParamSlotCount; ++index) {
        if (index < spec->param_count) {
            profile->values[index] = led_clamp_param(&spec->params[index],
                                                     profile->values[index]);
        } else {
            profile->values[index] = 0;
        }
    }
}

void led_clamp_state(led_state_t *state, uint16_t max_pixels)
{
    if (!state) {
        return;
    }
    state->count = led_clamp_u16(state->count, 1, static_cast<int>(max_pixels));
    state->effect = led_effect_from_index(state->effect);
    for (uint8_t effect = 0; effect < LED_EFFECT_COUNT; ++effect) {
        led_clamp_effect_profile(effect, &state->effect_profiles[effect]);
    }
}

void led_reset_effect_profiles_to_defaults(led_state_t *state)
{
    if (!state) {
        return;
    }

    for (uint8_t effect = 0; effect < LED_EFFECT_COUNT; ++effect) {
        const effect_spec_t *spec = led_effect_spec(effect);
        for (size_t index = 0; index < kEffectParamSlotCount; ++index) {
            state->effect_profiles[effect].values[index] =
                index < spec->param_count ? spec->params[index].default_value : 0;
        }
    }
}

void led_reset_effect_colors_to_defaults(led_state_t *state)
{
    if (!state) {
        return;
    }

    for (uint8_t effect = 0; effect < LED_EFFECT_COUNT; ++effect) {
        state->effect_colors[effect].red = kLedDefaultRed;
        state->effect_colors[effect].green = kLedDefaultGreen;
        state->effect_colors[effect].blue = kLedDefaultBlue;
    }

    // Sparkle is the exception: its twinkles are white, and tinting them with
    // the warm default would make the effect read as a flicker rather than a
    // spark. The per-effect colour is still user-settable.
    state->effect_colors[LED_EFFECT_SPARKLE].red = 255;
    state->effect_colors[LED_EFFECT_SPARKLE].green = 255;
    state->effect_colors[LED_EFFECT_SPARKLE].blue = 255;
}
