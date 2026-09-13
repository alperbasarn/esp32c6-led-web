// Pure protocol layer for the `homio/led/...` MQTT contract (docs/mqtt-contract.md).
//
// Everything declared here is deterministic and free of ESP-IDF/FreeRTOS
// dependencies: topic building, JSON encode/decode, the pairing state machine
// and the state-publish coalescer. That keeps the wire contract testable on a
// host compiler (test/host/test_mqtt_proto.cpp) and leaves mqtt_link.cpp with
// only the transport, NVS and threading glue.
//
// All buffers are caller-owned and bounded; nothing in this file allocates
// except cJSON parsing, which the caller performs and owns.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_PROTO_TOPIC_PREFIX    "homio/led/"
#define MQTT_PROTO_MODEL           "esp32c6-led-web"

// "led-a1b2c3"/"qnob-a1b2c3" plus slack; the contract caps ids well below this.
#define MQTT_PROTO_ID_MAX          24
#define MQTT_PROTO_NAME_MAX        25   // 24 user-facing bytes + NUL
#define MQTT_PROTO_TOPIC_MAX       64
#define MQTT_PROTO_COLOR_MAX       8    // "#RRGGBB" + NUL
#define MQTT_PROTO_EFFECT_MAX      16
#define MQTT_PROTO_PARAM_COUNT     5
#define MQTT_PROTO_MAX_CONTROLLERS 4
#define MQTT_PROTO_PAYLOAD_MAX     1024 // inbound cap; larger payloads are dropped
#define MQTT_PROTO_STATE_JSON_MAX  2048 // outbound state budget (actual doc ~250 B)
#define MQTT_PROTO_INFO_JSON_MAX   512
#define MQTT_PROTO_CTRL_JSON_MAX   512
#define MQTT_PROTO_PAIR_WINDOW_MS  60000
#define MQTT_PROTO_STATE_GAP_MS    100  // >= 100 ms between state publishes (<= 10/s)
#define MQTT_PROTO_PAIR_CODE_MIN   1
#define MQTT_PROTO_PAIR_CODE_MAX   6

// The full LED tuple the contract puts on `state`. Mirrors the fields and
// ranges of GET /api/state; `count` is read-only on the wire.
typedef struct {
    bool     power;
    uint8_t  brightness;                            // 0-255
    char     color[MQTT_PROTO_COLOR_MAX];           // "#RRGGBB"
    char     effect[MQTT_PROTO_EFFECT_MAX];         // effect name
    uint8_t  effect_params[MQTT_PROTO_PARAM_COUNT]; // active effect's profile
    char     effect_color[MQTT_PROTO_COLOR_MAX];    // active effect's color
    uint16_t count;
} led_tuple_t;

typedef struct {
    char id[MQTT_PROTO_ID_MAX];
    char name[MQTT_PROTO_NAME_MAX];
} mqtt_proto_controller_t;

typedef struct {
    mqtt_proto_controller_t items[MQTT_PROTO_MAX_CONTROLLERS];
    uint8_t                 count;
} mqtt_proto_controllers_t;

typedef enum {
    MQTT_PROTO_TOPIC_UNKNOWN = 0,
    MQTT_PROTO_TOPIC_SET,
    MQTT_PROTO_TOPIC_PAIR,
    MQTT_PROTO_TOPIC_UNPAIR,
} mqtt_proto_topic_t;

// ---- identity and topics ---------------------------------------------------

// "led-" + the last three MAC bytes, lower-case hex. Stable across reboots.
bool mqtt_proto_device_id_from_mac(const uint8_t mac[6], char *out, size_t out_len);

// "homio/led/<id>/<leaf>". Returns false if the buffer is too small.
bool mqtt_proto_build_topic(char *out, size_t out_len, const char *id, const char *leaf);

// Classify an inbound topic against this device's id. `topic` need not be
// NUL-terminated; pass its length.
mqtt_proto_topic_t mqtt_proto_classify_topic(const char *topic, size_t topic_len, const char *id);

// ---- untrusted-input sanitizers -------------------------------------------

// Controller/device ids: 1..MQTT_PROTO_ID_MAX-1 chars of [a-z0-9._-], upper
// case folded down. Anything else is rejected outright.
bool mqtt_proto_sanitize_id(const char *in, char *out, size_t out_len);

// User-facing labels: printable ASCII only, truncated to 24 bytes. Characters
// outside 0x20..0x7E are dropped rather than rejecting the whole message.
bool mqtt_proto_sanitize_name(const char *in, char *out, size_t out_len);

// ---- encoders (bounded, no allocation) -------------------------------------

// Returns the number of bytes written, or -1 when the document would not fit.
int mqtt_proto_encode_state(char *out, size_t out_len, const led_tuple_t *tuple, const char *src, uint32_t seq,
                            bool rejected);
int mqtt_proto_encode_info(char *out, size_t out_len, const char *id, const char *fw, const char *name,
                           uint16_t max_leds, const char *ip);
