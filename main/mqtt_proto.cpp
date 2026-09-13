// Pure protocol layer for the `homio/led/...` MQTT contract. See mqtt_proto.h.
//
// Encoding is done with a bounded writer instead of cJSON printing so no
// outbound publish allocates. Decoding uses cJSON (already linked for the web
// API); every field is optional, type-checked and range-checked because all of
// it arrives from the broker.

#include "mqtt_proto.h"

#include <stdio.h>
#include <string.h>

// ---- bounded JSON writer ---------------------------------------------------

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   ok;
} json_writer_t;

static void jw_init(json_writer_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->ok = (buf != NULL && cap > 0);
    if (w->ok) {
        w->buf[0] = '\0';
    }
}

static void jw_raw(json_writer_t *w, const char *text)
{
    if (!w->ok) {
        return;
    }
    size_t text_len = strlen(text);
    if (w->len + text_len + 1 > w->cap) {
        w->ok = false;
        return;
    }
    memcpy(w->buf + w->len, text, text_len);
    w->len += text_len;
    w->buf[w->len] = '\0';
}

static void jw_char(json_writer_t *w, char c)
{
    if (!w->ok) {
        return;
    }
    if (w->len + 2 > w->cap) {
        w->ok = false;
        return;
    }
    w->buf[w->len++] = c;
    w->buf[w->len] = '\0';
}

// Escapes into a JSON string body. Control characters and non-ASCII bytes are
// dropped rather than escaped: every string we emit is either device-generated
// or already passed mqtt_proto_sanitize_name().
static void jw_escaped(json_writer_t *w, const char *text)
{
    if (!text) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *) text; *p; ++p) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            jw_char(w, '\\');
            jw_char(w, (char) c);
        } else if (c >= 0x20 && c < 0x7f) {
            jw_char(w, (char) c);
        }
        if (!w->ok) {
            return;
        }
    }
}

static void jw_key(json_writer_t *w, const char *key, bool first)
{
    if (!first) {
        jw_char(w, ',');
    }
    jw_char(w, '"');
    jw_raw(w, key);
    jw_raw(w, "\":");
}

static void jw_string_field(json_writer_t *w, const char *key, const char *value, bool first)
{
    jw_key(w, key, first);
    jw_char(w, '"');
    jw_escaped(w, value);
    jw_char(w, '"');
}

static void jw_number_field(json_writer_t *w, const char *key, long value, bool first)
{
    char number[24];
    snprintf(number, sizeof(number), "%ld", value);
    jw_key(w, key, first);
    jw_raw(w, number);
}

static void jw_bool_field(json_writer_t *w, const char *key, bool value, bool first)
{
    jw_key(w, key, first);
    jw_raw(w, value ? "true" : "false");
}

// ---- identity and topics ---------------------------------------------------

bool mqtt_proto_device_id_from_mac(const uint8_t mac[6], char *out, size_t out_len)
{
    if (!mac || !out || out_len < 11) {
        return false;
    }
    snprintf(out, out_len, "led-%02x%02x%02x", mac[3], mac[4], mac[5]);
    return true;
}

bool mqtt_proto_build_topic(char *out, size_t out_len, const char *id, const char *leaf)
{
    if (!out || !id || !leaf || out_len == 0) {
        return false;
    }
    int written = snprintf(out, out_len, MQTT_PROTO_TOPIC_PREFIX "%s/%s", id, leaf);
    return written > 0 && (size_t) written < out_len;
}

