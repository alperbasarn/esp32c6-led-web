#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <limits>
#include <netinet/in.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_control.h"
#include "model/color_model.h"
#include "model/net_model.h"
#include "model/schedule_model.h"
#include "model/led_model.h"
#include "model/render_model.h"
#include "model/led_ease.h"
#include "model/http_encoding.h"
#include "led_strip.h"
#include "lwip/inet.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mqtt_link.h"
#include "mqtt_proto.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "release_pubkey.h"

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <esp_matter.h>
#include <platform/PlatformManager.h>
#include <setup_payload/OnboardingCodesUtil.h>

#define APP_WIFI_SSID_PREFIX     "ESP32C6-LED-"
#define APP_WIFI_PASS_PREFIX     "LedSetup-"
#define APP_WIFI_CHANNEL         6
#define APP_WIFI_MAX_STA_CONN    4

#define APP_LED_GPIO             CONFIG_APP_LED_GPIO
#define APP_LED_MAX_PIXELS       CONFIG_APP_LED_MAX_PIXELS
#define APP_NVS_NAMESPACE        "led_cfg"
#define APP_POST_BODY_LIMIT      1024
#define APP_OTA_CHUNK_SIZE       4096
#define APP_RMT_RESOLUTION_HZ    (10 * 1000 * 1000)
#define APP_QR_CODE_MAX          128
#define APP_MANUAL_CODE_MAX      32
#define APP_QR_URL_MAX           256
#define APP_AUTO_UPDATE_STATUS_MAX 160
#define APP_AUTO_UPDATE_VERSION_MAX 32
#define APP_AUTO_UPDATE_URL_MAX  384
#define APP_AUTO_UPDATE_JSON_LIMIT 16384
#define APP_AUTO_UPDATE_SIG_LIMIT  256
#define APP_AUTO_UPDATE_INTERVAL_MS  ((uint64_t)CONFIG_APP_UPDATE_INTERVAL_HOURS * 60ULL * 60ULL * 1000ULL)
#define APP_AUTO_UPDATE_JITTER_MS    ((uint64_t)CONFIG_APP_UPDATE_INITIAL_JITTER_MIN * 60ULL * 1000ULL)
#define APP_UPDATE_RELEASE_REPO  CONFIG_APP_RELEASE_REPO
#define APP_UPDATE_ASSET_NAME    CONFIG_APP_RELEASE_ASSET_NAME
#define APP_UPDATE_MANIFEST_NAME CONFIG_APP_RELEASE_MANIFEST_NAME
#define APP_UPDATE_MANIFEST_SIG_NAME CONFIG_APP_RELEASE_MANIFEST_SIG_NAME
#define APP_UPDATE_USER_AGENT    "esp32c6-led-web"
#define APP_UPDATE_SHA256_HEX_LEN 64
#define APP_UPDATE_SELF_TEST_TIMEOUT_MS ((uint64_t)CONFIG_APP_UPDATE_SELF_TEST_TIMEOUT_S * 1000ULL)
#define APP_UPDATE_OTA_BUF_SIZE  1024
#define APP_MAX_SCHEDULES        8
#define APP_SNTP_SERVER          "pool.ntp.org"
#define APP_TZ_DEFAULT           "UTC0"

static const char *TAG = "matter_led";
static constexpr auto kCommissioningTimeoutSeconds = 300;
static constexpr uint16_t kDefaultColorTempMireds = 0x00fa;

using namespace esp_matter;
using namespace chip::app::Clusters;

static led_strip_handle_t s_led_strip = nullptr;
static SemaphoreHandle_t s_state_mutex = nullptr;
static SemaphoreHandle_t s_led_mutex = nullptr;
static TaskHandle_t s_effect_task = nullptr;
static SemaphoreHandle_t s_ota_mutex = nullptr;
static httpd_handle_t s_http_server = nullptr;
static esp_netif_t *s_ap_netif = nullptr;
static led_state_t s_led_state = {
    kLedDefaultCount,
    kLedDefaultRed,
    kLedDefaultGreen,
    kLedDefaultBlue,
    kLedDefaultBrightness,
    kLedDefaultPower,
    LED_EFFECT_SOLID,
};
static uint16_t s_last_render_count = 0;
// Gamma correction LUT: maps linear light intent (0..255) to WS2812B driver code
// (0..255). Owned/read on the render path; populated once at boot by
// init_gamma_lut() before effect_task starts. 256 bytes of .bss.
static uint8_t s_gamma_lut[256];
static uint16_t s_light_endpoint_id = 0;
static uint8_t s_matter_hue = 0;
static uint8_t s_matter_saturation = 0;
static uint16_t s_matter_x = kColorDefaultCurrentX;
static uint16_t s_matter_y = kColorDefaultCurrentY;
static uint16_t s_matter_temp_mireds = kDefaultColorTempMireds;
static bool s_syncing_matter = false;
static bool s_network_handlers_registered = false;
static char s_ap_ip[16] = "192.168.4.1";
static char s_sta_ip[16] = "";
static char s_sta_ssid[33] = "";
static char s_sta_bssid[18] = "";
static int s_sta_rssi = 0;
static uint8_t s_sta_channel = 0;
static uint16_t s_sta_last_disconnect_reason = 0;
static uint32_t s_sta_connect_count = 0;
static uint32_t s_sta_disconnect_count = 0;
static uint32_t s_sta_last_event_ms = 0;
static uint32_t s_sta_last_ip_ms = 0;
static char s_ap_ssid[33] = "";
static char s_ap_password[65] = "";
static char s_runtime_ap_ssid[33] = "";
static char s_runtime_ap_password[65] = "";
static char s_matter_qr_code[APP_QR_CODE_MAX] = "";
static char s_matter_manual_code[APP_MANUAL_CODE_MAX] = "";
static char s_matter_qr_url[APP_QR_URL_MAX] = "";
static uint32_t s_matter_commissioned_count = 0;
static uint32_t s_matter_last_event_ms = 0;
static bool s_auto_update_busy = false;
// When true (the default), the periodic published-update check auto-installs
// any newer release the device is in cohort for. Toggleable via captive
// portal Configuration tab. Persisted in NVS under led_cfg/auto_inst.
static bool s_auto_install_enabled = true;
static bool s_auto_update_available = false;
static char s_auto_update_status[APP_AUTO_UPDATE_STATUS_MAX] = "Published update checks are idle.";
static char s_auto_update_latest_version[APP_AUTO_UPDATE_VERSION_MAX] = "";
static char s_auto_update_asset_url[APP_AUTO_UPDATE_URL_MAX] = "";
static TaskHandle_t s_auto_update_task = nullptr;

// Time-based automatic on/off. Fixed schedules persist in NVS and require a
// valid wall clock (SNTP). The relative one-shot timer is in-RAM only and uses
// a monotonic esp_timer deadline so it works without any time sync.

static schedule_entry_t s_schedules[APP_MAX_SCHEDULES] = {};
static char s_tz[40] = APP_TZ_DEFAULT;
static int64_t s_relative_deadline_us = 0;  // 0 = inactive (monotonic esp_timer deadline)
static uint8_t s_relative_action = 0;       // 0=off, 1=on
static int32_t s_sched_last_fired_min[APP_MAX_SCHEDULES] = {
    -1, -1, -1, -1, -1, -1, -1, -1};        // epoch-minute of last fire (dedupe)

// Debounced persistence for control changes that can arrive in bursts (an MQTT
// `set` follows a knob drag at up to 10/s). The web and Matter paths still
// write through immediately; only the MQTT path defers, and the flush runs on
// the schedule task — never on the effect task.
#define APP_PERSIST_DEBOUNCE_MS 2000
static bool    s_persist_pending = false;
static int64_t s_persist_due_us = 0;

// Pairing feedback on the strip. The MQTT link only raises a flag here; the
// effect task copies it inside its existing s_state_mutex critical section and
// renders the pattern after releasing the lock, so the LED path never holds the
// state mutex and the caller never blocks on the strip.
typedef enum {
    LED_INDICATOR_NONE = 0,
    LED_INDICATOR_CODE,    // repeat N blinks while a pairing window is open
    LED_INDICATOR_SUCCESS, // one green flash
    LED_INDICATOR_FAILURE, // one red flash
} led_indicator_mode_t;

#define APP_INDICATOR_BLINK_ON_MS  160
#define APP_INDICATOR_BLINK_OFF_MS 220
#define APP_INDICATOR_GAP_MS       1200
#define APP_INDICATOR_FLASH_MS     500
// The indicator lights every pixel, so it follows the user's own brightness
// (their supply is sized for it) inside a visible floor and a modest ceiling
// rather than blasting a long strip at full white.
#define APP_INDICATOR_MIN_LEVEL    48
#define APP_INDICATOR_MAX_LEVEL    160

static led_indicator_mode_t s_indicator_mode = LED_INDICATOR_NONE;
static uint8_t              s_indicator_code = 0;
static int64_t              s_indicator_started_us = 0;

// The web UI, embedded gzip-compressed from main/web/index.html by
// main/CMakeLists.txt. Only the compressed representation is stored: keeping
// an uncompressed copy as well would cost more flash than the raw literal
// this replaced, which was the point of compressing it. root_get_handler()
// therefore has to negotiate, and a client that refuses gzip gets a 406
// rather than a page -- see main/model/http_encoding.h.
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");

static void copy_string_value(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return;
    }

    if (!src) {
        dest[0] = '\0';
        return;
    }

    size_t copy_len = std::min(dest_size - 1, std::strlen(src));
    std::memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
}

typedef struct {
    char version[APP_AUTO_UPDATE_VERSION_MAX];
    char asset_url[APP_AUTO_UPDATE_URL_MAX];
    uint8_t sha256[32];                 // expected SHA-256 of the .bin
    size_t  asset_size;                 // expected byte size of the .bin (0 if unknown)
    uint8_t cohort_percent;             // rollout percent, 0..100 (default 100 if absent)
    bool    has_sha256;
} published_update_info_t;

