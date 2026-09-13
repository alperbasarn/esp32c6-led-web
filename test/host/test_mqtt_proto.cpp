// Host tests for the pure MQTT protocol layer (main/mqtt_proto.cpp).
//
// Build and run with test/host/run_tests.sh. These cover exactly the parts of
// the contract that do not need a device: topic building, JSON encode/decode,
// the pairing state machine and the publish coalescer.

#include <cstdio>
#include <cstring>
#include <string>

#include "mqtt_proto.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                \
        }                                                                                                              \
    } while (0)

#define CHECK_STR(actual, expected)                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (std::string(actual) != std::string(expected)) {                                                            \
            ++g_failures;                                                                                              \
            std::printf("FAIL %s:%d\n  expected: %s\n  actual:   %s\n", __FILE__, __LINE__, (expected),                 \
                        (const char *) (actual));                                                                      \
        }                                                                                                              \
    } while (0)

static void test_identity_and_topics()
{
    const uint8_t mac[6] = {0x40, 0x4c, 0xca, 0xa1, 0xb2, 0xc3};
    char          id[MQTT_PROTO_ID_MAX] = "";
    CHECK(mqtt_proto_device_id_from_mac(mac, id, sizeof(id)));
    CHECK_STR(id, "led-a1b2c3");

    char topic[MQTT_PROTO_TOPIC_MAX] = "";
    CHECK(mqtt_proto_build_topic(topic, sizeof(topic), id, "state"));
    CHECK_STR(topic, "homio/led/led-a1b2c3/state");

    char tiny[8] = "";
    CHECK(!mqtt_proto_build_topic(tiny, sizeof(tiny), id, "state"));

    const char *set_topic = "homio/led/led-a1b2c3/set";
    CHECK(mqtt_proto_classify_topic(set_topic, std::strlen(set_topic), id) == MQTT_PROTO_TOPIC_SET);
    const char *pair_topic = "homio/led/led-a1b2c3/pair";
    CHECK(mqtt_proto_classify_topic(pair_topic, std::strlen(pair_topic), id) == MQTT_PROTO_TOPIC_PAIR);
    const char *unpair_topic = "homio/led/led-a1b2c3/unpair";
    CHECK(mqtt_proto_classify_topic(unpair_topic, std::strlen(unpair_topic), id) == MQTT_PROTO_TOPIC_UNPAIR);
    // Another strip's topic must never be accepted.
    const char *other = "homio/led/led-ffffff/set";
    CHECK(mqtt_proto_classify_topic(other, std::strlen(other), id) == MQTT_PROTO_TOPIC_UNKNOWN);
    // A prefix of a valid topic must not match either.
    CHECK(mqtt_proto_classify_topic(set_topic, std::strlen(set_topic) - 1, id) == MQTT_PROTO_TOPIC_UNKNOWN);
}

static void test_sanitizers()
{
    char out[MQTT_PROTO_ID_MAX] = "";
    CHECK(mqtt_proto_sanitize_id("qnob-A1B2C3", out, sizeof(out)));
    CHECK_STR(out, "qnob-a1b2c3");
    CHECK(!mqtt_proto_sanitize_id("qnob/../#", out, sizeof(out)));
    CHECK(!mqtt_proto_sanitize_id("", out, sizeof(out)));
    CHECK(!mqtt_proto_sanitize_id("qnob a1", out, sizeof(out)));
    // Too long for the destination.
    CHECK(!mqtt_proto_sanitize_id("qnob-012345678901234567890123456789", out, sizeof(out)));

    char name[MQTT_PROTO_NAME_MAX] = "";
    CHECK(mqtt_proto_sanitize_name("Living room knob", name, sizeof(name)));
    CHECK_STR(name, "Living room knob");
    CHECK(mqtt_proto_sanitize_name("bad\x01\x02name", name, sizeof(name)));
    CHECK_STR(name, "badname");
    // 24-byte cap.
    CHECK(mqtt_proto_sanitize_name("0123456789012345678901234567890", name, sizeof(name)));
    CHECK(std::strlen(name) == MQTT_PROTO_NAME_MAX - 1);
}

