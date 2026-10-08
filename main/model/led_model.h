// The LED vocabulary: effect identities, their parameter catalogue, the state
// record, and the single clamp path that keeps all of it in range.
//
// Pure -- no ESP-IDF, no sdkconfig, no globals (see tools/check_pure.sh). That
// constraint is what forced the one real design change in this module: the
// pixel-count ceiling used to be read straight from CONFIG_APP_LED_MAX_PIXELS
// inside the clamp, which a host-compiled translation unit cannot see. It is
// now an explicit parameter, which is also what makes the boundary cases
// testable instead of fixed at whatever the build happened to configure.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Parameter slots reserved per effect. Every effect carries the same array so
// a profile can be stored and loaded without knowing which effect it belongs
// to; slots past an effect's param_count are held at zero.
#ifdef __cplusplus
constexpr size_t kEffectParamSlotCount = 5;
#else
#define kEffectParamSlotCount ((size_t) 5)
#endif

typedef enum {
    LED_EFFECT_SOLID = 0,
    LED_EFFECT_GLOW,
    LED_EFFECT_RAINBOW,
    LED_EFFECT_CHASE,
    LED_EFFECT_SPARKLE,
    LED_EFFECT_WAVE,
    LED_EFFECT_FIRE,
    LED_EFFECT_AURORA,
    LED_EFFECT_COUNT,
} led_effect_t;

typedef struct {
    const char *label;
    uint8_t min_value;
    uint8_t max_value;
    uint8_t default_value;
} effect_param_spec_t;

typedef struct {
    uint8_t param_count;
    effect_param_spec_t params[kEffectParamSlotCount];
} effect_spec_t;

typedef struct {
    uint8_t values[kEffectParamSlotCount];
} effect_params_t;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} effect_color_t;

typedef struct {
    uint16_t count;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t brightness;
    bool power;
    uint8_t effect;
    effect_params_t effect_profiles[LED_EFFECT_COUNT];
    effect_color_t effect_colors[LED_EFFECT_COUNT];
} led_state_t;

// Boot defaults. Power is deliberately false: the device always boots dark so
// the strip never lights during power-up, and apply_startup_power_policy() in
// the firmware re-asserts it after persisted state is loaded. That ownership
// lives here rather than being implied by a macro in app_main.cpp.
#ifdef __cplusplus
constexpr uint16_t kLedDefaultCount = 8;
constexpr uint8_t kLedDefaultRed = 255;
constexpr uint8_t kLedDefaultGreen = 96;
constexpr uint8_t kLedDefaultBlue = 32;
constexpr uint8_t kLedDefaultBrightness = 96;
constexpr bool kLedDefaultPower = false;
#else
#define kLedDefaultCount ((uint16_t) 8)
#define kLedDefaultRed ((uint8_t) 255)
#define kLedDefaultGreen ((uint8_t) 96)
#define kLedDefaultBlue ((uint8_t) 32)
#define kLedDefaultBrightness ((uint8_t) 96)
#define kLedDefaultPower false
#endif

uint8_t led_clamp_u8(int value);
uint16_t led_clamp_u16(int value, int min_value, int max_value);

// Any out-of-range effect index resolves to a valid one, so callers never have
// to validate before indexing the catalogue.
uint8_t led_effect_from_index(int value);

// Never NULL -- the index is clamped first.
const effect_spec_t *led_effect_spec(uint8_t effect);

// Wire names. Unknown input in either direction resolves to solid, which is
// the external contract: an unrecognised effect name does not fail a request.
const char *led_effect_to_name(uint8_t effect);
uint8_t led_effect_from_name(const char *effect_name);

// Clamps one value into one declared parameter range.
//
// Exposed separately because it is otherwise untestable: every parameter in the
// current catalogue declares max_value 255, so clamping to the declared maximum
// and clamping to 255 cannot be told apart by any input. Mutation testing found
// exactly that hole. Taking the spec as an argument lets the tests feed
// synthetic ranges and pin the behaviour before a future effect declares a
// tighter maximum and discovers the clamp was never enforcing it.
uint8_t led_clamp_param(const effect_param_spec_t *spec, uint8_t value);

// Holds one profile inside its effect's declared range, zeroing unused slots.
void led_clamp_effect_profile(uint8_t effect, effect_params_t *profile);

// The single clamp path for a whole state record: pixel count into
// [1, max_pixels], effect index into range, and every profile clamped.
// max_pixels is a parameter because the firmware's ceiling comes from
// sdkconfig, which a pure translation unit cannot read.
void led_clamp_state(led_state_t *state, uint16_t max_pixels);

void led_reset_effect_profiles_to_defaults(led_state_t *state);
void led_reset_effect_colors_to_defaults(led_state_t *state);

#ifdef __cplusplus
}
#endif