static void set_auto_update_state(bool busy, bool available, const char *latest_version, const char *asset_url,
                                  const char *status)
{
    if (!s_state_mutex) {
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_auto_update_busy = busy;
    s_auto_update_available = available;
    copy_string_value(s_auto_update_latest_version, sizeof(s_auto_update_latest_version), latest_version);
    copy_string_value(s_auto_update_asset_url, sizeof(s_auto_update_asset_url), asset_url);
    copy_string_value(s_auto_update_status, sizeof(s_auto_update_status), status);
    xSemaphoreGive(s_state_mutex);
}

static bool is_auto_install_enabled()
{
    if (!s_state_mutex) {
        return s_auto_install_enabled;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool v = s_auto_install_enabled;
    xSemaphoreGive(s_state_mutex);
    return v;
}

static void set_auto_install_enabled(bool enabled)
{
    if (!s_state_mutex) {
        s_auto_install_enabled = enabled;
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_auto_install_enabled = enabled;
    xSemaphoreGive(s_state_mutex);
}

static bool parse_version_parts(const char *text, int *parts, size_t part_count, size_t *used_count)
{
    if (!parts || part_count == 0) {
        return false;
    }

    std::fill(parts, parts + part_count, 0);
    size_t used = 0;
    bool found_any = false;
    const char *cursor = text ? text : "";
    while (*cursor != '\0' && used < part_count) {
        while (*cursor != '\0' && !std::isdigit(static_cast<unsigned char>(*cursor))) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        long value = 0;
        while (*cursor != '\0' && std::isdigit(static_cast<unsigned char>(*cursor))) {
            value = value * 10L + (*cursor - '0');
            ++cursor;
        }
        parts[used++] = static_cast<int>(std::clamp<long>(value, 0L, 999999L));
        found_any = true;
    }

    if (used_count) {
        *used_count = used;
    }
    return found_any;
}

static int compare_version_strings(const char *lhs, const char *rhs)
{
    int lhs_parts[6] = {};
    int rhs_parts[6] = {};
    size_t lhs_used = 0;
    size_t rhs_used = 0;
    bool lhs_ok = parse_version_parts(lhs, lhs_parts, sizeof(lhs_parts) / sizeof(lhs_parts[0]), &lhs_used);
    bool rhs_ok = parse_version_parts(rhs, rhs_parts, sizeof(rhs_parts) / sizeof(rhs_parts[0]), &rhs_used);

    if (lhs_ok && rhs_ok) {
        size_t count = std::max(lhs_used, rhs_used);
        for (size_t index = 0; index < count; ++index) {
            if (lhs_parts[index] < rhs_parts[index]) {
                return -1;
            }
            if (lhs_parts[index] > rhs_parts[index]) {
                return 1;
            }
        }
        return 0;
    }

    return std::strcmp(lhs ? lhs : "", rhs ? rhs : "");
}

static void normalize_release_version(const char *tag, char *output, size_t output_len)
{
    if (!output || output_len == 0) {
        return;
    }

    if (!tag) {
        output[0] = '\0';
        return;
    }

    const char *normalized = tag;
    if ((tag[0] == 'v' || tag[0] == 'V') && std::isdigit(static_cast<unsigned char>(tag[1]))) {
        normalized = tag + 1;
    }
    copy_string_value(output, output_len, normalized);
}

// Stable "latest" download URL pattern. GitHub redirects to the actual asset
// at S3; esp_http_client follows 30x by default.
static void build_release_url(char *out, size_t out_size, const char *asset_name)
{
    std::snprintf(out, out_size, "https://github.com/%s/releases/latest/download/%s",
                  APP_UPDATE_RELEASE_REPO, asset_name);
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static bool hex_decode(const char *hex, uint8_t *out, size_t out_len)
{
    if (!hex || !out) return false;
    if (std::strlen(hex) != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

// Device cohort id: stable [0, 99] derived from the Wi-Fi STA MAC. Used to
// decide whether this device participates in a partial rollout. We do NOT
// hash with any secret here — the goal is just deterministic bucketing.
static uint8_t device_cohort_id()
{
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        return 0;
    }
    // FNV-1a 32-bit over the 6-byte MAC.
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; ++i) {
        h ^= mac[i];
        h *= 16777619u;
    }
    return (uint8_t)(h % 100u);
}

// Verify an ECDSA-P256-SHA256 signature (DER-encoded) over `msg` using the
// PEM-encoded public key embedded in release_pubkey.h. Returns ESP_OK on
// valid signature, ESP_ERR_INVALID_STATE if no key configured.
static esp_err_t verify_manifest_signature(const uint8_t *msg, size_t msg_len,
                                            const uint8_t *sig, size_t sig_len)
{
#if CONFIG_APP_OTA_SIG_VERIFY
    // A production build compiled with signature verification MUST embed a key.
    // Catch an empty release_pubkey.h at build time so it can never ship.
    static_assert(sizeof(kReleasePubKeyPem) > 1,
                  "release_pubkey.h has no key but CONFIG_APP_OTA_SIG_VERIFY is enabled");
    if (kReleasePubKeyPemLen == 0) {
        ESP_LOGE(TAG, "No release public key embedded but sig-verify enabled — refusing.");
        return ESP_ERR_INVALID_STATE;
    }
    if (!msg || !sig || sig_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t hash[32];
    int rc = mbedtls_sha256(msg, msg_len, hash, 0);
    if (rc != 0) {
        return ESP_FAIL;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    rc = mbedtls_pk_parse_public_key(&pk,
                                     reinterpret_cast<const unsigned char *>(kReleasePubKeyPem),
                                     kReleasePubKeyPemLen + 1);
    if (rc != 0) {
        ESP_LOGE(TAG, "mbedtls_pk_parse_public_key failed: -0x%04x", -rc);
        mbedtls_pk_free(&pk);
        return ESP_FAIL;
    }

    rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sig_len);
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        ESP_LOGE(TAG, "manifest signature verify failed: -0x%04x", -rc);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
#else
    (void) msg; (void) msg_len; (void) sig; (void) sig_len;
    ESP_LOGW(TAG, "CONFIG_APP_OTA_SIG_VERIFY=n — manifest signature check skipped.");
    return ESP_OK;
#endif
}

// IDF's streaming HTTP client (open + fetch_headers + read) doesn't auto-
// follow redirects, but GitHub's /releases/latest/download/... URLs return
// 302 to an S3 asset URL. Follow up to 5 redirects manually.
static constexpr int kMaxHttpRedirects = 5;

static esp_err_t fetch_https_bytes(const char *url, uint8_t **buf_out, size_t *len_out, size_t max_size,
                                    const char *accept)
{
    if (!url || !buf_out || !len_out || max_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    *buf_out = nullptr;
    *len_out = 0;

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 15000;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.user_agent = APP_UPDATE_USER_AGENT;
    config.keep_alive_enable = true;
    // S3 (the GitHub-release redirect target) sends ~2 KB response headers
    // AND its signed redirect URL is ~1.5 KB by itself — both the RX header
    // buffer and the TX request-line buffer need to be much larger than
    // IDF's 512-byte defaults.
    config.buffer_size = 8192;
    config.buffer_size_tx = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }
    if (accept) {
        esp_http_client_set_header(client, "Accept", accept);
    }

    // GitHub occasionally resets the TLS connection (MBEDTLS_ERR_NET_CONN_RESET)
    // on rapid back-to-back requests. Treat open/fetch_headers as retryable.
    constexpr int kMaxTransientRetries = 2;
    int redirects = 0;
    int retries = 0;
    int status_code = 0;
    esp_err_t last_err = ESP_OK;
    while (true) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            last_err = err;
            if (++retries <= kMaxTransientRetries) {
                ESP_LOGW(TAG, "HTTP open failed (%s); retry %d/%d after backoff",
                         esp_err_to_name(err), retries, kMaxTransientRetries);
                vTaskDelay(pdMS_TO_TICKS(750));
                continue;
            }
            esp_http_client_cleanup(client);
            return err;
        }
        int headers_rc = esp_http_client_fetch_headers(client);
        if (headers_rc < 0) {
            last_err = ESP_FAIL;
            if (++retries <= kMaxTransientRetries) {
                ESP_LOGW(TAG, "fetch_headers failed; retry %d/%d after backoff",
                         retries, kMaxTransientRetries);
                esp_http_client_close(client);
                vTaskDelay(pdMS_TO_TICKS(750));
                continue;
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        status_code = esp_http_client_get_status_code(client);
        if (status_code == 301 || status_code == 302 || status_code == 303 ||
            status_code == 307 || status_code == 308) {
            if (++redirects > kMaxHttpRedirects) {
                ESP_LOGW(TAG, "Too many redirects fetching %s", url);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }
            // set_redirection() reads Location and updates the client URL;
            // the next open() targets the redirected endpoint.
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }
        break;
    }
    (void) last_err;

    if (status_code != 200) {
        ESP_LOGW(TAG, "HTTP %d for %s", status_code, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint8_t *buf = static_cast<uint8_t *>(malloc(max_size + 1));
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (true) {
        if (total >= max_size) {
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_HTTP_FETCH_HEADER;
        }
        int read = esp_http_client_read(client, reinterpret_cast<char *>(buf + total), max_size - total);
        if (read < 0) {
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (read == 0) break;
        total += static_cast<size_t>(read);
    }

    buf[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    *buf_out = buf;
    *len_out = total;
    return ESP_OK;
}

static esp_err_t fetch_latest_published_update(published_update_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }

    info->version[0] = '\0';
    info->asset_url[0] = '\0';
    info->asset_size = 0;
    info->cohort_percent = 100;
    info->has_sha256 = false;
    std::memset(info->sha256, 0, sizeof(info->sha256));

    char manifest_url[APP_AUTO_UPDATE_URL_MAX];
    char sig_url[APP_AUTO_UPDATE_URL_MAX];
    build_release_url(manifest_url, sizeof(manifest_url), APP_UPDATE_MANIFEST_NAME);
    build_release_url(sig_url, sizeof(sig_url), APP_UPDATE_MANIFEST_SIG_NAME);

    uint8_t *manifest_bytes = nullptr;
    size_t   manifest_len = 0;
    esp_err_t err = fetch_https_bytes(manifest_url, &manifest_bytes, &manifest_len,
                                       APP_AUTO_UPDATE_JSON_LIMIT, "application/json");
    if (err != ESP_OK) {
        return err;
    }

#if CONFIG_APP_OTA_SIG_VERIFY
    // Signature verification is compiled in, so an update can NEVER be trusted
    // without a key. If the embedded key is empty we refuse rather than silently
    // installing an unverified image (the old `if (len > 0)` guard skipped the
    // check entirely in that case).
    if (kReleasePubKeyPemLen == 0) {
        ESP_LOGE(TAG, "CONFIG_APP_OTA_SIG_VERIFY=y but no release public key embedded — refusing update");
        free(manifest_bytes);
        return ESP_ERR_INVALID_STATE;
    }
    {
        uint8_t *sig_bytes = nullptr;
        size_t   sig_len = 0;
        err = fetch_https_bytes(sig_url, &sig_bytes, &sig_len, APP_AUTO_UPDATE_SIG_LIMIT, "application/octet-stream");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not fetch manifest signature: %s", esp_err_to_name(err));
            free(manifest_bytes);
            // Distinct from "asset URL missing" so the UI message is accurate.
            return ESP_ERR_HTTP_CONNECT;
        }
        err = verify_manifest_signature(manifest_bytes, manifest_len, sig_bytes, sig_len);
        free(sig_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Manifest signature invalid — refusing");
            free(manifest_bytes);
            return ESP_ERR_INVALID_CRC;
        }
    }
#endif

    cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(manifest_bytes), manifest_len);
    free(manifest_bytes);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *chip = cJSON_GetObjectItemCaseSensitive(root, "chip");
    cJSON *app_obj = cJSON_GetObjectItemCaseSensitive(root, "app");
    if (!cJSON_IsString(version) || !cJSON_IsString(chip) || !cJSON_IsObject(app_obj)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (std::strcmp(chip->valuestring, "esp32c6") != 0) {
        ESP_LOGW(TAG, "Manifest chip=%s does not match esp32c6", chip->valuestring);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_VERSION;
    }

    normalize_release_version(version->valuestring, info->version, sizeof(info->version));

    cJSON *url = cJSON_GetObjectItemCaseSensitive(app_obj, "url");
    cJSON *sha = cJSON_GetObjectItemCaseSensitive(app_obj, "sha256");
    cJSON *size = cJSON_GetObjectItemCaseSensitive(app_obj, "size");
    if (!cJSON_IsString(version) || version->valuestring[0] == '\0' ||
        !cJSON_IsString(url) || url->valuestring[0] == '\0' ||
        !cJSON_IsString(sha) || std::strlen(sha->valuestring) != APP_UPDATE_SHA256_HEX_LEN ||
        !cJSON_IsNumber(size) || !std::isfinite(size->valuedouble) || size->valuedouble <= 0.0 ||
        std::floor(size->valuedouble) != size->valuedouble ||
        size->valuedouble > static_cast<double>(std::numeric_limits<size_t>::max()) ||
        !hex_decode(sha->valuestring, info->sha256, sizeof(info->sha256))) {
        ESP_LOGW(TAG, "Manifest app metadata is incomplete or invalid");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    copy_string_value(info->asset_url, sizeof(info->asset_url), url->valuestring);
    info->has_sha256 = true;
    info->asset_size = static_cast<size_t>(size->valuedouble);

    cJSON *rollout = cJSON_GetObjectItemCaseSensitive(root, "rollout");
    if (cJSON_IsObject(rollout)) {
        cJSON *pct = cJSON_GetObjectItemCaseSensitive(rollout, "percent");
        if (cJSON_IsNumber(pct)) {
            double p = pct->valuedouble;
            if (p < 0) p = 0;
            if (p > 100) p = 100;
            info->cohort_percent = (uint8_t) p;
        }
    }

    cJSON_Delete(root);
    return info->asset_url[0] != '\0' ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// Probation marker — persisted across reboots so we can implement an
// N-strike rollback policy. Lives in its own NVS namespace so factory reset
// of led_cfg doesn't disturb it.
//
//   slot    : "ota_0" / "ota_1" — partition label of the image on probation
//   ver     : manifest.version of that image (debug)
//   strikes : boot attempts so far. > APP_OTA_MAX_STRIKES → rollback.
#define APP_OTA_HEALTH_NS          "ota_health"
#define APP_OTA_HEALTH_KEY_SLOT    "slot"
#define APP_OTA_HEALTH_KEY_VER     "ver"
#define APP_OTA_HEALTH_KEY_STRIKES "strikes"
// manual=1 marks an image installed via the "Install From File" upload. Manual
// images self-test on Matter readiness alone (no STA required) so a bench-flash
// on a device that never joins home Wi-Fi is not rolled back. Absent key => 0.
#define APP_OTA_HEALTH_KEY_MANUAL  "manual"
#define APP_OTA_MAX_STRIKES        2

static esp_err_t ota_health_clear()
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(APP_OTA_HEALTH_NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

// `manual` (optional out-param, may be nullptr) reports whether the marker was
// written by a manual "Install From File" upload.
static esp_err_t ota_health_read(char *slot, size_t slot_len, char *ver, size_t ver_len, uint8_t *strikes,
                                 bool *manual)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(APP_OTA_HEALTH_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;  // ESP_ERR_NVS_NOT_FOUND if no marker yet
    }
    size_t s_len = slot_len;
    size_t v_len = ver_len;
    if (slot && slot_len) slot[0] = '\0';
    if (ver && ver_len)  ver[0] = '\0';
    if (strikes) *strikes = 0;
    if (manual) *manual = false;

    err = nvs_get_str(h, APP_OTA_HEALTH_KEY_SLOT, slot, &s_len);
    if (err != ESP_OK) { nvs_close(h); return err; }
    err = nvs_get_str(h, APP_OTA_HEALTH_KEY_VER, ver, &v_len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) { nvs_close(h); return err; }
    if (strikes) {
        uint8_t v = 0;
        err = nvs_get_u8(h, APP_OTA_HEALTH_KEY_STRIKES, &v);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            v = 0;
            err = ESP_OK;
        }
        *strikes = v;
    }
    if (manual) {
        // Absent for published OTA and for pre-existing markers; treat as false.
        uint8_t v = 0;
        if (nvs_get_u8(h, APP_OTA_HEALTH_KEY_MANUAL, &v) == ESP_OK) {
            *manual = v != 0;
        }
    }
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t ota_health_write(const char *slot, const char *ver, uint8_t strikes, bool manual)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(APP_OTA_HEALTH_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, APP_OTA_HEALTH_KEY_SLOT, slot ? slot : "");
    nvs_set_str(h, APP_OTA_HEALTH_KEY_VER, ver ? ver : "");
    nvs_set_u8(h, APP_OTA_HEALTH_KEY_STRIKES, strikes);
    nvs_set_u8(h, APP_OTA_HEALTH_KEY_MANUAL, manual ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Confirm a *markerless* running image if the bootloader left it
// PENDING_VERIFY. Such an image is NOT under our 2-strike probation: a
// USB/JTAG bench flash, an image already promoted by a prior successful
// self-test, a stale marker that references a different slot, or (rarely) a
// published OTA whose probation-marker write was lost. With
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y the bootloader leaves a freshly
// booted image in ESP_OTA_IMG_PENDING_VERIFY, and the running app MUST confirm
// itself via esp_ota_mark_app_valid_cancel_rollback() or (a) the bootloader
// rolls it back on the next reset and (b) every later esp_ota_begin() fails
// with ESP_ERR_OTA_ROLLBACK_INVALID_STATE — OTA is then permanently blocked.
// Nothing else confirms these images (self_test_task never marks valid), so we
// do it here. The image already passed SHA-256 and IDF image validation before
// the bootloader jumped into it, so confirming is safe. We only ever act on a
// PENDING_VERIFY image and never re-touch a VALID/UNDEFINED one; a failed state
// read is a silent no-op.
static void confirm_running_if_pending(const esp_partition_t *running)
{
    if (!running) {
        return;
    }
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_err_t serr = esp_ota_get_state_partition(running, &st);
    if (serr != ESP_OK) {
        // Can't determine the otadata state — leave rollback state untouched.
        ESP_LOGD(TAG, "OTA: esp_ota_get_state_partition(%s) failed: %s — "
                      "leaving rollback state untouched",
                 running->label, esp_err_to_name(serr));
        return;
    }
    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;  // already VALID/UNDEFINED/etc. — never touch a non-pending image
    }
    esp_err_t mv = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "OTA: markerless image on %s was PENDING_VERIFY — confirmed "
                  "valid to re-enable OTA (%s)",
             running->label, esp_err_to_name(mv));
}

// Called very early in app_main, right after NVS init. Manages the
// probationary boot lifecycle: if the running image is under probation,
// either roll back (too many failed attempts), or increment the strike
// counter and mark the image valid so the bootloader's built-in 1-strike
// rollback doesn't fire ahead of us.
static void init_ota_probation()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return;
    }

    char marker_slot[16] = "";
    char marker_ver[APP_AUTO_UPDATE_VERSION_MAX] = "";
    uint8_t strikes = 0;
    bool marker_manual = false;
    esp_err_t err = ota_health_read(marker_slot, sizeof(marker_slot),
                                    marker_ver, sizeof(marker_ver), &strikes, &marker_manual);
    if (err == ESP_ERR_NVS_NOT_FOUND || marker_slot[0] == '\0') {
        // No probation marker for this boot: the image is not under our 2-strike
        // probation (USB/JTAG flash, an image already promoted by a prior
        // self-test, or a fresh OTA whose marker write was lost). We do NOT
        // strike-count it. But if the bootloader left it PENDING_VERIFY it must
        // still be confirmed, or every future esp_ota_begin() fails with
        // ESP_ERR_OTA_ROLLBACK_INVALID_STATE and OTA is permanently blocked.
        // self_test_task never marks valid, so confirm it here.
        confirm_running_if_pending(running);
        return;
    }

    if (std::strcmp(marker_slot, running->label) != 0) {
        // Marker references a different slot than what's running. Either the
        // image was swapped by USB flash / manual revert, or the marker is
        // stale from a previous run. Clear it.
        ESP_LOGI(TAG, "OTA probation marker is stale (slot=%s, running=%s) — clearing",
                 marker_slot, running->label);
        ota_health_clear();
        // The running slot is not the one under probation, so it is
        // markerless-equivalent: same permanent-OTA-brick risk if it booted
        // PENDING_VERIFY (e.g. USB-flashed ota_0 with a leftover ota_1 marker).
        confirm_running_if_pending(running);
        return;
    }

    uint8_t next_strikes = strikes + 1;
    ESP_LOGI(TAG, "OTA probation: slot=%s ver=%s attempt=%u (max=%u)",
             marker_slot, marker_ver, next_strikes, APP_OTA_MAX_STRIKES);

    if (next_strikes > APP_OTA_MAX_STRIKES) {
        ESP_LOGE(TAG, "OTA probation: too many failed attempts — rolling back");
        const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
        if (other) {
            esp_err_t set_err = esp_ota_set_boot_partition(other);
            ESP_LOGW(TAG, "esp_ota_set_boot_partition(%s): %s",
                     other->label, esp_err_to_name(set_err));
        }
        ota_health_clear();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }

    // Persist the bumped strike count BEFORE marking valid. Order matters: a
    // crash between the increment and mark_valid still gives us a higher
    // strike count on the next boot. Preserve the manual flag across the
    // rewrite so self_test_task still relaxes the STA requirement for manual
    // images on this boot.
    ota_health_write(marker_slot, marker_ver, next_strikes, marker_manual);

    // Neuter the bootloader's 1-strike rollback so we can manage attempts
    // ourselves. self_test_task will clear the marker on success.
    esp_err_t mv = esp_ota_mark_app_valid_cancel_rollback();
    if (mv != ESP_OK && mv != ESP_ERR_NOT_SUPPORTED) {
        // Not in PENDING_VERIFY (image was already valid) — fine, ignore.
        ESP_LOGD(TAG, "mark_app_valid: %s", esp_err_to_name(mv));
    }
}

// Stream-download the firmware image into the inactive OTA partition while
// computing SHA-256 along the way. Returns ESP_OK only if the computed digest
// matches `expected_sha256` (when provided) and the image passes IDF validation.
static esp_err_t install_https_ota_with_verify(const published_update_info_t *info)
{
    if (!info || info->asset_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t http_config = {};
    http_config.url = info->asset_url;
    http_config.timeout_ms = 30000;
    http_config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.user_agent = APP_UPDATE_USER_AGENT;
    // S3 (redirect target for GitHub release assets) sends ~2 KB response
    // headers and its signed redirect URL is ~1.5 KB; both buffers need to
    // be much larger than IDF's 512-byte defaults.
    http_config.buffer_size = 8192;
    http_config.buffer_size_tx = 4096;
    http_config.keep_alive_enable = true;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;

    // esp_https_ota has no true resume — each attempt re-downloads from the
    // start — but GitHub/S3 occasionally reset the TLS connection mid-transfer.
    // Mirror fetch_https_bytes' bounded retry so a transient network blip does
    // not fail the whole install. Every failed attempt aborts (or finishes) its
    // handle before the next begin() so no partition write is left dangling.
    constexpr int kMaxOtaAttempts = 3;
    esp_https_ota_handle_t handle = nullptr;
    esp_err_t err = ESP_FAIL;
    int image_len = 0;
    for (int attempt = 1; attempt <= kMaxOtaAttempts; ++attempt) {
        handle = nullptr;
        err = esp_https_ota_begin(&ota_config, &handle);
        if (err != ESP_OK || handle == nullptr) {
            ESP_LOGE(TAG, "esp_https_ota_begin failed: %s (attempt %d/%d)",
                     esp_err_to_name(err), attempt, kMaxOtaAttempts);
            if (handle) {
                esp_https_ota_abort(handle);
                handle = nullptr;
            }
            err = (err == ESP_OK) ? ESP_FAIL : err;
            if (attempt < kMaxOtaAttempts) {
                vTaskDelay(pdMS_TO_TICKS(750));
                continue;
            }
            return err;
        }

        while (true) {
            err = esp_https_ota_perform(handle);
            if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
                break;
            }
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_https_ota_perform failed: %s (attempt %d/%d)",
                     esp_err_to_name(err), attempt, kMaxOtaAttempts);
            esp_https_ota_abort(handle);
            handle = nullptr;
            if (attempt < kMaxOtaAttempts) {
                vTaskDelay(pdMS_TO_TICKS(750));
                continue;
            }
            return err;
        }

        if (!esp_https_ota_is_complete_data_received(handle)) {
            ESP_LOGE(TAG, "OTA stream ended before all data received (attempt %d/%d)",
                     attempt, kMaxOtaAttempts);
            esp_https_ota_abort(handle);
            handle = nullptr;
            err = ESP_FAIL;
            if (attempt < kMaxOtaAttempts) {
                vTaskDelay(pdMS_TO_TICKS(750));
                continue;
            }
            return err;
        }

        image_len = esp_https_ota_get_image_size(handle);
        // esp_https_ota_finish releases the handle (even on error), so we do
        // NOT abort it afterwards; just clear our copy and retry from begin().
        err = esp_https_ota_finish(handle);
        handle = nullptr;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_https_ota_finish failed: %s (attempt %d/%d)",
                     esp_err_to_name(err), attempt, kMaxOtaAttempts);
            if (attempt < kMaxOtaAttempts) {
                vTaskDelay(pdMS_TO_TICKS(750));
                continue;
            }
            return err;
        }
        // Download + write + finish all succeeded — stop retrying and fall
        // through to the size / SHA-256 read-back verification below.
        break;
    }

    if (image_len <= 0) {
        ESP_LOGE(TAG, "Downloaded image has zero size");
        return ESP_FAIL;
    }
    if (info->asset_size != 0 && (size_t) image_len != info->asset_size) {
        ESP_LOGE(TAG, "Image size %d does not match manifest %u", image_len, (unsigned) info->asset_size);
        // Roll back boot partition to the running one before bailing.
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running) {
            esp_ota_set_boot_partition(running);
        }
        return ESP_ERR_INVALID_SIZE;
    }

    if (info->has_sha256) {
        // Read the freshly written partition back and SHA-256 it.
        const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
        if (!next) {
            ESP_LOGE(TAG, "Could not locate the just-written OTA partition");
            return ESP_FAIL;
        }

        mbedtls_sha256_context sha_ctx;
        mbedtls_sha256_init(&sha_ctx);
        if (mbedtls_sha256_starts(&sha_ctx, 0) != 0) {
            mbedtls_sha256_free(&sha_ctx);
            return ESP_FAIL;
        }

        uint8_t *buf = static_cast<uint8_t *>(malloc(APP_UPDATE_OTA_BUF_SIZE));
        if (!buf) {
            mbedtls_sha256_free(&sha_ctx);
            return ESP_ERR_NO_MEM;
        }
        size_t remaining = (size_t) image_len;
        size_t offset = 0;
        while (remaining > 0) {
            size_t chunk = remaining < APP_UPDATE_OTA_BUF_SIZE ? remaining : APP_UPDATE_OTA_BUF_SIZE;
            esp_err_t rerr = esp_partition_read(next, offset, buf, chunk);
            if (rerr != ESP_OK) {
                free(buf);
                mbedtls_sha256_free(&sha_ctx);
                return rerr;
            }
            if (mbedtls_sha256_update(&sha_ctx, buf, chunk) != 0) {
                free(buf);
                mbedtls_sha256_free(&sha_ctx);
                return ESP_FAIL;
            }
            offset += chunk;
            remaining -= chunk;
        }
        free(buf);

        uint8_t actual[32];
        if (mbedtls_sha256_finish(&sha_ctx, actual) != 0) {
            mbedtls_sha256_free(&sha_ctx);
            return ESP_FAIL;
        }
        mbedtls_sha256_free(&sha_ctx);

        if (std::memcmp(actual, info->sha256, sizeof(actual)) != 0) {
            ESP_LOGE(TAG, "OTA SHA-256 mismatch — reverting boot partition");
            const esp_partition_t *running = esp_ota_get_running_partition();
            if (running) {
                esp_ota_set_boot_partition(running);
            }
            return ESP_ERR_INVALID_CRC;
        }
        ESP_LOGI(TAG, "OTA SHA-256 verified (%d bytes)", image_len);
    } else {
        ESP_LOGW(TAG, "Manifest has no sha256 — installed without integrity check");
    }

    // Write the probation marker so the next boot enters the 2-strike state
    // machine. esp_https_ota_finish has already pointed the boot target at
    // the new slot, so esp_ota_get_next_update_partition(NULL) returns that
    // slot (the one we just wrote).
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (target) {
        // Published update: manual=false, so self-test requires STA + Matter.
        esp_err_t herr = ota_health_write(target->label, info->version, 0, false);
        if (herr != ESP_OK) {
            ESP_LOGW(TAG, "ota_health_write failed: %s (rollback safety degraded)",
                     esp_err_to_name(herr));
        }
    }

    return ESP_OK;
}

// Modes for the published-update worker. `kInstallForced` is reserved for the
// user's explicit Install Now button — it bypasses cohort gating because user
// action is a stronger signal than a rollout percent.
enum class published_update_mode_t {
    kCheckOnly,       // periodic check: refresh available version & status, never install
    kInstallForced,   // user clicked Install Now: install if newer, regardless of cohort
};

static published_update_mode_t s_pending_update_mode = published_update_mode_t::kCheckOnly;

static void set_pending_update_mode(published_update_mode_t mode)
{
    if (!s_state_mutex) {
        s_pending_update_mode = mode;
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_pending_update_mode = mode;
    xSemaphoreGive(s_state_mutex);
}

static published_update_mode_t take_pending_update_mode()
{
    if (!s_state_mutex) {
        published_update_mode_t mode = s_pending_update_mode;
        s_pending_update_mode = published_update_mode_t::kCheckOnly;
        return mode;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    published_update_mode_t mode = s_pending_update_mode;
    s_pending_update_mode = published_update_mode_t::kCheckOnly;
    xSemaphoreGive(s_state_mutex);
    return mode;
}

static void run_published_update_check(published_update_mode_t mode)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *current_version = app_desc ? app_desc->version : "unknown";

    if (s_sta_ip[0] == '\0') {
        set_auto_update_state(false, false, "", "", "Waiting for LAN Wi-Fi before checking published updates.");
        return;
    }

    if (!s_ota_mutex || xSemaphoreTake(s_ota_mutex, 0) != pdTRUE) {
        set_auto_update_state(false, false, "", "", "OTA is busy, so the published update check was skipped.");
        return;
    }

    set_auto_update_state(true, false, "", "", "Checking GitHub for the latest published firmware...");
    published_update_info_t release = {};
    esp_err_t err = fetch_latest_published_update(&release);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            set_auto_update_state(false, false, "", "",
                                  "The latest GitHub release does not include esp32c6_led_web.bin.");
        } else if (err == ESP_ERR_INVALID_RESPONSE) {
            set_auto_update_state(false, false, "", "", "The latest GitHub release metadata was invalid.");
        } else if (err == ESP_ERR_INVALID_CRC) {
            set_auto_update_state(false, false, "", "", "The latest release manifest signature did not verify.");
        } else if (err == ESP_ERR_HTTP_CONNECT) {
            set_auto_update_state(false, false, "", "",
                                  "Could not download the manifest signature — try again in a moment.");
        } else {
            set_auto_update_state(false, false, "", "", "Failed to fetch the latest published firmware.");
        }
        xSemaphoreGive(s_ota_mutex);
        return;
    }

    int compare = compare_version_strings(current_version, release.version);
    if (compare >= 0) {
        char status[APP_AUTO_UPDATE_STATUS_MAX];
        std::snprintf(status, sizeof(status), "Up to date on %s.", current_version);
        set_auto_update_state(false, false, release.version, release.asset_url, status);
        xSemaphoreGive(s_ota_mutex);
        return;
    }

    if (mode == published_update_mode_t::kCheckOnly) {
        const uint8_t cohort = device_cohort_id();
        const bool in_cohort = cohort < release.cohort_percent;
        const bool auto_install = is_auto_install_enabled();
        char status[APP_AUTO_UPDATE_STATUS_MAX];

        if (auto_install && in_cohort) {
            // fall through to install — the install block sets its own status
        } else {
            if (!auto_install) {
                std::snprintf(status, sizeof(status),
                              "Update %s available — auto-install is off, press Install Update.",
                              release.version);
            } else if (!in_cohort) {
                std::snprintf(status, sizeof(status),
                              "Update %s available (rollout %u%%; you can still install manually).",
                              release.version, release.cohort_percent);
            }
            set_auto_update_state(false, true, release.version, release.asset_url, status);
            xSemaphoreGive(s_ota_mutex);
            return;
        }
    }

    // mode == kInstallForced (or kCheckOnly fall-through with auto-install on
    // and in-cohort): install regardless of cohort gating.
    char status[APP_AUTO_UPDATE_STATUS_MAX];
    std::snprintf(status, sizeof(status), "Installing published update %s...", release.version);
    set_auto_update_state(true, true, release.version, release.asset_url, status);
    err = install_https_ota_with_verify(&release);
    if (err != ESP_OK) {
        std::snprintf(status, sizeof(status), "Published update %s failed to install (%s).",
                      release.version, esp_err_to_name(err));
        set_auto_update_state(false, true, release.version, release.asset_url, status);
        xSemaphoreGive(s_ota_mutex);
        return;
    }

    std::snprintf(status, sizeof(status), "Installed published update %s. Rebooting...", release.version);
    set_auto_update_state(false, false, release.version, release.asset_url, status);
    xSemaphoreGive(s_ota_mutex);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void auto_update_task(void *arg)
{
    (void) arg;

    // Initial jitter: random [0, APP_UPDATE_INITIAL_JITTER_MIN] minutes so the
    // periodic check doesn't run at boot+0 for every device on a fresh release.
    // A manual "check now"/"install now" notify wakes us early.
    uint32_t jitter_ms = 0;
    if (APP_AUTO_UPDATE_JITTER_MS > 0) {
        jitter_ms = (uint32_t)(esp_random() % APP_AUTO_UPDATE_JITTER_MS);
        ESP_LOGI(TAG, "Auto-update: initial jitter %u ms", (unsigned) jitter_ms);
    }
    if (jitter_ms > 0) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(jitter_ms));
    }

    while (true) {
        // Snapshot the pending mode (user request or default periodic check),
        // then reset to kCheckOnly so the next scheduled wake-up doesn't
        // accidentally repeat a forced install.
        published_update_mode_t mode = take_pending_update_mode();
        run_published_update_check(mode);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(APP_AUTO_UPDATE_INTERVAL_MS));
    }
}