static led_tuple_t sample_tuple()
{
    led_tuple_t tuple = {};
    tuple.power = true;
    tuple.brightness = 96;
    std::snprintf(tuple.color, sizeof(tuple.color), "%s", "#FF6020");
    std::snprintf(tuple.effect, sizeof(tuple.effect), "%s", "sparkle");
    tuple.effect_params[0] = 180;
    tuple.effect_params[1] = 60;
    tuple.effect_params[2] = 170;
    std::snprintf(tuple.effect_color, sizeof(tuple.effect_color), "%s", "#FFFFFF");
    tuple.count = 30;
    return tuple;
}

static void test_encoders()
{
    const led_tuple_t tuple = sample_tuple();
    char              doc[MQTT_PROTO_STATE_JSON_MAX] = "";

    int length = mqtt_proto_encode_state(doc, sizeof(doc), &tuple, "qnob-a1b2c3", 42, false);
    CHECK(length > 0);
    CHECK_STR(doc,
              "{\"power\":true,\"brightness\":96,\"color\":\"#FF6020\",\"effect\":\"sparkle\","
              "\"effect_params\":[180,60,170,0,0],\"effect_color\":\"#FFFFFF\",\"count\":30,"
              "\"src\":\"qnob-a1b2c3\",\"seq\":42}");
    CHECK(length == (int) std::strlen(doc));
    // The real document is far below the 2 KB budget.
    CHECK(length < 300);

    length = mqtt_proto_encode_state(doc, sizeof(doc), &tuple, "qnob-deadbe", 7, true);
    CHECK(length > 0);
    CHECK(std::strstr(doc, "\"rejected\":true") != nullptr);
    CHECK(std::strstr(doc, "\"src\":\"qnob-deadbe\"") != nullptr);

    // A buffer that cannot hold the document fails instead of truncating.
    char small[40] = "";
    CHECK(mqtt_proto_encode_state(small, sizeof(small), &tuple, "qnob-a1b2c3", 1, false) < 0);

    length = mqtt_proto_encode_info(doc, sizeof(doc), "led-a1b2c3", "1.11", "ESP32C6-LED-A1B2C3", 256, "192.168.1.44");
    CHECK(length > 0);
    CHECK_STR(doc,
              "{\"id\":\"led-a1b2c3\",\"model\":\"esp32c6-led-web\",\"fw\":\"1.11\","
              "\"name\":\"ESP32C6-LED-A1B2C3\",\"max_leds\":256,\"ip\":\"192.168.1.44\"}");

    mqtt_proto_controllers_t list = {};
    CHECK(mqtt_proto_encode_controllers(doc, sizeof(doc), &list) > 0);
    CHECK_STR(doc, "{\"paired\":[]}");

    CHECK(mqtt_proto_controller_add(&list, "qnob-a1b2c3", "Hall knob"));
    CHECK(mqtt_proto_controller_add(&list, "qnob-000111", "Kitchen \"knob\""));
    CHECK(mqtt_proto_encode_controllers(doc, sizeof(doc), &list) > 0);
    CHECK_STR(doc,
              "{\"paired\":[{\"id\":\"qnob-a1b2c3\",\"name\":\"Hall knob\"},"
              "{\"id\":\"qnob-000111\",\"name\":\"Kitchen \\\"knob\\\"\"}]}");
}