mqtt_proto_topic_t mqtt_proto_classify_topic(const char *topic, size_t topic_len, const char *id)
{
    if (!topic || !id) {
        return MQTT_PROTO_TOPIC_UNKNOWN;
    }

    static const struct {
        const char        *leaf;
        mqtt_proto_topic_t kind;
    } kLeaves[] = {
        {"set", MQTT_PROTO_TOPIC_SET},
        {"pair", MQTT_PROTO_TOPIC_PAIR},
        {"unpair", MQTT_PROTO_TOPIC_UNPAIR},
    };

    for (size_t i = 0; i < sizeof(kLeaves) / sizeof(kLeaves[0]); ++i) {
        char expected[MQTT_PROTO_TOPIC_MAX];
        if (!mqtt_proto_build_topic(expected, sizeof(expected), id, kLeaves[i].leaf)) {
            continue;
        }
        size_t expected_len = strlen(expected);
        if (expected_len == topic_len && memcmp(expected, topic, topic_len) == 0) {
            return kLeaves[i].kind;
        }
    }
    return MQTT_PROTO_TOPIC_UNKNOWN;
}

// ---- sanitizers ------------------------------------------------------------

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

bool mqtt_proto_sanitize_id(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    size_t length = strnlen(in, out_len);
    if (length == 0 || length >= out_len) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        char c = lower_ascii(in[i]);
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!allowed) {
            out[0] = '\0';
            return false;
        }
        out[i] = c;
    }
    out[length] = '\0';
    return true;
}

bool mqtt_proto_sanitize_name(const char *in, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    if (!in) {
        return false;
    }

    size_t written = 0;
    for (const unsigned char *p = (const unsigned char *) in; *p && written + 1 < out_len; ++p) {
        if (*p >= 0x20 && *p < 0x7f) {
            out[written++] = (char) *p;
        }
    }
    out[written] = '\0';
    return written > 0;
}

// ---- encoders --------------------------------------------------------------

int mqtt_proto_encode_state(char *out, size_t out_len, const led_tuple_t *tuple, const char *src, uint32_t seq,
                            bool rejected)
{
    if (!tuple) {
        return -1;
    }

    json_writer_t w;
    jw_init(&w, out, out_len);
    jw_char(&w, '{');
    jw_bool_field(&w, "power", tuple->power, true);
    jw_number_field(&w, "brightness", (long) tuple->brightness, false);
    jw_string_field(&w, "color", tuple->color, false);
    jw_string_field(&w, "effect", tuple->effect, false);

    jw_key(&w, "effect_params", false);
    jw_char(&w, '[');
    for (size_t i = 0; i < MQTT_PROTO_PARAM_COUNT; ++i) {
        if (i > 0) {
            jw_char(&w, ',');
        }
        char number[8];
        snprintf(number, sizeof(number), "%u", (unsigned) tuple->effect_params[i]);
        jw_raw(&w, number);
    }
    jw_char(&w, ']');

    jw_string_field(&w, "effect_color", tuple->effect_color, false);
    jw_number_field(&w, "count", (long) tuple->count, false);
    jw_string_field(&w, "src", src ? src : "", false);
    jw_number_field(&w, "seq", (long) seq, false);
    if (rejected) {
        jw_bool_field(&w, "rejected", true, false);
    }
    jw_char(&w, '}');

    return w.ok ? (int) w.len : -1;
}

int mqtt_proto_encode_info(char *out, size_t out_len, const char *id, const char *fw, const char *name,
                           uint16_t max_leds, const char *ip)
{
    json_writer_t w;
    jw_init(&w, out, out_len);
    jw_char(&w, '{');
    jw_string_field(&w, "id", id ? id : "", true);
    jw_string_field(&w, "model", MQTT_PROTO_MODEL, false);
    jw_string_field(&w, "fw", fw ? fw : "", false);
    jw_string_field(&w, "name", name ? name : "", false);
    jw_number_field(&w, "max_leds", (long) max_leds, false);
    jw_string_field(&w, "ip", ip ? ip : "", false);
    jw_char(&w, '}');
    return w.ok ? (int) w.len : -1;
}