int mqtt_proto_encode_controllers(char *out, size_t out_len, const mqtt_proto_controllers_t *list);

// ---- decoders (cJSON, all fields optional and range-checked) ---------------

typedef struct {
    char     from[MQTT_PROTO_ID_MAX];
    bool     has_from;
    uint32_t seq;
    bool     has_seq;
} mqtt_proto_envelope_t;

// Reads "from"/"seq" out of a `set` document. Never fails on missing fields;
// returns false only for a malformed (non-object) document.
bool mqtt_proto_parse_envelope(const cJSON *root, mqtt_proto_envelope_t *out);

// True when the document carries at least one settable LED field. Used to
// ignore empty/no-op `set` messages without touching the LED state.
bool mqtt_proto_set_has_fields(const cJSON *root);

typedef struct {
    char    controller[MQTT_PROTO_ID_MAX];
    char    name[MQTT_PROTO_NAME_MAX];
    bool    has_code;
    uint8_t code; // 1..6 when has_code
} mqtt_proto_pair_msg_t;

bool mqtt_proto_parse_pair(const cJSON *root, mqtt_proto_pair_msg_t *out);
bool mqtt_proto_parse_unpair(const cJSON *root, char *ctl_out, size_t out_len);

// ---- paired-controller list ------------------------------------------------

bool mqtt_proto_controller_find(const mqtt_proto_controllers_t *list, const char *id, size_t *index_out);
// Adds or refreshes the label. Returns false when the list is full.
bool mqtt_proto_controller_add(mqtt_proto_controllers_t *list, const char *id, const char *name);
bool mqtt_proto_controller_remove(mqtt_proto_controllers_t *list, const char *id);

// ---- pairing state machine -------------------------------------------------

typedef enum {
    MQTT_PAIR_IDLE = 0,
    MQTT_PAIR_WAITING_CODE,
} mqtt_pair_state_t;

typedef struct {
    mqtt_pair_state_t state;
    char              pending_id[MQTT_PROTO_ID_MAX];
    char              pending_name[MQTT_PROTO_NAME_MAX];
    uint8_t           code;        // the blink code the strip is showing
    int64_t           deadline_ms; // window end, monotonic ms
} mqtt_pair_ctx_t;

typedef enum {
    MQTT_PAIR_ACTION_NONE = 0,
    MQTT_PAIR_ACTION_BLINK_CODE, // window opened: blink `code` until confirmed
    MQTT_PAIR_ACTION_PAIRED,     // stored: publish controllers + flash green
    MQTT_PAIR_ACTION_REJECTED,   // wrong code, timeout or list full: flash red
} mqtt_pair_action_t;

typedef struct {
    mqtt_pair_action_t action;
    uint8_t            code; // valid for MQTT_PAIR_ACTION_BLINK_CODE
} mqtt_pair_outcome_t;

// `pair` without a code. `proposed_code` is the caller's random 1..6 draw (kept
// out of this module so the machine stays deterministic under test). A
// factory-fresh device — an empty controller list — pairs immediately.
mqtt_pair_outcome_t mqtt_pair_on_request(mqtt_pair_ctx_t *ctx, mqtt_proto_controllers_t *list, const char *ctl,
                                         const char *name, int64_t now_ms, uint8_t proposed_code);

// `pair` carrying a code; confirms an open window.
mqtt_pair_outcome_t mqtt_pair_on_code(mqtt_pair_ctx_t *ctx, mqtt_proto_controllers_t *list, const char *ctl,
                                      uint8_t code, int64_t now_ms);

// Expires an open window. Call it periodically.
mqtt_pair_outcome_t mqtt_pair_tick(mqtt_pair_ctx_t *ctx, int64_t now_ms);

// ---- state-publish coalescer ----------------------------------------------
//
// Keeps `state` publishes at or under 10/s while never losing a rejection: a
// pending rejection has its own slot, so a legitimate publish inside the same
// window cannot swallow it.
typedef struct {
    int64_t  last_publish_ms;
    bool     pending;
    char     src[MQTT_PROTO_ID_MAX];
    uint32_t seq;
    bool     rejected_pending;
    char     rejected_src[MQTT_PROTO_ID_MAX];
    uint32_t rejected_seq;
} mqtt_coalescer_t;

void mqtt_coalescer_mark(mqtt_coalescer_t *c, const char *src, uint32_t seq, bool rejected);
// Milliseconds until the next publish is due: 0 = now, >0 = wait, -1 = idle.
int32_t mqtt_coalescer_due_in(const mqtt_coalescer_t *c, int64_t now_ms);
// Pops the next document to publish. Returns false when nothing is due yet.
bool mqtt_coalescer_take(mqtt_coalescer_t *c, int64_t now_ms, char *src_out, size_t src_len, uint32_t *seq_out,
                         bool *rejected_out);

#ifdef __cplusplus
}
#endif
