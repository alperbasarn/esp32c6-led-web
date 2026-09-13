// MQTT link to the QNOB round HMI — device side of docs/mqtt-contract.md.
//
// Threading model
// ---------------
// * `mqtt_link` task: the only publisher. It owns the client lifecycle
//   (create/start/reconnect/destroy), drains the coalescer, and expires the
//   pairing window.
// * esp-mqtt's own task: delivers events. Inbound messages are parsed and
//   applied there (never published from there — a QoS 1 publish inside the
//   event handler can deadlock against the client task).
// * Callers (HTTP handlers, Matter callbacks, schedule task): only mark state
//   dirty and notify. Nothing on those paths blocks on the network.
//
// Locking: `s_mutex` guards the link's own state. It is never held across a
// publish or across a led_control_* call, so a Matter callback can always take
// it immediately.

#include "mqtt_link.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_control.h"
#include "mqtt_client.h"
#include "mqtt_proto.h"
#include "nvs.h"

static const char *TAG = "mqtt_link";

// Broker settings live in the app settings namespace so the existing factory
// reset (which erases that namespace) clears them along with the controller
// list. Every key stays within the 15-character NVS limit.
#define MQTT_NVS_NAMESPACE "led_cfg"
#define MQTT_NVS_KEY_HOST  "mq_host"
#define MQTT_NVS_KEY_PORT  "mq_port"
#define MQTT_NVS_KEY_USER  "mq_user"
#define MQTT_NVS_KEY_PASS  "mq_pass"
#define MQTT_NVS_KEY_TLS   "mq_tls"
#define MQTT_NVS_KEY_CTRL  "mq_ctl"

#define MQTT_LINK_HOST_MAX      64
#define MQTT_LINK_USER_MAX      48
#define MQTT_LINK_PASS_MAX      72
#define MQTT_LINK_ERR_MAX       80
#define MQTT_LINK_URI_MAX       (MQTT_LINK_HOST_MAX + 24)
#define MQTT_LINK_KEEPALIVE_S   30
#define MQTT_LINK_BACKOFF_MIN_MS 2000
#define MQTT_LINK_BACKOFF_MAX_MS 60000
#define MQTT_LINK_IDLE_WAIT_MS   60000
#define MQTT_LINK_TASK_STACK     5120
#define MQTT_LINK_TASK_PRIO      4
#define MQTT_LINK_DEFAULT_PORT_TLS   8883
#define MQTT_LINK_DEFAULT_PORT_PLAIN 1883
#define MQTT_LINK_CONTROLLERS_VERSION 1

typedef enum {
    LINK_DISABLED = 0, // no broker configured
    LINK_WAITING,      // configured, waiting for the station to get an IP
    LINK_CONNECTING,
    LINK_CONNECTED,
    LINK_ERROR,
} link_status_t;

typedef struct {
    char     host[MQTT_LINK_HOST_MAX];
    uint16_t port;
    char     user[MQTT_LINK_USER_MAX];
    char     pass[MQTT_LINK_PASS_MAX];
    bool     tls;
} link_cfg_t;

typedef struct {
    uint8_t                 version;
    uint8_t                 count;
    mqtt_proto_controller_t items[MQTT_PROTO_MAX_CONTROLLERS];
} controllers_blob_t;

// ---- state (guarded by s_mutex unless noted) -------------------------------

static SemaphoreHandle_t s_mutex = nullptr;
static TaskHandle_t      s_task = nullptr;
static bool              s_ready = false; // init() completed

static link_cfg_t               s_cfg = {};
static mqtt_proto_controllers_t s_controllers = {};
static mqtt_pair_ctx_t          s_pair = {};
static mqtt_coalescer_t         s_coalescer = {};

static char          s_device_id[MQTT_PROTO_ID_MAX] = "";
static link_status_t s_status = LINK_DISABLED;
static char          s_last_error[MQTT_LINK_ERR_MAX] = "";
static bool          s_net_up = false;
static bool          s_connected = false;
static uint32_t      s_cfg_generation = 0;
static uint32_t      s_backoff_ms = MQTT_LINK_BACKOFF_MIN_MS;
static int64_t       s_next_attempt_ms = 0;
static bool          s_pending_session = false;     // publish info/status/controllers
static bool          s_pending_controllers = false; // controller list changed
static bool          s_pending_factory_announce = false; // clear retained topics, then disconnect
static uint32_t      s_local_seq = 0;