static void test_decoders()
{
    cJSON *root = cJSON_Parse("{\"brightness\":120,\"from\":\"QNOB-A1B2C3\",\"seq\":9}");
    CHECK(root != nullptr);
    mqtt_proto_envelope_t envelope = {};
    CHECK(mqtt_proto_parse_envelope(root, &envelope));
    CHECK(envelope.has_from);
    CHECK_STR(envelope.from, "qnob-a1b2c3");
    CHECK(envelope.has_seq && envelope.seq == 9);
    CHECK(mqtt_proto_set_has_fields(root));
    cJSON_Delete(root);

    root = cJSON_Parse("{\"from\":\"qnob-a1b2c3\"}");
    CHECK(mqtt_proto_parse_envelope(root, &envelope));
    CHECK(!envelope.has_seq);
    CHECK(!mqtt_proto_set_has_fields(root));
    cJSON_Delete(root);

    // A hostile id is rejected rather than sanitized into something usable.
    root = cJSON_Parse("{\"from\":\"../#\",\"brightness\":5}");
    CHECK(mqtt_proto_parse_envelope(root, &envelope));
    CHECK(!envelope.has_from);
    cJSON_Delete(root);

    mqtt_proto_pair_msg_t pair = {};
    root = cJSON_Parse("{\"controller\":\"qnob-a1b2c3\",\"name\":\"Hall knob\"}");
    CHECK(mqtt_proto_parse_pair(root, &pair));
    CHECK_STR(pair.controller, "qnob-a1b2c3");
    CHECK_STR(pair.name, "Hall knob");
    CHECK(!pair.has_code);
    cJSON_Delete(root);

    root = cJSON_Parse("{\"controller\":\"qnob-a1b2c3\",\"code\":4}");
    CHECK(mqtt_proto_parse_pair(root, &pair));
    CHECK(pair.has_code && pair.code == 4);
    cJSON_Delete(root);

    // Out-of-range codes stay flagged so they count as a failed confirmation.
    root = cJSON_Parse("{\"controller\":\"qnob-a1b2c3\",\"code\":99}");
    CHECK(mqtt_proto_parse_pair(root, &pair));
    CHECK(pair.has_code && pair.code == 0);
    cJSON_Delete(root);

    root = cJSON_Parse("{\"name\":\"no controller\"}");
    CHECK(!mqtt_proto_parse_pair(root, &pair));
    cJSON_Delete(root);

    char controller[MQTT_PROTO_ID_MAX] = "";
    root = cJSON_Parse("{\"controller\":\"qnob-a1b2c3\"}");
    CHECK(mqtt_proto_parse_unpair(root, controller, sizeof(controller)));
    CHECK_STR(controller, "qnob-a1b2c3");
    cJSON_Delete(root);

    root = cJSON_Parse("{\"controller\":42}");
    CHECK(!mqtt_proto_parse_unpair(root, controller, sizeof(controller)));
    cJSON_Delete(root);
}

static void test_controller_list()
{
    mqtt_proto_controllers_t list = {};
    for (int i = 0; i < MQTT_PROTO_MAX_CONTROLLERS; ++i) {
        char id[MQTT_PROTO_ID_MAX];
        std::snprintf(id, sizeof(id), "qnob-00000%d", i);
        CHECK(mqtt_proto_controller_add(&list, id, "knob"));
    }
    CHECK(list.count == MQTT_PROTO_MAX_CONTROLLERS);
    CHECK(!mqtt_proto_controller_add(&list, "qnob-999999", "one too many"));

    // Re-adding an existing controller refreshes the label without growing.
    CHECK(mqtt_proto_controller_add(&list, "qnob-000000", "renamed"));
    CHECK(list.count == MQTT_PROTO_MAX_CONTROLLERS);
    size_t index = 99;
    CHECK(mqtt_proto_controller_find(&list, "qnob-000000", &index));
    CHECK(index == 0);
    CHECK_STR(list.items[0].name, "renamed");

    CHECK(mqtt_proto_controller_remove(&list, "qnob-000000"));
    CHECK(list.count == MQTT_PROTO_MAX_CONTROLLERS - 1);
    CHECK(!mqtt_proto_controller_find(&list, "qnob-000000", nullptr));
    CHECK(!mqtt_proto_controller_remove(&list, "qnob-000000"));
    // The remaining entries keep their order.
    CHECK_STR(list.items[0].id, "qnob-000001");
}

static void test_pairing_first_is_free()
{
    mqtt_proto_controllers_t list = {};
    mqtt_pair_ctx_t          ctx = {};

    mqtt_pair_outcome_t outcome = mqtt_pair_on_request(&ctx, &list, "qnob-a1b2c3", "Hall knob", 1000, 3);
    CHECK(outcome.action == MQTT_PAIR_ACTION_PAIRED);
    CHECK(list.count == 1);
    CHECK(ctx.state == MQTT_PAIR_IDLE);
    CHECK_STR(list.items[0].name, "Hall knob");

    // A second request from the same controller is idempotent.
    outcome = mqtt_pair_on_request(&ctx, &list, "qnob-a1b2c3", "Hall knob 2", 2000, 3);
    CHECK(outcome.action == MQTT_PAIR_ACTION_PAIRED);
    CHECK(list.count == 1);
    CHECK_STR(list.items[0].name, "Hall knob 2");
}

