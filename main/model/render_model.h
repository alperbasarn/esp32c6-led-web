// The render kernel: one pixel's colour, as a pure function of state, index
// and time.
//
// Pure -- no ESP-IDF, no FreeRTOS, no globals (see tools/check_pure.sh). Before
// this module existed the kernel was unreachable by any test: it was called
// only from apply_led_state(), which holds s_led_mutex, talks to the led_strip
// driver, and reads the clock via xTaskGetTickCount(). Every frame of Fire and
// Aurora went out unverified.
//
// Two reads of firmware state had to become parameters for that to change:
//
//   now_ms     was already a parameter, but the caller derived it from the tick
//              count, so a test could never pin a frame. It still comes from
//              the tick count; the point is that nothing here reads a clock.
//
//   gamma_lut  was the file-scope s_gamma_lut. It is now supplied by the
//              caller, which is what lets a test drive a known ramp -- or an
//              identity ramp -- and see the composed linear light intent
//              directly instead of through a gamma curve.
//
// What deliberately did NOT move: the frame loop, the mutex, the strip driver
// and the shrink-clear rule all stay in apply_led_state(). The WBS sketched a
// frame-level "render into a caller-provided buffer" entry point, but a frame
// buffer is 3 bytes x up to CONFIG_APP_LED_MAX_PIXELS, which would land on the
// effect task's stack for no behavioural gain. The per-pixel out-parameters
// already are caller-provided storage.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "led_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// Gamma exponent for the output LUT. 2.2 is the perceptual sweet spot at 8-bit
// output: it evens out the ramp (smooth premium fades) without crushing so much
// of the low end that dim settings round to black.
#ifdef __cplusplus
constexpr double kRenderGammaExponent = 2.2;
#else
#define kRenderGammaExponent 2.2
#endif

#define RENDER_GAMMA_LUT_SIZE 256

// Fills lut[0..255] with a gamma curve: lut[i] = round(255 * (i/255)^gamma),
// with a "video" floor -- every non-zero input maps to at least code 1, so the
// lowest "on" brightness or colour never collapses to fully off, which on a
// dimmable light reads as a black cliff. lut[0] == 0 and lut[255] == 255 still
// hold, and render_effect_pixel()'s black early-return relies on the first.
void render_gamma_lut_build(uint8_t *lut, double gamma);

// 0..255 as 0.0..1.0.
double render_normalized_u8(uint8_t value);

// Rounds and clamps a channel value into a byte.
uint8_t render_float_to_u8(double value);

// One channel of the 8-bit colour wheel. channel: 0=red, 1=green, 2=blue.
uint8_t render_wheel_channel(uint8_t wheel_pos, uint8_t channel);

// Integer hash used to make per-pixel randomness deterministic -- the same
// pixel must look the same on every device and across reboots, so this is a
// hash of the index, never a PRNG with hidden state.
uint32_t render_pseudo_random_u32(uint32_t value);

// Smoothstep-interpolated 1-D value noise. Returns 0..1, never NaN (frac is
// always in [0,1)), and is deterministic for negative x via floor plus integer
// wrap. Two hash samples per call, no tables and no library calls. Drives
// Fire's scrolling heat field.
double render_value_noise_1d(double x, uint32_t seed);

// Maps a 0..255 speed parameter onto a cycle length, interpolating between a
// slow and a fast bound. Never returns zero -- the callers divide by it.
//
// REQUIRES slow_ms > fast_ms. Not merely a convention: the implementation
// computes `slow_ms - fast_ms` in unsigned arithmetic, so a caller passing them
// the other way round underflows to a huge value, drives the interpolation
// hugely negative, and converts a negative double to uint32_t -- undefined
// behaviour. Every current caller passes a slow bound above the fast one.
//
// Recorded rather than fixed here: hardening it would make this module diverge
// from the frozen pre-move reference the extraction is proven against. It
// belongs in the deferred backlog (WBS 7.8), not inside a slice whose whole
// claim is that behaviour did not change.
//
// Mutation testing also showed the implementation's own max(fast_ms, ...) to be
// unreachable for any uint8_t value: speed tops out at exactly 1.0, where the
// interpolation lands on fast_ms precisely. It is defensive only.
uint32_t render_effect_cycle_ms(uint8_t value, uint32_t slow_ms, uint32_t fast_ms);

// One pixel of the current frame.
//
// Writes black and returns early when there is nothing to show: a NULL state or
// output, power off, zero brightness, or an index at or past state->count.
//
// gamma_lut must hold RENDER_GAMMA_LUT_SIZE entries. NULL means no gamma, i.e.
// identity -- chosen over emitting black so a wiring mistake shows up as a
// too-bright strip rather than a dead one.
void render_effect_pixel(const led_state_t *state, const uint8_t *gamma_lut,
                         uint16_t index, uint32_t now_ms,
                         uint8_t *out_red, uint8_t *out_green, uint8_t *out_blue);

#ifdef __cplusplus
}
#endif