// Owned by the link task only.
static esp_mqtt_client_handle_t s_client = nullptr;
static bool                     s_client_started = false;
static uint32_t                 s_client_generation = 0;
static char                     s_topic_state[MQTT_PROTO_TOPIC_MAX] = "";
static char                     s_topic_status[MQTT_PROTO_TOPIC_MAX] = "";
static char                     s_topic_info[MQTT_PROTO_TOPIC_MAX] = "";
static char                     s_topic_controllers[MQTT_PROTO_TOPIC_MAX] = "";
static char                     s_topic_set[MQTT_PROTO_TOPIC_MAX] = "";
static char                     s_topic_pair[MQTT_PROTO_TOPIC_MAX] = "";
static char                     s_topic_unpair[MQTT_PROTO_TOPIC_MAX] = "";
static char                     s_uri[MQTT_LINK_URI_MAX] = "";
static char                     s_out_doc[MQTT_PROTO_STATE_JSON_MAX];

// Owned by the esp-mqtt task only (inbound reassembly).
static char               s_in_payload[MQTT_PROTO_PAYLOAD_MAX + 1];
static size_t             s_in_len = 0;
static bool               s_in_drop = false;
static mqtt_proto_topic_t s_in_kind = MQTT_PROTO_TOPIC_UNKNOWN;