static void test_pairing_code_flow()
{
    mqtt_proto_controllers_t list = {};
    mqtt_pair_ctx_t          ctx = {};
    CHECK(mqtt_proto_controller_add(&list, "qnob-first0", "First"));

    mqtt_pair_outcome_t outcome = mqtt_pair_on_request(&ctx, &list, "qnob-second", "Second", 1000, 5);
    CHECK(outcome.action == MQTT_PAIR_ACTION_BLINK_CODE);
    CHECK(outcome.code == 5);
    CHECK(ctx.state == MQTT_PAIR_WAITING_CODE);
    CHECK(ctx.deadline_ms == 1000 + MQTT_PROTO_PAIR_WINDOW_MS);

    // A repeated request keeps the code the user is already counting.
    outcome = mqtt_pair_on_request(&ctx, &list, "qnob-second", "Second", 2000, 2);
    CHECK(outcome.action == MQTT_PAIR_ACTION_BLINK_CODE);
    CHECK(outcome.code == 5);

    // Wrong code: nothing is stored and the window closes.
    outcome = mqtt_pair_on_code(&ctx, &list, "qnob-second", 4, 3000);
    CHECK(outcome.action == MQTT_PAIR_ACTION_REJECTED);
    CHECK(list.count == 1);
    CHECK(ctx.state == MQTT_PAIR_IDLE);

    // Correct code on a fresh window stores the controller.
    outcome = mqtt_pair_on_request(&ctx, &list, "qnob-second", "Second", 4000, 6);
    CHECK(outcome.action == MQTT_PAIR_ACTION_BLINK_CODE && outcome.code == 6);
    outcome = mqtt_pair_on_code(&ctx, &list, "qnob-second", 6, 5000);
    CHECK(outcome.action == MQTT_PAIR_ACTION_PAIRED);
    CHECK(list.count == 2);
    CHECK(mqtt_proto_controller_find(&list, "qnob-second", nullptr));

    // A code from a different controller never confirms someone else's window.
    outcome = mqtt_pair_on_request(&ctx, &list, "qnob-third0", "Third", 6000, 2);
    CHECK(outcome.action == MQTT_PAIR_ACTION_BLINK_CODE);
    outcome = mqtt_pair_on_code(&ctx, &list, "qnob-other0", 2, 6500);
    CHECK(outcome.action == MQTT_PAIR_ACTION_REJECTED);
    CHECK(!mqtt_proto_controller_find(&list, "qnob-third0", nullptr));
    CHECK(!mqtt_proto_controller_find(&list, "qnob-other0", nullptr));
}

static void test_pairing_timeout_and_limits()
{
    mqtt_proto_controllers_t list = {};
    mqtt_pair_ctx_t          ctx = {};
    CHECK(mqtt_proto_controller_add(&list, "qnob-first0", "First"));

    mqtt_pair_outcome_t outcome = mqtt_pair_on_request(&ctx, &list, "qnob-second", "Second", 0, 1);
    CHECK(outcome.action == MQTT_PAIR_ACTION_BLINK_CODE);
    CHECK(mqtt_pair_tick(&ctx, MQTT_PROTO_PAIR_WINDOW_MS - 1).action == MQTT_PAIR_ACTION_NONE);
    CHECK(mqtt_pair_tick(&ctx, MQTT_PROTO_PAIR_WINDOW_MS).action == MQTT_PAIR_ACTION_REJECTED);
    CHECK(ctx.state == MQTT_PAIR_IDLE);
    CHECK(list.count == 1);

    // A code arriving after the window closed does not pair.
    outcome = mqtt_pair_on_code(&ctx, &list, "qnob-second", 1, MQTT_PROTO_PAIR_WINDOW_MS + 10);
    CHECK(outcome.action == MQTT_PAIR_ACTION_REJECTED);
    CHECK(list.count == 1);

    // Expiry inside on_request opens a fresh window rather than reusing a stale one.
    outcome = mqtt_pair_on_request(&ctx, &list, "qnob-second", "Second", 1000, 2);
    CHECK(outcome.action == MQTT_PAIR_ACTION_BLINK_CODE && outcome.code == 2);
    outcome = mqtt_pair_on_request(&ctx, &list, "qnob-third0", "Third", 1000 + MQTT_PROTO_PAIR_WINDOW_MS, 3);
    CHECK(outcome.action == MQTT_PAIR_ACTION_BLINK_CODE && outcome.code == 3);
    CHECK_STR(ctx.pending_id, "qnob-third0");

    // A full list rejects a new controller outright.
    mqtt_proto_controllers_t full = {};
    for (int i = 0; i < MQTT_PROTO_MAX_CONTROLLERS; ++i) {
        char id[MQTT_PROTO_ID_MAX];
        std::snprintf(id, sizeof(id), "qnob-00000%d", i);
        CHECK(mqtt_proto_controller_add(&full, id, "knob"));
    }
    mqtt_pair_ctx_t full_ctx = {};
    outcome = mqtt_pair_on_request(&full_ctx, &full, "qnob-999999", "Extra", 1000, 4);
    CHECK(outcome.action == MQTT_PAIR_ACTION_REJECTED);
    CHECK(full_ctx.state == MQTT_PAIR_IDLE);

    // Out-of-range proposed codes are clamped into 1..6.
    mqtt_proto_controllers_t list2 = {};
    CHECK(mqtt_proto_controller_add(&list2, "qnob-first0", "First"));
    mqtt_pair_ctx_t ctx2 = {};
    outcome = mqtt_pair_on_request(&ctx2, &list2, "qnob-second", "Second", 0, 0);
    CHECK(outcome.code >= MQTT_PROTO_PAIR_CODE_MIN && outcome.code <= MQTT_PROTO_PAIR_CODE_MAX);
}

