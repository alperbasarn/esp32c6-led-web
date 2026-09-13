// The LED core's public entry points, implemented in app_main.cpp.
//
// app_main.cpp owns the canonical LED state (s_state_mutex), the strip
// (s_led_mutex) and the effect task. Everything outside it — today the MQTT
// link — goes through this header instead of touching those globals, so there
// is exactly one validation/clamping path for control changes.
//
// Locking rule for callers: never hold your own module lock across a
// led_control_* call. These functions take s_state_mutex internally.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "mqtt_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_CONTROL_OK = 0,
    LED_CONTROL_ERR_FIELDS,       // missing/mistyped fields (full-tuple mode)
    LED_CONTROL_ERR_COLOR,        // "color" is not #RRGGBB
    LED_CONTROL_ERR_EFFECT_COLOR, // "effect_color" is not #RRGGBB
    LED_CONTROL_ERR_EMPTY,        // nothing settable in the document
    LED_CONTROL_ERR_SAVE,         // applied, but the NVS write failed
} led_control_result_t;

// Applies a control document — the body of POST /api/control, or the payload of
// an MQTT `set`. `require_full_tuple` keeps the web API's "Missing fields"
// contract; the MQTT path passes false so any subset is accepted. Clamping,
// Matter tracker refresh, the effect-task notify and the Matter sync are
// identical for both callers.
//
// `persist_now` writes NVS inside the call (web); false marks the state dirty
// and lets led_control_persist_tick() flush it ~2 s later (MQTT sets, which can
// arrive at 10/s during a knob drag).
led_control_result_t led_control_apply_json(const cJSON *root, bool require_full_tuple, bool persist_now);

// Flushes a debounced persist when it is due. Called once per second from the
// schedule task; never call it from the effect task.
void led_control_persist_tick(void);

// Snapshot of the wire tuple (power, brightness, color, effect, params, effect
// color, count) taken under s_state_mutex.
void led_control_get_tuple(led_tuple_t *out);

// Device identity for the MQTT `info` document.
const char *led_control_firmware_version(void);
void        led_control_get_device_name(char *out, size_t out_len); // SoftAP SSID
void        led_control_get_sta_ip(char *out, size_t out_len);      // "" when offline
uint16_t    led_control_max_leds(void);

// ---- pairing feedback on the strip ----------------------------------------
//
// The effect task renders these; the caller only sets a flag, so an indicator
// never blocks the caller and never touches the LED strip on its thread.
void led_control_indicator_blink(uint8_t code); // repeat `code` blinks (1-6) until cleared
void led_control_indicator_flash(bool success); // one green (true) or red (false) flash
void led_control_indicator_clear(void);

#ifdef __cplusplus
}
#endif
