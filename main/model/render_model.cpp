#include "render_model.h"

#include <algorithm>
#include <cmath>

// Gamma is applied through this one helper so the NULL-LUT contract lives in a
// single place: no table means identity, not black.
static uint8_t apply_gamma(const uint8_t *lut, double value)
{
    uint8_t code = render_float_to_u8(value);
    return lut ? lut[code] : code;
}

// ---------------------------------------------------------------------------
// Everything below was lifted verbatim out of app_main.cpp by
// scratchpad extraction rather than retyped; only identifiers changed, plus the
// gamma application and the kernel's signature. See render_model.h.
// ---------------------------------------------------------------------------

uint32_t render_pseudo_random_u32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

double render_value_noise_1d(double x, uint32_t seed)
{
    double xi_f = std::floor(x);
    uint32_t xi = static_cast<uint32_t>(static_cast<int64_t>(xi_f));
    double frac = x - xi_f;                       // [0,1)
    auto lattice = [&](uint32_t i) {
        return static_cast<double>(render_pseudo_random_u32(i * 2654435761u + seed) & 0xffffu) / 65535.0;
    };
    double a = lattice(xi);
    double b = lattice(xi + 1u);
    double s = frac * frac * (3.0 - 2.0 * frac);  // smoothstep
    return a + (b - a) * s;                        // [0,1]
}

uint8_t render_wheel_channel(uint8_t wheel_pos, uint8_t channel)
{
    if (wheel_pos < 85) {
        return channel == 0 ? static_cast<uint8_t>(255 - wheel_pos * 3)
                            : (channel == 1 ? static_cast<uint8_t>(wheel_pos * 3) : 0);
    }
    if (wheel_pos < 170) {
        wheel_pos = static_cast<uint8_t>(wheel_pos - 85);
        return channel == 1 ? static_cast<uint8_t>(255 - wheel_pos * 3)
                            : (channel == 2 ? static_cast<uint8_t>(wheel_pos * 3) : 0);
    }
    wheel_pos = static_cast<uint8_t>(wheel_pos - 170);
    return channel == 2 ? static_cast<uint8_t>(255 - wheel_pos * 3)
                        : (channel == 0 ? static_cast<uint8_t>(wheel_pos * 3) : 0);
}

uint8_t render_float_to_u8(double value)
{
    return led_clamp_u8(static_cast<int>(std::lround(std::clamp(value, 0.0, 255.0))));
}

void render_gamma_lut_build(uint8_t *lut, double gamma)
{
    if (!lut) {
        return;
    }
    for (int i = 0; i < 256; ++i) {
        double normalized = static_cast<double>(i) / 255.0;
        long code = std::lround(255.0 * std::pow(normalized, gamma));
        if (code < 0) {
            code = 0;
        } else if (code > 255) {
            code = 255;
        }
        // Keep any lit input lit: a positive command must emit some light.
        if (i > 0 && code == 0) {
            code = 1;
        }
        lut[i] = static_cast<uint8_t>(code);
    }
}

double render_normalized_u8(uint8_t value)
{
    return static_cast<double>(value) / 255.0;
}

uint32_t render_effect_cycle_ms(uint8_t value, uint32_t slow_ms, uint32_t fast_ms)
{
    double speed = render_normalized_u8(std::max<uint8_t>(1, value));
    double interpolated = static_cast<double>(slow_ms) - (static_cast<double>(slow_ms - fast_ms) * speed);
    return std::max<uint32_t>(fast_ms, static_cast<uint32_t>(std::lround(interpolated)));
}