// ---- small helpers ---------------------------------------------------------

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static inline void link_lock(void)
{
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static inline void link_unlock(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

static void link_notify(void)
{
    TaskHandle_t task = s_task;
    if (task) {
        xTaskNotifyGive(task);
    }
}

static const char *status_text(link_status_t status)
{
    switch (status) {
    case LINK_WAITING:
        return "waiting for network";
    case LINK_CONNECTING:
        return "connecting";
    case LINK_CONNECTED:
        return "connected";
    case LINK_ERROR:
        return "error";
    case LINK_DISABLED:
    default:
        return "not configured";
    }
}

static void set_error_locked(const char *text)
{
    snprintf(s_last_error, sizeof(s_last_error), "%s", text ? text : "");
}

// ---- NVS -------------------------------------------------------------------

static void load_settings(void)
{
    nvs_handle_t handle = 0;
    if (nvs_open(MQTT_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    size_t length = sizeof(s_cfg.host);
    if (nvs_get_str(handle, MQTT_NVS_KEY_HOST, s_cfg.host, &length) != ESP_OK) {
        s_cfg.host[0] = '\0';
    }
    length = sizeof(s_cfg.user);
    if (nvs_get_str(handle, MQTT_NVS_KEY_USER, s_cfg.user, &length) != ESP_OK) {
        s_cfg.user[0] = '\0';
    }
    length = sizeof(s_cfg.pass);
    if (nvs_get_str(handle, MQTT_NVS_KEY_PASS, s_cfg.pass, &length) != ESP_OK) {
        s_cfg.pass[0] = '\0';
    }
    uint16_t port = 0;
    if (nvs_get_u16(handle, MQTT_NVS_KEY_PORT, &port) == ESP_OK) {
        s_cfg.port = port;
    }
    uint8_t tls = 1;
    if (nvs_get_u8(handle, MQTT_NVS_KEY_TLS, &tls) == ESP_OK) {
        s_cfg.tls = tls != 0;
    } else {
        s_cfg.tls = true;
    }

    controllers_blob_t blob = {};
    size_t blob_len = sizeof(blob);
    if (nvs_get_blob(handle, MQTT_NVS_KEY_CTRL, &blob, &blob_len) == ESP_OK && blob_len == sizeof(blob) &&
        blob.version == MQTT_LINK_CONTROLLERS_VERSION && blob.count <= MQTT_PROTO_MAX_CONTROLLERS) {
        s_controllers.count = blob.count;
        for (uint8_t i = 0; i < blob.count; ++i) {
            s_controllers.items[i] = blob.items[i];
            s_controllers.items[i].id[MQTT_PROTO_ID_MAX - 1] = '\0';
            s_controllers.items[i].name[MQTT_PROTO_NAME_MAX - 1] = '\0';
        }
    }
    nvs_close(handle);
}

static esp_err_t save_settings_locked(void)
{
    nvs_handle_t handle = 0;
    esp_err_t    err = nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, MQTT_NVS_KEY_HOST, s_cfg.host);
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, MQTT_NVS_KEY_PORT, s_cfg.port);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, MQTT_NVS_KEY_USER, s_cfg.user);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, MQTT_NVS_KEY_PASS, s_cfg.pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, MQTT_NVS_KEY_TLS, s_cfg.tls ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t save_controllers_locked(void)
{
    controllers_blob_t blob = {};
    blob.version = MQTT_LINK_CONTROLLERS_VERSION;
    blob.count = s_controllers.count;
    for (uint8_t i = 0; i < s_controllers.count && i < MQTT_PROTO_MAX_CONTROLLERS; ++i) {
        blob.items[i] = s_controllers.items[i];
    }

    nvs_handle_t handle = 0;
    esp_err_t    err = nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, MQTT_NVS_KEY_CTRL, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

// ---- publishing (link task only) -------------------------------------------

static void publish_raw(const char *topic, const char *payload, int len, int qos, bool retain)
{
    if (!s_client || !topic || !payload) {
        return;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, len, qos, retain ? 1 : 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "publish to %s failed", topic);
    }
}

static void publish_state_doc(const char *src, uint32_t seq, bool rejected)
{
    led_tuple_t tuple = {};
    led_control_get_tuple(&tuple);
    int length = mqtt_proto_encode_state(s_out_doc, sizeof(s_out_doc), &tuple, src, seq, rejected);
    if (length < 0) {
        ESP_LOGW(TAG, "state document did not fit");
        return;
    }
    publish_raw(s_topic_state, s_out_doc, length, 1, true);
}

static void publish_info_doc(void)
{
    char name[33] = "";
    char ip[16] = "";
    led_control_get_device_name(name, sizeof(name));
    led_control_get_sta_ip(ip, sizeof(ip));
    int length = mqtt_proto_encode_info(s_out_doc, sizeof(s_out_doc), s_device_id, led_control_firmware_version(), name,
                                        led_control_max_leds(), ip);
    if (length < 0) {
        return;
    }
    publish_raw(s_topic_info, s_out_doc, length, 1, true);
}

static void publish_controllers_doc(void)
{
    mqtt_proto_controllers_t snapshot;
    link_lock();
    snapshot = s_controllers;
    link_unlock();

    int length = mqtt_proto_encode_controllers(s_out_doc, sizeof(s_out_doc), &snapshot);
    if (length < 0) {
        return;
    }
    publish_raw(s_topic_controllers, s_out_doc, length, 1, true);
}

// ---- inbound (esp-mqtt task) -----------------------------------------------

static void handle_set(const cJSON *root)
{
    mqtt_proto_envelope_t envelope = {};
    if (!mqtt_proto_parse_envelope(root, &envelope) || !envelope.has_from) {
        ESP_LOGW(TAG, "set without a usable \"from\" ignored");
        return;
    }

    bool paired = false;
    link_lock();
    paired = mqtt_proto_controller_find(&s_controllers, envelope.from, nullptr);
    link_unlock();

    if (!paired) {
        // Pairing is ownership, not security: nothing changes, and the sender
        // is told so through a `state` carrying "rejected".
        ESP_LOGW(TAG, "set from unpaired controller %s rejected", envelope.from);
        link_lock();
        mqtt_coalescer_mark(&s_coalescer, envelope.from, envelope.seq, true);
        link_unlock();
        link_notify();
        return;
    }

    if (!mqtt_proto_set_has_fields(root)) {
        return;
    }

    // Same validation and clamping path as POST /api/control; the MQTT side
    // just allows a subset of the tuple and persists on a debounce.
    led_control_result_t result = led_control_apply_json(root, false, false);
    if (result != LED_CONTROL_OK) {
        ESP_LOGW(TAG, "set from %s rejected by the control path (%d)", envelope.from, (int) result);
    }

    // Publish either way: on a rejected field the unchanged tuple corrects the
    // controller's optimistic view.
    link_lock();
    mqtt_coalescer_mark(&s_coalescer, envelope.from, envelope.seq, false);
    link_unlock();
    link_notify();
}

static void handle_pair(const cJSON *root)
{
    mqtt_proto_pair_msg_t message = {};
    if (!mqtt_proto_parse_pair(root, &message)) {
        ESP_LOGW(TAG, "malformed pair message ignored");
        return;
    }

    const uint8_t proposed = (uint8_t) (MQTT_PROTO_PAIR_CODE_MIN +
                                        (esp_random() % (MQTT_PROTO_PAIR_CODE_MAX - MQTT_PROTO_PAIR_CODE_MIN + 1)));
    mqtt_pair_outcome_t outcome;
    bool                store_failed = false;

    link_lock();
    if (message.has_code) {
        outcome = mqtt_pair_on_code(&s_pair, &s_controllers, message.controller, message.code, now_ms());
    } else {
        outcome =
            mqtt_pair_on_request(&s_pair, &s_controllers, message.controller, message.name, now_ms(), proposed);
    }
    if (outcome.action == MQTT_PAIR_ACTION_PAIRED) {
        store_failed = save_controllers_locked() != ESP_OK;
        s_pending_controllers = true;
    }
    link_unlock();

    switch (outcome.action) {
    case MQTT_PAIR_ACTION_BLINK_CODE:
        ESP_LOGI(TAG, "pairing window open for %s, blink code %u", message.controller, (unsigned) outcome.code);
        led_control_indicator_blink(outcome.code);
        break;
    case MQTT_PAIR_ACTION_PAIRED:
        ESP_LOGI(TAG, "controller %s paired%s", message.controller, store_failed ? " (NVS write failed)" : "");
        led_control_indicator_flash(true);
        link_notify();
        break;
    case MQTT_PAIR_ACTION_REJECTED:
        ESP_LOGW(TAG, "pairing attempt from %s rejected", message.controller);
        led_control_indicator_flash(false);
        break;
    case MQTT_PAIR_ACTION_NONE:
    default:
        break;
    }
}

static void handle_unpair(const cJSON *root)
{
    char controller[MQTT_PROTO_ID_MAX] = "";
    if (!mqtt_proto_parse_unpair(root, controller, sizeof(controller))) {
        return;
    }

    bool removed = false;
    link_lock();
    removed = mqtt_proto_controller_remove(&s_controllers, controller);
    if (removed) {
        save_controllers_locked();
        s_pending_controllers = true;
    }
    link_unlock();

    if (removed) {
        ESP_LOGI(TAG, "controller %s unpaired", controller);
        link_notify();
    }
}

static void dispatch_message(mqtt_proto_topic_t kind, const char *payload, size_t length)
{
    if (kind == MQTT_PROTO_TOPIC_UNKNOWN || length == 0) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(payload, length);
    if (!root) {
        ESP_LOGW(TAG, "unparsable payload dropped");
        return;
    }

    switch (kind) {
    case MQTT_PROTO_TOPIC_SET:
        handle_set(root);
        break;
    case MQTT_PROTO_TOPIC_PAIR:
        handle_pair(root);
        break;
    case MQTT_PROTO_TOPIC_UNPAIR:
        handle_unpair(root);
        break;
    default:
        break;
    }
    cJSON_Delete(root);
}

static void handle_data_event(esp_mqtt_event_handle_t event)
{
    if (event->current_data_offset == 0) {
        s_in_len = 0;
        s_in_drop = false;
        s_in_kind = mqtt_proto_classify_topic(event->topic, (size_t) event->topic_len, s_device_id);
        if (event->total_data_len > (int) MQTT_PROTO_PAYLOAD_MAX) {
            ESP_LOGW(TAG, "payload of %d bytes exceeds the %d byte cap, dropped", event->total_data_len,
                     (int) MQTT_PROTO_PAYLOAD_MAX);
            s_in_drop = true;
        }
    }

    if (!s_in_drop && event->data_len > 0) {
        if (s_in_len + (size_t) event->data_len > MQTT_PROTO_PAYLOAD_MAX) {
            s_in_drop = true;
        } else {
            memcpy(s_in_payload + s_in_len, event->data, (size_t) event->data_len);
            s_in_len += (size_t) event->data_len;
            s_in_payload[s_in_len] = '\0';
        }
    }

    const bool complete = (event->current_data_offset + event->data_len) >= event->total_data_len;
    if (!complete) {
        return;
    }
    if (!s_in_drop) {
        dispatch_message(s_in_kind, s_in_payload, s_in_len);
    }
    s_in_len = 0;
    s_in_drop = false;
    s_in_kind = MQTT_PROTO_TOPIC_UNKNOWN;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void) handler_args;
    (void) base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) event_data;

    switch ((esp_mqtt_event_id_t) event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to broker");
        link_lock();
        s_connected = true;
        s_status = LINK_CONNECTED;
        s_backoff_ms = MQTT_LINK_BACKOFF_MIN_MS;
        s_pending_session = true;
        set_error_locked("");
        link_unlock();
        link_notify();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker");
        link_lock();
        s_connected = false;
        if (s_status != LINK_DISABLED) {
            s_status = s_net_up ? LINK_CONNECTING : LINK_WAITING;
        }
        // The retry site owns the backoff growth; this only schedules the next
        // attempt at the current interval.
        s_next_attempt_ms = now_ms() + s_backoff_ms;
        link_unlock();
        link_notify();
        break;

    case MQTT_EVENT_ERROR: {
        char text[MQTT_LINK_ERR_MAX] = "broker error";
        if (event->error_handle) {
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                snprintf(text, sizeof(text), "transport error (tls 0x%x, errno %d)",
                         (unsigned) (-event->error_handle->esp_tls_last_esp_err),
                         event->error_handle->esp_transport_sock_errno);
            } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                snprintf(text, sizeof(text), "connection refused by broker (code %d)",
                         (int) event->error_handle->connect_return_code);
            }
        }
        ESP_LOGW(TAG, "%s", text);
        link_lock();
        set_error_locked(text);
        if (s_status != LINK_DISABLED) {
            s_status = LINK_ERROR;
        }
        link_unlock();
        break;
    }

    case MQTT_EVENT_DATA:
        handle_data_event(event);
        break;

    default:
        break;
    }
}