static bool matter_is_ready();  // defined below; used by self_test_task

// Self-test: a freshly OTA-installed image is considered healthy when (a) we
// have an STA IP (or AP credentials at minimum) and (b) the Matter stack is
// running. On success we clear the probation marker — the image is permanent.
// On timeout we just reboot. The next boot's init_ota_probation() will
// increment the strike count and roll back after APP_OTA_MAX_STRIKES failures.
//
// Manages probation strictly via the NVS marker (written by
// install_https_ota_with_verify), not the partition state. Marking the running
// image valid is handled entirely by init_ota_probation(): via the strike path
// for a matching marker, or via confirm_running_if_pending() for a markerless
// or stale-marker PENDING_VERIFY image. This task therefore only ever clears
// the marker on success; it never calls mark_app_valid_cancel_rollback().
static void self_test_task(void *arg)
{
    (void) arg;

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        vTaskDelete(nullptr);
        return;
    }

    char marker_slot[16] = "";
    char marker_ver[APP_AUTO_UPDATE_VERSION_MAX] = "";
    uint8_t strikes = 0;
    bool marker_manual = false;
    esp_err_t err = ota_health_read(marker_slot, sizeof(marker_slot),
                                    marker_ver, sizeof(marker_ver), &strikes, &marker_manual);
    if (err != ESP_OK || marker_slot[0] == '\0' ||
        std::strcmp(marker_slot, running->label) != 0) {
        // No active probation — nothing to verify.
        ESP_LOGI(TAG, "self-test: no probation marker for %s; skipping", running->label);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "self-test: probation active (ver=%s strikes=%u manual=%d), waiting up to %llu ms for %s",
             marker_ver, strikes, (int) marker_manual, APP_UPDATE_SELF_TEST_TIMEOUT_MS,
             marker_manual ? "Matter" : "STA + Matter");

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(APP_UPDATE_SELF_TEST_TIMEOUT_MS);
    while (xTaskGetTickCount() < deadline) {
        // Manual (bench-flashed) images may never join home Wi-Fi, so they are
        // promoted on Matter readiness alone. Published images still require an
        // STA IP as evidence the update didn't break connectivity.
        bool sta_ok = marker_manual || (s_sta_ip[0] != '\0');
        bool matter_ok = matter_is_ready();
        if (sta_ok && matter_ok) {
            ota_health_clear();
            ESP_LOGI(TAG, "self-test: OK — STA up, Matter ready. Image promoted.");
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGE(TAG, "self-test: deadline reached without STA+Matter — rebooting "
                  "(strike %u of %u allowed)", strikes, APP_OTA_MAX_STRIKES);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    vTaskDelete(nullptr);
}

static void set_generated_ap_credentials()
{
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        uint32_t fallback = esp_random();
        std::snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%06" PRIX32, APP_WIFI_SSID_PREFIX, fallback & 0xFFFFFFU);
        std::snprintf(s_ap_password, sizeof(s_ap_password), "%s%08" PRIX32, APP_WIFI_PASS_PREFIX, esp_random());
        return;
    }

    std::snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%02X%02X%02X", APP_WIFI_SSID_PREFIX, mac[3], mac[4], mac[5]);
    std::snprintf(s_ap_password, sizeof(s_ap_password), "%s%02X%02X%02X%08" PRIX32, APP_WIFI_PASS_PREFIX, mac[1],
                  mac[4], mac[5], esp_random());
}

static bool matter_is_ready()
{
    return esp_matter::is_started() && s_light_endpoint_id != 0;
}

static bool ap_config_restart_required()
{
    return std::strcmp(s_ap_ssid, s_runtime_ap_ssid) != 0 || std::strcmp(s_ap_password, s_runtime_ap_password) != 0;
}

static bool validate_ap_credentials(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return false;
    }

    size_t ssid_len = std::strlen(ssid);
    size_t pass_len = std::strlen(password);
    if (ssid_len == 0 || ssid_len > 32) {
        return false;
    }

    if (pass_len < 8 || pass_len > 63) {
        return false;
    }

    return true;
}

static uint8_t brightness_to_matter_level(uint8_t brightness)
{
    return static_cast<uint8_t>((static_cast<uint32_t>(brightness) * 254 + 127) / 255);
}

static uint8_t matter_level_to_brightness(uint8_t level)
{
    return static_cast<uint8_t>((static_cast<uint32_t>(level) * 255 + 127) / 254);
}

static void refresh_matter_hs_trackers_from_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    color_rgb_to_matter_hs(red, green, blue, &s_matter_hue, &s_matter_saturation);
}

// Firmware wrapper: populate the file-scope gamma LUT from the configured
// exponent. Call once at boot before the first render.
static void init_gamma_lut(void)
{
    render_gamma_lut_build(s_gamma_lut, kRenderGammaExponent);
}