int mqtt_proto_encode_controllers(char *out, size_t out_len, const mqtt_proto_controllers_t *list)
{
    json_writer_t w;
    jw_init(&w, out, out_len);
    jw_raw(&w, "{\"paired\":[");
    if (list) {
        for (uint8_t i = 0; i < list->count && i < MQTT_PROTO_MAX_CONTROLLERS; ++i) {
            if (i > 0) {
                jw_char(&w, ',');
            }
            jw_char(&w, '{');
            jw_string_field(&w, "id", list->items[i].id, true);
            jw_string_field(&w, "name", list->items[i].name, false);
            jw_char(&w, '}');
        }
    }
    jw_raw(&w, "]}");
    return w.ok ? (int) w.len : -1;
}

// ---- decoders --------------------------------------------------------------

bool mqtt_proto_parse_envelope(const cJSON *root, mqtt_proto_envelope_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!cJSON_IsObject(root)) {
        return false;
    }

    const cJSON *from = cJSON_GetObjectItemCaseSensitive(root, "from");
    if (cJSON_IsString(from) && from->valuestring) {
        out->has_from = mqtt_proto_sanitize_id(from->valuestring, out->from, sizeof(out->from));
    }

    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    if (cJSON_IsNumber(seq) && seq->valuedouble >= 0.0 && seq->valuedouble <= 4294967295.0) {
        out->seq = (uint32_t) seq->valuedouble;
        out->has_seq = true;
    }
    return true;
}

bool mqtt_proto_set_has_fields(const cJSON *root)
{
    if (!cJSON_IsObject(root)) {
        return false;
    }
    static const char *kFields[] = {"power", "brightness", "color", "effect", "effect_params", "effect_color"};
    for (size_t i = 0; i < sizeof(kFields) / sizeof(kFields[0]); ++i) {
        if (cJSON_GetObjectItemCaseSensitive(root, kFields[i]) != NULL) {
            return true;
        }
    }
    return false;
}

bool mqtt_proto_parse_pair(const cJSON *root, mqtt_proto_pair_msg_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!cJSON_IsObject(root)) {
        return false;
    }

    const cJSON *controller = cJSON_GetObjectItemCaseSensitive(root, "controller");
    if (!cJSON_IsString(controller) || !controller->valuestring ||
        !mqtt_proto_sanitize_id(controller->valuestring, out->controller, sizeof(out->controller))) {
        return false;
    }

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(name) && name->valuestring) {
        mqtt_proto_sanitize_name(name->valuestring, out->name, sizeof(out->name));
    }

    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (cJSON_IsNumber(code)) {
        const double value = code->valuedouble;
        if (value < MQTT_PROTO_PAIR_CODE_MIN || value > MQTT_PROTO_PAIR_CODE_MAX) {
            // Out-of-range code: keep it flagged so the caller treats it as a
            // failed confirmation instead of silently reopening a window.
            out->has_code = true;
            out->code = 0;
        } else {
            out->has_code = true;
            out->code = (uint8_t) value;
        }
    }
    return true;
}

bool mqtt_proto_parse_unpair(const cJSON *root, char *ctl_out, size_t out_len)
{
    if (!ctl_out || out_len == 0) {
        return false;
    }
    ctl_out[0] = '\0';
    if (!cJSON_IsObject(root)) {
        return false;
    }
    const cJSON *controller = cJSON_GetObjectItemCaseSensitive(root, "controller");
    if (!cJSON_IsString(controller) || !controller->valuestring) {
        return false;
    }
    return mqtt_proto_sanitize_id(controller->valuestring, ctl_out, out_len);
}

// ---- paired-controller list ------------------------------------------------

bool mqtt_proto_controller_find(const mqtt_proto_controllers_t *list, const char *id, size_t *index_out)
{
    if (!list || !id) {
        return false;
    }
    for (uint8_t i = 0; i < list->count && i < MQTT_PROTO_MAX_CONTROLLERS; ++i) {
        if (strncmp(list->items[i].id, id, MQTT_PROTO_ID_MAX) == 0) {
            if (index_out) {
                *index_out = i;
            }
            return true;
        }
    }
    return false;
}

