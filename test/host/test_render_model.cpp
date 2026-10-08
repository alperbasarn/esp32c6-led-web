// Host tests for main/model/render_model.cpp.
//
// This is the first time any test has reached the render kernel. It previously
// ran only inside apply_led_state(), behind s_led_mutex, the led_strip driver
// and xTaskGetTickCount() -- so every frame of Fire and Aurora shipped
// unverified, including the ones added in 1.11.
//
// `reference` below is the pre-move code lifted verbatim out of app_main.cpp by
// the same extraction that produced the module (not retyped -- 186 lines of
// float arithmetic is where a silent one-character difference would hide). It
// keeps the ORIGINAL identifiers, so a divergence means the extraction changed
// behaviour and not that the test was written to agree with the result.
//
// Two differences from the original are unavoidable and shared by both sides:
// the kernel takes the gamma LUT as a parameter instead of reading the
// file-scope s_gamma_lut, and gamma is applied through one helper so the
// NULL-means-identity contract lives in a single place.

#include "../../main/model/render_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

static long g_checks = 0;
static long g_failures = 0;

static void fail(const char *what)
{
    ++g_failures;
    if (g_failures < 20) {
        std::printf("FAIL  %s\n", what);
    }
}

static void check(bool ok, const char *what)
{
    ++g_checks;
    if (!ok) {
        fail(what);
    }
}