static esp_err_t apply_led_state(const led_state_t *state)
{
    if (!state || !s_led_strip || !s_led_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_led_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t now_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint16_t render_count = std::min<uint16_t>(state->count, APP_LED_MAX_PIXELS);
    esp_err_t err = ESP_OK;

    for (uint16_t i = 0; i < render_count; ++i) {
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
        render_effect_pixel(state, s_gamma_lut, i, now_ms, &red, &green, &blue);
        err = led_strip_set_pixel(s_led_strip, i, red, green, blue);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set pixel failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
    }

    // When the configured count shrinks, clear the pixels that were active in
    // the previous frame so they cannot remain lit beyond the new boundary.
    for (uint16_t i = render_count; i < s_last_render_count; ++i) {
        err = led_strip_set_pixel(s_led_strip, i, 0, 0, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "clear inactive pixel failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
    }

    err = led_strip_refresh(s_led_strip);
    if (err == ESP_OK) {
        s_last_render_count = render_count;
    }

cleanup:
    xSemaphoreGive(s_led_mutex);
    return err;
}

// Renders one flat colour across the configured count. Used by the pairing
// indicator only; it takes s_led_mutex exactly like apply_led_state() and keeps
// the same shrink bookkeeping, so a later effect frame still clears leftovers.
static esp_err_t apply_solid_frame(uint16_t count, uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_led_strip || !s_led_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_led_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint16_t render_count = std::min<uint16_t>(count, APP_LED_MAX_PIXELS);
    esp_err_t err = ESP_OK;
    for (uint16_t i = 0; i < render_count; ++i) {
        err = led_strip_set_pixel(s_led_strip, i, red, green, blue);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }
    for (uint16_t i = render_count; i < s_last_render_count; ++i) {
        err = led_strip_set_pixel(s_led_strip, i, 0, 0, 0);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }
    err = led_strip_refresh(s_led_strip);
    if (err == ESP_OK) {
        s_last_render_count = render_count;
    }

cleanup:
    xSemaphoreGive(s_led_mutex);
    return err;
}

static void notify_effect_task()
{
    TaskHandle_t effect_task_handle = s_effect_task;
    if (effect_task_handle) {
        xTaskNotifyGive(effect_task_handle);
    }
}

static esp_err_t save_state_to_nvs(const led_state_t *state)
{
    esp_err_t ret = ESP_OK;
    nvs_handle_t nvs_handle = 0;
    char key[16];
    ESP_RETURN_ON_ERROR(nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle), TAG, "nvs_open failed");
    ESP_GOTO_ON_ERROR(nvs_set_u16(nvs_handle, "count", state->count), cleanup, TAG, "save count failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(nvs_handle, "red", state->red), cleanup, TAG, "save red failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(nvs_handle, "green", state->green), cleanup, TAG, "save green failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(nvs_handle, "blue", state->blue), cleanup, TAG, "save blue failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(nvs_handle, "bright", state->brightness), cleanup, TAG, "save brightness failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(nvs_handle, "power", state->power ? 1 : 0), cleanup, TAG, "save power failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(nvs_handle, "effect", state->effect), cleanup, TAG, "save effect failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(nvs_handle, "auto_inst", s_auto_install_enabled ? 1 : 0), cleanup, TAG,
                      "save auto_install_enabled failed");
    ESP_GOTO_ON_ERROR(nvs_set_u16(nvs_handle, "matter_x", s_matter_x), cleanup, TAG, "save matter x failed");
    ESP_GOTO_ON_ERROR(nvs_set_u16(nvs_handle, "matter_y", s_matter_y), cleanup, TAG, "save matter y failed");
    ESP_GOTO_ON_ERROR(nvs_set_u16(nvs_handle, "matter_temp", s_matter_temp_mireds), cleanup, TAG,
                      "save matter temperature failed");
    ESP_GOTO_ON_ERROR(nvs_set_str(nvs_handle, "ap_ssid", s_ap_ssid), cleanup, TAG, "save ap ssid failed");
    ESP_GOTO_ON_ERROR(nvs_set_str(nvs_handle, "ap_pass", s_ap_password), cleanup, TAG, "save ap password failed");
    for (uint8_t effect = 0; effect < LED_EFFECT_COUNT; ++effect) {
        for (size_t index = 0; index < kEffectParamSlotCount; ++index) {
            std::snprintf(key, sizeof(key), "e%u_p%u", effect, static_cast<unsigned>(index));
            ESP_GOTO_ON_ERROR(nvs_set_u8(nvs_handle, key, state->effect_profiles[effect].values[index]),
                              cleanup, TAG, "save effect profile failed");
        }
        std::snprintf(key, sizeof(key), "e%u_cr", effect);
        ESP_GOTO_ON_ERROR(nvs_set_u8(nvs_handle, key, state->effect_colors[effect].red), cleanup, TAG,
                          "save effect color red failed");
        std::snprintf(key, sizeof(key), "e%u_cg", effect);
        ESP_GOTO_ON_ERROR(nvs_set_u8(nvs_handle, key, state->effect_colors[effect].green), cleanup, TAG,
                          "save effect color green failed");
        std::snprintf(key, sizeof(key), "e%u_cb", effect);
        ESP_GOTO_ON_ERROR(nvs_set_u8(nvs_handle, key, state->effect_colors[effect].blue), cleanup, TAG,
                          "save effect color blue failed");
    }
    ESP_GOTO_ON_ERROR(nvs_commit(nvs_handle), cleanup, TAG, "nvs_commit failed");

cleanup:
    nvs_close(nvs_handle);
    return ret;
}

static bool load_state_from_nvs()
{
    nvs_handle_t nvs_handle = 0;
    char key[16];
    if (nvs_open(APP_NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK) {
        led_clamp_state(&s_led_state, APP_LED_MAX_PIXELS);
        refresh_matter_hs_trackers_from_rgb(s_led_state.red, s_led_state.green, s_led_state.blue);
        return false;
    }

    uint16_t count = s_led_state.count;
    uint8_t red = s_led_state.red;
    uint8_t green = s_led_state.green;
    uint8_t blue = s_led_state.blue;
    uint8_t brightness = s_led_state.brightness;
    uint8_t power = s_led_state.power ? 1 : 0;
    uint8_t effect = s_led_state.effect;
    uint16_t matter_x = s_matter_x;
    uint16_t matter_y = s_matter_y;
    uint16_t matter_temp = s_matter_temp_mireds;
    size_t ssid_len = sizeof(s_ap_ssid);
    size_t pass_len = sizeof(s_ap_password);

    nvs_get_u16(nvs_handle, "count", &count);
    nvs_get_u8(nvs_handle, "red", &red);
    nvs_get_u8(nvs_handle, "green", &green);
    nvs_get_u8(nvs_handle, "blue", &blue);
    nvs_get_u8(nvs_handle, "bright", &brightness);
    nvs_get_u8(nvs_handle, "power", &power);
    nvs_get_u8(nvs_handle, "effect", &effect);
    nvs_get_u16(nvs_handle, "matter_x", &matter_x);
    nvs_get_u16(nvs_handle, "matter_y", &matter_y);
    nvs_get_u16(nvs_handle, "matter_temp", &matter_temp);
    uint8_t auto_install = 1;  // default ON when key missing
    nvs_get_u8(nvs_handle, "auto_inst", &auto_install);
    nvs_get_str(nvs_handle, "ap_ssid", s_ap_ssid, &ssid_len);
    nvs_get_str(nvs_handle, "ap_pass", s_ap_password, &pass_len);
    for (uint8_t loaded_effect = 0; loaded_effect < LED_EFFECT_COUNT; ++loaded_effect) {
        for (size_t index = 0; index < kEffectParamSlotCount; ++index) {
            std::snprintf(key, sizeof(key), "e%u_p%u", loaded_effect, static_cast<unsigned>(index));
            nvs_get_u8(nvs_handle, key, &s_led_state.effect_profiles[loaded_effect].values[index]);
        }
        std::snprintf(key, sizeof(key), "e%u_cr", loaded_effect);
        nvs_get_u8(nvs_handle, key, &s_led_state.effect_colors[loaded_effect].red);
        std::snprintf(key, sizeof(key), "e%u_cg", loaded_effect);
        nvs_get_u8(nvs_handle, key, &s_led_state.effect_colors[loaded_effect].green);
        std::snprintf(key, sizeof(key), "e%u_cb", loaded_effect);
        nvs_get_u8(nvs_handle, key, &s_led_state.effect_colors[loaded_effect].blue);
    }
    nvs_close(nvs_handle);

    s_led_state.count = count;
    s_led_state.red = red;
    s_led_state.green = green;
    s_led_state.blue = blue;
    s_led_state.brightness = brightness;
    s_led_state.power = power != 0;
    s_led_state.effect = effect;
    s_matter_x = matter_x;
    s_matter_y = matter_y;
    s_matter_temp_mireds = matter_temp;
    set_auto_install_enabled(auto_install != 0);
    led_clamp_state(&s_led_state, APP_LED_MAX_PIXELS);
    refresh_matter_hs_trackers_from_rgb(s_led_state.red, s_led_state.green, s_led_state.blue);
    return true;
}

static void apply_startup_power_policy()
{
    // Always boot dark to avoid lighting the strip during power-up.
    s_led_state.power = false;
}

static bool parse_hex_color(const char *color, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (!color || std::strlen(color) != 7 || color[0] != '#') {
        return false;
    }

    unsigned int parsed_red = 0;
    unsigned int parsed_green = 0;
    unsigned int parsed_blue = 0;
    if (std::sscanf(color + 1, "%02x%02x%02x", &parsed_red, &parsed_green, &parsed_blue) != 3) {
        return false;
    }

    *red = static_cast<uint8_t>(parsed_red);
    *green = static_cast<uint8_t>(parsed_green);
    *blue = static_cast<uint8_t>(parsed_blue);
    return true;
}

static void format_hex_color(uint8_t red, uint8_t green, uint8_t blue, char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0) {
        return;
    }

    std::snprintf(buffer, buffer_len, "#%02X%02X%02X", red, green, blue);
}

static void update_ip_string_from_netif(const char *if_key, char *output, size_t output_len)
{
    if (!output || output_len == 0) {
        return;
    }

    output[0] = '\0';
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(if_key);
    if (!netif) {
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return;
    }
    if (ip_info.ip.addr == 0) {
        return;
    }

    std::snprintf(output, output_len, IPSTR, IP2STR(&ip_info.ip));
}

static void build_http_url(const char *ip, char *output, size_t output_len)
{
    if (!output || output_len == 0) {
        return;
    }

    output[0] = '\0';
    if (!ip || ip[0] == '\0') {
        return;
    }

    std::snprintf(output, output_len, "http://%s", ip);
}

static void refresh_ip_strings()
{
    char ap_ip[sizeof(s_ap_ip)] = "";
    char sta_ip[sizeof(s_sta_ip)] = "";

    update_ip_string_from_netif("WIFI_AP_DEF", ap_ip, sizeof(ap_ip));
    update_ip_string_from_netif("WIFI_STA_DEF", sta_ip, sizeof(sta_ip));

    if (ap_ip[0] != '\0') {
        std::snprintf(s_ap_ip, sizeof(s_ap_ip), "%s", ap_ip);
    }
    if (sta_ip[0] != '\0') {
        std::snprintf(s_sta_ip, sizeof(s_sta_ip), "%s", sta_ip);
    } else {
        s_sta_ip[0] = '\0';
    }
}

static bool matter_is_commissioned()
{
    if (!matter_is_ready()) {
        return false;
    }
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
}

static size_t matter_fabric_count()
{
    if (!matter_is_ready()) {
        return 0;
    }
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return chip::Server::GetInstance().GetFabricTable().FabricCount();
}

static bool matter_is_commissioning_window_open()
{
    if (!matter_is_ready()) {
        return false;
    }

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
}

static void refresh_matter_onboarding_data()
{
    if (!matter_is_ready()) {
        s_matter_qr_code[0] = '\0';
        s_matter_manual_code[0] = '\0';
        s_matter_qr_url[0] = '\0';
        return;
    }

    chip::MutableCharSpan qr_span(s_matter_qr_code, sizeof(s_matter_qr_code));
    chip::MutableCharSpan manual_span(s_matter_manual_code, sizeof(s_matter_manual_code));
    auto rendezvous = chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE);

    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        if (GetQRCode(qr_span, rendezvous) == CHIP_NO_ERROR) {
            s_matter_qr_code[qr_span.size()] = '\0';
        } else {
            s_matter_qr_code[0] = '\0';
        }

        if (GetManualPairingCode(manual_span, rendezvous) == CHIP_NO_ERROR) {
            s_matter_manual_code[manual_span.size()] = '\0';
        } else {
            s_matter_manual_code[0] = '\0';
        }
    }

    if (s_matter_qr_code[0] != '\0') {
        if (GetQRCodeUrl(s_matter_qr_url, sizeof(s_matter_qr_url),
                         chip::CharSpan(s_matter_qr_code, std::strlen(s_matter_qr_code))) != CHIP_NO_ERROR) {
            s_matter_qr_url[0] = '\0';
        }
    } else {
        s_matter_qr_url[0] = '\0';
    }
}

static const esp_partition_t *get_revert_partition(esp_app_desc_t *app_desc)
{
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    const esp_partition_t *candidate_partition = esp_ota_get_next_update_partition(nullptr);
    if (!candidate_partition || candidate_partition == running_partition) {
        return nullptr;
    }

    esp_app_desc_t description = {};
    if (esp_ota_get_partition_description(candidate_partition, &description) != ESP_OK) {
        return nullptr;
    }

    if (app_desc) {
        *app_desc = description;
    }
    return candidate_partition;
}

// Keep destructive administration actions on the local setup network until
// the web service has authenticated sessions and TLS transport.
static bool request_is_from_softap(httpd_req_t *req)
{
    if (!req || !s_ap_netif) {
        return false;
    }

    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        return false;
    }

    struct sockaddr_in peer = {};
    socklen_t peer_len = sizeof(peer);
    if (getpeername(sockfd, reinterpret_cast<struct sockaddr *>(&peer), &peer_len) < 0 ||
        peer.sin_family != AF_INET) {
        return false;
    }

    // Identify which interface accepted the connection by its LOCAL address.
    // The SoftAP netif answers on its own IP (e.g. 192.168.4.1), which is
    // distinct from the DHCP-assigned STA IP even when the upstream LAN also
    // uses 192.168.4.0/24. A pure peer-subnet mask compare fails OPEN under
    // that overlap (a LAN client masks into the AP subnet), so interface
    // identity — not subnet membership — is the real admin boundary.
    struct sockaddr_in local = {};
    socklen_t local_len = sizeof(local);
    if (getsockname(sockfd, reinterpret_cast<struct sockaddr *>(&local), &local_len) < 0 ||
        local.sin_family != AF_INET) {
        return false;
    }

    esp_netif_ip_info_t ap_info = {};
    if (esp_netif_get_ip_info(s_ap_netif, &ap_info) != ESP_OK || ap_info.netmask.addr == 0) {
        return false;
    }

    // Authoritative check: the request arrived on the SoftAP interface (local
    // address == AP IP). Keep the peer-subnet compare as an additional guard.
    const bool on_ap_iface = local.sin_addr.s_addr == ap_info.ip.addr;
    const bool peer_in_ap_subnet =
        (peer.sin_addr.s_addr & ap_info.netmask.addr) == (ap_info.ip.addr & ap_info.netmask.addr);
    return on_ap_iface && peer_in_ap_subnet;
}

static esp_err_t require_softap_admin(httpd_req_t *req);

static void sync_matter_state_work_handler(intptr_t arg)
{
    (void) arg;
    if (!matter_is_ready()) {
        return;
    }

    led_state_t snapshot;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    snapshot = s_led_state;
    xSemaphoreGive(s_state_mutex);

    uint8_t hue = 0;
    uint8_t saturation = 0;
    uint16_t current_x = 0;
    uint16_t current_y = 0;
    color_rgb_to_matter_hs(snapshot.red, snapshot.green, snapshot.blue, &hue, &saturation);
    color_rgb_to_matter_xy(snapshot.red, snapshot.green, snapshot.blue, &current_x, &current_y);

    esp_err_t err = ESP_OK;
    s_syncing_matter = true;

    esp_matter_attr_val_t on_off_value = esp_matter_bool(snapshot.power);
    esp_matter_attr_val_t level_value = esp_matter_nullable_uint8(brightness_to_matter_level(snapshot.brightness));
    esp_matter_attr_val_t hue_value = esp_matter_uint8(hue);
    esp_matter_attr_val_t saturation_value = esp_matter_uint8(saturation);
    esp_matter_attr_val_t x_value = esp_matter_uint16(current_x);
    esp_matter_attr_val_t y_value = esp_matter_uint16(current_y);
    esp_matter_attr_val_t color_mode_value =
        esp_matter_enum8(static_cast<uint8_t>(ColorControl::ColorMode::kCurrentHueAndCurrentSaturation));

    err = attribute::update(s_light_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &on_off_value);
    if (err == ESP_OK) {
        err = attribute::update(s_light_endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id,
                                &level_value);
    }
    if (err == ESP_OK) {
        err = attribute::update(s_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentHue::Id,
                                &hue_value);
    }
    if (err == ESP_OK) {
        err = attribute::update(s_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentSaturation::Id,
                                &saturation_value);
    }
    if (err == ESP_OK) {
        err = attribute::update(s_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentX::Id,
                                &x_value);
    }
    if (err == ESP_OK) {
        err = attribute::update(s_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentY::Id,
                                &y_value);
    }
    if (err == ESP_OK) {
        err = attribute::update(s_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::ColorMode::Id,
                                &color_mode_value);
    }
    if (err == ESP_OK) {
        err = attribute::update(s_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::EnhancedColorMode::Id,
                                &color_mode_value);
    }

    s_syncing_matter = false;
    if (err == ESP_OK) {
        // Update the Matter trackers under the state mutex: control_post_handler
        // writes s_matter_x/y under the same lock, and this handler runs on the
        // CHIP thread. Keep the critical section to just the assignments — do
        // not hold the mutex across the attribute::update calls above.
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_matter_hue = hue;
        s_matter_saturation = saturation;
        s_matter_x = current_x;
        s_matter_y = current_y;
        xSemaphoreGive(s_state_mutex);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Matter attribute sync failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t sync_matter_state_from_led_state()
{
    if (!matter_is_ready()) {
        return ESP_OK;
    }

    CHIP_ERROR schedule_err = chip::DeviceLayer::PlatformMgr().ScheduleWork(sync_matter_state_work_handler, 0);
    if (schedule_err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Failed to schedule Matter sync: %" CHIP_ERROR_FORMAT, schedule_err.Format());
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ---- Shared control path (web API and MQTT link) ---------------------------
//
// POST /api/control and an inbound MQTT `set` both land here, so the field
// validation, the clamps, the Matter tracker refresh, the persist and the
// effect-task notify exist exactly once. `require_full_tuple` keeps the web
// API's "Missing fields" contract; the MQTT link passes false because the
// contract lets a controller send any subset of the tuple.
led_control_result_t led_control_apply_json(const cJSON *root, bool require_full_tuple, bool persist_now)
{
    if (!cJSON_IsObject(root)) {
        return LED_CONTROL_ERR_FIELDS;
    }

    const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    const cJSON *color = cJSON_GetObjectItemCaseSensitive(root, "color");
    const cJSON *effect = cJSON_GetObjectItemCaseSensitive(root, "effect");
    const cJSON *effect_params = cJSON_GetObjectItemCaseSensitive(root, "effect_params");
    const cJSON *effect_color = cJSON_GetObjectItemCaseSensitive(root, "effect_color");
    const cJSON *power = cJSON_GetObjectItemCaseSensitive(root, "power");

    if (require_full_tuple) {
        if (!cJSON_IsNumber(brightness) || !cJSON_IsString(color) || !cJSON_IsString(effect) ||
            !cJSON_IsArray(effect_params) || !(cJSON_IsString(effect_color) || effect_color == nullptr) ||
            !(cJSON_IsBool(power) || power == nullptr)) {
            return LED_CONTROL_ERR_FIELDS;
        }
    } else {
        // Partial update: every field is optional, but a present field must
        // still carry the right type — inbound MQTT payloads are untrusted.
        if ((brightness && !cJSON_IsNumber(brightness)) || (color && !cJSON_IsString(color)) ||
            (effect && !cJSON_IsString(effect)) || (effect_params && !cJSON_IsArray(effect_params)) ||
            (effect_color && !cJSON_IsString(effect_color)) || (power && !cJSON_IsBool(power))) {
            return LED_CONTROL_ERR_FIELDS;
        }
        if (!brightness && !color && !effect && !effect_params && !effect_color && !power) {
            return LED_CONTROL_ERR_EMPTY;
        }
    }

    led_state_t updated;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    updated = s_led_state;
    xSemaphoreGive(s_state_mutex);

    // LED "count" is a Configuration item, settable only via the SoftAP-gated
    // /api/config. Neither this path nor the MQTT link parses or applies it.
    if (brightness) {
        updated.brightness = led_clamp_u8(static_cast<int>(brightness->valuedouble));
    }
    if (color && !parse_hex_color(color->valuestring, &updated.red, &updated.green, &updated.blue)) {
        return LED_CONTROL_ERR_COLOR;
    }
    if (effect) {
        updated.effect = led_effect_from_name(effect->valuestring);
    }
    if (effect_params) {
        for (size_t index = 0; index < kEffectParamSlotCount; ++index) {
            const cJSON *item = cJSON_GetArrayItem(effect_params, index);
            if (cJSON_IsNumber(item)) {
                updated.effect_profiles[updated.effect].values[index] = led_clamp_u8(static_cast<int>(item->valuedouble));
            }
        }
    }
    if (effect_color &&
        !parse_hex_color(effect_color->valuestring, &updated.effect_colors[updated.effect].red,
                         &updated.effect_colors[updated.effect].green, &updated.effect_colors[updated.effect].blue)) {
        return LED_CONTROL_ERR_EFFECT_COLOR;
    }
    led_clamp_effect_profile(updated.effect, &updated.effect_profiles[updated.effect]);
    if (power) {
        updated.power = cJSON_IsTrue(power);
    } else if (brightness) {
        // Same rule as the web page: a brightness-only change carries power.
        updated.power = updated.brightness > 0;
    }
    led_clamp_state(&updated, APP_LED_MAX_PIXELS);
    const bool effect_color_present = effect_color != nullptr;
    const bool effect_params_present = effect_params != nullptr;

    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    led_state_t committed = s_led_state;
    committed.brightness = updated.brightness;
    committed.red = updated.red;
    committed.green = updated.green;
    committed.blue = updated.blue;
    committed.effect = updated.effect;
    committed.power = updated.power;
    if (effect_params_present || require_full_tuple) {
        committed.effect_profiles[updated.effect] = updated.effect_profiles[updated.effect];
    }
    if (effect_color_present) {
        committed.effect_colors[updated.effect] = updated.effect_colors[updated.effect];
    }
    led_clamp_state(&committed, APP_LED_MAX_PIXELS);
    s_led_state = committed;
    refresh_matter_hs_trackers_from_rgb(s_led_state.red, s_led_state.green, s_led_state.blue);
    color_rgb_to_matter_xy(s_led_state.red, s_led_state.green, s_led_state.blue, &s_matter_x, &s_matter_y);
    if (persist_now) {
        s_persist_pending = false;
        err = save_state_to_nvs(&s_led_state);
    } else {
        // Debounced: a burst of MQTT sets collapses into one flash write.
        s_persist_pending = true;
        s_persist_due_us = esp_timer_get_time() + (int64_t) APP_PERSIST_DEBOUNCE_MS * 1000;
    }
    xSemaphoreGive(s_state_mutex);

    // RAM state is already committed, so drive the strip regardless of whether
    // the NVS persist succeeded.
    notify_effect_task();
    sync_matter_state_from_led_state();

    return err == ESP_OK ? LED_CONTROL_OK : LED_CONTROL_ERR_SAVE;
}

void led_control_persist_tick(void)
{
    if (!s_state_mutex) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_persist_pending && esp_timer_get_time() >= s_persist_due_us) {
        s_persist_pending = false;
        esp_err_t err = save_state_to_nvs(&s_led_state);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "debounced persist failed: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_state_mutex);
}

void led_control_get_tuple(led_tuple_t *out)
{
    if (!out) {
        return;
    }
    led_state_t snapshot;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    snapshot = s_led_state;
    xSemaphoreGive(s_state_mutex);

    std::memset(out, 0, sizeof(*out));
    out->power = snapshot.power;
    out->brightness = snapshot.brightness;
    out->count = snapshot.count;
    format_hex_color(snapshot.red, snapshot.green, snapshot.blue, out->color, sizeof(out->color));
    copy_string_value(out->effect, sizeof(out->effect), led_effect_to_name(snapshot.effect));
    const uint8_t effect = led_effect_from_index(snapshot.effect);
    for (size_t index = 0; index < kEffectParamSlotCount && index < MQTT_PROTO_PARAM_COUNT; ++index) {
        out->effect_params[index] = snapshot.effect_profiles[effect].values[index];
    }
    format_hex_color(snapshot.effect_colors[effect].red, snapshot.effect_colors[effect].green,
                     snapshot.effect_colors[effect].blue, out->effect_color, sizeof(out->effect_color));
}

const char *led_control_firmware_version(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    return app_desc ? app_desc->version : "unknown";
}

void led_control_get_device_name(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const char *name = s_runtime_ap_ssid[0] != '\0' ? s_runtime_ap_ssid : s_ap_ssid;
    copy_string_value(out, out_len, name);
    xSemaphoreGive(s_state_mutex);
}

void led_control_get_sta_ip(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    copy_string_value(out, out_len, s_sta_ip);
    xSemaphoreGive(s_state_mutex);
}

uint16_t led_control_max_leds(void)
{
    return static_cast<uint16_t>(APP_LED_MAX_PIXELS);
}

static void set_indicator(led_indicator_mode_t mode, uint8_t code)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_indicator_mode = mode;
    s_indicator_code = code;
    s_indicator_started_us = esp_timer_get_time();
    xSemaphoreGive(s_state_mutex);
    notify_effect_task();
}

void led_control_indicator_blink(uint8_t code)
{
    if (code < MQTT_PROTO_PAIR_CODE_MIN) {
        code = MQTT_PROTO_PAIR_CODE_MIN;
    }
    if (code > MQTT_PROTO_PAIR_CODE_MAX) {
        code = MQTT_PROTO_PAIR_CODE_MAX;
    }
    set_indicator(LED_INDICATOR_CODE, code);
}

void led_control_indicator_flash(bool success)
{
    set_indicator(success ? LED_INDICATOR_SUCCESS : LED_INDICATOR_FAILURE, 0);
}

void led_control_indicator_clear(void)
{
    set_indicator(LED_INDICATOR_NONE, 0);
}

// ---- Time subsystem, schedules, and the automatic on/off scheduler ---------

static void apply_tz()
{
    setenv("TZ", s_tz, 1);
    tzset();
}

static bool time_is_valid()
{
    return time(NULL) > 1700000000;
}

static void start_sntp_once()
{
    static bool s_sntp_started = false;
    if (s_sntp_started) {
        return;
    }
    s_sntp_started = true;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(APP_SNTP_SERVER);
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }
    apply_tz();
}

static esp_err_t save_schedules()
{
    esp_err_t ret = ESP_OK;
    nvs_handle_t nvs_handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle), TAG, "nvs_open failed");
    ESP_GOTO_ON_ERROR(nvs_set_blob(nvs_handle, "sched", s_schedules, sizeof(s_schedules)), cleanup, TAG,
                      "save schedules failed");
    ESP_GOTO_ON_ERROR(nvs_set_str(nvs_handle, "tz", s_tz), cleanup, TAG, "save tz failed");
    ESP_GOTO_ON_ERROR(nvs_commit(nvs_handle), cleanup, TAG, "nvs_commit failed");

cleanup:
    nvs_close(nvs_handle);
    return ret;
}