bool mqtt_proto_controller_add(mqtt_proto_controllers_t *list, const char *id, const char *name)
{
    if (!list || !id || id[0] == '\0') {
        return false;
    }

    size_t index = 0;
    if (mqtt_proto_controller_find(list, id, &index)) {
        if (name && name[0] != '\0') {
            snprintf(list->items[index].name, MQTT_PROTO_NAME_MAX, "%s", name);
        }
        return true;
    }
    if (list->count >= MQTT_PROTO_MAX_CONTROLLERS) {
        return false;
    }

    mqtt_proto_controller_t *slot = &list->items[list->count];
    snprintf(slot->id, MQTT_PROTO_ID_MAX, "%s", id);
    snprintf(slot->name, MQTT_PROTO_NAME_MAX, "%s", (name && name[0] != '\0') ? name : id);
    list->count++;
    return true;
}

bool mqtt_proto_controller_remove(mqtt_proto_controllers_t *list, const char *id)
{
    size_t index = 0;
    if (!mqtt_proto_controller_find(list, id, &index)) {
        return false;
    }
    for (size_t i = index; i + 1 < list->count; ++i) {
        list->items[i] = list->items[i + 1];
    }
    list->count--;
    memset(&list->items[list->count], 0, sizeof(list->items[list->count]));
    return true;
}

// ---- pairing state machine -------------------------------------------------

static mqtt_pair_outcome_t pair_outcome(mqtt_pair_action_t action, uint8_t code)
{
    mqtt_pair_outcome_t outcome;
    outcome.action = action;
    outcome.code = code;
    return outcome;
}

static void pair_clear(mqtt_pair_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = MQTT_PAIR_IDLE;
}

mqtt_pair_outcome_t mqtt_pair_on_request(mqtt_pair_ctx_t *ctx, mqtt_proto_controllers_t *list, const char *ctl,
                                         const char *name, int64_t now_ms, uint8_t proposed_code)
{
    if (!ctx || !list || !ctl || ctl[0] == '\0') {
        return pair_outcome(MQTT_PAIR_ACTION_NONE, 0);
    }

    // Expire a stale window first so a late request opens a fresh one.
    if (ctx->state == MQTT_PAIR_WAITING_CODE && now_ms >= ctx->deadline_ms) {
        pair_clear(ctx);
    }

    // Already ours: idempotent re-pair, refresh the label only.
    if (mqtt_proto_controller_find(list, ctl, NULL)) {
        mqtt_proto_controller_add(list, ctl, name);
        pair_clear(ctx);
        return pair_outcome(MQTT_PAIR_ACTION_PAIRED, 0);
    }

    // Factory-fresh strip: the very first controller pairs with one tap.
    if (list->count == 0) {
        if (!mqtt_proto_controller_add(list, ctl, name)) {
            return pair_outcome(MQTT_PAIR_ACTION_REJECTED, 0);
        }
        pair_clear(ctx);
        return pair_outcome(MQTT_PAIR_ACTION_PAIRED, 0);
    }

    if (list->count >= MQTT_PROTO_MAX_CONTROLLERS) {
        return pair_outcome(MQTT_PAIR_ACTION_REJECTED, 0);
    }

    // A window already open for this controller keeps its code, so a repeated
    // request does not reshuffle the blinks the user is counting.
    if (ctx->state == MQTT_PAIR_WAITING_CODE && strncmp(ctx->pending_id, ctl, MQTT_PROTO_ID_MAX) == 0) {
        ctx->deadline_ms = now_ms + MQTT_PROTO_PAIR_WINDOW_MS;
        return pair_outcome(MQTT_PAIR_ACTION_BLINK_CODE, ctx->code);
    }

    uint8_t code = proposed_code;
    if (code < MQTT_PROTO_PAIR_CODE_MIN || code > MQTT_PROTO_PAIR_CODE_MAX) {
        code = MQTT_PROTO_PAIR_CODE_MIN;
    }

    pair_clear(ctx);
    ctx->state = MQTT_PAIR_WAITING_CODE;
    snprintf(ctx->pending_id, sizeof(ctx->pending_id), "%s", ctl);
    if (name) {
        snprintf(ctx->pending_name, sizeof(ctx->pending_name), "%s", name);
    }
    ctx->code = code;
    ctx->deadline_ms = now_ms + MQTT_PROTO_PAIR_WINDOW_MS;
    return pair_outcome(MQTT_PAIR_ACTION_BLINK_CODE, code);
}

