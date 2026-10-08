// Host tests for main/model/led_model.cpp.
//
// `reference` below is a verbatim copy of the code as it stood in app_main.cpp
// before the move, with one unavoidable difference: the original read the pixel
// ceiling from CONFIG_APP_LED_MAX_PIXELS, a compile-time macro a host build
// cannot see, so the reference takes it as a parameter exactly as the extracted
// version does. Every other line is byte-for-byte the original, so a divergence
// here means the extraction changed behaviour rather than that the test and the
// implementation were written to agree with each other.
//
// The effect catalogue is part of the external contract: the web UI renders the
// labels verbatim and the min/max/default triples bound what a client may send.
// Those are checked entry by entry rather than trusted.

#include "../../main/model/led_model.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

static int g_checks = 0;
static int g_failures = 0;

static void fail(const char *what)
{
    ++g_failures;
    std::printf("FAIL  %s\n", what);
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

static inline uint8_t clamp_u8(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

static inline uint16_t clamp_u16(int value, int min_value, int max_value)
{
    return static_cast<uint16_t>(std::clamp(value, min_value, max_value));
}

static inline uint8_t effect_from_index(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, static_cast<int>(LED_EFFECT_COUNT) - 1));
}

static const effect_spec_t &get_effect_spec(uint8_t effect)
{
    return kEffectSpecs[effect_from_index(effect)];
}

static const char *effect_to_name(uint8_t effect)
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

static uint8_t effect_from_name(const char *effect_name)
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

static void clamp_effect_profile(uint8_t effect, effect_params_t *profile)
{
    if (!profile) {
        return;
    }

    const effect_spec_t &spec = get_effect_spec(effect);
    for (size_t index = 0; index < kEffectParamSlotCount; ++index) {
        if (index < spec.param_count) {
            profile->values[index] = static_cast<uint8_t>(
                std::clamp(static_cast<int>(profile->values[index]),
                           static_cast<int>(spec.params[index].min_value),
                           static_cast<int>(spec.params[index].max_value)));
        } else {
            profile->values[index] = 0;
        }
    }
}

// Original body, with APP_LED_MAX_PIXELS lifted to a parameter.
static void clamp_state(led_state_t *state, uint16_t max_pixels)
{
    if (!state) {
        return;
    }
    state->count = clamp_u16(state->count, 1, max_pixels);
    state->effect = effect_from_index(state->effect);
    for (uint8_t effect = 0; effect < LED_EFFECT_COUNT; ++effect) {
        clamp_effect_profile(effect, &state->effect_profiles[effect]);
    }
}

static void reset_effect_profiles_to_defaults(led_state_t *state)
{
    if (!state) {
        return;
    }

    for (uint8_t effect = 0; effect < LED_EFFECT_COUNT; ++effect) {
        const effect_spec_t &spec = get_effect_spec(effect);
        for (size_t index = 0; index < kEffectParamSlotCount; ++index) {
            state->effect_profiles[effect].values[index] = index < spec.param_count ? spec.params[index].default_value : 0;
        }
    }
}

static void reset_effect_colors_to_defaults(led_state_t *state)
{
    if (!state) {
        return;
    }

    for (uint8_t effect = 0; effect < LED_EFFECT_COUNT; ++effect) {
        state->effect_colors[effect].red = 255;    // APP_LED_DEFAULT_RED
        state->effect_colors[effect].green = 96;   // APP_LED_DEFAULT_GREEN
        state->effect_colors[effect].blue = 32;    // APP_LED_DEFAULT_BLUE
    }

    state->effect_colors[LED_EFFECT_SPARKLE].red = 255;
    state->effect_colors[LED_EFFECT_SPARKLE].green = 255;
    state->effect_colors[LED_EFFECT_SPARKLE].blue = 255;
}

}  // namespace reference

// ---------------------------------------------------------------------------

static bool states_equal(const led_state_t &a, const led_state_t &b)
{
    if (a.count != b.count || a.red != b.red || a.green != b.green ||
        a.blue != b.blue || a.brightness != b.brightness || a.power != b.power ||
        a.effect != b.effect) {
        return false;
    }
    for (int e = 0; e < LED_EFFECT_COUNT; ++e) {
        for (size_t i = 0; i < kEffectParamSlotCount; ++i) {
            if (a.effect_profiles[e].values[i] != b.effect_profiles[e].values[i]) {
                return false;
            }
        }
        if (a.effect_colors[e].red != b.effect_colors[e].red ||
            a.effect_colors[e].green != b.effect_colors[e].green ||
            a.effect_colors[e].blue != b.effect_colors[e].blue) {
            return false;
        }
    }
    return true;
}