// ---- client lifecycle (link task only) -------------------------------------

static void build_topics(void)
{
    mqtt_proto_build_topic(s_topic_state, sizeof(s_topic_state), s_device_id, "state");
    mqtt_proto_build_topic(s_topic_status, sizeof(s_topic_status), s_device_id, "status");
    mqtt_proto_build_topic(s_topic_info, sizeof(s_topic_info), s_device_id, "info");
    mqtt_proto_build_topic(s_topic_controllers, sizeof(s_topic_controllers), s_device_id, "controllers");
    mqtt_proto_build_topic(s_topic_set, sizeof(s_topic_set), s_device_id, "set");
    mqtt_proto_build_topic(s_topic_pair, sizeof(s_topic_pair), s_device_id, "pair");
    mqtt_proto_build_topic(s_topic_unpair, sizeof(s_topic_unpair), s_device_id, "unpair");
}

static void destroy_client(bool announce_offline)
{
    if (!s_client) {
        return;
    }
    if (announce_offline && s_connected) {
        // A clean disconnect does not fire the LWT, so say it explicitly.
        publish_raw(s_topic_status, "offline", 7, 1, true);
    }
    if (s_client_started) {
        esp_mqtt_client_stop(s_client);
    }
    esp_mqtt_client_destroy(s_client);
    s_client = nullptr;
    s_client_started = false;

    link_lock();
    s_connected = false;
    link_unlock();
}