static void load_schedules()
{
    nvs_handle_t nvs_handle = 0;
    if (nvs_open(APP_NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t blob_len = 0;
        if (nvs_get_blob(nvs_handle, "sched", nullptr, &blob_len) == ESP_OK &&
            blob_len == sizeof(s_schedules)) {
            nvs_get_blob(nvs_handle, "sched", s_schedules, &blob_len);
        }
        size_t tz_len = sizeof(s_tz);
        nvs_get_str(nvs_handle, "tz", s_tz, &tz_len);
        nvs_close(nvs_handle);
    }
    apply_tz();
}

// Toggle power only, preserving color/brightness/effect, through the same
// commit path used elsewhere. Safe to call from a FreeRTOS task.
static void apply_power_action(bool on)
{
    bool changed = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_led_state.power != on) {
        s_led_state.power = on;
        save_state_to_nvs(&s_led_state);
        changed = true;
    }
    xSemaphoreGive(s_state_mutex);
    notify_effect_task();
    sync_matter_state_from_led_state();
    if (changed) {
        mqtt_link_state_changed("schedule");
    }
}

static void schedule_task(void *arg)
{
    (void) arg;
    for (;;) {
        // Tick every second so the relative timer fires within ~1 s of its
        // deadline (matching the UI countdown) instead of lagging up to the old
        // 10 s poll interval. Wall-clock schedules still de-dupe per minute.
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Flush a debounced control persist (MQTT `set` bursts) if one is due.
        // This task is the right owner: it already ticks once a second and,
        // unlike the effect task, may block on a flash write.
        led_control_persist_tick();

        // Relative one-shot timer (monotonic; survives without time sync).
        bool relative_fire = false;
        uint8_t relative_action = 0;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        int64_t deadline = s_relative_deadline_us;
        if (schedule_relative_due(deadline, esp_timer_get_time())) {
            relative_action = s_relative_action;
            s_relative_deadline_us = 0;
            relative_fire = true;
        }
        xSemaphoreGive(s_state_mutex);
        if (relative_fire) {
            ESP_LOGI(TAG, "Relative timer fired: power %s", relative_action == 1 ? "on" : "off");
            apply_power_action(relative_action == 1);
        }

        // Fixed wall-clock schedules (require a valid clock via SNTP).
        if (time_is_valid()) {
            time_t now = time(NULL);
            struct tm lt;
            localtime_r(&now, &lt);
            int32_t cur_min = (int32_t)(now / 60);
            for (size_t i = 0; i < APP_MAX_SCHEDULES; ++i) {
                schedule_entry_t entry = s_schedules[i];
                if (schedule_entry_due(&entry, lt.tm_wday, lt.tm_hour, lt.tm_min, cur_min,
                                       s_sched_last_fired_min[i])) {
                    s_sched_last_fired_min[i] = cur_min;
                    ESP_LOGI(TAG, "Schedule %u fired at %02d:%02d: power %s", (unsigned)i, lt.tm_hour,
                             lt.tm_min, entry.action == 1 ? "on" : "off");
                    apply_power_action(entry.action == 1);
                }
            }
        }
    }
}

static esp_err_t send_state_json(httpd_req_t *req)
{
    const bool softap_admin = request_is_from_softap(req);
    led_state_t snapshot;
    bool auto_update_busy = false;
    bool auto_update_available = false;
    char auto_update_status[APP_AUTO_UPDATE_STATUS_MAX] = "";
    char auto_update_latest_version[APP_AUTO_UPDATE_VERSION_MAX] = "";
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    snapshot = s_led_state;
    auto_update_busy = s_auto_update_busy;
    auto_update_available = s_auto_update_available;
    copy_string_value(auto_update_status, sizeof(auto_update_status), s_auto_update_status);
    copy_string_value(auto_update_latest_version, sizeof(auto_update_latest_version), s_auto_update_latest_version);
    xSemaphoreGive(s_state_mutex);

    refresh_ip_strings();
    const bool matter_window_open = matter_is_commissioning_window_open();
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    const esp_partition_t *ota_target_partition = esp_ota_get_next_update_partition(nullptr);
    esp_app_desc_t revert_desc = {};
    const esp_partition_t *revert_partition = get_revert_partition(&revert_desc);
    char ap_url[32] = "";
    char lan_url[32] = "";
    build_http_url(s_ap_ip, ap_url, sizeof(ap_url));
    build_http_url(s_sta_ip, lan_url, sizeof(lan_url));

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Failed to build response");
    }

    char color_hex[8];
    format_hex_color(snapshot.red, snapshot.green, snapshot.blue, color_hex, sizeof(color_hex));

    cJSON_AddNumberToObject(root, "count", snapshot.count);
    cJSON_AddNumberToObject(root, "brightness", snapshot.brightness);
    cJSON_AddStringToObject(root, "color", color_hex);
    cJSON_AddStringToObject(root, "effect", led_effect_to_name(snapshot.effect));
    cJSON *effect_profiles = cJSON_AddObjectToObject(root, "effect_profiles");
    for (uint8_t effect = 0; effect < LED_EFFECT_COUNT; ++effect) {
        cJSON *profile = cJSON_AddArrayToObject(effect_profiles, led_effect_to_name(effect));
        for (size_t index = 0; index < kEffectParamSlotCount; ++index) {
            cJSON_AddItemToArray(profile, cJSON_CreateNumber(snapshot.effect_profiles[effect].values[index]));
        }
    }
    cJSON *effect_colors = cJSON_AddObjectToObject(root, "effect_colors");
    for (uint8_t effect = 0; effect < LED_EFFECT_COUNT; ++effect) {
        char effect_color_hex[8];
        format_hex_color(snapshot.effect_colors[effect].red, snapshot.effect_colors[effect].green,
                         snapshot.effect_colors[effect].blue, effect_color_hex, sizeof(effect_color_hex));
        cJSON_AddStringToObject(effect_colors, led_effect_to_name(effect), effect_color_hex);
    }
    cJSON_AddBoolToObject(root, "power", snapshot.power);
    cJSON_AddNumberToObject(root, "max_leds", APP_LED_MAX_PIXELS);
    cJSON_AddNumberToObject(root, "gpio", APP_LED_GPIO);
    cJSON_AddStringToObject(root, "ap_ssid", s_runtime_ap_ssid);
    cJSON_AddStringToObject(root, "ap_ip", s_ap_ip);
    cJSON_AddStringToObject(root, "ap_url", ap_url);
    cJSON_AddBoolToObject(root, "softap_admin", softap_admin);
    cJSON_AddStringToObject(root, "sta_ip", s_sta_ip);
    cJSON_AddStringToObject(root, "lan_url", lan_url);
    cJSON_AddBoolToObject(root, "sta_connected", s_sta_ip[0] != '\0');
    cJSON_AddStringToObject(root, "config_ap_ssid", s_ap_ssid);
    cJSON_AddBoolToObject(root, "ap_restart_required", ap_config_restart_required());
    cJSON_AddBoolToObject(root, "matter_ready", matter_is_ready());
    cJSON_AddBoolToObject(root, "commissioned", matter_is_commissioned());
    cJSON_AddNumberToObject(root, "matter_endpoint", s_light_endpoint_id);
    cJSON_AddNumberToObject(root, "matter_fabric_count", static_cast<double>(matter_fabric_count()));
    cJSON_AddBoolToObject(root, "matter_window_open", matter_window_open);
    cJSON_AddStringToObject(root, "fw_version", app_desc ? app_desc->version : "unknown");
    cJSON_AddStringToObject(root, "running_partition", running_partition ? running_partition->label : "");
    cJSON_AddStringToObject(root, "ota_target_partition", ota_target_partition ? ota_target_partition->label : "");
    cJSON_AddBoolToObject(root, "revert_available", revert_partition != nullptr);
    cJSON_AddStringToObject(root, "revert_partition", revert_partition ? revert_partition->label : "");
    cJSON_AddStringToObject(root, "revert_version", revert_partition ? revert_desc.version : "");
    cJSON_AddBoolToObject(root, "auto_update_busy", auto_update_busy);
    cJSON_AddBoolToObject(root, "auto_update_available", auto_update_available);
    cJSON_AddStringToObject(root, "auto_update_status", auto_update_status);
    cJSON_AddStringToObject(root, "auto_update_latest_version", auto_update_latest_version);
    cJSON_AddBoolToObject(root, "auto_install_enabled", is_auto_install_enabled());
    cJSON_AddStringToObject(root, "sta_ssid", s_sta_ssid);
    cJSON_AddStringToObject(root, "sta_bssid", s_sta_bssid);
    cJSON_AddNumberToObject(root, "sta_rssi", s_sta_rssi);
    cJSON_AddNumberToObject(root, "sta_channel", s_sta_channel);
    cJSON_AddNumberToObject(root, "sta_last_disconnect_reason", s_sta_last_disconnect_reason);
    cJSON_AddStringToObject(root, "sta_last_disconnect_reason_text", net_wifi_disconnect_reason_text(s_sta_last_disconnect_reason));
    cJSON_AddNumberToObject(root, "sta_connect_count", static_cast<double>(s_sta_connect_count));
    cJSON_AddNumberToObject(root, "sta_disconnect_count", static_cast<double>(s_sta_disconnect_count));
    cJSON_AddNumberToObject(root, "sta_last_event_ms", static_cast<double>(s_sta_last_event_ms));
    cJSON_AddNumberToObject(root, "sta_last_ip_ms", static_cast<double>(s_sta_last_ip_ms));
    cJSON_AddNumberToObject(root, "matter_commissioned_count", static_cast<double>(s_matter_commissioned_count));
    cJSON_AddNumberToObject(root, "matter_last_event_ms", static_cast<double>(s_matter_last_event_ms));
    const bool expose_onboarding = softap_admin && matter_window_open;
    cJSON_AddStringToObject(root, "manual_code", expose_onboarding ? s_matter_manual_code : "");
    cJSON_AddStringToObject(root, "qr_code", expose_onboarding ? s_matter_qr_code : "");
    cJSON_AddStringToObject(root, "qr_url", expose_onboarding ? s_matter_qr_url : "");

    // Time subsystem + relative timer (compact fields; schedules live on
    // /api/schedule to keep this response lean).
    const bool time_valid = time_is_valid();
    char now_local[20] = "";
    if (time_valid) {
        time_t now = time(NULL);
        struct tm lt = {};
        localtime_r(&now, &lt);
        strftime(now_local, sizeof(now_local), "%Y-%m-%d %H:%M:%S", &lt);
    }
    cJSON_AddBoolToObject(root, "time_valid", time_valid);
    cJSON_AddStringToObject(root, "now_local", now_local);
    int64_t rel_deadline = 0;
    uint8_t rel_action = 0;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    rel_deadline = s_relative_deadline_us;
    rel_action = s_relative_action;
    xSemaphoreGive(s_state_mutex);
    const bool rel_active = rel_deadline != 0;
    int rel_remaining_s = 0;
    if (rel_active) {
        rel_remaining_s = schedule_relative_remaining_s(rel_deadline, esp_timer_get_time());
    }
    cJSON_AddBoolToObject(root, "relative_active", rel_active);
    cJSON_AddNumberToObject(root, "relative_action", rel_action);
    cJSON_AddNumberToObject(root, "relative_remaining_s", rel_remaining_s);

    // MQTT link status, paired controllers and (SoftAP admins only) the broker
    // address. The broker password is never part of this document.
    mqtt_link_add_state_json(root, softap_admin);

    char *response = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!response) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Failed to encode response");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, response);
    free(response);
    return err;
}