mqtt_pair_outcome_t mqtt_pair_on_code(mqtt_pair_ctx_t *ctx, mqtt_proto_controllers_t *list, const char *ctl,
                                      uint8_t code, int64_t now_ms)
{
    if (!ctx || !list || !ctl || ctl[0] == '\0') {
        return pair_outcome(MQTT_PAIR_ACTION_NONE, 0);
    }

    if (ctx->state != MQTT_PAIR_WAITING_CODE) {
        return pair_outcome(MQTT_PAIR_ACTION_REJECTED, 0);
    }
    if (now_ms >= ctx->deadline_ms) {
        pair_clear(ctx);
        return pair_outcome(MQTT_PAIR_ACTION_REJECTED, 0);
    }
    if (strncmp(ctx->pending_id, ctl, MQTT_PROTO_ID_MAX) != 0 || code != ctx->code) {
        pair_clear(ctx);
        return pair_outcome(MQTT_PAIR_ACTION_REJECTED, 0);
    }

    const bool added = mqtt_proto_controller_add(list, ctx->pending_id, ctx->pending_name);
    pair_clear(ctx);
    return pair_outcome(added ? MQTT_PAIR_ACTION_PAIRED : MQTT_PAIR_ACTION_REJECTED, 0);
}

mqtt_pair_outcome_t mqtt_pair_tick(mqtt_pair_ctx_t *ctx, int64_t now_ms)
{
    if (!ctx || ctx->state != MQTT_PAIR_WAITING_CODE || now_ms < ctx->deadline_ms) {
        return pair_outcome(MQTT_PAIR_ACTION_NONE, 0);
    }
    pair_clear(ctx);
    return pair_outcome(MQTT_PAIR_ACTION_REJECTED, 0);
}

// ---- coalescer -------------------------------------------------------------

void mqtt_coalescer_mark(mqtt_coalescer_t *c, const char *src, uint32_t seq, bool rejected)
{
    if (!c) {
        return;
    }
    if (rejected) {
        c->rejected_pending = true;
        snprintf(c->rejected_src, sizeof(c->rejected_src), "%s", src ? src : "");
        c->rejected_seq = seq;
        return;
    }
    c->pending = true;
    snprintf(c->src, sizeof(c->src), "%s", src ? src : "");
    c->seq = seq;
}

int32_t mqtt_coalescer_due_in(const mqtt_coalescer_t *c, int64_t now_ms)
{
    if (!c || (!c->pending && !c->rejected_pending)) {
        return -1;
    }
    const int64_t earliest = c->last_publish_ms + MQTT_PROTO_STATE_GAP_MS;
    if (now_ms >= earliest) {
        return 0;
    }
    return (int32_t) (earliest - now_ms);
}

bool mqtt_coalescer_take(mqtt_coalescer_t *c, int64_t now_ms, char *src_out, size_t src_len, uint32_t *seq_out,
                         bool *rejected_out)
{
    if (!c || !src_out || src_len == 0 || !seq_out || !rejected_out) {
        return false;
    }
    if (mqtt_coalescer_due_in(c, now_ms) != 0) {
        return false;
    }

    // Rejections go out first: they answer a specific controller and must not
    // be overtaken by an unrelated state change in the same window.
    if (c->rejected_pending) {
        snprintf(src_out, src_len, "%s", c->rejected_src);
        *seq_out = c->rejected_seq;
        *rejected_out = true;
        c->rejected_pending = false;
    } else {
        snprintf(src_out, src_len, "%s", c->src);
        *seq_out = c->seq;
        *rejected_out = false;
        c->pending = false;
    }
    c->last_publish_ms = now_ms;
    return true;
}