static bool create_client(const link_cfg_t *cfg)
{
    snprintf(s_uri, sizeof(s_uri), "%s://%s:%u", cfg->tls ? "mqtts" : "mqtt", cfg->host, (unsigned) cfg->port);

    esp_mqtt_client_config_t config = {};
    config.broker.address.uri = s_uri;
    if (cfg->tls) {
        config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    config.credentials.client_id = s_device_id;
    if (cfg->user[0] != '\0') {
        config.credentials.username = cfg->user;
    }
    if (cfg->pass[0] != '\0') {
        config.credentials.authentication.password = cfg->pass;
    }
    config.session.keepalive = MQTT_LINK_KEEPALIVE_S;
    config.session.last_will.topic = s_topic_status;
    config.session.last_will.msg = "offline";
    config.session.last_will.msg_len = 7;
    config.session.last_will.qos = 1;
    config.session.last_will.retain = 1;
    // Our own exponential backoff drives reconnects (see service_connection).
    config.network.disable_auto_reconnect = true;
    config.network.timeout_ms = 10000;
    config.buffer.size = 1024;
    config.buffer.out_size = 1024;

    s_client = esp_mqtt_client_init(&config);
    if (!s_client) {
        ESP_LOGE(TAG, "client init failed");
        return false;
    }
    if (esp_mqtt_client_register_event(s_client, (esp_mqtt_event_id_t) ESP_EVENT_ANY_ID, mqtt_event_handler,
                                       nullptr) != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
        return false;
    }
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        ESP_LOGE(TAG, "client start failed");
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
        return false;
    }
    s_client_started = true;
    ESP_LOGI(TAG, "connecting to %s as %s", s_uri, s_device_id);
    return true;
}