// Serves the embedded page. The decoded bytes are identical to the literal
// this replaced; only the transfer coding changed.
//
// The 406 branch is not a convenience: the uncompressed page is not on the
// device at all, so a client that forbids gzip genuinely cannot be served it.
// Every browser and captive-portal agent accepts gzip, and a request carrying
// no Accept-Encoding at all counts as accepting it (RFC 9110 12.5.3), so this
// path needs a deliberately crafted request to reach.
static esp_err_t root_get_handler(httpd_req_t *req)
{
    char accept[HTTP_ACCEPT_ENCODING_MAX];
    bool gzip_ok;

    size_t len = httpd_req_get_hdr_value_len(req, "Accept-Encoding");
    if (len == 0) {
        // Absent field. Note this is NOT the same as an empty value, which
        // would mean identity-only -- httpd reports both as length 0, so treat
        // the ambiguous case the permissive way the RFC prescribes for absence.
        gzip_ok = http_accepts_gzip(nullptr);
    } else if (len >= sizeof(accept)) {
        gzip_ok = http_accepts_gzip_truncated();
    } else if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", accept,
                                           sizeof(accept)) == ESP_OK) {
        gzip_ok = http_accepts_gzip(accept);
    } else {
        gzip_ok = http_accepts_gzip_truncated();
    }

    if (!gzip_ok) {
        ESP_LOGW(TAG, "GET / refused gzip; the page is stored compressed only");
        httpd_resp_set_status(req, "406 Not Acceptable");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(
            req, "This page is stored gzip-compressed to fit the flash "
                 "budget, and no uncompressed copy exists on the device. "
                 "Retry with 'Accept-Encoding: gzip' (curl --compressed).\n");
    }

    const size_t gz_len = (size_t) (index_html_gz_end - index_html_gz_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *) index_html_gz_start, gz_len);
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void) err;
    return captive_redirect_handler(req);
}

static esp_err_t state_get_handler(httpd_req_t *req)
{
    return send_state_json(req);
}

static char *read_request_body(httpd_req_t *req)
{
    if (!req || req->content_len <= 0 || req->content_len >= APP_POST_BODY_LIMIT) {
        return nullptr;
    }

    char *body = static_cast<char *>(malloc(req->content_len + 1));
    if (!body) {
        return nullptr;
    }

    int remaining = req->content_len;
    int offset = 0;
    while (remaining > 0) {
        int received = httpd_req_recv(req, body + offset, remaining);
        if (received <= 0) {
            free(body);
            return nullptr;
        }
        offset += received;
        remaining -= received;
    }
    body[offset] = '\0';
    return body;
}

static esp_err_t control_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= APP_POST_BODY_LIMIT) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid request body");
    }

    char *body = read_request_body(req);
    if (!body) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Failed to read request body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid JSON");
    }

    // LED "count" is a Configuration item, settable only via the SoftAP-gated
    // /api/config. This ungated handler (also POST /api/state) intentionally
    // does not parse or apply count. Validation, clamping, persistence and the
    // Matter sync all live in led_control_apply_json(), which the MQTT link
    // shares.
    const led_control_result_t result = led_control_apply_json(root, true, true);
    cJSON_Delete(root);

    switch (result) {
    case LED_CONTROL_OK:
        break;
    case LED_CONTROL_ERR_COLOR:
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid color");
    case LED_CONTROL_ERR_EFFECT_COLOR:
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid effect color");
    case LED_CONTROL_ERR_SAVE:
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Failed to update LED state");
    case LED_CONTROL_ERR_FIELDS:
    case LED_CONTROL_ERR_EMPTY:
    default:
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Missing fields");
    }

    mqtt_link_state_changed("web");

    return send_state_json(req);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    esp_err_t access_err = require_softap_admin(req);
    if (access_err != ESP_OK) {
        return access_err;
    }

    if (req->content_len <= 0 || req->content_len >= APP_POST_BODY_LIMIT) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid request body");
    }

    char *body = read_request_body(req);
    if (!body) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Failed to read request body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid JSON");
    }

    cJSON *count = cJSON_GetObjectItemCaseSensitive(root, "count");
    cJSON *ap_ssid = cJSON_GetObjectItemCaseSensitive(root, "ap_ssid");
    cJSON *ap_password = cJSON_GetObjectItemCaseSensitive(root, "ap_password");
    cJSON *auto_install = cJSON_GetObjectItemCaseSensitive(root, "auto_install_enabled");
    if (!cJSON_IsNumber(count) || !cJSON_IsString(ap_ssid) ||
        (ap_password && !cJSON_IsString(ap_password))) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Missing configuration fields");
    }

    char current_password[sizeof(s_ap_password)] = "";
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    copy_string_value(current_password, sizeof(current_password), s_ap_password);
    xSemaphoreGive(s_state_mutex);
    const bool change_password = ap_password && ap_password->valuestring[0] != '\0';
    const char *password_value = change_password ? ap_password->valuestring : current_password;

    if (!validate_ap_credentials(ap_ssid->valuestring, password_value)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "AP SSID must be 1-32 chars and password must be 8-63 chars");
    }

    // Broker settings ride along on this SoftAP-gated endpoint: the MQTT link
    // must never learn them from a LAN client. Absent fields change nothing.
    esp_err_t mqtt_err = mqtt_link_apply_config_json(root);
    if (mqtt_err == ESP_ERR_INVALID_ARG) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid MQTT broker settings");
    }

    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_led_state.count = static_cast<uint16_t>(count->valuedouble);
    led_clamp_state(&s_led_state, APP_LED_MAX_PIXELS);
    copy_string_value(s_ap_ssid, sizeof(s_ap_ssid), ap_ssid->valuestring);
    if (change_password) {
        copy_string_value(s_ap_password, sizeof(s_ap_password), password_value);
    }
    if (cJSON_IsBool(auto_install)) {
        s_auto_install_enabled = cJSON_IsTrue(auto_install);
    }
    err = save_state_to_nvs(&s_led_state);
    xSemaphoreGive(s_state_mutex);
    cJSON_Delete(root);

    // Apply to the strip regardless of the persistence result (RAM state is
    // already updated); still report 500 if the save itself failed.
    notify_effect_task();
    mqtt_link_state_changed("web");

    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Failed to save configuration");
    }

    return send_state_json(req);
}

static esp_err_t state_post_handler(httpd_req_t *req)
{
    return control_post_handler(req);
}

static esp_err_t send_message_json(httpd_req_t *req, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_sendstr(req, message ? message : "Request complete");
    }

    cJSON_AddStringToObject(root, "message", message ? message : "Request complete");
    char *response = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!response) {
        return httpd_resp_sendstr(req, message ? message : "Request complete");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, response);
    free(response);
    return err;
}

static esp_err_t require_softap_admin(httpd_req_t *req)
{
    if (request_is_from_softap(req)) {
        return ESP_OK;
    }

    httpd_resp_set_status(req, "403 Forbidden");
    return send_message_json(req, "This administration action is available from the device SoftAP only.");
}

// Removing a paired HMI is an administration action, so it follows the same
// SoftAP boundary as the rest of the Configuration tab.
static esp_err_t mqtt_unpair_post_handler(httpd_req_t *req)
{
    esp_err_t access_err = require_softap_admin(req);
    if (access_err != ESP_OK) {
        return access_err;
    }

    char *body = read_request_body(req);
    if (!body) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_message_json(req, "Failed to read request body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_message_json(req, "Invalid JSON");
    }

    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(id)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return send_message_json(req, "Missing controller id");
    }

    esp_err_t err = mqtt_link_unpair(id->valuestring);
    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "404 Not Found");
        return send_message_json(req, "That controller is not paired with this strip.");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_message_json(req, "Invalid controller id");
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_message_json(req, "Controller removed, but saving the list failed.");
    }
    return send_message_json(req, "Controller unpaired");
}

static esp_err_t matter_pairing_window_post_handler(httpd_req_t *req)
{
    esp_err_t access_err = require_softap_admin(req);
    if (access_err != ESP_OK) {
        return access_err;
    }

    if (!matter_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_message_json(req, "Matter stack is not ready.");
    }

    bool already_open = false;
    bool commissioned = false;
    CHIP_ERROR open_err = CHIP_NO_ERROR;
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        auto &commission_mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
        already_open = commission_mgr.IsCommissioningWindowOpen();
        if (!already_open) {
            commissioned = chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
            if (!commissioned) {
                open_err = commission_mgr.OpenBasicCommissioningWindow(
                    chip::System::Clock::Seconds16(kCommissioningTimeoutSeconds),
                    chip::CommissioningWindowAdvertisement::kDnssdOnly);
            }
        }
    }

    if (commissioned) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_message_json(req,
                                 "Matter is already commissioned; use an existing Matter administrator to add a controller.");
    }

    if (open_err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "SoftAP pairing-window request failed: %" CHIP_ERROR_FORMAT, open_err.Format());
        char message[192] = {};
        std::snprintf(message, sizeof(message), "Matter rejected the pairing-window request (%" CHIP_ERROR_FORMAT ").",
                      open_err.Format());
        httpd_resp_set_status(req, "409 Conflict");
        return send_message_json(req, message);
    }

    ESP_LOGI(TAG, "SoftAP pairing-window request accepted%s", already_open ? "; window was already open" : "");
    httpd_resp_set_status(req, "200 OK");
    if (already_open) {
        return send_message_json(req, "Matter pairing window is already open.");
    }

    char message[96] = {};
    std::snprintf(message, sizeof(message), "Matter pairing window opened for %u seconds.",
                  static_cast<unsigned int>(kCommissioningTimeoutSeconds));
    return send_message_json(req, message);
}

static esp_err_t erase_app_settings_namespace()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void reboot_task(void *arg)
{
    (void) arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void factory_reset_task(void *arg)
{
    (void) arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    // Drop the paired controllers and broker settings from RAM and clear the
    // retained topics before the namespace erase takes the stored copies.
    mqtt_link_prepare_factory_reset();
    esp_err_t err = erase_app_settings_namespace();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase app settings namespace: %s", esp_err_to_name(err));
    }

    err = esp_matter::factory_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Matter factory reset failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    vTaskDelete(nullptr);
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    esp_err_t access_err = require_softap_admin(req);
    if (access_err != ESP_OK) {
        return access_err;
    }

    if (!s_ota_mutex) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_message_json(req, "OTA subsystem is not ready");
    }

    if (xSemaphoreTake(s_ota_mutex, 0) != pdTRUE) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_message_json(req, "Another OTA update is already in progress");
    }

    esp_err_t result = ESP_OK;
    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;
    char *buffer = nullptr;
    int remaining = 0;
    esp_app_desc_t manual_desc = {};
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(nullptr);

    if (!update_partition) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        result = send_message_json(req, "No OTA target partition is available");
        goto cleanup;
    }

    if (req->content_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        result = send_message_json(req, "Firmware payload is empty");
        goto cleanup;
    }

    if (req->content_len > static_cast<int>(update_partition->size)) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        result = send_message_json(req, "Firmware image is larger than the OTA partition");
        goto cleanup;
    }

    buffer = static_cast<char *>(malloc(APP_OTA_CHUNK_SIZE));
    if (!buffer) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        result = send_message_json(req, "Failed to allocate OTA buffer");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Starting OTA upload to %s (%" PRIu32 " bytes), content length=%d",
             update_partition->label, update_partition->size, req->content_len);

    result = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    if (result != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        result = send_message_json(req, "Failed to start OTA update");
        goto cleanup;
    }
    ota_started = true;

    remaining = req->content_len;
    while (remaining > 0) {
        int received = httpd_req_recv(req, buffer, std::min(remaining, APP_OTA_CHUNK_SIZE));
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "OTA upload receive failed: %d", received);
            httpd_resp_set_status(req, "400 Bad Request");
            result = send_message_json(req, "Failed to receive firmware data");
            goto cleanup;
        }

        result = esp_ota_write(ota_handle, buffer, received);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(result));
            httpd_resp_set_status(req, "500 Internal Server Error");
            result = send_message_json(req, "Failed while writing the OTA image");
            goto cleanup;
        }

        remaining -= received;
    }

    result = esp_ota_end(ota_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "OTA finalize failed: %s", esp_err_to_name(result));
        httpd_resp_set_status(req, "400 Bad Request");
        result = send_message_json(req, "Firmware validation failed");
        goto cleanup;
    }
    ota_started = false;

    // Manual uploads must enter the same probation flow as published OTA.
    // Without this marker, a build with bootloader rollback enabled can be
    // reverted on the next reboot before the application ever self-tests.
    result = esp_ota_get_partition_description(update_partition, &manual_desc);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read manual OTA image description: %s", esp_err_to_name(result));
        httpd_resp_set_status(req, "400 Bad Request");
        result = send_message_json(req, "Firmware metadata could not be read");
        goto cleanup;
    }
    // manual=true: this image self-tests on Matter readiness alone so a
    // bench-flashed device that never joins home Wi-Fi is not rolled back.
    result = ota_health_write(update_partition->label, manual_desc.version, 0, true);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write manual OTA probation marker: %s", esp_err_to_name(result));
        httpd_resp_set_status(req, "500 Internal Server Error");
        result = send_message_json(req, "Could not prepare OTA rollback protection");
        goto cleanup;
    }

    result = esp_ota_set_boot_partition(update_partition);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to select OTA partition: %s", esp_err_to_name(result));
        ota_health_clear();
        httpd_resp_set_status(req, "500 Internal Server Error");
        result = send_message_json(req, "Firmware was written but the boot slot could not be updated");
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA update stored in %s, reboot scheduled", update_partition->label);
    httpd_resp_set_status(req, "200 OK");
    result = send_message_json(req, "Firmware installed successfully. Rebooting into the new OTA slot.");
    xTaskCreate(reboot_task, "ota_reboot", 2048, nullptr, 4, nullptr);

cleanup:
    if (ota_started) {
        esp_ota_abort(ota_handle);
    }
    free(buffer);
    xSemaphoreGive(s_ota_mutex);
    return result;
}

static esp_err_t check_update_post_handler(httpd_req_t *req)
{
    esp_err_t access_err = require_softap_admin(req);
    if (access_err != ESP_OK) {
        return access_err;
    }

    if (!s_auto_update_task) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_message_json(req, "Published update task is not ready.");
    }

    set_pending_update_mode(published_update_mode_t::kCheckOnly);
    set_auto_update_state(false, false, "", "", "Published update check queued.");
    xTaskNotifyGive(s_auto_update_task);
    httpd_resp_set_status(req, "200 OK");
    return send_message_json(req, "Published update check queued.");
}

static esp_err_t install_update_post_handler(httpd_req_t *req)
{
    esp_err_t access_err = require_softap_admin(req);
    if (access_err != ESP_OK) {
        return access_err;
    }

    if (!s_auto_update_task) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_message_json(req, "Published update task is not ready.");
    }
    bool update_available = false;
    bool update_busy = false;
    char latest_version[APP_AUTO_UPDATE_VERSION_MAX] = "";
    char asset_url[APP_AUTO_UPDATE_URL_MAX] = "";
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    update_available = s_auto_update_available;
    update_busy = s_auto_update_busy;
    copy_string_value(latest_version, sizeof(latest_version), s_auto_update_latest_version);
    copy_string_value(asset_url, sizeof(asset_url), s_auto_update_asset_url);
    xSemaphoreGive(s_state_mutex);

    if (!update_available || latest_version[0] == '\0') {
        httpd_resp_set_status(req, "409 Conflict");
        return send_message_json(req,
            "No published update is currently available. Run Check For Updates first.");
    }
    if (update_busy) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_message_json(req, "A published update operation is already in progress.");
    }

    set_pending_update_mode(published_update_mode_t::kInstallForced);
    set_auto_update_state(true, true, latest_version, asset_url,
                          "Install requested. Downloading and verifying...");
    xTaskNotifyGive(s_auto_update_task);
    httpd_resp_set_status(req, "200 OK");
    return send_message_json(req, "Install queued. The device will reboot after success.");
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    esp_err_t access_err = require_softap_admin(req);
    if (access_err != ESP_OK) {
        return access_err;
    }

    httpd_resp_set_status(req, "200 OK");
    esp_err_t err = send_message_json(req, "Device will reboot now.");
    xTaskCreate(reboot_task, "device_reboot", 2048, nullptr, 4, nullptr);
    return err;
}

static esp_err_t revert_post_handler(httpd_req_t *req)
{
    esp_err_t access_err = require_softap_admin(req);
    if (access_err != ESP_OK) {
        return access_err;
    }

    esp_app_desc_t revert_desc = {};
    const esp_partition_t *revert_partition = get_revert_partition(&revert_desc);
    if (!revert_partition) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_message_json(req, "No previous firmware slot is available to revert to.");
    }

    esp_err_t err = esp_ota_set_boot_partition(revert_partition);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_message_json(req, "Failed to select the previous firmware slot.");
    }

    char message[160];
    std::snprintf(message, sizeof(message), "Reverting to %s on %s. Device will reboot now.",
                  revert_desc.version, revert_partition->label);
    httpd_resp_set_status(req, "200 OK");
    err = send_message_json(req, message);
    xTaskCreate(reboot_task, "revert_reboot", 2048, nullptr, 4, nullptr);
    return err;
}

static esp_err_t factory_reset_post_handler(httpd_req_t *req)
{
    esp_err_t access_err = require_softap_admin(req);
    if (access_err != ESP_OK) {
        return access_err;
    }

    httpd_resp_set_status(req, "200 OK");
    esp_err_t err = send_message_json(req, "Factory reset started. The device will erase settings and reboot.");
    xTaskCreate(factory_reset_task, "factory_reset", 3072, nullptr, 4, nullptr);
    return err;
}