// Deterministic pseudo-random fill so a failure is reproducible.
static uint32_t g_seed = 0x9e3779b9u;
static uint32_t next_rand()
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

int main()
{
    // --- the clamps, exhaustively over a wide integer range -----------------
    for (int v = -1000; v <= 1000; ++v) {
        ++g_checks;
        if (led_clamp_u8(v) != reference::clamp_u8(v)) {
            fail("led_clamp_u8 diverged");
            break;
        }
    }
    for (int v = -300; v <= 300; ++v) {
        for (int lo = -5; lo <= 5; ++lo) {
            for (int hi = lo; hi <= lo + 10; ++hi) {
                ++g_checks;
                if (led_clamp_u16(v, lo, hi) != reference::clamp_u16(v, lo, hi)) {
                    fail("led_clamp_u16 diverged");
                    v = 301; lo = 6; break;
                }
            }
        }
    }

    // --- effect index resolution -------------------------------------------
    for (int v = -500; v <= 500; ++v) {
        ++g_checks;
        if (led_effect_from_index(v) != reference::effect_from_index(v)) {
            fail("led_effect_from_index diverged");
            break;
        }
    }
    check(led_effect_from_index(-1) == LED_EFFECT_SOLID, "negative index clamps to solid");
    check(led_effect_from_index(999) == LED_EFFECT_AURORA, "overlarge index clamps to the last effect");

    // --- the catalogue, entry by entry (it is the external contract) --------
    for (int e = 0; e < LED_EFFECT_COUNT; ++e) {
        const effect_spec_t *got = led_effect_spec((uint8_t) e);
        const effect_spec_t &want = reference::get_effect_spec((uint8_t) e);
        check(got != nullptr, "led_effect_spec never returns NULL");
        if (!got) {
            continue;
        }
        check(got->param_count == want.param_count, "param_count matches");
        for (size_t i = 0; i < kEffectParamSlotCount; ++i) {
            check(std::strcmp(got->params[i].label, want.params[i].label) == 0,
                  "param label matches");
            check(got->params[i].min_value == want.params[i].min_value, "param min matches");
            check(got->params[i].max_value == want.params[i].max_value, "param max matches");
            check(got->params[i].default_value == want.params[i].default_value,
                  "param default matches");
        }
        // A declared parameter whose max is below its min would make the clamp
        // nonsensical, and a default outside its own range would be clamped
        // away the first time it was used.
        for (size_t i = 0; i < got->param_count; ++i) {
            check(got->params[i].min_value <= got->params[i].max_value,
                  "declared param range is not inverted");
            check(got->params[i].default_value >= got->params[i].min_value &&
                      got->params[i].default_value <= got->params[i].max_value,
                  "declared default lies inside its own range");
            check(got->params[i].label[0] != '\0', "a counted param has a label");
        }
        for (size_t i = got->param_count; i < kEffectParamSlotCount; ++i) {
            check(got->params[i].label[0] == '\0', "an uncounted slot has no label");
        }
    }
    // Out-of-range indices must still yield the clamped entry, not read past.
    check(led_effect_spec(200) == led_effect_spec(LED_EFFECT_COUNT - 1),
          "out-of-range spec lookup clamps");

    // --- names, both directions, over every byte ----------------------------
    for (int e = 0; e <= 255; ++e) {
        ++g_checks;
        if (std::strcmp(led_effect_to_name((uint8_t) e),
                        reference::effect_to_name((uint8_t) e)) != 0) {
            fail("led_effect_to_name diverged");
            break;
        }
    }
    // Round-trip: every effect's own name maps back to it.
    for (int e = 0; e < LED_EFFECT_COUNT; ++e) {
        check(led_effect_from_name(led_effect_to_name((uint8_t) e)) == e,
              "name round-trips to the same effect");
    }
    // Names are distinct, or two effects would be indistinguishable on the wire.
    for (int a = 0; a < LED_EFFECT_COUNT; ++a) {
        for (int b = a + 1; b < LED_EFFECT_COUNT; ++b) {
            check(std::strcmp(led_effect_to_name((uint8_t) a),
                              led_effect_to_name((uint8_t) b)) != 0,
                  "effect names are distinct");
        }
    }
    static const char *kProbes[] = {
        "solid", "glow", "rainbow", "chase", "sparkle", "wave", "fire", "aurora",
        "", " ", "SOLID", "Glow", "fir", "fires", "aurora ", " aurora",
        "rainbow\n", "unknown", "0", "1", "sparkle2", "x",
    };
    for (const char *probe : kProbes) {
        ++g_checks;
        if (led_effect_from_name(probe) != reference::effect_from_name(probe)) {
            std::printf("FAIL  led_effect_from_name(\"%s\") diverged\n", probe);
            ++g_failures;
        }
    }
    check(led_effect_from_name(nullptr) == reference::effect_from_name(nullptr),
          "NULL name matches the reference");
    check(led_effect_from_name(nullptr) == LED_EFFECT_SOLID, "NULL name is solid");
    check(led_effect_from_name("Glow") == LED_EFFECT_SOLID,
          "matching is case-SENSITIVE, as it always was");

    // --- profile clamping, every effect x every slot x boundary values ------
    for (int e = 0; e < (int) LED_EFFECT_COUNT + 3; ++e) {
        for (int v = 0; v <= 255; ++v) {
            effect_params_t mine{};
            effect_params_t theirs{};
            for (size_t i = 0; i < kEffectParamSlotCount; ++i) {
                mine.values[i] = (uint8_t) v;
                theirs.values[i] = (uint8_t) v;
            }
            led_clamp_effect_profile((uint8_t) e, &mine);
            reference::clamp_effect_profile((uint8_t) e, &theirs);
            ++g_checks;
            if (std::memcmp(&mine, &theirs, sizeof mine) != 0) {
                fail("led_clamp_effect_profile diverged");
                e = 99; break;
            }
        }
    }
    // A param declared min=1 must never be clamped to 0: a zero speed would
    // freeze the effect rather than slow it.
    for (int e = 0; e < LED_EFFECT_COUNT; ++e) {
        const effect_spec_t *spec = led_effect_spec((uint8_t) e);
        effect_params_t p{};
        led_clamp_effect_profile((uint8_t) e, &p);  // all zeros in
        for (size_t i = 0; i < spec->param_count; ++i) {
            check(p.values[i] >= spec->params[i].min_value,
                  "zeroed profile clamps up to the declared minimum");
        }
    }
    ++g_checks;
    led_clamp_effect_profile(0, nullptr);  // NULL profile must be a no-op, not a crash

    // --- led_clamp_param against SYNTHETIC ranges ---------------------------
    // The real catalogue declares max_value 255 everywhere, so no catalogue
    // input can distinguish "clamp to the declared max" from "clamp to 255".
    // Mutation testing proved that blind spot by replacing the upper bound with
    // 255 and watching every test still pass. These ranges are the fix.
    for (int lo = 0; lo <= 255; lo += 17) {
        for (int hi = 0; hi <= 255; hi += 17) {
            for (int v = 0; v <= 255; v += 5) {
                effect_param_spec_t spec{"synthetic", (uint8_t) lo, (uint8_t) hi, (uint8_t) lo};
                uint8_t got = led_clamp_param(&spec, (uint8_t) v);
                ++g_checks;
                if (hi < lo) {
                    // Inverted declaration: min wins rather than invoking
                    // std::clamp's undefined behaviour.
                    if (got != (uint8_t) lo) {
                        fail("inverted param range must yield min");
                        lo = 256; hi = 256; break;
                    }
                } else {
                    int want = v < lo ? lo : (v > hi ? hi : v);
                    if (got != (uint8_t) want) {
                        std::printf("FAIL  led_clamp_param(min=%d,max=%d,%d)=%d want %d\n",
                                    lo, hi, v, got, want);
                        ++g_failures;
                        lo = 256; hi = 256; break;
                    }
                }
            }
        }
    }
    // A tight range really does bite at both ends.
    {
        effect_param_spec_t tight{"tight", 10, 20, 15};
        check(led_clamp_param(&tight, 0) == 10, "below a tight min clamps up");
        check(led_clamp_param(&tight, 255) == 20, "above a tight max clamps down");
        check(led_clamp_param(&tight, 15) == 15, "inside a tight range is untouched");
        check(led_clamp_param(&tight, 10) == 10, "at the min is untouched");
        check(led_clamp_param(&tight, 20) == 20, "at the max is untouched");
    }
    check(led_clamp_param(nullptr, 42) == 42, "NULL spec returns the value unchanged");

    // --- whole-state clamping, including the lifted max_pixels parameter ----
    for (int trial = 0; trial < 4000; ++trial) {
        led_state_t mine{};
        mine.count = (uint16_t) (next_rand() % 2000);
        mine.red = (uint8_t) next_rand();
        mine.green = (uint8_t) next_rand();
        mine.blue = (uint8_t) next_rand();
        mine.brightness = (uint8_t) next_rand();
        mine.power = (next_rand() & 1) != 0;
        mine.effect = (uint8_t) next_rand();
        for (int e = 0; e < LED_EFFECT_COUNT; ++e) {
            for (size_t i = 0; i < kEffectParamSlotCount; ++i) {
                mine.effect_profiles[e].values[i] = (uint8_t) next_rand();
            }
            mine.effect_colors[e].red = (uint8_t) next_rand();
            mine.effect_colors[e].green = (uint8_t) next_rand();
            mine.effect_colors[e].blue = (uint8_t) next_rand();
        }
        led_state_t theirs = mine;

        static const uint16_t kCeilings[] = {1, 2, 8, 64, 255, 256, 512, 1024};
        uint16_t ceiling = kCeilings[trial % 8];
        led_clamp_state(&mine, ceiling);
        reference::clamp_state(&theirs, ceiling);
        ++g_checks;
        if (!states_equal(mine, theirs)) {
            fail("led_clamp_state diverged");
            break;
        }
        // Invariants the firmware relies on, independent of the reference.
        check(mine.count >= 1, "clamped count is at least one pixel");
        check(mine.count <= ceiling, "clamped count respects the ceiling");
        check(mine.effect < LED_EFFECT_COUNT, "clamped effect is a valid index");
        // Colours are NOT clamped by this path -- they are already uint8_t and
        // the original never touched them. Pinned so a future "tidy-up" that
        // starts rewriting colours here shows up as a failure.
        for (int e = 0; e < LED_EFFECT_COUNT; ++e) {
            check(mine.effect_colors[e].red == theirs.effect_colors[e].red,
                  "clamp_state leaves effect colours alone");
        }
    }
    // Idempotence: clamping a clamped state changes nothing.
    {
        led_state_t s{};
        s.count = 9999;
        s.effect = 200;
        for (int e = 0; e < LED_EFFECT_COUNT; ++e) {
            for (size_t i = 0; i < kEffectParamSlotCount; ++i) {
                s.effect_profiles[e].values[i] = (uint8_t) (i * 57 + e * 13);
            }
        }
        led_clamp_state(&s, 64);
        led_state_t once = s;
        led_clamp_state(&s, 64);
        check(states_equal(once, s), "led_clamp_state is idempotent");
    }
    ++g_checks;
    led_clamp_state(nullptr, 64);  // must not crash

    // --- the two reset paths ------------------------------------------------
    {
        led_state_t mine{};
        led_state_t theirs{};
        led_reset_effect_profiles_to_defaults(&mine);
        reference::reset_effect_profiles_to_defaults(&theirs);
        check(states_equal(mine, theirs), "profile defaults match the reference");

        // Defaults must survive their own clamp, or the device would boot with
        // values it immediately rewrites.
        led_state_t clamped = mine;
        clamped.count = 8;
        led_clamp_state(&clamped, 512);
        for (int e = 0; e < LED_EFFECT_COUNT; ++e) {
            for (size_t i = 0; i < kEffectParamSlotCount; ++i) {
                check(clamped.effect_profiles[e].values[i] ==
                          mine.effect_profiles[e].values[i],
                      "default profile is already in range");
            }
        }
    }
    {
        led_state_t mine{};
        led_state_t theirs{};
        led_reset_effect_colors_to_defaults(&mine);
        reference::reset_effect_colors_to_defaults(&theirs);
        check(states_equal(mine, theirs), "colour defaults match the reference");

        check(mine.effect_colors[LED_EFFECT_SPARKLE].red == 255 &&
                  mine.effect_colors[LED_EFFECT_SPARKLE].green == 255 &&
                  mine.effect_colors[LED_EFFECT_SPARKLE].blue == 255,
              "sparkle defaults to white");
        for (int e = 0; e < LED_EFFECT_COUNT; ++e) {
            if (e == LED_EFFECT_SPARKLE) {
                continue;
            }
            check(mine.effect_colors[e].red == kLedDefaultRed &&
                      mine.effect_colors[e].green == kLedDefaultGreen &&
                      mine.effect_colors[e].blue == kLedDefaultBlue,
                  "every other effect defaults to the warm default");
        }
    }
    ++g_checks;
    led_reset_effect_profiles_to_defaults(nullptr);
    led_reset_effect_colors_to_defaults(nullptr);

    // --- the boot defaults the firmware seeds its state with ----------------
    check(kLedDefaultPower == false,
          "the device boots dark; apply_startup_power_policy relies on it");
    check(kLedDefaultCount >= 1, "default pixel count survives the clamp floor");
    check(kLedDefaultBrightness > 0, "a zero default brightness would boot invisible");

    std::printf("led_model: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