static void service_connection(void)
{
    link_cfg_t cfg = {};
    bool       configured = false;
    bool       net_up = false;
    bool       connected = false;
    uint32_t   generation = 0;
    int64_t    next_attempt = 0;

    bool announce_factory_reset = false;

    link_lock();
    cfg = s_cfg;
    configured = s_cfg.host[0] != '\0';
    net_up = s_net_up;
    connected = s_connected;
    generation = s_cfg_generation;
    next_attempt = s_next_attempt_ms;
    announce_factory_reset = s_pending_factory_announce;
    s_pending_factory_announce = false;
    link_unlock();

    if (announce_factory_reset && s_client && connected) {
        // Drop the retained controller list so the next HMI that subscribes
        // does not inherit this strip's old owners. The teardown below publishes
        // the retained "offline".
        publish_raw(s_topic_controllers, "{\"paired\":[]}", 13, 1, true);
    }

    if (s_client && generation != s_client_generation) {
        ESP_LOGI(TAG, "broker settings changed, restarting the link");
        destroy_client(true);
    }

    if (!configured || !net_up) {
        if (s_client) {
            destroy_client(true);
        }
        link_lock();
        s_status = configured ? LINK_WAITING : LINK_DISABLED;
        link_unlock();
        return;
    }

    if (!s_client) {
        s_client_generation = generation;
        link_lock();
        s_status = LINK_CONNECTING;
        link_unlock();
        if (!create_client(&cfg)) {
            link_lock();
            s_status = LINK_ERROR;
            set_error_locked("client start failed");
            s_next_attempt_ms = now_ms() + s_backoff_ms;
            s_backoff_ms =
                (s_backoff_ms * 2 > MQTT_LINK_BACKOFF_MAX_MS) ? MQTT_LINK_BACKOFF_MAX_MS : s_backoff_ms * 2;
            link_unlock();
        }
        return;
    }

    if (!connected && now_ms() >= next_attempt) {
        ESP_LOGI(TAG, "retrying broker connection");
        link_lock();
        s_next_attempt_ms = now_ms() + s_backoff_ms;
        s_backoff_ms = (s_backoff_ms * 2 > MQTT_LINK_BACKOFF_MAX_MS) ? MQTT_LINK_BACKOFF_MAX_MS : s_backoff_ms * 2;
        link_unlock();
        if (esp_mqtt_client_reconnect(s_client) != ESP_OK) {
            // The client refused to redial (it can end up in a state this API
            // will not resume). Drop it; the next pass builds a fresh one.
            ESP_LOGW(TAG, "reconnect refused, recreating the client");
            destroy_client(false);
        }
    }
}

static void service_pairing(void)
{
    mqtt_pair_outcome_t outcome;
    link_lock();
    outcome = mqtt_pair_tick(&s_pair, now_ms());
    link_unlock();

    if (outcome.action == MQTT_PAIR_ACTION_REJECTED) {
        ESP_LOGW(TAG, "pairing window expired");
        led_control_indicator_flash(false);
    }
}

static void service_publishes(void)
{
    bool connected = false;
    bool session = false;
    bool controllers = false;

    link_lock();
    connected = s_connected;
    // Keep the flags until they can actually go out: a change made while the
    // broker is unreachable must still be published once we are back.
    if (connected) {
        session = s_pending_session;
        controllers = s_pending_controllers;
        s_pending_session = false;
        s_pending_controllers = false;
    }
    link_unlock();

    if (!connected || !s_client) {
        return;
    }

    if (session) {
        esp_mqtt_client_subscribe_single(s_client, s_topic_set, 1);
        esp_mqtt_client_subscribe_single(s_client, s_topic_pair, 1);
        esp_mqtt_client_subscribe_single(s_client, s_topic_unpair, 1);
        publish_info_doc();
        publish_raw(s_topic_status, "online", 6, 1, true);
        publish_controllers_doc();
        publish_state_doc("device", 0, false);
        link_lock();
        s_coalescer.last_publish_ms = now_ms();
        link_unlock();
        // The bench number for "what does the link cost": heap with Wi-Fi,
        // Matter and a live TLS session to the broker.
        ESP_LOGI(TAG, "session published; free heap %" PRIu32 " B, low water %" PRIu32 " B", esp_get_free_heap_size(),
                 esp_get_minimum_free_heap_size());
        return;
    }

    if (controllers) {
        publish_controllers_doc();
    }

    char     src[MQTT_PROTO_ID_MAX] = "";
    uint32_t seq = 0;
    bool     rejected = false;
    bool     due = false;

    link_lock();
    due = mqtt_coalescer_take(&s_coalescer, now_ms(), src, sizeof(src), &seq, &rejected);
    link_unlock();

    if (due) {
        publish_state_doc(src, seq, rejected);
    }
}

static uint32_t compute_wait_ms(void)
{
    const int64_t now = now_ms();
    uint32_t      wait = MQTT_LINK_IDLE_WAIT_MS;

    link_lock();
    const bool configured = s_cfg.host[0] != '\0';
    if (s_pending_session || s_pending_controllers) {
        wait = 0;
    } else if (s_connected) {
        const int32_t due = mqtt_coalescer_due_in(&s_coalescer, now);
        if (due == 0) {
            wait = 0;
        } else if (due > 0 && (uint32_t) due < wait) {
            wait = (uint32_t) due;
        }
    } else if (configured && s_net_up) {
        const int64_t remain = s_next_attempt_ms - now;
        wait = (remain <= 0) ? 0 : (uint32_t) ((remain < (int64_t) wait) ? remain : (int64_t) wait);
    }
    if (s_pair.state == MQTT_PAIR_WAITING_CODE) {
        const int64_t remain = s_pair.deadline_ms - now;
        const uint32_t pair_wait = (remain <= 0) ? 0 : (uint32_t) remain;
        if (pair_wait < wait) {
            wait = pair_wait;
        }
    }
    link_unlock();
    return wait;
}