// ---------------------------------------------------------------------------
// Frozen pre-move implementation.
// ---------------------------------------------------------------------------
namespace reference {

static uint8_t apply_gamma(const uint8_t *lut, double value);

static uint32_t pseudo_random_u32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

static double value_noise_1d(double x, uint32_t seed)
{
    double xi_f = std::floor(x);
    uint32_t xi = static_cast<uint32_t>(static_cast<int64_t>(xi_f));
    double frac = x - xi_f;                       // [0,1)
    auto lattice = [&](uint32_t i) {
        return static_cast<double>(pseudo_random_u32(i * 2654435761u + seed) & 0xffffu) / 65535.0;
    };
    double a = lattice(xi);
    double b = lattice(xi + 1u);
    double s = frac * frac * (3.0 - 2.0 * frac);  // smoothstep
    return a + (b - a) * s;                        // [0,1]
}

static uint8_t wheel_channel(uint8_t wheel_pos, uint8_t channel)
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

static uint8_t float_to_u8(double value)
{
    return led_clamp_u8(static_cast<int>(std::lround(std::clamp(value, 0.0, 255.0))));
}

static void gamma_lut_build(uint8_t *lut, double gamma)
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

static inline double normalized_u8(uint8_t value)
{
    return static_cast<double>(value) / 255.0;
}

static uint32_t effect_cycle_ms_from_value(uint8_t value, uint32_t slow_ms, uint32_t fast_ms)
{
    double speed = normalized_u8(std::max<uint8_t>(1, value));
    double interpolated = static_cast<double>(slow_ms) - (static_cast<double>(slow_ms - fast_ms) * speed);
    return std::max<uint32_t>(fast_ms, static_cast<uint32_t>(std::lround(interpolated)));
}

static void render_effect_pixel(const led_state_t *state, const uint8_t *gamma_lut,
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
        double floor = 0.03 + normalized_u8(params.values[1]) * 0.62;
        double depth = 0.12 + normalized_u8(params.values[2]) * 0.88;
        uint32_t cycle = effect_cycle_ms_from_value(params.values[0], 3200U, 600U);
        double phase = (static_cast<double>(now_ms % cycle) / static_cast<double>(cycle)) * 2.0 * M_PI;
        double pulse = floor + (1.0 - floor) * (((std::sin(phase) + 1.0) * 0.5) * depth);
        brightness_scale *= pulse;
        break;
    }
    case LED_EFFECT_RAINBOW: {
        uint32_t cycle = effect_cycle_ms_from_value(params.values[0], 6000U, 450U);
        double length_leds = 2.0 + normalized_u8(params.values[1]) * std::max<double>(8.0, static_cast<double>(active_count) * 3.0);
        double travel_leds = (static_cast<double>(now_ms % cycle) / static_cast<double>(cycle)) * length_leds;
        double start_offset = normalized_u8(params.values[3]) * 255.0;
        double wheel_position = std::fmod((((static_cast<double>(index) + travel_leds) / length_leds) * 255.0) + start_offset, 256.0);
        if (wheel_position < 0.0) {
            wheel_position += 256.0;
        }
        uint8_t wheel_pos = static_cast<uint8_t>(wheel_position);
        double rainbow_mix = normalized_u8(params.values[2]);
        double contrast = normalized_u8(params.values[4]);
        double wave_phase = (((static_cast<double>(index) + travel_leds) / length_leds) * 2.0 * M_PI) +
                            (normalized_u8(params.values[3]) * 2.0 * M_PI);
        double rainbow_wave = 0.5 + 0.5 * std::sin(wave_phase);
        double contrast_scale = (1.0 - contrast) + contrast * rainbow_wave;
        red = red * (1.0 - rainbow_mix) + wheel_channel(wheel_pos, 0) * rainbow_mix;
        green = green * (1.0 - rainbow_mix) + wheel_channel(wheel_pos, 1) * rainbow_mix;
        blue = blue * (1.0 - rainbow_mix) + wheel_channel(wheel_pos, 2) * rainbow_mix;
        brightness_scale *= 0.3 + contrast_scale * 0.7;
        break;
    }
    case LED_EFFECT_CHASE: {
        uint32_t cycle = effect_cycle_ms_from_value(params.values[0], 4200U, 260U);      // ms per full loop
        double head = std::fmod(static_cast<double>(now_ms) / static_cast<double>(cycle), 1.0)
                      * static_cast<double>(active_count);                                // continuous 0..active_count

        // continuous distance measured backward from the head (tail trails behind)
        double behind = std::fmod(static_cast<double>(head) - static_cast<double>(index)
                                  + static_cast<double>(active_count), static_cast<double>(active_count));

        double tail_len   = 1.0 + normalized_u8(params.values[1]) * 14.0;                 // pixels, continuous
        double sharpness  = 0.5 + normalized_u8(params.values[2]) * 3.5;

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
        double density = normalized_u8(params.values[0]);
        double base    = normalized_u8(params.values[1]) * 0.35;

        uint32_t h      = pseudo_random_u32(index * 2654435761u);
        double   offset = static_cast<double>(h & 0xffffu) / 65535.0;                    // phase 0..1
        double   rate   = 0.5 + static_cast<double>((h >> 16) & 0xffffu) / 65535.0 * 1.5; // 0.5..2.0

        uint32_t cycle = effect_cycle_ms_from_value(params.values[2], 2600U, 500U);       // full twinkle period
        double phase = std::fmod(static_cast<double>(now_ms) / static_cast<double>(cycle) * rate + offset, 1.0);
        double env   = 0.5 - 0.5 * std::cos(phase * 2.0 * M_PI);                          // [0,1] smooth
        env = std::pow(env, 1.5);                                                          // crisper peak, soft tail

        uint32_t g   = pseudo_random_u32(index * 40503u + 7u);
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
        uint32_t cycle = effect_cycle_ms_from_value(params.values[0], 3600U, 550U);
        double phase = (static_cast<double>(now_ms % cycle) / static_cast<double>(cycle)) * 2.0 * M_PI;
        double wavelength = 0.7 + normalized_u8(params.values[1]) * 6.3;
        double position = (static_cast<double>(index) / active_count) * 2.0 * M_PI * wavelength;
        double depth = normalized_u8(params.values[2]);
        double floor = 0.05 + (1.0 - depth) * 0.55;
        double wave = floor + (1.0 - floor) * ((std::sin(phase - position) + 1.0) * 0.5);
        brightness_scale *= wave;
        break;
    }
    case LED_EFFECT_FIRE: {
        double t     = static_cast<double>(now_ms) / 1000.0;                 // seconds
        double flow  = 0.4 + normalized_u8(params.values[2]) * 3.0;          // cells/sec, >0
        double scale = 0.15 + normalized_u8(params.values[3]) * 1.20;        // spatial freq, >0

        // Two-octave scrolling heat field (flows along the strip over time)
        double n1 = value_noise_1d(index * scale        - t * flow,        0x1000u);
        double n2 = value_noise_1d(index * scale * 2.7   - t * flow * 1.9,  0x2000u);
        double heat = n1 * 0.65 + n2 * 0.35;                                 // [0,1]

        // Cooling shifts the whole field down toward black
        heat -= normalized_u8(params.values[0]) * 0.45;

        // Sparse bright embers (deterministic per pixel + coarse time bucket)
        double spark_prob = normalized_u8(params.values[1]);
        uint32_t sbucket  = static_cast<uint32_t>(t * (2.0 + flow * 4.0));
        uint32_t sr       = pseudo_random_u32(index * 40503u + sbucket * 668265263u);
        double   s0       = static_cast<double>(sr & 0xffffu) / 65535.0;
        if (s0 < spark_prob * 0.10) {
            heat += 0.5 + 0.5 * (static_cast<double>((sr >> 16) & 0xffffu) / 65535.0);
        }
        heat = std::clamp(heat, 0.0, 1.0);

        // HeatColor-style palette: black -> red -> orange -> yellow -> white
        double warm = 0.5 + normalized_u8(params.values[4]) * 0.9;           // 0.5..1.4
        double h3 = heat * 3.0;
        red   = std::clamp(h3,               0.0, 1.0) * 255.0;
        green = std::clamp((h3 - 1.0) * warm, 0.0, 1.0) * 255.0;
        blue  = std::clamp((h3 - 2.0) * warm, 0.0, 1.0) * 255.0;
        // brightness_scale (master) left as-is; low heat is dark via the palette itself
        break;
    }
    case LED_EFFECT_AURORA: {
        double t     = static_cast<double>(now_ms) / 1000.0;
        double drift = 0.02 + normalized_u8(params.values[0]) * 0.30;        // cycles/sec (slow)
        double scale = 0.20 + normalized_u8(params.values[1]) * 2.00;
        double idx_n = static_cast<double>(index) / active_count;            // [0,1), active_count>=1

        // Organic drifting hue field from two out-of-phase slow waves
        double p1 = std::sin((idx_n * scale * 2.0 * M_PI)        + t * drift * 2.0 * M_PI);
        double p2 = std::sin((idx_n * scale * M_PI * 1.7)        - t * drift * 4.1);
        double field = p1 * 0.6 + p2 * 0.4;                                   // [-1,1]

        double hue = normalized_u8(params.values[3]) + field * (normalized_u8(params.values[4]) * 0.5);
        hue -= std::floor(hue);                                               // wrap to [0,1)
        uint8_t wheel_pos = static_cast<uint8_t>(hue * 255.0);

        double sat = normalized_u8(params.values[2]);
        red   = wheel_channel(wheel_pos, 0) * sat + 255.0 * (1.0 - sat);
        green = wheel_channel(wheel_pos, 1) * sat + 255.0 * (1.0 - sat);
        blue  = wheel_channel(wheel_pos, 2) * sat + 255.0 * (1.0 - sat);

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
static uint8_t apply_gamma(const uint8_t *lut, double value)
{
    uint8_t code = float_to_u8(value);
    return lut ? lut[code] : code;
}

}  // namespace reference

// ---------------------------------------------------------------------------

static uint8_t g_lut[RENDER_GAMMA_LUT_SIZE];
static uint8_t g_identity[RENDER_GAMMA_LUT_SIZE];

// Deterministic generator so any failure is reproducible from the trial index.
static uint32_t g_seed = 0x243f6a88u;
static uint32_t rnd()
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

// Compares one pixel between the extracted kernel and the frozen reference.
static bool same_pixel(const led_state_t *state, const uint8_t *lut,
                       uint16_t index, uint32_t now_ms, const char *where)
{
    uint8_t ar = 0xAA, ag = 0xAA, ab = 0xAA;
    uint8_t br = 0x55, bg = 0x55, bb = 0x55;
    render_effect_pixel(state, lut, index, now_ms, &ar, &ag, &ab);
    reference::render_effect_pixel(state, lut, index, now_ms, &br, &bg, &bb);
    ++g_checks;
    if (ar != br || ag != bg || ab != bb) {
        ++g_failures;
        if (g_failures < 20) {
            std::printf("FAIL  %s: effect=%u index=%u now=%u got %u,%u,%u want %u,%u,%u\n",
                        where, state ? state->effect : 255u, index, now_ms,
                        ar, ag, ab, br, bg, bb);
        }
        return false;
    }
    return true;
}

static void fill_random_state(led_state_t *s)
{
    s->count = (uint16_t) (1 + rnd() % 64);
    s->red = (uint8_t) rnd();
    s->green = (uint8_t) rnd();
    s->blue = (uint8_t) rnd();
    s->brightness = (uint8_t) (1 + rnd() % 255);
    s->power = true;
    s->effect = (uint8_t) (rnd() % LED_EFFECT_COUNT);
    for (int e = 0; e < LED_EFFECT_COUNT; ++e) {
        for (size_t i = 0; i < kEffectParamSlotCount; ++i) {
            s->effect_profiles[e].values[i] = (uint8_t) rnd();
        }
        s->effect_colors[e].red = (uint8_t) rnd();
        s->effect_colors[e].green = (uint8_t) rnd();
        s->effect_colors[e].blue = (uint8_t) rnd();
    }
}

int main()
{
    render_gamma_lut_build(g_lut, kRenderGammaExponent);
    for (int i = 0; i < RENDER_GAMMA_LUT_SIZE; ++i) {
        g_identity[i] = (uint8_t) i;
    }

    // --- the gamma LUT, and the invariants the kernel leans on ---------------
    {
        uint8_t ref_lut[RENDER_GAMMA_LUT_SIZE];
        reference::gamma_lut_build(ref_lut, 2.2);
        check(std::memcmp(g_lut, ref_lut, sizeof g_lut) == 0,
              "gamma LUT matches the reference byte for byte");

        check(g_lut[0] == 0, "lut[0] is 0 -- the black early-return depends on it");
        bool monotonic = true;
        bool lit_stays_lit = true;
        for (int i = 1; i < RENDER_GAMMA_LUT_SIZE; ++i) {
            if (g_lut[i] < g_lut[i - 1]) {
                monotonic = false;
            }
            if (g_lut[i] == 0) {
                lit_stays_lit = false;
            }
        }
        check(monotonic, "gamma LUT is non-decreasing (a brighter command is never dimmer)");
        check(lit_stays_lit, "every non-zero command emits some light");
        check(g_lut[255] == 255, "full scale maps to full scale");

        // Exponents the firmware does not use, to pin the builder itself.
        for (double gamma : {1.0, 1.8, 2.2, 2.8}) {
            uint8_t mine[RENDER_GAMMA_LUT_SIZE], theirs[RENDER_GAMMA_LUT_SIZE];
            render_gamma_lut_build(mine, gamma);
            reference::gamma_lut_build(theirs, gamma);
            check(std::memcmp(mine, theirs, sizeof mine) == 0,
                  "gamma LUT matches the reference at a non-default exponent");
        }
        uint8_t linear[RENDER_GAMMA_LUT_SIZE];
        render_gamma_lut_build(linear, 1.0);
        bool is_identity = true;
        for (int i = 0; i < RENDER_GAMMA_LUT_SIZE; ++i) {
            if (linear[i] != (uint8_t) i) {
                is_identity = false;
            }
        }
        check(is_identity, "gamma 1.0 is the identity ramp");
        ++g_checks;
        render_gamma_lut_build(nullptr, 2.2);  // must not crash
    }

    // --- the small pure helpers --------------------------------------------
    for (int v = 0; v <= 255; ++v) {
        check(render_normalized_u8((uint8_t) v) == reference::normalized_u8((uint8_t) v),
              "render_normalized_u8 matches");
        for (int c = 0; c < 3; ++c) {
            ++g_checks;
            if (render_wheel_channel((uint8_t) v, (uint8_t) c) !=
                reference::wheel_channel((uint8_t) v, (uint8_t) c)) {
                fail("render_wheel_channel diverged");
            }
        }
        ++g_checks;
        if (render_pseudo_random_u32((uint32_t) v) != reference::pseudo_random_u32((uint32_t) v)) {
            fail("render_pseudo_random_u32 diverged");
        }
    }
    // The wheel's three channels should always total roughly full scale --
    // that is what makes it a hue sweep at constant brightness rather than a
    // ramp that dims in the middle.
    for (int v = 0; v <= 255; ++v) {
        int sum = render_wheel_channel((uint8_t) v, 0) +
                  render_wheel_channel((uint8_t) v, 1) +
                  render_wheel_channel((uint8_t) v, 2);
        check(sum >= 250 && sum <= 257, "wheel channels sum to about full scale");
    }
    // The hash must be a pure function of its input: the same pixel has to look
    // the same on every device and after every reboot.
    for (uint32_t v = 0; v < 4096; ++v) {
        ++g_checks;
        if (render_pseudo_random_u32(v) != render_pseudo_random_u32(v)) {
            fail("render_pseudo_random_u32 is not deterministic");
        }
    }
    {
        // ...and distinct enough that neighbouring pixels do not twinkle in step.
        int collisions = 0;
        for (uint32_t v = 0; v < 2048; ++v) {
            if ((render_pseudo_random_u32(v) & 0xffffu) ==
                (render_pseudo_random_u32(v + 1) & 0xffffu)) {
                ++collisions;
            }
        }
        check(collisions < 5, "adjacent pixel hashes rarely collide");
    }
    for (int i = 0; i < 20000; ++i) {
        double x = ((double) (int32_t) rnd() / 2147483648.0) * 40.0;
        uint32_t seed = rnd();
        ++g_checks;
        double a = render_value_noise_1d(x, seed);
        double b = reference::value_noise_1d(x, seed);
        if (a != b) {
            fail("render_value_noise_1d diverged");
        }
        if (!(a >= 0.0 && a <= 1.0)) {
            fail("value noise escaped [0,1]");
        }
    }
    {
        // Continuity: the heat field must not jump, or Fire would strobe.
        double worst = 0.0;
        for (double x = -5.0; x < 5.0; x += 0.001) {
            double d = std::fabs(render_value_noise_1d(x + 0.001, 0x1000u) -
                                 render_value_noise_1d(x, 0x1000u));
            worst = std::max(worst, d);
        }
        check(worst < 0.05, "value noise is continuous across a small step");
    }
    for (int v = 0; v <= 255; ++v) {
        for (auto bounds : {std::pair<uint32_t, uint32_t>{3200, 600},
                            {6000, 450}, {4200, 260}, {2600, 500}, {3600, 550}}) {
            ++g_checks;
            uint32_t mine = render_effect_cycle_ms((uint8_t) v, bounds.first, bounds.second);
            uint32_t theirs = reference::effect_cycle_ms_from_value((uint8_t) v, bounds.first,
                                                                    bounds.second);
            if (mine != theirs) {
                fail("render_effect_cycle_ms diverged");
            }
            // Every caller divides by this.
            // Note: the max(fast_ms, ...) in the implementation is UNREACHABLE
            // for any uint8_t value -- speed tops out at exactly 1.0, where the
            // interpolation lands on fast_ms precisely. Mutation testing
            // confirmed that removing the clamp changes nothing, so this is
            // equivalence rather than a gap. It would only bite if a caller
            // passed slow_ms < fast_ms, which underflows the unsigned
            // subtraction; no caller does, and that hazard is recorded in
            // render_model.h rather than fixed inside a no-behaviour-change
            // extraction.
            if (mine == 0) {
                fail("cycle length of zero would divide by zero on the render path");
            }
            if (mine < bounds.second) {
                fail("cycle length fell below the fast bound");
            }
        }
    }
    for (int v = -1000; v <= 1000; ++v) {
        double d = (double) v / 2.0;
        check(render_float_to_u8(d) == reference::float_to_u8(d),
              "render_float_to_u8 matches");
    }

    // --- the kernel: every effect, swept over time and position -------------
    for (int effect = 0; effect < LED_EFFECT_COUNT; ++effect) {
        led_state_t s{};
        s.count = 16;
        s.red = 200; s.green = 120; s.blue = 40;
        s.brightness = 180;
        s.power = true;
        s.effect = (uint8_t) effect;
        led_reset_effect_profiles_to_defaults(&s);
        led_reset_effect_colors_to_defaults(&s);
        for (uint32_t now = 0; now < 8000; now += 37) {
            for (uint16_t i = 0; i < s.count; ++i) {
                same_pixel(&s, g_lut, i, now, "default profile sweep");
            }
        }
    }

    // --- every effect x every parameter driven to its extremes --------------
    for (int effect = 0; effect < LED_EFFECT_COUNT; ++effect) {
        const effect_spec_t *spec = led_effect_spec((uint8_t) effect);
        for (size_t p = 0; p < spec->param_count; ++p) {
            for (int extreme = 0; extreme < 3; ++extreme) {
                led_state_t s{};
                s.count = 24;
                s.red = 255; s.green = 255; s.blue = 255;
                s.brightness = 255;
                s.power = true;
                s.effect = (uint8_t) effect;
                led_reset_effect_profiles_to_defaults(&s);
                led_reset_effect_colors_to_defaults(&s);
                uint8_t v = extreme == 0 ? spec->params[p].min_value
                            : extreme == 1 ? spec->params[p].max_value
                                           : (uint8_t) 0;  // below min, unclamped
                s.effect_profiles[effect].values[p] = v;
                for (uint32_t now = 0; now < 4000; now += 53) {
                    for (uint16_t i = 0; i < s.count; i += 3) {
                        same_pixel(&s, g_lut, i, now, "parameter extreme");
                    }
                }
            }
        }
    }

    // --- random states, both gamma ramps ------------------------------------
    for (int trial = 0; trial < 1200; ++trial) {
        led_state_t s{};
        fill_random_state(&s);
        const uint8_t *lut = (trial % 3 == 0) ? g_identity
                             : (trial % 3 == 1) ? g_lut : nullptr;
        for (int k = 0; k < 12; ++k) {
            uint32_t now = rnd();
            uint16_t i = (uint16_t) (rnd() % (s.count + 2));  // sometimes past the end
            same_pixel(&s, lut, i, now, "random state");
        }
    }

    // --- the early-return conditions ----------------------------------------
    {
        led_state_t s{};
        s.count = 8; s.red = 255; s.green = 255; s.blue = 255;
        s.brightness = 255; s.power = true; s.effect = LED_EFFECT_RAINBOW;
        led_reset_effect_profiles_to_defaults(&s);
        led_reset_effect_colors_to_defaults(&s);

        uint8_t r = 9, g = 9, b = 9;

        led_state_t off = s; off.power = false;
        render_effect_pixel(&off, g_lut, 0, 1234, &r, &g, &b);
        check(r == 0 && g == 0 && b == 0, "power off renders black");
        same_pixel(&off, g_lut, 0, 1234, "power off");

        led_state_t dark = s; dark.brightness = 0;
        r = g = b = 9;
        render_effect_pixel(&dark, g_lut, 0, 1234, &r, &g, &b);
        check(r == 0 && g == 0 && b == 0, "zero brightness renders black");
        same_pixel(&dark, g_lut, 0, 1234, "zero brightness");

        r = g = b = 9;
        render_effect_pixel(&s, g_lut, s.count, 1234, &r, &g, &b);
        check(r == 0 && g == 0 && b == 0, "index at count renders black");
        same_pixel(&s, g_lut, s.count, 1234, "index at count");
        same_pixel(&s, g_lut, 60000, 1234, "index far past count");

        // brightness 1 is the lowest LIT state: the fade-to-off path keeps
        // rendering until it reaches 0, so 1 must not be black.
        led_state_t dim = s; dim.brightness = 1; dim.effect = LED_EFFECT_SOLID;
        r = g = b = 0;
        render_effect_pixel(&dim, g_identity, 0, 0, &r, &g, &b);
        check(r > 0 || g > 0 || b > 0, "brightness 1 is lit, not black");

        // The early-return must not consult the LUT at all.
        //
        // With any LUT the builder produces, lut[0] == 0, so "return black"
        // and "compose zero light and look it up" give the same answer -- which
        // means dropping the brightness==0 guard entirely is invisible. Mutation
        // testing proved exactly that. A pathological ramp where lut[0] != 0
        // separates the two paths and pins the guard as load-bearing.
        uint8_t hot[RENDER_GAMMA_LUT_SIZE];
        for (int i = 0; i < RENDER_GAMMA_LUT_SIZE; ++i) {
            hot[i] = 0xFF;
        }
        led_state_t dark2 = s; dark2.brightness = 0;
        r = g = b = 0;
        render_effect_pixel(&dark2, hot, 0, 4321, &r, &g, &b);
        check(r == 0 && g == 0 && b == 0,
              "zero brightness short-circuits before the LUT is consulted");
        led_state_t off2 = s; off2.power = false;
        r = g = b = 0;
        render_effect_pixel(&off2, hot, 0, 4321, &r, &g, &b);
        check(r == 0 && g == 0 && b == 0, "power off short-circuits before the LUT");
        r = g = b = 0;
        render_effect_pixel(&s, hot, s.count + 5, 4321, &r, &g, &b);
        check(r == 0 && g == 0 && b == 0, "out-of-range index short-circuits before the LUT");
        // ...and a lit pixel does reach that same LUT, so the test above is not
        // passing merely because the ramp was ignored everywhere.
        r = g = b = 0;
        render_effect_pixel(&s, hot, 0, 4321, &r, &g, &b);
        check(r == 0xFF && g == 0xFF && b == 0xFF,
              "a lit pixel does go through the LUT");

        // NULL inputs must be survivable, and must not write through.
        ++g_checks;
        render_effect_pixel(nullptr, g_lut, 0, 0, &r, &g, &b);
        check(r == 0 && g == 0 && b == 0, "NULL state yields black");
        ++g_checks;
        render_effect_pixel(&s, g_lut, 0, 0, nullptr, nullptr, nullptr);
        ++g_checks;
        render_effect_pixel(&s, nullptr, 0, 0, &r, &g, &b);  // NULL LUT = identity
    }

    // --- count == 1, the degenerate strip -----------------------------------
    for (int effect = 0; effect < LED_EFFECT_COUNT; ++effect) {
        led_state_t s{};
        s.count = 1; s.red = 180; s.green = 90; s.blue = 220;
        s.brightness = 200; s.power = true; s.effect = (uint8_t) effect;
        led_reset_effect_profiles_to_defaults(&s);
        led_reset_effect_colors_to_defaults(&s);
        for (uint32_t now = 0; now < 6000; now += 29) {
            same_pixel(&s, g_lut, 0, now, "single-pixel strip");
        }
    }

    // --- the clock near wraparound ------------------------------------------
    // now_ms comes from a 32-bit tick count, so it wraps after ~49 days of
    // uptime. The kernel must not diverge or divide by zero there.
    for (int effect = 0; effect < LED_EFFECT_COUNT; ++effect) {
        led_state_t s{};
        s.count = 12; s.red = 255; s.green = 200; s.blue = 100;
        s.brightness = 220; s.power = true; s.effect = (uint8_t) effect;
        led_reset_effect_profiles_to_defaults(&s);
        led_reset_effect_colors_to_defaults(&s);
        static const uint32_t kEdges[] = {
            0u, 1u, 0x7fffffffu, 0x80000000u,
            0xfffffff0u, 0xfffffffeu, 0xffffffffu,
        };
        for (uint32_t now : kEdges) {
            for (uint16_t i = 0; i < s.count; ++i) {
                same_pixel(&s, g_lut, i, now, "clock edge");
            }
        }
    }

    // --- solid is time-invariant, the animated effects are not --------------
    {
        led_state_t s{};
        s.count = 10; s.red = 123; s.green = 45; s.blue = 67;
        s.brightness = 150; s.power = true; s.effect = LED_EFFECT_SOLID;
        led_reset_effect_profiles_to_defaults(&s);
        led_reset_effect_colors_to_defaults(&s);
        uint8_t r0, g0, b0, r1, g1, b1;
        render_effect_pixel(&s, g_lut, 3, 0, &r0, &g0, &b0);
        render_effect_pixel(&s, g_lut, 3, 999983, &r1, &g1, &b1);
        check(r0 == r1 && g0 == g1 && b0 == b1, "solid does not vary with time");

        for (int effect = 1; effect < LED_EFFECT_COUNT; ++effect) {
            s.effect = (uint8_t) effect;
            bool varies = false;
            uint8_t pr, pg, pb;
            render_effect_pixel(&s, g_lut, 3, 0, &pr, &pg, &pb);
            for (uint32_t now = 50; now <= 6000 && !varies; now += 50) {
                uint8_t cr, cg, cb;
                render_effect_pixel(&s, g_lut, 3, now, &cr, &cg, &cb);
                if (cr != pr || cg != pg || cb != pb) {
                    varies = true;
                }
            }
            check(varies, "an animated effect actually changes over time");
        }
    }

    std::printf("render_model: %ld checks, %ld failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