void render_effect_pixel(const led_state_t *state, const uint8_t *gamma_lut,
                         uint16_t index, uint32_t now_ms,
                         uint8_t *out_red, uint8_t *out_green, uint8_t *out_blue)
{
    if (!state || !out_red || !out_green || !out_blue || !state->power || state->brightness == 0 || index >= state->count) {
        if (out_red) {
            *out_red = 0;
        }
        if (out_green) {
            *out_green = 0;
        }
        if (out_blue) {
            *out_blue = 0;
        }
        return;
    }

    double brightness_scale = static_cast<double>(state->brightness) / 255.0;
    double red = static_cast<double>(state->red);
    double green = static_cast<double>(state->green);
    double blue = static_cast<double>(state->blue);
    uint16_t active_count = std::max<uint16_t>(state->count, 1);
    const effect_params_t &params = state->effect_profiles[state->effect];

    switch (state->effect) {
    case LED_EFFECT_GLOW: {
        double floor = 0.03 + render_normalized_u8(params.values[1]) * 0.62;
        double depth = 0.12 + render_normalized_u8(params.values[2]) * 0.88;
        uint32_t cycle = render_effect_cycle_ms(params.values[0], 3200U, 600U);
        double phase = (static_cast<double>(now_ms % cycle) / static_cast<double>(cycle)) * 2.0 * M_PI;
        double pulse = floor + (1.0 - floor) * (((std::sin(phase) + 1.0) * 0.5) * depth);
        brightness_scale *= pulse;
        break;
    }
    case LED_EFFECT_RAINBOW: {
        uint32_t cycle = render_effect_cycle_ms(params.values[0], 6000U, 450U);
        double length_leds = 2.0 + render_normalized_u8(params.values[1]) * std::max<double>(8.0, static_cast<double>(active_count) * 3.0);
        double travel_leds = (static_cast<double>(now_ms % cycle) / static_cast<double>(cycle)) * length_leds;
        double start_offset = render_normalized_u8(params.values[3]) * 255.0;
        double wheel_position = std::fmod((((static_cast<double>(index) + travel_leds) / length_leds) * 255.0) + start_offset, 256.0);
        if (wheel_position < 0.0) {
            wheel_position += 256.0;
        }
        uint8_t wheel_pos = static_cast<uint8_t>(wheel_position);
        double rainbow_mix = render_normalized_u8(params.values[2]);
        double contrast = render_normalized_u8(params.values[4]);
        double wave_phase = (((static_cast<double>(index) + travel_leds) / length_leds) * 2.0 * M_PI) +
                            (render_normalized_u8(params.values[3]) * 2.0 * M_PI);
        double rainbow_wave = 0.5 + 0.5 * std::sin(wave_phase);
        double contrast_scale = (1.0 - contrast) + contrast * rainbow_wave;
        red = red * (1.0 - rainbow_mix) + render_wheel_channel(wheel_pos, 0) * rainbow_mix;
        green = green * (1.0 - rainbow_mix) + render_wheel_channel(wheel_pos, 1) * rainbow_mix;
        blue = blue * (1.0 - rainbow_mix) + render_wheel_channel(wheel_pos, 2) * rainbow_mix;
        brightness_scale *= 0.3 + contrast_scale * 0.7;
        break;
    }
    case LED_EFFECT_CHASE: {
        uint32_t cycle = render_effect_cycle_ms(params.values[0], 4200U, 260U);      // ms per full loop
        double head = std::fmod(static_cast<double>(now_ms) / static_cast<double>(cycle), 1.0)
                      * static_cast<double>(active_count);                                // continuous 0..active_count

        // continuous distance measured backward from the head (tail trails behind)
        double behind = std::fmod(static_cast<double>(head) - static_cast<double>(index)
                                  + static_cast<double>(active_count), static_cast<double>(active_count));

        double tail_len   = 1.0 + render_normalized_u8(params.values[1]) * 14.0;                 // pixels, continuous
        double sharpness  = 0.5 + render_normalized_u8(params.values[2]) * 3.5;

        double trail = 0.0;
        if (behind <= tail_len) {
            double f = 1.0 - behind / tail_len;                                          // 1 at head -> 0 at tail end
            trail = std::pow(std::clamp(f, 0.0, 1.0), sharpness);
        }
        // anti-alias the leading edge: the pixel just AHEAD of a sub-pixel head
        double ahead = static_cast<double>(active_count) - behind;
        if (ahead < 1.0) {
            trail = std::max(trail, std::pow(1.0 - ahead, sharpness));
        }
        brightness_scale *= trail;
        break;
    }
    case LED_EFFECT_SPARKLE: {
        double density = render_normalized_u8(params.values[0]);
        double base    = render_normalized_u8(params.values[1]) * 0.35;

        uint32_t h      = render_pseudo_random_u32(index * 2654435761u);
        double   offset = static_cast<double>(h & 0xffffu) / 65535.0;                    // phase 0..1
        double   rate   = 0.5 + static_cast<double>((h >> 16) & 0xffffu) / 65535.0 * 1.5; // 0.5..2.0

        uint32_t cycle = render_effect_cycle_ms(params.values[2], 2600U, 500U);       // full twinkle period
        double phase = std::fmod(static_cast<double>(now_ms) / static_cast<double>(cycle) * rate + offset, 1.0);
        double env   = 0.5 - 0.5 * std::cos(phase * 2.0 * M_PI);                          // [0,1] smooth
        env = std::pow(env, 1.5);                                                          // crisper peak, soft tail

        uint32_t g   = render_pseudo_random_u32(index * 40503u + 7u);
        double   gate = static_cast<double>(g & 0xffffu) / 65535.0;
        double   active = (gate < (0.15 + density * 0.85)) ? 1.0 : 0.0;

        double level = base + (1.0 - base) * (env * active);   // inactive -> base; active twinkles base..1
        red   = static_cast<double>(state->effect_colors[LED_EFFECT_SPARKLE].red);
        green = static_cast<double>(state->effect_colors[LED_EFFECT_SPARKLE].green);
        blue  = static_cast<double>(state->effect_colors[LED_EFFECT_SPARKLE].blue);
        brightness_scale *= level;
        break;
    }
    case LED_EFFECT_WAVE: {
        uint32_t cycle = render_effect_cycle_ms(params.values[0], 3600U, 550U);
        double phase = (static_cast<double>(now_ms % cycle) / static_cast<double>(cycle)) * 2.0 * M_PI;
        double wavelength = 0.7 + render_normalized_u8(params.values[1]) * 6.3;
        double position = (static_cast<double>(index) / active_count) * 2.0 * M_PI * wavelength;
        double depth = render_normalized_u8(params.values[2]);
        double floor = 0.05 + (1.0 - depth) * 0.55;
        double wave = floor + (1.0 - floor) * ((std::sin(phase - position) + 1.0) * 0.5);
        brightness_scale *= wave;
        break;
    }
    case LED_EFFECT_FIRE: {
        double t     = static_cast<double>(now_ms) / 1000.0;                 // seconds
        double flow  = 0.4 + render_normalized_u8(params.values[2]) * 3.0;          // cells/sec, >0
        double scale = 0.15 + render_normalized_u8(params.values[3]) * 1.20;        // spatial freq, >0

        // Two-octave scrolling heat field (flows along the strip over time)
        double n1 = render_value_noise_1d(index * scale        - t * flow,        0x1000u);
        double n2 = render_value_noise_1d(index * scale * 2.7   - t * flow * 1.9,  0x2000u);
        double heat = n1 * 0.65 + n2 * 0.35;                                 // [0,1]

        // Cooling shifts the whole field down toward black
        heat -= render_normalized_u8(params.values[0]) * 0.45;

        // Sparse bright embers (deterministic per pixel + coarse time bucket)
        double spark_prob = render_normalized_u8(params.values[1]);
        uint32_t sbucket  = static_cast<uint32_t>(t * (2.0 + flow * 4.0));
        uint32_t sr       = render_pseudo_random_u32(index * 40503u + sbucket * 668265263u);
        double   s0       = static_cast<double>(sr & 0xffffu) / 65535.0;
        if (s0 < spark_prob * 0.10) {
            heat += 0.5 + 0.5 * (static_cast<double>((sr >> 16) & 0xffffu) / 65535.0);
        }
        heat = std::clamp(heat, 0.0, 1.0);

        // HeatColor-style palette: black -> red -> orange -> yellow -> white
        double warm = 0.5 + render_normalized_u8(params.values[4]) * 0.9;           // 0.5..1.4
        double h3 = heat * 3.0;
        red   = std::clamp(h3,               0.0, 1.0) * 255.0;
        green = std::clamp((h3 - 1.0) * warm, 0.0, 1.0) * 255.0;
        blue  = std::clamp((h3 - 2.0) * warm, 0.0, 1.0) * 255.0;
        // brightness_scale (master) left as-is; low heat is dark via the palette itself
        break;
    }
    case LED_EFFECT_AURORA: {
        double t     = static_cast<double>(now_ms) / 1000.0;
        double drift = 0.02 + render_normalized_u8(params.values[0]) * 0.30;        // cycles/sec (slow)
        double scale = 0.20 + render_normalized_u8(params.values[1]) * 2.00;
        double idx_n = static_cast<double>(index) / active_count;            // [0,1), active_count>=1

        // Organic drifting hue field from two out-of-phase slow waves
        double p1 = std::sin((idx_n * scale * 2.0 * M_PI)        + t * drift * 2.0 * M_PI);
        double p2 = std::sin((idx_n * scale * M_PI * 1.7)        - t * drift * 4.1);
        double field = p1 * 0.6 + p2 * 0.4;                                   // [-1,1]

        double hue = render_normalized_u8(params.values[3]) + field * (render_normalized_u8(params.values[4]) * 0.5);
        hue -= std::floor(hue);                                               // wrap to [0,1)
        uint8_t wheel_pos = static_cast<uint8_t>(hue * 255.0);

        double sat = render_normalized_u8(params.values[2]);
        red   = render_wheel_channel(wheel_pos, 0) * sat + 255.0 * (1.0 - sat);
        green = render_wheel_channel(wheel_pos, 1) * sat + 255.0 * (1.0 - sat);
        blue  = render_wheel_channel(wheel_pos, 2) * sat + 255.0 * (1.0 - sat);

        // Soft luminance breathing with an ambient floor (0.55..1.0)
        double bfield = 0.5 + 0.5 * std::sin((idx_n * scale * 2.0 * 2.0 * M_PI) - t * drift * 3.3);
        brightness_scale *= 0.55 + 0.45 * bfield;
        break;
    }
    case LED_EFFECT_SOLID:
    default:
        break;
    }

    // Apply gamma last, per channel, on the composed linear light intent
    // (channel x brightness_scale). This folds effect brightness modulation and
    // the temporally-eased brightness/color into a single gamma pass, giving a
    // perceptually even ramp. gamma_lut[0]==0, so the black early-return above
    // stays consistent.
    *out_red = apply_gamma(gamma_lut, red * brightness_scale);
    *out_green = apply_gamma(gamma_lut, green * brightness_scale);
    *out_blue = apply_gamma(gamma_lut, blue * brightness_scale);
}