static void effect_task(void *arg)
{
    (void) arg;

    // Displayed/eased state. Single-owner: only effect_task ever touches these,
    // so they need no mutex and MUST NOT be exposed to HTTP/Matter. They hold
    // the in-flight (eased) brightness/color; the target lives in s_led_state.
    static bool s_disp_init = false;
    static double s_disp_brightness = 0.0;  // 0..255, eased
    static double s_disp_r = 0.0;           // 0..255, eased
    static double s_disp_g = 0.0;
    static double s_disp_b = 0.0;
    static TickType_t s_disp_last_tick = 0; // for measured-dt easing

    while (true) {
        led_state_t          snapshot = {};
        led_indicator_mode_t indicator = LED_INDICATOR_NONE;
        uint8_t              indicator_code = 0;
        int64_t              indicator_started_us = 0;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        snapshot = s_led_state;
        indicator = s_indicator_mode;
        indicator_code = s_indicator_code;
        indicator_started_us = s_indicator_started_us;
        xSemaphoreGive(s_state_mutex);

        // Pairing feedback owns the strip while it runs: a blink code during an
        // open pairing window, then one green or red flash. Rendering happens
        // after the state mutex is released, exactly like the normal path.
        if (indicator != LED_INDICATOR_NONE) {
            const int64_t elapsed_ms = (esp_timer_get_time() - indicator_started_us) / 1000;
            const uint8_t level = static_cast<uint8_t>(
                std::clamp<int>(snapshot.brightness, APP_INDICATOR_MIN_LEVEL, APP_INDICATOR_MAX_LEVEL));
            uint8_t red = 0;
            uint8_t green = 0;
            uint8_t blue = 0;
            bool    done = false;

            if (indicator == LED_INDICATOR_CODE) {
                const int64_t slot = APP_INDICATOR_BLINK_ON_MS + APP_INDICATOR_BLINK_OFF_MS;
                const int64_t train = slot * (indicator_code > 0 ? indicator_code : 1);
                const int64_t cycle = train + APP_INDICATOR_GAP_MS;
                const int64_t phase = elapsed_ms % cycle;
                if (phase < train && (phase % slot) < APP_INDICATOR_BLINK_ON_MS) {
                    red = green = blue = level; // neutral white, visible in any state
                }
            } else {
                if (elapsed_ms >= APP_INDICATOR_FLASH_MS) {
                    done = true;
                } else if (indicator == LED_INDICATOR_SUCCESS) {
                    green = level;
                } else {
                    red = level;
                }
            }

            if (done) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                // Only clear what we saw: a newer indicator may have started.
                if (s_indicator_mode == indicator && s_indicator_started_us == indicator_started_us) {
                    s_indicator_mode = LED_INDICATOR_NONE;
                    s_indicator_code = 0;
                }
                xSemaphoreGive(s_state_mutex);
                // Fade the normal picture back in from black instead of
                // snapping: keep the colour targets, restart the brightness ease.
                s_disp_brightness = 0.0;
                s_disp_last_tick = xTaskGetTickCount();
                continue;
            }

            esp_err_t indicator_err = apply_solid_frame(snapshot.count, s_gamma_lut[red], s_gamma_lut[green],
                                                        s_gamma_lut[blue]);
            if (indicator_err != ESP_OK) {
                ESP_LOGW(TAG, "indicator render failed: %s", esp_err_to_name(indicator_err));
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(40));
            continue;
        }

        // Easing runs after releasing s_state_mutex and before the render (which
        // takes s_led_mutex) -- the two locks are never held at once.
        // Power is encoded entirely in the brightness target (0 when off). Color
        // targets track the snapshot unconditionally, so a color change while on
        // crossfades and a color set while off pre-positions for the next on.
        double bri_target = snapshot.power ? static_cast<double>(snapshot.brightness) : 0.0;
        double r_target = static_cast<double>(snapshot.red);
        double g_target = static_cast<double>(snapshot.green);
        double b_target = static_cast<double>(snapshot.blue);

        if (!s_disp_init) {
            // First boot: start at the correct hue (no color sweep) but dark, so
            // brightness eases 0 -> target as a graceful fade-in from black. This
            // cannot overshoot, so there is no startup flash. If power is off,
            // bri_target is 0 and the strip simply stays dark.
            s_disp_r = r_target;
            s_disp_g = g_target;
            s_disp_b = b_target;
            s_disp_brightness = 0.0;
            s_disp_last_tick = xTaskGetTickCount();
            s_disp_init = true;
        }

        // Measured-dt easing: an early notify wake advances the fade only by its
        // true elapsed time, so rapid mid-fade state changes stay time-correct
        // instead of over-easing. The delta is capped inside led_ease_advance();
        // see kLedEaseMaxDtMs for why that cap is load-bearing rather than
        // hygiene. The tick measurement and the eased storage stay here because
        // they are the task's, not the rule's.
        TickType_t now = xTaskGetTickCount();
        double dt_ms = static_cast<double>((now - s_disp_last_tick) * portTICK_PERIOD_MS);
        s_disp_last_tick = now;

        led_ease_rgb_t eased = {s_disp_brightness, s_disp_r, s_disp_g, s_disp_b};
        const led_ease_rgb_t ease_target = {bri_target, r_target, g_target, b_target};
        bool settled = led_ease_advance(&eased, &ease_target, dt_ms, kLedEaseTauMs);
        s_disp_brightness = eased.brightness;
        s_disp_r = eased.red;
        s_disp_g = eased.green;
        s_disp_b = eased.blue;

        // Build a render_state copy of the snapshot (effect, params, count come
        // straight through -- NOT eased) and override only the eased fields.
        // snapshot itself stays the TARGET for the wake decision and is what
        // Matter reads elsewhere; the eased values are display-only.
        led_state_t render_state = snapshot;
        render_state.brightness = render_float_to_u8(s_disp_brightness);
        render_state.red = render_float_to_u8(s_disp_r);
        render_state.green = render_float_to_u8(s_disp_g);
        render_state.blue = render_float_to_u8(s_disp_b);
        // Derive power from the DISPLAYED brightness so a power-off fade keeps
        // rendering frames until it truly reaches 0 (where render_effect_pixel's
        // brightness==0 guard yields black -- the settled off state).
        render_state.power = led_ease_power_from_brightness(render_state.brightness);

        esp_err_t err = apply_led_state(&render_state);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "LED render failed: %s", esp_err_to_name(err));
        }

        // Keep frames flowing while the target animates OR while a fade is in
        // flight. Block indefinitely only when both not-animated AND settled;
        // any control/Matter/schedule change calls notify_effect_task() to wake.
        const bool animated = snapshot.power && snapshot.effect != LED_EFFECT_SOLID;
        const bool settling = !settled;
        const TickType_t wait = (animated || settling) ? pdMS_TO_TICKS(40) : portMAX_DELAY;
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

// ---- Schedule / timer HTTP endpoints ---------------------------------------

static esp_err_t schedule_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Failed to build response");
    }

    schedule_entry_t schedules[APP_MAX_SCHEDULES];
    char tz[sizeof(s_tz)] = "";
    int64_t rel_deadline = 0;
    uint8_t rel_action = 0;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    std::memcpy(schedules, s_schedules, sizeof(schedules));
    copy_string_value(tz, sizeof(tz), s_tz);
    rel_deadline = s_relative_deadline_us;
    rel_action = s_relative_action;
    xSemaphoreGive(s_state_mutex);

    const bool time_valid = time_is_valid();
    char now_local[20] = "";
    if (time_valid) {
        time_t now = time(NULL);
        struct tm lt = {};
        localtime_r(&now, &lt);
        strftime(now_local, sizeof(now_local), "%Y-%m-%d %H:%M:%S", &lt);
    }
    cJSON_AddBoolToObject(root, "time_valid", time_valid);
    cJSON_AddStringToObject(root, "now_local", now_local);
    cJSON_AddStringToObject(root, "tz", tz);

    cJSON *arr = cJSON_AddArrayToObject(root, "schedules");
    for (size_t i = 0; i < APP_MAX_SCHEDULES; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "enabled", schedules[i].enabled);
        cJSON_AddNumberToObject(item, "hour", schedules[i].hour);
        cJSON_AddNumberToObject(item, "minute", schedules[i].minute);
        cJSON_AddNumberToObject(item, "days", schedules[i].days);
        cJSON_AddNumberToObject(item, "action", schedules[i].action);
        cJSON_AddItemToArray(arr, item);
    }

    cJSON *relative = cJSON_AddObjectToObject(root, "relative");
    const bool rel_active = rel_deadline != 0;
    int rel_remaining_s = 0;
    if (rel_active) {
        rel_remaining_s = schedule_relative_remaining_s(rel_deadline, esp_timer_get_time());
    }
    cJSON_AddBoolToObject(relative, "active", rel_active);
    cJSON_AddNumberToObject(relative, "action", rel_action);
    cJSON_AddNumberToObject(relative, "remaining_s", rel_remaining_s);

    char *response = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!response) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Failed to encode response");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, response);
    free(response);
    return err;
}

static esp_err_t schedule_post_handler(httpd_req_t *req)
{
    esp_err_t access_err = require_softap_admin(req);
    if (access_err != ESP_OK) {
        return access_err;
    }

    // 8 schedules + tz stay well under 2 KB; keep a dedicated limit here since
    // read_request_body() enforces the smaller APP_POST_BODY_LIMIT.
    if (req->content_len <= 0 || req->content_len >= 2048) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid request body");
    }

    char *body = static_cast<char *>(malloc(req->content_len + 1));
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Out of memory");
    }
    int remaining = req->content_len;
    int offset = 0;
    while (remaining > 0) {
        int received = httpd_req_recv(req, body + offset, remaining);
        if (received <= 0) {
            free(body);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "Failed to read request body");
        }
        offset += received;
        remaining -= received;
    }
    body[offset] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid JSON");
    }

    cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "tz");
    cJSON *schedules = cJSON_GetObjectItemCaseSensitive(root, "schedules");
    if ((tz && !cJSON_IsString(tz)) || (schedules && !cJSON_IsArray(schedules))) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid schedule payload");
    }
    if (tz && std::strlen(tz->valuestring) > sizeof(s_tz) - 1) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Time zone string too long");
    }

    bool tz_changed = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (tz) {
        copy_string_value(s_tz, sizeof(s_tz), tz->valuestring);
        tz_changed = true;
    }
    std::memset(s_schedules, 0, sizeof(s_schedules));
    for (size_t i = 0; i < APP_MAX_SCHEDULES; ++i) {
        s_sched_last_fired_min[i] = -1;
    }
    if (schedules) {
        int n = cJSON_GetArraySize(schedules);
        if (n > APP_MAX_SCHEDULES) {
            n = APP_MAX_SCHEDULES;
        }
        for (int i = 0; i < n; ++i) {
            cJSON *item = cJSON_GetArrayItem(schedules, i);
            if (!cJSON_IsObject(item)) {
                continue;
            }
            cJSON *enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
            cJSON *hour = cJSON_GetObjectItemCaseSensitive(item, "hour");
            cJSON *minute = cJSON_GetObjectItemCaseSensitive(item, "minute");
            cJSON *days = cJSON_GetObjectItemCaseSensitive(item, "days");
            cJSON *action = cJSON_GetObjectItemCaseSensitive(item, "action");
            const bool enabled_on = cJSON_IsTrue(enabled) ||
                                    (cJSON_IsNumber(enabled) && enabled->valuedouble != 0);
            const bool action_on = cJSON_IsTrue(action) ||
                                   (cJSON_IsNumber(action) && action->valuedouble != 0);
            s_schedules[i].enabled = enabled_on ? 1 : 0;
            s_schedules[i].hour = cJSON_IsNumber(hour)
                                      ? static_cast<uint8_t>(std::clamp(static_cast<int>(hour->valuedouble), 0, 23))
                                      : 0;
            s_schedules[i].minute = cJSON_IsNumber(minute)
                                        ? static_cast<uint8_t>(std::clamp(static_cast<int>(minute->valuedouble), 0, 59))
                                        : 0;
            s_schedules[i].days = cJSON_IsNumber(days)
                                      ? static_cast<uint8_t>(std::clamp(static_cast<int>(days->valuedouble), 0, 127))
                                      : 0;
            s_schedules[i].action = action_on ? 1 : 0;
        }
    }
    xSemaphoreGive(s_state_mutex);
    cJSON_Delete(root);

    if (tz_changed) {
        apply_tz();
    }
    esp_err_t save_err = save_schedules();
    if (save_err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Failed to save schedules");
    }

    return schedule_get_handler(req);
}

static esp_err_t timer_post_handler(httpd_req_t *req)
{
    char *body = read_request_body(req);
    if (!body) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Failed to read request body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid JSON");
    }

    cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    cJSON *minutes = cJSON_GetObjectItemCaseSensitive(root, "minutes");
    if (!cJSON_IsNumber(action) || !cJSON_IsNumber(minutes)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Missing fields");
    }
    int action_val = static_cast<int>(action->valuedouble);
    if (action_val != 0 && action_val != 1) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid action");
    }
    int minutes_val = std::clamp(static_cast<int>(minutes->valuedouble), 0, 1440);
    cJSON_Delete(root);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (minutes_val <= 0) {
        s_relative_deadline_us = 0;
    } else {
        s_relative_action = static_cast<uint8_t>(action_val ? 1 : 0);
        s_relative_deadline_us = esp_timer_get_time() + static_cast<int64_t>(minutes_val) * 60 * 1000000;
    }
    xSemaphoreGive(s_state_mutex);

    char message[64];
    if (minutes_val <= 0) {
        std::snprintf(message, sizeof(message), "Timer cancelled.");
    } else {
        std::snprintf(message, sizeof(message), "Will turn %s in %d minute%s.",
                      action_val ? "on" : "off", minutes_val, minutes_val == 1 ? "" : "s");
    }
    return send_message_json(req, message);
}

static void start_webserver()
{
    if (s_http_server) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 28;
    config.stack_size = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    ESP_ERROR_CHECK(httpd_start(&s_http_server, &config));

    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = root_get_handler;

    httpd_uri_t state_get = {};
    state_get.uri = "/api/state";
    state_get.method = HTTP_GET;
    state_get.handler = state_get_handler;

    httpd_uri_t state_post = {};
    state_post.uri = "/api/state";
    state_post.method = HTTP_POST;
    state_post.handler = state_post_handler;

    httpd_uri_t control_post = {};
    control_post.uri = "/api/control";
    control_post.method = HTTP_POST;
    control_post.handler = control_post_handler;

    httpd_uri_t config_post = {};
    config_post.uri = "/api/config";
    config_post.method = HTTP_POST;
    config_post.handler = config_post_handler;

    httpd_uri_t mqtt_unpair_post = {};
    mqtt_unpair_post.uri = "/api/mqtt/unpair";
    mqtt_unpair_post.method = HTTP_POST;
    mqtt_unpair_post.handler = mqtt_unpair_post_handler;

    httpd_uri_t matter_pairing_window_post = {};
    matter_pairing_window_post.uri = "/api/matter/pairing-window";
    matter_pairing_window_post.method = HTTP_POST;
    matter_pairing_window_post.handler = matter_pairing_window_post_handler;

    httpd_uri_t ota_post = {};
    ota_post.uri = "/api/ota";
    ota_post.method = HTTP_POST;
    ota_post.handler = ota_post_handler;

    httpd_uri_t check_update_post = {};
    check_update_post.uri = "/api/check-update";
    check_update_post.method = HTTP_POST;
    check_update_post.handler = check_update_post_handler;

    httpd_uri_t install_update_post = {};
    install_update_post.uri = "/api/install-update";
    install_update_post.method = HTTP_POST;
    install_update_post.handler = install_update_post_handler;

    httpd_uri_t reboot_post = {};
    reboot_post.uri = "/api/reboot";
    reboot_post.method = HTTP_POST;
    reboot_post.handler = reboot_post_handler;

    httpd_uri_t revert_post = {};
    revert_post.uri = "/api/revert";
    revert_post.method = HTTP_POST;
    revert_post.handler = revert_post_handler;

    httpd_uri_t factory_reset_post = {};
    factory_reset_post.uri = "/api/factory-reset";
    factory_reset_post.method = HTTP_POST;
    factory_reset_post.handler = factory_reset_post_handler;

    httpd_uri_t schedule_get = {};
    schedule_get.uri = "/api/schedule";
    schedule_get.method = HTTP_GET;
    schedule_get.handler = schedule_get_handler;

    httpd_uri_t schedule_post = {};
    schedule_post.uri = "/api/schedule";
    schedule_post.method = HTTP_POST;
    schedule_post.handler = schedule_post_handler;

    httpd_uri_t timer_post = {};
    timer_post.uri = "/api/timer";
    timer_post.method = HTTP_POST;
    timer_post.handler = timer_post_handler;

    httpd_uri_t generate_204 = {};
    generate_204.uri = "/generate_204";
    generate_204.method = HTTP_GET;
    generate_204.handler = captive_redirect_handler;

    httpd_uri_t hotspot_detect = {};
    hotspot_detect.uri = "/hotspot-detect.html";
    hotspot_detect.method = HTTP_GET;
    hotspot_detect.handler = captive_redirect_handler;

    httpd_uri_t ncsi = {};
    ncsi.uri = "/ncsi.txt";
    ncsi.method = HTTP_GET;
    ncsi.handler = captive_redirect_handler;

    httpd_uri_t connecttest = {};
    connecttest.uri = "/connecttest.txt";
    connecttest.method = HTTP_GET;
    connecttest.handler = captive_redirect_handler;

    httpd_uri_t canonical = {};
    canonical.uri = "/canonical.html";
    canonical.method = HTTP_GET;
    canonical.handler = captive_redirect_handler;

    httpd_uri_t fwlink = {};
    fwlink.uri = "/fwlink";
    fwlink.method = HTTP_GET;
    fwlink.handler = captive_redirect_handler;

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &state_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &state_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &control_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &config_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &mqtt_unpair_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &matter_pairing_window_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &ota_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &check_update_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &install_update_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &reboot_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &revert_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &factory_reset_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &schedule_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &schedule_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &timer_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &generate_204));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &hotspot_detect));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &ncsi));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &connecttest));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &canonical));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &fwlink));
    ESP_ERROR_CHECK(httpd_register_err_handler(s_http_server, HTTPD_404_NOT_FOUND, not_found_handler));
}

static size_t dns_skip_name(const uint8_t *packet, size_t length, size_t offset)
{
    while (offset < length) {
        uint8_t label_len = packet[offset];
        if (label_len == 0) {
            return offset + 1;
        }
        if ((label_len & 0xC0) == 0xC0) {
            return offset + 2;
        }
        offset += label_len + 1;
    }
    return 0;
}

