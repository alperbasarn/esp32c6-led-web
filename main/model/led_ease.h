// Temporal easing of the displayed brightness and colour: the advance rule that
// turns a step change in target into a smooth fade.
//
// Pure -- no ESP-IDF, no FreeRTOS, no globals (see tools/check_pure.sh).
//
// The two helpers here were already pure; the rule built out of them was not.
// It lived inline in effect_task(), interleaved with xTaskGetTickCount(), four
// function-static doubles and the task's wait decision, so the parts worth
// testing -- the frame-delta cap, the snap-on-settle threshold, and the
// requirement that ALL four channels settle together -- could only be exercised
// by running the task on hardware and watching the strip.
//
// These are display-only values. The target state remains what Matter reads and
// what the wake decision is made against; nothing here feeds back into it.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
// Time constant for the exponential easing. 220 ms sits inside the 180-320 ms
// band: 63% of a step in 220 ms, 95% in about 660 ms.
constexpr double kLedEaseTauMs = 220.0;
// Snap threshold: half an 8-bit code. Once every eased value is within half a
// code of its target the rendered frame is already identical, so snapping
// exactly onto the target lets the render task stop spinning.
constexpr double kLedEaseEpsilon = 0.5;
// Frame-delta bounds.
//
// The upper bound is load-bearing, not hygiene. When a fade settles the task
// blocks on portMAX_DELAY, so the next measured delta is the whole idle gap --
// possibly minutes. Feeding that to the easing would carry the first frame of a
// fresh fade almost all the way to the target, producing exactly the hard step
// the easing exists to remove. 60 ms passes normal ~40 ms frame jitter through
// untouched.
constexpr double kLedEaseMinDtMs = 1.0;
constexpr double kLedEaseMaxDtMs = 60.0;
#else
#define kLedEaseTauMs 220.0
#define kLedEaseEpsilon 0.5
#define kLedEaseMinDtMs 1.0
#define kLedEaseMaxDtMs 60.0
#endif

// The eased channels. Doubles, held on the 0..255 scale rather than normalised,
// so a value converts to a byte without rescaling.
typedef struct {
    double brightness;
    double red;
    double green;
    double blue;
} led_ease_rgb_t;

// Exponential smoothing factor for a measured frame delta: 1 - exp(-dt/tau).
// A non-positive tau means "no easing": jump straight to the target.
double led_ease_alpha(double dt_ms, double tau_ms);

// One step toward a target: cur + (tgt - cur) * alpha. Monotonic, and never
// overshoots for alpha in [0,1].
double led_ease_step(double cur, double tgt, double alpha);

// Holds a measured frame delta inside the usable band. See kLedEaseMaxDtMs for
// why the upper bound matters.
double led_ease_clamp_dt(double dt_ms);

// Advances every channel one frame toward target and reports whether the fade
// has settled.
//
// Order matters and is preserved from the original: all four channels step
// first, THEN the settle test runs against the stepped values, and only then
// are they snapped exactly onto the target. Settling is all-or-nothing -- a
// fade is still in flight while any single channel is outside the threshold,
// which is what stops a colour change from being declared finished because
// brightness happened to arrive first.
//
// dt_ms is the raw measured delta; the cap is applied here. Returns true when
// settled, which is the caller's signal that it may block indefinitely instead
// of scheduling another frame.
bool led_ease_advance(led_ease_rgb_t *current, const led_ease_rgb_t *target,
                      double dt_ms, double tau_ms);

// Power as the render path derives it: from the DISPLAYED brightness, not the
// target. A power-off fade therefore keeps rendering frames until brightness
// truly reaches zero, where the render kernel's brightness==0 guard yields
// black -- the settled off state. Deriving it from the target instead would cut
// the strip to black on the first frame and discard the fade.
bool led_ease_power_from_brightness(uint8_t displayed_brightness);

#ifdef __cplusplus
}
#endif