static void test_coalescer()
{
    mqtt_coalescer_t c = {};
    char             src[MQTT_PROTO_ID_MAX] = "";
    uint32_t         seq = 0;
    bool             rejected = false;

    CHECK(mqtt_coalescer_due_in(&c, 0) == -1);

    // First publish goes out immediately.
    mqtt_coalescer_mark(&c, "web", 1, false);
    CHECK(mqtt_coalescer_due_in(&c, 1000) == 0);
    CHECK(mqtt_coalescer_take(&c, 1000, src, sizeof(src), &seq, &rejected));
    CHECK_STR(src, "web");
    CHECK(seq == 1 && !rejected);
    CHECK(mqtt_coalescer_due_in(&c, 1000) == -1);

    // A burst inside the window collapses to one publish, 100 ms later.
    for (int i = 0; i < 20; ++i) {
        mqtt_coalescer_mark(&c, "qnob-a1b2c3", (uint32_t) (100 + i), false);
    }
    CHECK(mqtt_coalescer_due_in(&c, 1050) == 50);
    CHECK(!mqtt_coalescer_take(&c, 1050, src, sizeof(src), &seq, &rejected));
    CHECK(mqtt_coalescer_take(&c, 1100, src, sizeof(src), &seq, &rejected));
    CHECK_STR(src, "qnob-a1b2c3");
    CHECK(seq == 119); // the newest state, not the oldest
    CHECK(!rejected);

    // A rejection is never swallowed by a normal publish in the same window.
    mqtt_coalescer_mark(&c, "qnob-bad000", 5, true);
    mqtt_coalescer_mark(&c, "matter", 6, false);
    CHECK(mqtt_coalescer_take(&c, 1200, src, sizeof(src), &seq, &rejected));
    CHECK_STR(src, "qnob-bad000");
    CHECK(rejected && seq == 5);
    CHECK(mqtt_coalescer_take(&c, 1300, src, sizeof(src), &seq, &rejected));
    CHECK_STR(src, "matter");
    CHECK(!rejected && seq == 6);
    CHECK(mqtt_coalescer_due_in(&c, 1400) == -1);

    // 10/s ceiling: 30 marks spread over a second yield at most 10 publishes.
    mqtt_coalescer_t rate = {};
    int              publishes = 0;
    for (int ms = 0; ms < 1000; ms += 33) {
        mqtt_coalescer_mark(&rate, "qnob-a1b2c3", (uint32_t) ms, false);
        if (mqtt_coalescer_take(&rate, ms, src, sizeof(src), &seq, &rejected)) {
            ++publishes;
        }
    }
    CHECK(publishes <= 10);
}

int main()
{
    test_identity_and_topics();
    test_sanitizers();
    test_encoders();
    test_decoders();
    test_controller_list();
    test_pairing_first_is_free();
    test_pairing_code_flow();
    test_pairing_timeout_and_limits();
    test_coalescer();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