static size_t build_dns_response(const uint8_t *query, size_t query_len, uint8_t *response, size_t response_len)
{
    if (query_len < 12 || response_len < 12) {
        return 0;
    }

    uint16_t qdcount = (static_cast<uint16_t>(query[4]) << 8) | query[5];
    if (qdcount == 0) {
        return 0;
    }

    size_t question_end = dns_skip_name(query, query_len, 12);
    if (question_end == 0 || question_end + 4 > query_len) {
        return 0;
    }

    uint16_t qtype = (static_cast<uint16_t>(query[question_end]) << 8) | query[question_end + 1];
    uint16_t qclass = (static_cast<uint16_t>(query[question_end + 2]) << 8) | query[question_end + 3];

    if (question_end + 4 + 16 > response_len) {
        return 0;
    }

    std::memcpy(response, query, question_end + 4);
    response[2] = 0x81;
    response[3] = 0x80;
    response[4] = 0x00;
    response[5] = 0x01;
    response[6] = 0x00;
    response[7] = (qtype == 1 && qclass == 1) ? 0x01 : 0x00;
    response[8] = 0x00;
    response[9] = 0x00;
    response[10] = 0x00;
    response[11] = 0x00;

    if (qtype != 1 || qclass != 1) {
        return question_end + 4;
    }

    size_t offset = question_end + 4;
    response[offset++] = 0xC0;
    response[offset++] = 0x0C;
    response[offset++] = 0x00;
    response[offset++] = 0x01;
    response[offset++] = 0x00;
    response[offset++] = 0x01;
    response[offset++] = 0x00;
    response[offset++] = 0x00;
    response[offset++] = 0x00;
    response[offset++] = 0x3C;
    response[offset++] = 0x00;
    response[offset++] = 0x04;

    unsigned int octet0 = 192;
    unsigned int octet1 = 168;
    unsigned int octet2 = 4;
    unsigned int octet3 = 1;
    if (std::sscanf(s_ap_ip, "%u.%u.%u.%u", &octet0, &octet1, &octet2, &octet3) != 4) {
        return 0;
    }
    response[offset++] = static_cast<uint8_t>(octet0);
    response[offset++] = static_cast<uint8_t>(octet1);
    response[offset++] = static_cast<uint8_t>(octet2);
    response[offset++] = static_cast<uint8_t>(octet3);
    return offset;
}

static void captive_dns_task(void *arg)
{
    (void) arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(nullptr);
        return;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(sock);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Captive DNS server listening on %s:53", s_ap_ip);

    while (true) {
        uint8_t query[512];
        uint8_t response[512];
        struct sockaddr_in client_addr = {};
        socklen_t client_len = sizeof(client_addr);
        ssize_t received = recvfrom(sock, query, sizeof(query), 0, reinterpret_cast<struct sockaddr *>(&client_addr), &client_len);
        if (received <= 0) {
            continue;
        }

        size_t response_len = build_dns_response(query, static_cast<size_t>(received), response, sizeof(response));
        if (response_len > 0) {
            sendto(sock, response, response_len, 0, reinterpret_cast<struct sockaddr *>(&client_addr), client_len);
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void) arg;
    if (event_base != WIFI_EVENT) {
        return;
    }

    switch (event_id) {
    case WIFI_EVENT_AP_STACONNECTED: {
        auto *event = static_cast<wifi_event_ap_staconnected_t *>(event_data);
        ESP_LOGI(TAG, "station " MACSTR " joined, aid=%d", MAC2STR(event->mac), event->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        auto *event = static_cast<wifi_event_ap_stadisconnected_t *>(event_data);
        ESP_LOGI(TAG, "station " MACSTR " left, aid=%d", MAC2STR(event->mac), event->aid);
        break;
    }
    case WIFI_EVENT_STA_CONNECTED:
        s_sta_connect_count++;
        s_sta_last_event_ms = esp_log_timestamp();
        s_sta_last_disconnect_reason = 0;
        {
            wifi_ap_record_t ap_info = {};
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                copy_string_value(s_sta_ssid, sizeof(s_sta_ssid), reinterpret_cast<const char *>(ap_info.ssid));
                std::snprintf(s_sta_bssid, sizeof(s_sta_bssid), MACSTR, MAC2STR(ap_info.bssid));
                s_sta_rssi = ap_info.rssi;
                s_sta_channel = ap_info.primary;
            }
        }
        ESP_LOGI(TAG, "Matter station connected to upstream Wi-Fi");
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        s_sta_disconnect_count++;
        s_sta_last_event_ms = esp_log_timestamp();
        if (event_data) {
            auto *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
            s_sta_last_disconnect_reason = event->reason;
            ESP_LOGW(TAG, "Matter station disconnected, reason=%u", static_cast<unsigned int>(event->reason));
        }
        s_sta_ip[0] = '\0';
        set_auto_update_state(false, false, s_auto_update_latest_version, s_auto_update_asset_url,
                              "Waiting for LAN Wi-Fi before checking published updates.");
        mqtt_link_network_down();
        ESP_LOGI(TAG, "Matter station disconnected from upstream Wi-Fi");
        break;
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void) arg;
    if (event_base != IP_EVENT) {
        return;
    }

    if (event_id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        std::snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_last_ip_ms = esp_log_timestamp();
        ESP_LOGI(TAG, "Matter station IP: %s", s_sta_ip);
        ESP_LOGI(TAG, "LAN web UI available at http://%s", s_sta_ip);
        start_sntp_once();
        mqtt_link_network_up();
        if (s_auto_update_task) {
            // Refresh available-version info — never auto-install.
            set_pending_update_mode(published_update_mode_t::kCheckOnly);
            xTaskNotifyGive(s_auto_update_task);
        }
    }
}

static esp_err_t ensure_wifi_started()
{
    int8_t ignored = 0;
    esp_err_t err = esp_wifi_get_max_tx_power(&ignored);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        return esp_wifi_start();
    }
    return err;
}

static void start_softap_overlay()
{
    if (!s_network_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, nullptr));
        s_network_handlers_registered = true;
    }

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        ESP_ERROR_CHECK(s_ap_netif ? ESP_OK : ESP_FAIL);
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    ESP_ERROR_CHECK(esp_wifi_get_mode(&mode));
    if (mode == WIFI_MODE_STA) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    } else if (mode == WIFI_MODE_NULL) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    }

    wifi_config_t wifi_config = {};
    std::memset(wifi_config.ap.ssid, 0, sizeof(wifi_config.ap.ssid));
    std::memset(wifi_config.ap.password, 0, sizeof(wifi_config.ap.password));
    size_t ssid_len = std::min(sizeof(wifi_config.ap.ssid), std::strlen(s_ap_ssid));
    size_t password_len = std::min(sizeof(wifi_config.ap.password) - 1, std::strlen(s_ap_password));
    std::memcpy(wifi_config.ap.ssid, s_ap_ssid, ssid_len);
    std::memcpy(wifi_config.ap.password, s_ap_password, password_len);
    wifi_config.ap.ssid_len = ssid_len;
    wifi_config.ap.channel = APP_WIFI_CHANNEL;
    wifi_config.ap.max_connection = APP_WIFI_MAX_STA_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    copy_string_value(s_runtime_ap_ssid, sizeof(s_runtime_ap_ssid), s_ap_ssid);
    copy_string_value(s_runtime_ap_password, sizeof(s_runtime_ap_password), s_ap_password);
    ESP_ERROR_CHECK(ensure_wifi_started());

    refresh_ip_strings();
    ESP_LOGI(TAG, "SoftAP started: SSID=%s password=%s", s_runtime_ap_ssid, s_runtime_ap_password);
    ESP_LOGI(TAG, "Open http://%s", s_ap_ip);
    if (s_sta_ip[0] != '\0') {
        ESP_LOGI(TAG, "LAN access available at http://%s", s_sta_ip);
    }
}

static void init_led_strip()
{
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = APP_LED_GPIO;
    strip_config.max_leds = APP_LED_MAX_PIXELS;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = APP_RMT_RESOLUTION_HZ;
    rmt_config.flags.with_dma = false;

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip));
    ESP_ERROR_CHECK(led_strip_clear(s_led_strip));
}

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    (void) endpoint_id;
    (void) priv_data;
    ESP_LOGI(TAG, "Identification callback: type=%u effect=%u variant=%u", type, effect_id, effect_variant);
    return ESP_OK;
}

static esp_err_t persist_state_locked()
{
    return save_state_to_nvs(&s_led_state);
}

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    (void) priv_data;
    if (type != esp_matter::attribute::PRE_UPDATE || endpoint_id != s_light_endpoint_id || s_syncing_matter) {
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    led_state_t updated = s_led_state;

    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        updated.power = val->val.b;
    } else if (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
        updated.brightness = matter_level_to_brightness(val->val.u8);
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::CurrentHue::Id) {
        s_matter_hue = val->val.u8;
        color_matter_hs_to_rgb(s_matter_hue, s_matter_saturation, &updated.red, &updated.green, &updated.blue);
        color_rgb_to_matter_xy(updated.red, updated.green, updated.blue, &s_matter_x, &s_matter_y);
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::CurrentSaturation::Id) {
        s_matter_saturation = val->val.u8;
        color_matter_hs_to_rgb(s_matter_hue, s_matter_saturation, &updated.red, &updated.green, &updated.blue);
        color_rgb_to_matter_xy(updated.red, updated.green, updated.blue, &s_matter_x, &s_matter_y);
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::CurrentX::Id) {
        s_matter_x = val->val.u16;
        color_matter_xy_to_rgb(s_matter_x, s_matter_y, &updated.red, &updated.green, &updated.blue);
        refresh_matter_hs_trackers_from_rgb(updated.red, updated.green, updated.blue);
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::CurrentY::Id) {
        s_matter_y = val->val.u16;
        color_matter_xy_to_rgb(s_matter_x, s_matter_y, &updated.red, &updated.green, &updated.blue);
        refresh_matter_hs_trackers_from_rgb(updated.red, updated.green, updated.blue);
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
        s_matter_temp_mireds = val->val.u16;
        color_temp_mireds_to_rgb(s_matter_temp_mireds, &updated.red, &updated.green, &updated.blue);
        refresh_matter_hs_trackers_from_rgb(updated.red, updated.green, updated.blue);
        color_rgb_to_matter_xy(updated.red, updated.green, updated.blue, &s_matter_x, &s_matter_y);
    } else {
        xSemaphoreGive(s_state_mutex);
        return ESP_OK;
    }

    led_clamp_state(&updated, APP_LED_MAX_PIXELS);
    s_led_state = updated;
    err = persist_state_locked();
    xSemaphoreGive(s_state_mutex);
    // Drive the strip even if the NVS persist failed — RAM state is the source
    // of truth for the effect task; the persist error is still returned.
    notify_effect_task();
    // Tell the HMI a remote change happened. Cheap: this only marks the link's
    // coalescer and notifies its task, so the Matter callback never blocks.
    mqtt_link_state_changed("matter");
    return err;
}

static void set_initial_matter_color_attributes()
{
    uint8_t hue = 0;
    uint8_t saturation = 0;
    color_rgb_to_matter_hs(s_led_state.red, s_led_state.green, s_led_state.blue, &hue, &saturation);
    s_matter_hue = hue;
    s_matter_saturation = saturation;

    attribute_t *attribute = attribute::get(s_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentHue::Id);
    if (attribute) {
        esp_matter_attr_val_t value = esp_matter_uint8(hue);
        attribute::set_val(attribute, &value);
    }

    attribute = attribute::get(s_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentSaturation::Id);
    if (attribute) {
        esp_matter_attr_val_t value = esp_matter_uint8(saturation);
        attribute::set_val(attribute, &value);
    }

    attribute = attribute::get(s_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::ColorMode::Id);
    if (attribute) {
        esp_matter_attr_val_t value = esp_matter_enum8(static_cast<uint8_t>(ColorControl::ColorMode::kCurrentHueAndCurrentSaturation));
        attribute::set_val(attribute, &value);
    }

    attribute = attribute::get(s_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::EnhancedColorMode::Id);
    if (attribute) {
        esp_matter_attr_val_t value = esp_matter_enum8(static_cast<uint8_t>(ColorControl::ColorMode::kCurrentHueAndCurrentSaturation));
        attribute::set_val(attribute, &value);
    }
}

static void configure_matter_node()
{
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    assert(node != nullptr);

    endpoint::extended_color_light::config_t light_config;
    light_config.on_off.on_off = s_led_state.power;
    light_config.on_off_lighting.start_up_on_off = nullable<uint8_t>(s_led_state.power ? 1 : 0);
    light_config.level_control.current_level = nullable<uint8_t>(brightness_to_matter_level(s_led_state.brightness));
    light_config.level_control.on_level = nullable<uint8_t>(brightness_to_matter_level(s_led_state.brightness));
    light_config.level_control_lighting.start_up_current_level = nullable<uint8_t>(brightness_to_matter_level(s_led_state.brightness));
    light_config.color_control.color_mode = static_cast<uint8_t>(ColorControl::ColorMode::kCurrentHueAndCurrentSaturation);
    light_config.color_control.enhanced_color_mode = static_cast<uint8_t>(ColorControl::ColorMode::kCurrentHueAndCurrentSaturation);
    light_config.color_control_color_temperature.color_temperature_mireds = s_matter_temp_mireds;
    light_config.color_control_color_temperature.start_up_color_temperature_mireds = nullable<uint16_t>(s_matter_temp_mireds);
    light_config.color_control_xy.current_x = s_matter_x;
    light_config.color_control_xy.current_y = s_matter_y;

    endpoint_t *endpoint = endpoint::extended_color_light::create(node, &light_config, ENDPOINT_FLAG_NONE, nullptr);
    assert(endpoint != nullptr);
    s_light_endpoint_id = endpoint::get_id(endpoint);

    set_initial_matter_color_attributes();

    attribute_t *level_attribute = attribute::get(s_light_endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
    if (level_attribute) {
        attribute::set_deferred_persistence(level_attribute);
    }

    attribute_t *hue_attribute = attribute::get(s_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentHue::Id);
    if (hue_attribute) {
        attribute::set_deferred_persistence(hue_attribute);
    }

    attribute_t *saturation_attribute = attribute::get(s_light_endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentSaturation::Id);
    if (saturation_attribute) {
        attribute::set_deferred_persistence(saturation_attribute);
    }

    attribute_t *temperature_attribute = attribute::get(s_light_endpoint_id, ColorControl::Id,
                                                        ColorControl::Attributes::ColorTemperatureMireds::Id);
    if (temperature_attribute) {
        attribute::set_deferred_persistence(temperature_attribute);
    }
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    (void) arg;
    s_matter_last_event_ms = esp_log_timestamp();
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        s_matter_commissioned_count++;
        ESP_LOGI(TAG, "Matter commissioning complete");
        refresh_ip_strings();
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Matter commissioning session started");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Matter commissioning session stopped");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Matter commissioning window opened");
        refresh_matter_onboarding_data();
        PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Matter commissioning window closed");
        break;

    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        refresh_ip_strings();
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
        ESP_LOGI(TAG, "Matter fabric removed");
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            auto &commission_mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
            if (!commission_mgr.IsCommissioningWindowOpen()) {
                CHIP_ERROR err = commission_mgr.OpenBasicCommissioningWindow(
                    chip::System::Clock::Seconds16(kCommissioningTimeoutSeconds),
                    chip::CommissioningWindowAdvertisement::kDnssdOnly);
                if (err != CHIP_NO_ERROR) {
                    ESP_LOGE(TAG, "Failed to reopen commissioning window: %" CHIP_ERROR_FORMAT, err.Format());
                }
            }
        }
        break;
    }

    default:
        break;
    }
}

extern "C" void app_main()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Probationary-boot check — must run before any task that might crash and
    // take us back through the bootloader without incrementing the strike count.
    init_ota_probation();

    s_state_mutex = xSemaphoreCreateMutex();
    assert(s_state_mutex != nullptr);
    s_led_mutex = xSemaphoreCreateMutex();
    assert(s_led_mutex != nullptr);
    s_ota_mutex = xSemaphoreCreateMutex();
    assert(s_ota_mutex != nullptr);

    led_reset_effect_profiles_to_defaults(&s_led_state);
    led_reset_effect_colors_to_defaults(&s_led_state);
    set_generated_ap_credentials();
    bool loaded_state_from_nvs = load_state_from_nvs();
    load_schedules();
    apply_startup_power_policy();
    init_led_strip();
    init_gamma_lut();  // populate s_gamma_lut before effect_task's first render

    if (!loaded_state_from_nvs) {
        ESP_ERROR_CHECK(save_state_to_nvs(&s_led_state));
    }

    configure_matter_node();
    ESP_ERROR_CHECK(esp_matter::start(app_event_cb));
    refresh_matter_onboarding_data();
    PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));

    start_softap_overlay();
    // The link needs the station MAC (available now) and its own NVS keys; it
    // only dials out once the station has an IP.
    mqtt_link_init();
    start_webserver();
    set_auto_update_state(false, false, "", "",
                          "Waiting for LAN Wi-Fi before checking published updates.");
    xTaskCreate(auto_update_task, "auto_update", 8192, nullptr, 4, &s_auto_update_task);
    xTaskCreate(self_test_task, "self_test", 4096, nullptr, 5, nullptr);
    xTaskCreate(effect_task, "effect_task", 4096, nullptr, 4, &s_effect_task);
    xTaskCreate(captive_dns_task, "captive_dns", 4096, nullptr, 4, nullptr);
    xTaskCreate(schedule_task, "schedule", 4096, nullptr, 4, nullptr);

    // Free heap is logged here and again when the MQTT link connects, so the
    // cost of the link on a running device can be read off the serial monitor.
    ESP_LOGI(TAG,
             "Project ready. LEDs=%u power=%u brightness=%u effect=%s color=#%02X%02X%02X free heap=%" PRIu32 " B",
             s_led_state.count, s_led_state.power, s_led_state.brightness, led_effect_to_name(s_led_state.effect),
             s_led_state.red, s_led_state.green, s_led_state.blue, esp_get_free_heap_size());
}