static void mqtt_link_task(void *arg)
{
    (void) arg;
    for (;;) {
        const uint32_t wait = compute_wait_ms();
        ulTaskNotifyTake(pdTRUE, wait == 0 ? 0 : pdMS_TO_TICKS(wait));
        service_connection();
        service_pairing();
        service_publishes();
    }
}

// ---- public API ------------------------------------------------------------

void mqtt_link_init(void)
{
    if (s_ready) {
        return;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "mutex allocation failed; MQTT link disabled");
        return;
    }

    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK ||
        !mqtt_proto_device_id_from_mac(mac, s_device_id, sizeof(s_device_id))) {
        snprintf(s_device_id, sizeof(s_device_id), "led-000000");
    }
    build_topics();

    s_cfg.tls = true;
    s_cfg.port = MQTT_LINK_DEFAULT_PORT_TLS;
    load_settings();
    if (s_cfg.port == 0) {
        s_cfg.port = s_cfg.tls ? MQTT_LINK_DEFAULT_PORT_TLS : MQTT_LINK_DEFAULT_PORT_PLAIN;
    }
    s_status = (s_cfg.host[0] != '\0') ? LINK_WAITING : LINK_DISABLED;

    if (xTaskCreate(mqtt_link_task, "mqtt_link", MQTT_LINK_TASK_STACK, nullptr, MQTT_LINK_TASK_PRIO, &s_task) !=
        pdPASS) {
        ESP_LOGE(TAG, "task creation failed; MQTT link disabled");
        return;
    }

    s_ready = true;
    ESP_LOGI(TAG, "device id %s, broker %s", s_device_id, s_cfg.host[0] ? s_cfg.host : "(not configured)");
}

void mqtt_link_network_up(void)
{
    if (!s_ready) {
        return;
    }
    link_lock();
    s_net_up = true;
    s_next_attempt_ms = 0;
    s_backoff_ms = MQTT_LINK_BACKOFF_MIN_MS;
    link_unlock();
    link_notify();
}

void mqtt_link_network_down(void)
{
    if (!s_ready) {
        return;
    }
    link_lock();
    s_net_up = false;
    link_unlock();
    link_notify();
}

void mqtt_link_state_changed(const char *src)
{
    if (!s_ready) {
        return;
    }
    link_lock();
    const uint32_t seq = ++s_local_seq;
    mqtt_coalescer_mark(&s_coalescer, src ? src : "device", seq, false);
    link_unlock();
    link_notify();
}

