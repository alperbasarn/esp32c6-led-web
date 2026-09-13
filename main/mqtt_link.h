// MQTT link to the QNOB round HMI — the device side of docs/mqtt-contract.md.
//
// Owns its own task, mutex and NVS keys; talks to the LED core only through
// led_control.h. app_main.cpp calls the functions below and never reaches into
// the link's state.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Loads broker settings and the paired-controller list from NVS, derives the
// device id from the STA MAC, and starts the link task. Safe to call once,
// early in app_main, before Wi-Fi is up.
void mqtt_link_init(void);

// Station got / lost an IP. The link connects only while the station is up.
void mqtt_link_network_up(void);
void mqtt_link_network_down(void);

// A local change (web, Matter, schedule, timer) altered the LED state. `src`
// goes into the published `state` document; publishes are coalesced to at most
// 10/s. Cheap and non-blocking: safe from Matter callbacks and HTTP handlers.
void mqtt_link_state_changed(const char *src);

// Applies the `mqtt_*` fields of a POST /api/config body. The caller must have
// already enforced the SoftAP-admin boundary — broker settings are never
// accepted from a LAN client. Absent fields leave the current settings alone;
// an empty "mqtt_pass" keeps the stored password.
esp_err_t mqtt_link_apply_config_json(const cJSON *root);

// Adds the link's fields to GET /api/state. Host/port/user are exposed to
// SoftAP admins only; the broker password is never returned.
void mqtt_link_add_state_json(cJSON *root, bool softap_admin);

// Removes a paired controller and republishes the retained `controllers` topic.
esp_err_t mqtt_link_unpair(const char *controller_id);

// Clears the in-RAM controller list and broker settings and tells the broker
// this strip is going away. Call before erasing the NVS namespace; the NVS keys
// themselves live in the app settings namespace, so the existing factory reset
// erase already removes them.
void mqtt_link_prepare_factory_reset(void);

#ifdef __cplusplus
}
#endif