esp_err_t mqtt_link_apply_config_json(const cJSON *root)
{
    if (!s_ready || !cJSON_IsObject(root)) {
        return ESP_OK;
    }

    const cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "mqtt_host");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "mqtt_port");
    const cJSON *user = cJSON_GetObjectItemCaseSensitive(root, "mqtt_user");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "mqtt_pass");
    const cJSON *tls = cJSON_GetObjectItemCaseSensitive(root, "mqtt_tls");

    if (!host && !port && !user && !pass && !tls) {
        return ESP_OK; // nothing MQTT-related in this config post
    }
    if ((host && !cJSON_IsString(host)) || (user && !cJSON_IsString(user)) || (pass && !cJSON_IsString(pass)) ||
        (port && !cJSON_IsNumber(port)) || (tls && !cJSON_IsBool(tls))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (host && strlen(host->valuestring) >= MQTT_LINK_HOST_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (user && strlen(user->valuestring) >= MQTT_LINK_USER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass && strlen(pass->valuestring) >= MQTT_LINK_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (port && (port->valuedouble < 1 || port->valuedouble > 65535)) {
        return ESP_ERR_INVALID_ARG;
    }

    link_lock();
    if (host) {
        snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", host->valuestring);
    }
    if (tls) {
        s_cfg.tls = cJSON_IsTrue(tls);
    }
    if (user) {
        snprintf(s_cfg.user, sizeof(s_cfg.user), "%s", user->valuestring);
    }
    // An empty password keeps the stored one, mirroring the AP password field.
    if (pass && pass->valuestring[0] != '\0') {
        snprintf(s_cfg.pass, sizeof(s_cfg.pass), "%s", pass->valuestring);
    }
    if (port) {
        s_cfg.port = (uint16_t) port->valuedouble;
    } else if (s_cfg.port == 0) {
        s_cfg.port = s_cfg.tls ? MQTT_LINK_DEFAULT_PORT_TLS : MQTT_LINK_DEFAULT_PORT_PLAIN;
    }
    esp_err_t err = save_settings_locked();
    s_cfg_generation++;
    s_backoff_ms = MQTT_LINK_BACKOFF_MIN_MS;
    s_next_attempt_ms = 0;
    set_error_locked("");
    s_status = (s_cfg.host[0] != '\0') ? (s_net_up ? LINK_CONNECTING : LINK_WAITING) : LINK_DISABLED;
    link_unlock();

    link_notify();
    return err;
}

void mqtt_link_add_state_json(cJSON *root, bool softap_admin)
{
    if (!root) {
        return;
    }

    link_cfg_t               cfg = {};
    mqtt_proto_controllers_t controllers = {};
    link_status_t            status = LINK_DISABLED;
    char                     error[MQTT_LINK_ERR_MAX] = "";
    bool                     pairing = false;

    if (s_ready) {
        link_lock();
        cfg = s_cfg;
        controllers = s_controllers;
        status = s_status;
        snprintf(error, sizeof(error), "%s", s_last_error);
        pairing = s_pair.state == MQTT_PAIR_WAITING_CODE;
        link_unlock();
    }

    cJSON_AddStringToObject(root, "mqtt_device_id", s_device_id);
    cJSON_AddBoolToObject(root, "mqtt_configured", cfg.host[0] != '\0');
    cJSON_AddStringToObject(root, "mqtt_status", status_text(status));
    cJSON_AddStringToObject(root, "mqtt_error", error);
    cJSON_AddBoolToObject(root, "mqtt_pairing", pairing);
    // Broker identity is administration data: SoftAP clients only, and the
    // password is never returned to anybody.
    cJSON_AddStringToObject(root, "mqtt_host", softap_admin ? cfg.host : "");
    cJSON_AddNumberToObject(root, "mqtt_port", softap_admin ? cfg.port : 0);
    cJSON_AddStringToObject(root, "mqtt_user", softap_admin ? cfg.user : "");
    cJSON_AddBoolToObject(root, "mqtt_tls", cfg.tls);
    cJSON_AddNumberToObject(root, "mqtt_max_controllers", MQTT_PROTO_MAX_CONTROLLERS);

    cJSON *array = cJSON_AddArrayToObject(root, "mqtt_controllers");
    if (!array) {
        return;
    }
    for (uint8_t i = 0; i < controllers.count && i < MQTT_PROTO_MAX_CONTROLLERS; ++i) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry) {
            return;
        }
        cJSON_AddStringToObject(entry, "id", controllers.items[i].id);
        cJSON_AddStringToObject(entry, "name", controllers.items[i].name);
        cJSON_AddItemToArray(array, entry);
    }
}

esp_err_t mqtt_link_unpair(const char *controller_id)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    char sanitized[MQTT_PROTO_ID_MAX] = "";
    if (!mqtt_proto_sanitize_id(controller_id ? controller_id : "", sanitized, sizeof(sanitized))) {
        return ESP_ERR_INVALID_ARG;
    }

    bool      removed = false;
    esp_err_t save_err = ESP_OK;
    link_lock();
    removed = mqtt_proto_controller_remove(&s_controllers, sanitized);
    if (removed) {
        save_err = save_controllers_locked();
        s_pending_controllers = true;
    }
    link_unlock();

    if (!removed) {
        return ESP_ERR_NOT_FOUND;
    }
    link_notify();
    return save_err;
}

void mqtt_link_prepare_factory_reset(void)
{
    if (!s_ready) {
        return;
    }

    link_lock();
    memset(&s_controllers, 0, sizeof(s_controllers));
    memset(&s_pair, 0, sizeof(s_pair));
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.tls = true;
    s_cfg.port = MQTT_LINK_DEFAULT_PORT_TLS;
    s_status = LINK_DISABLED;
    s_pending_factory_announce = true;
    s_cfg_generation++;
    link_unlock();

    // The link task owns the client handle, so ask it to clear the retained
    // topics and tear the connection down, then give it a moment before the
    // caller erases NVS and reboots. Best effort by design.
    link_notify();
    vTaskDelay(pdMS_TO_TICKS(300));
    led_control_indicator_clear();
}
