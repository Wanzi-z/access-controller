#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "automation.h"
#include "enrollment.h"
#include "wiegand.h"
#include "wiegand_registry.h"
#include "rf_registry.h"
#include "store.h"
#include "esp_system.h"

static const char *API_TAG = "api_server";

#define STATE_RESPONSE_BUFFER_SIZE 24576
#define OTA_UPLOAD_BUFFER_SIZE 4096
#define OTA_REBOOT_DELAY_MS 1500
#define WIFI_REBOOT_DELAY_MS 1000

#ifndef BUILD_GIT_COMMIT
#define BUILD_GIT_COMMIT "unknown"
#endif

#ifndef BUILD_GIT_BRANCH
#define BUILD_GIT_BRANCH "unknown"
#endif

#ifndef BUILD_GIT_DIRTY
#define BUILD_GIT_DIRTY 0
#endif

static SemaphoreHandle_t s_response_mutex;
static SemaphoreHandle_t s_ota_mutex;
static SemaphoreHandle_t s_state_snapshot_mutex;
static char s_state_response_buffer[STATE_RESPONSE_BUFFER_SIZE];

static const char *wifi_auth_mode_name(wifi_auth_mode_t authmode);

extern cJSON *lock_state_snapshot(void);
extern cJSON *exit_state_snapshot(void);
extern cJSON *fob_state_snapshot(void);
extern cJSON *keypad_state_snapshot(void);
extern cJSON *motion_state_snapshot(void);
extern cJSON *wiegand_state_snapshot(void);
extern cJSON *wiegand_state_summary_snapshot(void);
extern cJSON *system_logs_snapshot(void);
extern cJSON *rf_receiver_diagnostics_snapshot(void);
extern cJSON *rf_receiver_diagnostics_summary_snapshot(void);
extern cJSON *rf_receiver_line_test_snapshot(void);
extern void buzzer_set_quiet_test_mode(bool enabled);
extern bool buzzer_get_quiet_test_mode(void);
extern void beep_keypad_force(int beeps, int channel);
extern void keypad_push_test(int channel, int pulses, int active_ms, int idle_ms, bool active_high);

extern void handle_lock_message(cJSON *payload);
extern void handle_exit_message(cJSON *payload);
extern void handle_fob_message(cJSON *payload);
extern void handle_keypad_message(cJSON *payload);
extern void handle_motion_message(cJSON *payload);
extern void handle_authorize_message(cJSON *payload);

extern char device_id[100];
extern uint32_t get_u32(const char *key, uint32_t default_value);
extern uint32_t get_user_count_from_flash(void);
extern cJSON *load_user_from_flash(uint32_t user_id);
extern esp_err_t store_user_to_flash(char *uuid, char *name, char *pin);
extern esp_err_t delete_user_from_flash(const char *uuid);
extern esp_err_t delete_all_users_from_flash(void);
extern void modify_user_from_flash(const char *uuid, const char *newName, const char *newPin);

// Forward declaration
static cJSON *keypad_users_snapshot(void);

static void delayed_restart_task(void *arg) {
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

static void schedule_restart(uint32_t delay_ms) {
    xTaskCreate(delayed_restart_task, "delayed_restart", 2048, (void *)(uintptr_t)delay_ms, 5, NULL);
}

static void mac_to_string(const uint8_t mac[6], char *buf, size_t size) {
    if (!buf || size == 0) {
        return;
    }
    snprintf(buf, size, MACSTR, MAC2STR(mac));
}

static const char *api_reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "Power-on";
        case ESP_RST_EXT: return "External";
        case ESP_RST_SW: return "Software";
        case ESP_RST_PANIC: return "Panic";
        case ESP_RST_INT_WDT: return "Interrupt WDT";
        case ESP_RST_TASK_WDT: return "Task WDT";
        case ESP_RST_WDT: return "Other WDT";
        case ESP_RST_DEEPSLEEP: return "Deep sleep";
        case ESP_RST_BROWNOUT: return "Brownout";
        case ESP_RST_SDIO: return "SDIO";
        default: return "Unknown";
    }
}

static void add_netif_ip(cJSON *object, const char *field, const char *if_key) {
    if (!object || !field || !if_key) {
        return;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(if_key);
    if (!netif) {
        cJSON_AddNullToObject(object, field);
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        cJSON_AddNullToObject(object, field);
        return;
    }

    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
    cJSON_AddStringToObject(object, field, ip);
}

static void add_netif_ip_detail(cJSON *object, const char *prefix, const char *if_key) {
    if (!object || !prefix || !if_key) {
        return;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(if_key);
    if (!netif) {
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return;
    }

    char field[32];
    char ip[16];

    snprintf(field, sizeof(field), "%s_gateway", prefix);
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.gw));
    cJSON_AddStringToObject(object, field, ip);

    snprintf(field, sizeof(field), "%s_netmask", prefix);
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.netmask));
    cJSON_AddStringToObject(object, field, ip);
}

static int wifi_quality_from_rssi(int rssi) {
    int quality = ((rssi + 90) * 100) / 60;
    if (quality < 0) {
        return 0;
    }
    if (quality > 100) {
        return 100;
    }
    return quality;
}

static cJSON *network_state_snapshot(void) {
    cJSON *network = cJSON_CreateObject();
    if (!network) {
        return NULL;
    }

    add_netif_ip(network, "wifi_sta_ip", "WIFI_STA_DEF");
    add_netif_ip(network, "wifi_ap_ip", "WIFI_AP_DEF");
    add_netif_ip(network, "eth_ip", "ETH_DEF");
    add_netif_ip_detail(network, "wifi_sta", "WIFI_STA_DEF");

    uint8_t mac[6] = {0};
    char mac_str[18];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        mac_to_string(mac, mac_str, sizeof(mac_str));
        cJSON_AddStringToObject(network, "wifi_sta_mac", mac_str);
    } else {
        cJSON_AddNullToObject(network, "wifi_sta_mac");
    }

    if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
        mac_to_string(mac, mac_str, sizeof(mac_str));
        cJSON_AddStringToObject(network, "wifi_ap_mac", mac_str);
    } else {
        cJSON_AddNullToObject(network, "wifi_ap_mac");
    }

    if (esp_read_mac(mac, ESP_MAC_ETH) == ESP_OK) {
        mac_to_string(mac, mac_str, sizeof(mac_str));
        cJSON_AddStringToObject(network, "eth_mac", mac_str);
    } else {
        cJSON_AddNullToObject(network, "eth_mac");
    }

    wifi_ap_record_t ap_info;
    memset(&ap_info, 0, sizeof(ap_info));
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddBoolToObject(network, "wifi_sta_connected", true);
        cJSON_AddNumberToObject(network, "wifi_sta_rssi", ap_info.rssi);
        cJSON_AddNumberToObject(network, "wifi_sta_quality", wifi_quality_from_rssi(ap_info.rssi));
        cJSON_AddNumberToObject(network, "wifi_sta_channel", ap_info.primary);
        cJSON_AddStringToObject(network, "wifi_sta_auth", wifi_auth_mode_name(ap_info.authmode));
        mac_to_string(ap_info.bssid, mac_str, sizeof(mac_str));
        cJSON_AddStringToObject(network, "wifi_sta_bssid", mac_str);
    } else {
        cJSON_AddBoolToObject(network, "wifi_sta_connected", false);
        cJSON_AddNullToObject(network, "wifi_sta_rssi");
        cJSON_AddNullToObject(network, "wifi_sta_quality");
        cJSON_AddNullToObject(network, "wifi_sta_channel");
        cJSON_AddNullToObject(network, "wifi_sta_auth");
        cJSON_AddNullToObject(network, "wifi_sta_bssid");
    }

    return network;
}

static const char *wifi_auth_mode_name(wifi_auth_mode_t authmode) {
    switch (authmode) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2 Enterprise";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "Secured";
    }
}

static cJSON *wifi_scan_snapshot(void) {
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        return NULL;
    }

    wifi_mode_t original_mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&original_mode);
    if (err != ESP_OK || original_mode == WIFI_MODE_NULL) {
        ESP_LOGW(API_TAG, "Wi-Fi scan unavailable (%s)", esp_err_to_name(err));
        return array;
    }

    bool restore_ap_mode = false;
    if (original_mode == WIFI_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGW(API_TAG, "Failed to enable APSTA for scan (%s)", esp_err_to_name(err));
            return array;
        }
        restore_ap_mode = true;
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    err = esp_wifi_scan_start(&scan_config, true);
    if (err == ESP_OK) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        uint16_t record_count = ap_count > 20 ? 20 : ap_count;
        wifi_ap_record_t records[20];
        memset(records, 0, sizeof(records));
        err = esp_wifi_scan_get_ap_records(&record_count, records);
        if (err == ESP_OK) {
            for (uint16_t i = 0; i < record_count; i++) {
                if (records[i].ssid[0] == '\0') {
                    continue;
                }
                cJSON *item = cJSON_CreateObject();
                if (!item) {
                    continue;
                }
                char bssid[18];
                snprintf(bssid, sizeof(bssid), MACSTR, MAC2STR(records[i].bssid));
                cJSON_AddStringToObject(item, "ssid", (const char *)records[i].ssid);
                cJSON_AddStringToObject(item, "bssid", bssid);
                cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
                cJSON_AddNumberToObject(item, "channel", records[i].primary);
                cJSON_AddStringToObject(item, "auth", wifi_auth_mode_name(records[i].authmode));
                cJSON_AddBoolToObject(item, "secure", records[i].authmode != WIFI_AUTH_OPEN);
                cJSON_AddItemToArray(array, item);
            }
        }
    }

    if (err != ESP_OK) {
        ESP_LOGW(API_TAG, "Wi-Fi scan failed (%s)", esp_err_to_name(err));
    }

    if (restore_ap_mode) {
        esp_err_t restore_err = esp_wifi_set_mode(original_mode);
        if (restore_err != ESP_OK) {
            ESP_LOGW(API_TAG, "Failed to restore Wi-Fi mode after scan (%s)", esp_err_to_name(restore_err));
        }
    }

    return array;
}

static const char *ota_state_name(esp_ota_img_states_t state) {
    switch (state) {
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID: return "valid";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted";
        case ESP_OTA_IMG_UNDEFINED:
        default:
            return "undefined";
    }
}

static void add_partition_info(cJSON *parent, const char *field, const esp_partition_t *partition) {
    if (!parent || !field) {
        return;
    }

    cJSON *object = cJSON_CreateObject();
    if (!object) {
        return;
    }

    if (partition) {
        cJSON_AddStringToObject(object, "label", partition->label);
        cJSON_AddNumberToObject(object, "subtype", partition->subtype);
        cJSON_AddNumberToObject(object, "address", partition->address);
        cJSON_AddNumberToObject(object, "size", partition->size);
    } else {
        cJSON_AddNullToObject(object, "label");
        cJSON_AddNullToObject(object, "subtype");
        cJSON_AddNullToObject(object, "address");
        cJSON_AddNullToObject(object, "size");
    }

    cJSON_AddItemToObject(parent, field, object);
}

static cJSON *build_firmware_info_object(void) {
    cJSON *firmware = cJSON_CreateObject();
    if (!firmware) {
        return NULL;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        cJSON_AddStringToObject(firmware, "projectName", desc->project_name);
        cJSON_AddStringToObject(firmware, "projectVersion", desc->version);
        cJSON_AddStringToObject(firmware, "idfVersion", desc->idf_ver);
        cJSON_AddStringToObject(firmware, "buildDate", desc->date);
        cJSON_AddStringToObject(firmware, "buildTime", desc->time);
    }
    cJSON_AddStringToObject(firmware, "gitCommit", BUILD_GIT_COMMIT);
    cJSON_AddStringToObject(firmware, "gitBranch", BUILD_GIT_BRANCH);
    cJSON_AddBoolToObject(firmware, "gitDirty", BUILD_GIT_DIRTY != 0);

    char elf_sha[65] = {0};
    esp_app_get_elf_sha256(elf_sha, sizeof(elf_sha));
    cJSON_AddStringToObject(firmware, "elfSha256", elf_sha);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    add_partition_info(firmware, "runningPartition", running);
    add_partition_info(firmware, "bootPartition", boot);
    add_partition_info(firmware, "nextUpdatePartition", next);

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        cJSON_AddStringToObject(firmware, "otaState", ota_state_name(state));
    } else {
        cJSON_AddStringToObject(firmware, "otaState", "undefined");
    }

    cJSON_AddNumberToObject(firmware, "otaPartitionCount", esp_ota_get_app_partition_count());
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    cJSON_AddBoolToObject(firmware, "rollbackEnabled", true);
#else
    cJSON_AddBoolToObject(firmware, "rollbackEnabled", false);
#endif
    cJSON_AddBoolToObject(firmware, "rollbackPossible", esp_ota_check_rollback_is_possible());
    cJSON_AddNumberToObject(firmware, "maxUploadBytes", next ? next->size : 0);

    return firmware;
}

static void add_firmware_info(cJSON *system) {
    static cJSON *cached_firmware = NULL;
    static int64_t cached_at_us = 0;
    const int64_t now_us = esp_timer_get_time();

    if (!system) {
        return;
    }

    if (!cached_firmware || now_us - cached_at_us > 1000000LL) {
        cJSON *fresh = build_firmware_info_object();
        if (fresh) {
            cJSON_Delete(cached_firmware);
            cached_firmware = fresh;
            cached_at_us = now_us;
        }
    }

    cJSON *firmware = cached_firmware ? cJSON_Duplicate(cached_firmware, true) : build_firmware_info_object();
    if (firmware) {
        cJSON_AddItemToObject(system, "firmware", firmware);
    }
}

static cJSON *build_state_snapshot(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON *device = cJSON_CreateObject();
    if (device) {
        cJSON_AddStringToObject(device, "uuid", device_id);
        cJSON *network = network_state_snapshot();
        if (network) {
            cJSON_AddItemToObject(device, "network", network);
        }
        cJSON_AddItemToObject(root, "device", device);
    }

    cJSON *server = cJSON_CreateObject();
    if (server) {
        char server_url[160] = {0};
        char server_host[32] = {0};
        char server_port[8] = {0};
        load_server_url_from_flash(server_url, sizeof(server_url));
        load_server_info_from_flash(server_host, server_port);
        cJSON_AddStringToObject(server, "url", server_url);
        cJSON_AddStringToObject(server, "host", server_host);
        cJSON_AddStringToObject(server, "port", server_port);
        cJSON_AddBoolToObject(server, "requireReachable", load_server_require_reachable());
        cJSON_AddItemToObject(root, "server", server);
    }

    cJSON *system = cJSON_CreateObject();
    if (system) {
        cJSON_AddNumberToObject(system, "uptimeSeconds", (double)(esp_timer_get_time() / 1000000ULL));
        cJSON_AddNumberToObject(system, "freeHeap", esp_get_free_heap_size());
        cJSON_AddNumberToObject(system, "minFreeHeap", esp_get_minimum_free_heap_size());
        cJSON_AddNumberToObject(system, "largestFreeBlock", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        cJSON_AddStringToObject(system, "resetReason", api_reset_reason_name(esp_reset_reason()));
        add_firmware_info(system);
        cJSON_AddItemToObject(root, "system", system);
    }

    cJSON *locks = lock_state_snapshot();
    if (!locks) {
        locks = cJSON_CreateArray();
    }
    cJSON_AddItemToObject(root, "locks", locks);

    cJSON *exits = exit_state_snapshot();
    if (!exits) {
        exits = cJSON_CreateArray();
    }
    cJSON_AddItemToObject(root, "exits", exits);

    cJSON *fobs = fob_state_snapshot();
    if (!fobs) {
        fobs = cJSON_CreateArray();
    }
    cJSON_AddItemToObject(root, "fobs", fobs);

    cJSON *keypads = keypad_state_snapshot();
    if (!keypads) {
        keypads = cJSON_CreateArray();
    }
    cJSON_AddItemToObject(root, "keypads", keypads);

    cJSON *motions = motion_state_snapshot();
    if (!motions) {
        motions = cJSON_CreateArray();
    }
    cJSON_AddItemToObject(root, "motions", motions);

    cJSON *wiegand = wiegand_state_summary_snapshot();
    if (!wiegand) {
        wiegand = cJSON_CreateObject();
    }
    cJSON_AddItemToObject(root, "wiegand", wiegand);

    cJSON *rf = rf_state_summary_snapshot();
    if (!rf) {
        rf = cJSON_CreateObject();
    }
    if (rf) {
        cJSON *receiver = rf_receiver_diagnostics_summary_snapshot();
        if (receiver) {
            cJSON_AddItemToObject(rf, "receiver", receiver);
        }
    }
    cJSON_AddItemToObject(root, "rf", rf);

    cJSON *enrollment = enrollment_state_snapshot();
    if (!enrollment) {
        enrollment = cJSON_CreateObject();
    }
    cJSON_AddItemToObject(root, "enrollment", enrollment);

    /* Wi-Fi state */
    cJSON *wifi = cJSON_CreateObject();
    if (wifi) {
        char ssid[32] = {0};
        char pwd[64] = {0};
        load_wifi_credentials_from_flash(ssid, pwd);
        cJSON_AddStringToObject(wifi, "active_ssid", ssid);
        cJSON_AddItemToObject(root, "wifi", wifi);
    }

    return root;
}

static esp_err_t send_json_response(httpd_req_t *req, cJSON *json) {
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to build JSON response");
    }

    if (!s_response_mutex) {
        s_response_mutex = xSemaphoreCreateMutex();
    }

    if (!s_response_mutex || xSemaphoreTake(s_response_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        cJSON_Delete(json);
        ESP_LOGW(API_TAG, "Timed out waiting for JSON response buffer");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Response buffer busy");
    }

    if (!cJSON_PrintPreallocated(json, s_state_response_buffer, STATE_RESPONSE_BUFFER_SIZE, false)) {
        xSemaphoreGive(s_response_mutex);
        cJSON_Delete(json);
        ESP_LOGE(API_TAG, "JSON response exceeds static buffer (%d bytes)", STATE_RESPONSE_BUFFER_SIZE);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Response too large");
    }

    size_t resp_len = strlen(s_state_response_buffer);
    ESP_LOGD(API_TAG, "json response size=%zu heap=%lu min=%lu largest=%lu",
             resp_len,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t result = httpd_resp_send(req, s_state_response_buffer, resp_len);
    cJSON_Delete(json);
    xSemaphoreGive(s_response_mutex);
    return result;
}

static esp_err_t send_full_state_response(httpd_req_t *req) {
    bool mutex_held = false;
    if (!s_state_snapshot_mutex) {
        s_state_snapshot_mutex = xSemaphoreCreateMutex();
    }

    if (s_state_snapshot_mutex) {
        if (xSemaphoreTake(s_state_snapshot_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
            ESP_LOGW(API_TAG, "Timed out waiting to build full state snapshot");
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "State snapshot busy");
        }
        mutex_held = true;
    }

    cJSON *state = build_state_snapshot();
    esp_err_t result = state
        ? send_json_response(req, state)
        : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to build state");

    if (mutex_held) {
        xSemaphoreGive(s_state_snapshot_mutex);
    }
    return result;
}

static esp_err_t read_json_body(httpd_req_t *req, cJSON **out_payload) {
    if (!out_payload) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t max_len = 4096;
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > (int)max_len) {
        ESP_LOGE(API_TAG, "Invalid JSON body length: %d", total_len);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[total_len] = '\0';

    cJSON *payload = cJSON_Parse(buf);
    free(buf);
    if (!payload) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_payload = payload;
    return ESP_OK;
}

static esp_err_t api_state_get_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return send_full_state_response(req);
}

static esp_err_t api_signals_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to build signal response");
    }

    cJSON *locks = lock_state_snapshot();
    cJSON_AddItemToObject(root, "locks", locks ? locks : cJSON_CreateArray());

    cJSON *exits = exit_state_snapshot();
    cJSON_AddItemToObject(root, "exits", exits ? exits : cJSON_CreateArray());

    cJSON *fobs = fob_state_snapshot();
    cJSON_AddItemToObject(root, "fobs", fobs ? fobs : cJSON_CreateArray());

    cJSON *keypads = keypad_state_snapshot();
    cJSON_AddItemToObject(root, "keypads", keypads ? keypads : cJSON_CreateArray());

    cJSON *motions = motion_state_snapshot();
    cJSON_AddItemToObject(root, "motions", motions ? motions : cJSON_CreateArray());

    cJSON *wiegand = wiegand_state_summary_snapshot();
    cJSON_AddItemToObject(root, "wiegand", wiegand ? wiegand : cJSON_CreateObject());

    cJSON *rf = rf_state_summary_snapshot();
    if (rf) {
        cJSON *receiver = rf_receiver_diagnostics_summary_snapshot();
        if (receiver) {
            cJSON_AddItemToObject(rf, "receiver", receiver);
        }
    }
    cJSON_AddItemToObject(root, "rf", rf ? rf : cJSON_CreateObject());

    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return send_json_response(req, root);
}

static esp_err_t api_discovery_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to build discovery response");
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "service", "access-controller");
    cJSON_AddStringToObject(root, "deviceKind", "access_controller");
    cJSON_AddStringToObject(root, "name", "Access Controller");

    cJSON *device = cJSON_CreateObject();
    if (device) {
        cJSON_AddStringToObject(device, "uuid", device_id);
        cJSON *network = network_state_snapshot();
        if (network) {
            cJSON_AddItemToObject(device, "network", network);
        }
        cJSON_AddItemToObject(root, "device", device);
    }

    cJSON *capabilities = cJSON_CreateArray();
    if (capabilities) {
        const char *items[] = {
            "access-control",
            "esp32",
            "ota-upload",
            "rollback",
            "wifi-config",
            "wiegand",
            "rf",
            "keypad",
            "motion",
            "logs",
            "websocket",
            "web-ui",
        };
        for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
            cJSON_AddItemToArray(capabilities, cJSON_CreateString(items[i]));
        }
        cJSON_AddItemToObject(root, "capabilities", capabilities);
    }

    cJSON *api = cJSON_CreateObject();
    if (api) {
        cJSON_AddStringToObject(api, "state", "/api/state");
        cJSON_AddStringToObject(api, "signals", "/api/signals");
        cJSON_AddStringToObject(api, "otaUpload", "/api/ota/upload");
        cJSON_AddStringToObject(api, "logs", "/api/logs");
        cJSON_AddStringToObject(api, "wifiScan", "/api/wifi/scan");
        cJSON_AddStringToObject(api, "wifiList", "/api/wifi/list");
        cJSON_AddStringToObject(api, "websocket", "/ws");
        cJSON_AddItemToObject(root, "api", api);
    }

    cJSON *system = cJSON_CreateObject();
    if (system) {
        cJSON_AddNumberToObject(system, "uptimeSeconds", (double)(esp_timer_get_time() / 1000000ULL));
        add_firmware_info(system);
        cJSON_AddItemToObject(root, "system", system);
    }

    return send_json_response(req, root);
}

static esp_err_t api_keypad_users_get_handler(httpd_req_t *req) {
    return send_json_response(req, keypad_users_snapshot());
}

static esp_err_t api_logs_get_handler(httpd_req_t *req) {
    return send_json_response(req, system_logs_snapshot());
}

static void ota_reboot_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();
}

static esp_err_t send_plain_error(httpd_req_t *req, const char *status, const char *message) {
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, message ? message : "Request failed");
}

static esp_err_t api_ota_upload_post_handler(httpd_req_t *req) {
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware binary is required");
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA update partition available");
    }
    if ((size_t)req->content_len > update_partition->size) {
        return send_plain_error(req, "413 Payload Too Large", "Firmware binary exceeds OTA partition size");
    }

    if (!s_ota_mutex) {
        s_ota_mutex = xSemaphoreCreateMutex();
    }
    if (!s_ota_mutex || xSemaphoreTake(s_ota_mutex, 0) != pdTRUE) {
        return send_plain_error(req, "409 Conflict", "OTA update already in progress");
    }

    uint8_t *buffer = malloc(OTA_UPLOAD_BUFFER_SIZE);
    if (!buffer) {
        xSemaphoreGive(s_ota_mutex);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to allocate OTA buffer");
    }

    esp_ota_handle_t update_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, req->content_len, &update_handle);
    if (err != ESP_OK) {
        free(buffer);
        xSemaphoreGive(s_ota_mutex);
        ESP_LOGE(API_TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
            return send_plain_error(req, "409 Conflict", "Current firmware is still pending rollback validation");
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start OTA update");
    }

    int remaining = req->content_len;
    size_t written = 0;
    while (remaining > 0) {
        int to_read = MIN(remaining, OTA_UPLOAD_BUFFER_SIZE);
        int received = httpd_req_recv(req, (char *)buffer, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            ESP_LOGE(API_TAG, "OTA receive failed after %zu bytes", written);
            esp_ota_abort(update_handle);
            free(buffer);
            xSemaphoreGive(s_ota_mutex);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive firmware binary");
        }

        err = esp_ota_write(update_handle, buffer, received);
        if (err != ESP_OK) {
            ESP_LOGE(API_TAG, "esp_ota_write failed after %zu bytes (%s)", written, esp_err_to_name(err));
            esp_ota_abort(update_handle);
            free(buffer);
            xSemaphoreGive(s_ota_mutex);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware binary is not a valid ESP32 app image");
        }

        written += (size_t)received;
        remaining -= received;
    }

    free(buffer);

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(API_TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
        xSemaphoreGive(s_ota_mutex);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware image validation failed");
    }

    esp_app_desc_t uploaded_desc;
    memset(&uploaded_desc, 0, sizeof(uploaded_desc));
    esp_err_t desc_err = esp_ota_get_partition_description(update_partition, &uploaded_desc);

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(API_TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
        xSemaphoreGive(s_ota_mutex);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to select uploaded firmware");
    }

    automation_record_log("OTA firmware uploaded; rebooting into new image");

    cJSON *response = cJSON_CreateObject();
    if (!response) {
        xSemaphoreGive(s_ota_mutex);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Firmware installed but response allocation failed");
    }

    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddBoolToObject(response, "reboot", true);
    cJSON_AddNumberToObject(response, "bytes", written);
    cJSON_AddNumberToObject(response, "rebootDelayMs", OTA_REBOOT_DELAY_MS);
    cJSON_AddStringToObject(response, "partition", update_partition->label);
    if (desc_err == ESP_OK) {
        cJSON_AddStringToObject(response, "projectName", uploaded_desc.project_name);
        cJSON_AddStringToObject(response, "projectVersion", uploaded_desc.version);
        cJSON_AddStringToObject(response, "buildDate", uploaded_desc.date);
        cJSON_AddStringToObject(response, "buildTime", uploaded_desc.time);
    }

    if (xTaskCreate(ota_reboot_task, "ota_reboot", 2048, NULL, 5, NULL) != pdPASS) {
        xSemaphoreGive(s_ota_mutex);
        cJSON_Delete(response);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Firmware installed but reboot task failed");
    }

    xSemaphoreGive(s_ota_mutex);
    return send_json_response(req, response);
}

static esp_err_t send_wiegand_state_response(httpd_req_t *req) {
    return send_json_response(req, wiegand_state_snapshot());
}

static esp_err_t api_wiegand_get_handler(httpd_req_t *req) {
    ESP_LOGI(API_TAG, "Wiegand state requested");
    return send_wiegand_state_response(req);
}

static esp_err_t api_wiegand_register_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) {
        ESP_LOGW(API_TAG, "Invalid Wiegand register payload (%s)", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON payload");
    }

    int channel = 0;
    const cJSON *channel_item = cJSON_GetObjectItemCaseSensitive(payload, "channel");
    if (cJSON_IsNumber(channel_item)) {
        channel = (int)channel_item->valuedouble;
    }

    ESP_LOGI(API_TAG, "Starting Wiegand registration (channel=%d)", channel);
    err = wiegand_registration_start((uint8_t)channel);
    cJSON_Delete(payload);

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(API_TAG, "Wiegand registration already active");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Registration already active");
    }
    if (err != ESP_OK) {
        ESP_LOGE(API_TAG, "Failed to start Wiegand registration (%s)", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start registration");
    }

    return send_wiegand_state_response(req);
}

static esp_err_t api_wiegand_stop_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) {
        ESP_LOGW(API_TAG, "Invalid Wiegand stop payload (%s)", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON payload");
    }

    bool promote = false;
    const cJSON *promote_item = cJSON_GetObjectItemCaseSensitive(payload, "promote");
    if (cJSON_IsBool(promote_item)) {
        promote = cJSON_IsTrue(promote_item);
    }
    cJSON_Delete(payload);

    ESP_LOGI(API_TAG, "Stopping Wiegand registration (promote=%s)", promote ? "true" : "false");
    err = wiegand_registration_stop(promote);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(API_TAG, "Attempted to stop inactive Wiegand registration");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Registration is not active");
    }
    if (err != ESP_OK) {
        ESP_LOGE(API_TAG, "Failed to stop Wiegand registration (%s)", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to stop registration");
    }

    return send_wiegand_state_response(req);
}

static esp_err_t api_wiegand_rename_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) {
        ESP_LOGW(API_TAG, "Invalid Wiegand rename payload (%s)", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON payload");
    }

    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(payload, "id");
    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(payload, "name");
    const cJSON *channel_item = cJSON_GetObjectItemCaseSensitive(payload, "channel");
    const cJSON *channel_mask_item = cJSON_GetObjectItemCaseSensitive(payload, "channel_mask");
    const cJSON *alert_item = cJSON_GetObjectItemCaseSensitive(payload, "alert");
    const cJSON *alert_target_item = cJSON_GetObjectItemCaseSensitive(payload, "alert_target");
    const cJSON *status_item = cJSON_GetObjectItemCaseSensitive(payload, "status");
    const cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(payload, "enabled");
    const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(payload, "mode");
    const char *id_raw = (cJSON_IsString(id_item) && id_item->valuestring) ? id_item->valuestring : NULL;
    const char *name_raw = (cJSON_IsString(name_item) && name_item->valuestring) ? name_item->valuestring : NULL;

    if (!id_raw || id_raw[0] == '\0' || !name_raw || name_raw[0] == '\0') {
        cJSON_Delete(payload);
        ESP_LOGW(API_TAG, "Wiegand rename missing id or name");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Both id and name are required");
    }
    if (strlen(name_raw) >= WIEGAND_USER_NAME_MAX) {
        cJSON_Delete(payload);
        ESP_LOGW(API_TAG, "Wiegand rename name too long (%u >= %d)", (unsigned)strlen(name_raw), WIEGAND_USER_NAME_MAX);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Name exceeds maximum length");
    }

    // Copy id and name to local buffers BEFORE freeing payload to avoid use-after-free
    char id[WIEGAND_USER_ID_MAX];
    char name[WIEGAND_USER_NAME_MAX];
    strlcpy(id, id_raw, sizeof(id));
    strlcpy(name, name_raw, sizeof(name));
    const wiegand_user_t *existing = wiegand_registry_find_by_id(id);
    uint8_t channel = existing ? existing->channel : 0;
    uint8_t channel_mask = existing ? existing->channel_mask : 3;
    bool alert = existing ? existing->alert : true;
    int alert_target = existing ? existing->alert_target : alert_target_from_bool(alert);
    char mode[WIEGAND_USER_MODE_MAX];
    strlcpy(mode, (existing && existing->mode[0] != '\0') ? existing->mode : "momentary", sizeof(mode));
    wiegand_user_status_t status = existing ? existing->status : WIEGAND_USER_STATUS_ACTIVE;
    if (cJSON_IsNumber(channel_item)) {
        int requested_channel = (int)channel_item->valuedouble;
        if (requested_channel < 0 || requested_channel > 2) {
            cJSON_Delete(payload);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid channel");
        }
        channel = (uint8_t)requested_channel;
    }
    if (cJSON_IsNumber(channel_mask_item)) {
        int requested_mask = (int)channel_mask_item->valuedouble;
        if (requested_mask <= 0 || requested_mask > 3) {
            cJSON_Delete(payload);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid channel mask");
        }
        channel_mask = (uint8_t)requested_mask;
    }
    if (cJSON_IsBool(alert_item)) {
        alert = cJSON_IsTrue(alert_item);
    }
    if (cJSON_IsString(alert_target_item) && alert_target_item->valuestring) {
        alert_target = alert_target_from_string(alert_target_item->valuestring, alert);
    } else if (cJSON_IsNumber(alert_target_item)) {
        alert_target = alert_target_normalize(alert_target_item->valueint, alert);
    } else {
        alert_target = alert_target_normalize(alert_target, alert);
    }
    if (cJSON_IsString(mode_item) && mode_item->valuestring) {
        if (strcmp(mode_item->valuestring, "momentary") != 0 &&
            strcmp(mode_item->valuestring, "toggle") != 0 &&
            strcmp(mode_item->valuestring, "latch") != 0) {
            cJSON_Delete(payload);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mode");
        }
        strlcpy(mode, mode_item->valuestring, sizeof(mode));
    }
    if (cJSON_IsBool(enabled_item)) {
        status = cJSON_IsTrue(enabled_item) ? WIEGAND_USER_STATUS_ACTIVE : WIEGAND_USER_STATUS_DISABLED;
    }
    if (cJSON_IsNumber(status_item)) {
        int requested_status = (int)status_item->valuedouble;
        if (requested_status < WIEGAND_USER_STATUS_PENDING || requested_status > WIEGAND_USER_STATUS_DISABLED) {
            cJSON_Delete(payload);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid status");
        }
        status = (wiegand_user_status_t)requested_status;
    }
    cJSON_Delete(payload);

    ESP_LOGI(API_TAG,
             "Updating Wiegand user %s name='%s' mode=%s reader=%u channel_mask=%u alert_target=%s",
             id,
             name,
             mode,
             (unsigned)channel,
             (unsigned)channel_mask,
             alert_target_to_string(alert_target));
    err = wiegand_registry_update_config(id, name, mode, channel, channel_mask, alert, alert_target);
    if (err == ESP_OK && existing && status != existing->status) {
        err = wiegand_registry_update_status(id, status);
    }

    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(API_TAG, "Wiegand user not found: %s", id);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "User not found");
    }
    if (err != ESP_OK) {
        ESP_LOGE(API_TAG, "Failed to rename Wiegand user %s (%s)", id, esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to rename user");
    }

    // UX: if the user was just enrolled via registration, they start as PENDING.
    // Promote to ACTIVE when the operator saves a name so the tag works immediately.
    esp_err_t promote_err = wiegand_registry_update_status(id, WIEGAND_USER_STATUS_ACTIVE);
    if (promote_err == ESP_OK) {
        const wiegand_user_t *user = wiegand_registry_find_by_id(id);
        ESP_LOGI(API_TAG,
                 "Wiegand user %s promoted to ACTIVE (status=%d)",
                 id,
                 user ? (int)user->status : -1);
    } else {
        ESP_LOGW(API_TAG, "Wiegand user %s promote to ACTIVE failed (%s)", id, esp_err_to_name(promote_err));
    }

    return send_wiegand_state_response(req);
}

static esp_err_t handle_json_post(httpd_req_t *req, void (*handler)(cJSON *), cJSON *(*state_builder)(void)) {
    const size_t max_len = 2048;
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > (int)max_len) {
        ESP_LOGE(API_TAG, "Invalid content length: %d", total_len);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
    }

    char *buf = malloc(total_len + 1);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request body");
        }
        received += ret;
    }
    buf[total_len] = '\0';

    cJSON *payload = cJSON_Parse(buf);
    free(buf);

    if (!payload) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    handler(cJSON_Duplicate(payload, 1));
    cJSON_Delete(payload);

    cJSON *state = state_builder ? state_builder() : cJSON_CreateObject();
    if (!state) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to build state");
    }
    return send_json_response(req, state);
}

static void lock_message_wrapper(cJSON *payload) {
    if (payload) {
        handle_lock_message(payload);
    }
}

static void exit_message_wrapper(cJSON *payload) {
    if (payload) {
        handle_exit_message(payload);
    }
}

static void fob_message_wrapper(cJSON *payload) {
    if (payload) {
        handle_fob_message(payload);
    }
}

static void keypad_message_wrapper(cJSON *payload) {
    if (payload) {
        handle_keypad_message(payload);
    }
}

static void motion_message_wrapper(cJSON *payload) {
    if (payload) {
        handle_motion_message(payload);
    }
}

static esp_err_t api_lock_post_handler(httpd_req_t *req) {
    return handle_json_post(req, lock_message_wrapper, lock_state_snapshot);
}

static esp_err_t api_exit_post_handler(httpd_req_t *req) {
    return handle_json_post(req, exit_message_wrapper, exit_state_snapshot);
}

static esp_err_t api_fob_post_handler(httpd_req_t *req) {
    return handle_json_post(req, fob_message_wrapper, fob_state_snapshot);
}

static esp_err_t api_keypad_post_handler(httpd_req_t *req) {
    return handle_json_post(req, keypad_message_wrapper, keypad_state_snapshot);
}

static esp_err_t api_motion_post_handler(httpd_req_t *req) {
    return handle_json_post(req, motion_message_wrapper, motion_state_snapshot);
}

static esp_err_t api_buzzer_quiet_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(payload, "enabled");
    bool enabled = enabled_item ? cJSON_IsTrue(enabled_item) : true;
    cJSON_Delete(payload);

    buzzer_set_quiet_test_mode(enabled);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "quietTestMode", buzzer_get_quiet_test_mode());
    return send_json_response(req, root);
}

static esp_err_t api_buzzer_error_beep_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const cJSON *beeps_item = cJSON_GetObjectItemCaseSensitive(payload, "beeps");
    const cJSON *channel_item = cJSON_GetObjectItemCaseSensitive(payload, "channel");
    int beeps = cJSON_IsNumber(beeps_item) ? beeps_item->valueint : 2;
    int channel = cJSON_IsNumber(channel_item) ? channel_item->valueint : 1;
    if (beeps < 1) beeps = 1;
    if (beeps > 5) beeps = 5;
    if (channel < 0 || channel > 2) channel = 1;
    cJSON_Delete(payload);

    beep_keypad_force(beeps, channel);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json_response(req, root);
}

static esp_err_t api_keypad_push_test_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const cJSON *channel_item = cJSON_GetObjectItemCaseSensitive(payload, "channel");
    const cJSON *pulses_item = cJSON_GetObjectItemCaseSensitive(payload, "pulses");
    const cJSON *active_item = cJSON_GetObjectItemCaseSensitive(payload, "activeMs");
    const cJSON *idle_item = cJSON_GetObjectItemCaseSensitive(payload, "idleMs");
    const cJSON *active_high_item = cJSON_GetObjectItemCaseSensitive(payload, "activeHigh");

    int channel = cJSON_IsNumber(channel_item) ? channel_item->valueint : 1;
    int pulses = cJSON_IsNumber(pulses_item) ? pulses_item->valueint : 1;
    int active_ms = cJSON_IsNumber(active_item) ? active_item->valueint : 300;
    int idle_ms = cJSON_IsNumber(idle_item) ? idle_item->valueint : 300;
    bool active_high = active_high_item ? cJSON_IsTrue(active_high_item) : true;
    if (channel < 0 || channel > 2) channel = 1;
    cJSON_Delete(payload);

    keypad_push_test(channel, pulses, active_ms, idle_ms, active_high);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "channel", channel);
    cJSON_AddNumberToObject(root, "pulses", pulses);
    cJSON_AddNumberToObject(root, "activeMs", active_ms);
    cJSON_AddNumberToObject(root, "idleMs", idle_ms);
    cJSON_AddBoolToObject(root, "activeHigh", active_high);
    return send_json_response(req, root);
}

static esp_err_t api_wifi_post_handler(httpd_req_t *req) {
    return handle_json_post(req, handle_authorize_message, build_state_snapshot);
}

static esp_err_t api_server_post_handler(httpd_req_t *req) {
    return handle_json_post(req, handle_authorize_message, build_state_snapshot);
}

static esp_err_t api_wifi_get_handler(httpd_req_t *req) {
    cJSON *wifi = cJSON_CreateObject();
    if (!wifi) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to build Wi-Fi state");
    }

    char ssid[32] = {0};
    char pwd[64] = {0};
    load_wifi_credentials_from_flash(ssid, pwd);
    cJSON_AddStringToObject(wifi, "active_ssid", ssid);

    cJSON *network = network_state_snapshot();
    if (network) {
        cJSON_AddItemToObject(wifi, "network", network);
    }

    cJSON *list = wifi_list_snapshot();
    if (list) {
        cJSON_AddItemToObject(wifi, "networks", list);
    }

    return send_json_response(req, wifi);
}

static esp_err_t api_wifi_list_get_handler(httpd_req_t *req) {
    return send_json_response(req, wifi_list_snapshot());
}

static esp_err_t send_wifi_update_response(httpd_req_t *req, bool reboot) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to build Wi-Fi response");
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "reboot", reboot);

    char ssid[32] = {0};
    char pwd[64] = {0};
    load_wifi_credentials_from_flash(ssid, pwd);
    cJSON_AddStringToObject(root, "active_ssid", ssid);

    cJSON *list = wifi_list_snapshot();
    if (list) {
        cJSON_AddItemToObject(root, "networks", list);
    }

    cJSON *network = network_state_snapshot();
    if (network) {
        cJSON_AddItemToObject(root, "network", network);
    }

    return send_json_response(req, root);
}

static esp_err_t api_wifi_scan_get_handler(httpd_req_t *req) {
    return send_json_response(req, wifi_scan_snapshot());
}

static esp_err_t api_wifi_add_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(payload, "ssid");
    const cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(payload, "password");
    const char *ssid = cJSON_IsString(ssid_item) ? ssid_item->valuestring : NULL;
    const char *pwd = cJSON_IsString(pass_item) ? pass_item->valuestring : "";
    if (!ssid || ssid[0] == '\0') {
        cJSON_Delete(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    }
    err = wifi_list_add(ssid, pwd);
    cJSON_Delete(payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save WiFi");
    }
    esp_err_t send_err = send_wifi_update_response(req, true);
    schedule_restart(WIFI_REBOOT_DELAY_MS);
    if (send_err != ESP_OK) return send_err;
    return ESP_OK;
}

static esp_err_t api_wifi_delete_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(payload, "ssid");
    const char *ssid = cJSON_IsString(ssid_item) ? ssid_item->valuestring : NULL;
    if (!ssid || ssid[0] == '\0') {
        cJSON_Delete(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    }
    char active_ssid[32] = {0};
    char active_pwd[64] = {0};
    load_wifi_credentials_from_flash(active_ssid, active_pwd);
    bool deleted_active = (strcmp(active_ssid, ssid) == 0);

    err = wifi_list_delete(ssid);
    cJSON_Delete(payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete WiFi");
    }
    esp_err_t send_err = send_wifi_update_response(req, deleted_active);
    if (deleted_active) {
        schedule_restart(WIFI_REBOOT_DELAY_MS);
    }
    return send_err;
}

static esp_err_t api_wifi_connect_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(payload, "ssid");
    const char *ssid = cJSON_IsString(ssid_item) ? ssid_item->valuestring : NULL;
    if (!ssid || ssid[0] == '\0') {
        cJSON_Delete(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    }
    err = wifi_list_set_active(ssid);
    cJSON_Delete(payload);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "ssid not found");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to activate WiFi");
    }
    esp_err_t send_err = send_wifi_update_response(req, true);
    schedule_restart(WIFI_REBOOT_DELAY_MS);
    if (send_err != ESP_OK) return send_err;
    return ESP_OK;
}


static esp_err_t api_favicon_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/x-icon");
    return httpd_resp_send(req, NULL, 0);
}

// Build JSON array of keypad PIN users
static bool keypad_user_has_pins(cJSON *user) {
    if (!user) {
        return false;
    }

    const cJSON *pin = cJSON_GetObjectItemCaseSensitive(user, "pin");
    if (cJSON_IsString(pin) && pin->valuestring && pin->valuestring[0] != '\0') {
        return true;
    }

    const cJSON *pins = cJSON_GetObjectItemCaseSensitive(user, "pins");
    if (!cJSON_IsArray(pins)) {
        return false;
    }

    int count = cJSON_GetArraySize(pins);
    for (int i = 0; i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem(pins, i);
        if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
            return true;
        }
    }
    return false;
}

static bool keypad_user_is_empty_default(cJSON *user) {
    if (!user || keypad_user_has_pins(user)) {
        return false;
    }

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(user, "name");
    return cJSON_IsString(name) && name->valuestring && strcmp(name->valuestring, "Default User") == 0;
}

static bool pin_string_is_valid(const char *pin) {
    if (!pin) {
        return false;
    }
    size_t len = strlen(pin);
    if (len < 4 || len > 8) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            return false;
        }
    }
    return true;
}

static bool pin_mode_is_valid(const char *mode) {
    return mode &&
        (strcmp(mode, "toggle") == 0 ||
         strcmp(mode, "momentary") == 0 ||
         strcmp(mode, "latch") == 0 ||
         strcmp(mode, "exit") == 0 ||
         strcmp(mode, "power_on") == 0 ||
         strcmp(mode, "power_off") == 0);
}

static void prune_empty_default_keypad_users(void) {
    bool pruned = false;

    do {
        pruned = false;
        uint32_t user_count = get_user_count_from_flash();
        for (uint32_t i = 0; i < user_count; i++) {
            cJSON *user = load_user_from_flash(i + 1);
            if (!user) {
                continue;
            }

            const cJSON *uuid_item = cJSON_GetObjectItemCaseSensitive(user, "uuid");
            char uuid[64] = {0};
            if (keypad_user_is_empty_default(user) &&
                cJSON_IsString(uuid_item) &&
                uuid_item->valuestring &&
                uuid_item->valuestring[0] != '\0') {
                strlcpy(uuid, uuid_item->valuestring, sizeof(uuid));
            }
            cJSON_Delete(user);

            if (uuid[0] == '\0') {
                continue;
            }

            esp_err_t err = delete_user_from_flash(uuid);
            if (err == ESP_OK) {
                ESP_LOGI(API_TAG, "Pruned empty Default User keypad record: %s", uuid);
                pruned = true;
                break;
            }
            ESP_LOGW(API_TAG, "Failed to prune empty Default User keypad record %s (%s)", uuid, esp_err_to_name(err));
        }
    } while (pruned);
}

static cJSON *keypad_users_snapshot(void) {
    prune_empty_default_keypad_users();

    cJSON *array = cJSON_CreateArray();
    if (!array) return NULL;

    uint64_t now_ms = esp_timer_get_time() / 1000ULL;
    uint32_t user_count = get_user_count_from_flash();
    for (uint32_t i = 0; i < user_count; i++) {
        cJSON *user = load_user_from_flash(i + 1);
        if (user) {
            add_default_pin_user_config(user);
            const cJSON *last_used_ms = cJSON_GetObjectItemCaseSensitive(user, "last_used_ms");
            if (cJSON_IsNumber(last_used_ms) && last_used_ms->valuedouble > 0) {
                uint64_t used_ms = (uint64_t)last_used_ms->valuedouble;
                uint64_t age_ms = now_ms >= used_ms ? now_ms - used_ms : 0;
                const cJSON *last_used_unix_time = cJSON_GetObjectItemCaseSensitive(user, "last_used_unix_time");
                const cJSON *last_used_pin = cJSON_GetObjectItemCaseSensitive(user, "last_used_pin");
                cJSON *last_used = cJSON_CreateObject();
                if (last_used) {
                    cJSON_AddNumberToObject(last_used, "used_ms", (double)used_ms);
                    cJSON_AddNumberToObject(last_used, "unixTime",
                                            cJSON_IsNumber(last_used_unix_time) ? last_used_unix_time->valuedouble : 0);
                    cJSON_AddNumberToObject(last_used, "age_ms", (double)age_ms);
                    if (cJSON_IsString(last_used_pin) && last_used_pin->valuestring) {
                        cJSON_AddStringToObject(last_used, "pin", last_used_pin->valuestring);
                    }
                    cJSON_DeleteItemFromObject(user, "lastUsed");
                    cJSON_AddItemToObject(user, "lastUsed", last_used);
                }
            }
            cJSON_AddItemToArray(array, user);
        }
    }
    return array;
}

// Generate a random UUID
static void generate_uuid(char *buf, size_t len) {
    const char *hex = "0123456789abcdef";
    for (size_t i = 0; i + 1 < len; i++) {
        buf[i] = hex[esp_random() % 16];
    }
    buf[len - 1] = '\0';
}

// POST /api/keypad/user - Add new PIN user
static esp_err_t api_keypad_user_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(payload, "name");
    const cJSON *pin_item = cJSON_GetObjectItemCaseSensitive(payload, "pin");

    const char *name = cJSON_IsString(name_item) ? name_item->valuestring : NULL;
    const char *pin = cJSON_IsString(pin_item) ? pin_item->valuestring : NULL;

    if (!name || name[0] == '\0') {
        cJSON_Delete(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Name required");
    }

    if (pin && pin[0] != '\0' && !pin_string_is_valid(pin)) {
        cJSON_Delete(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "PIN must be 4-8 digits");
    }

    char uuid[33];
    generate_uuid(uuid, sizeof(uuid));

    ESP_LOGI(API_TAG, "Adding keypad user: name=%s, pin=****", name);
    err = store_user_to_flash(uuid, (char *)name, (char *)(pin ? pin : ""));
    cJSON_Delete(payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save PIN user");
    }

    return send_json_response(req, keypad_users_snapshot());
}

// DELETE /api/keypad/user - Delete PIN user by UUID
static esp_err_t api_keypad_user_delete_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const cJSON *uuid_item = cJSON_GetObjectItemCaseSensitive(payload, "uuid");
    const cJSON *pin_index_item = cJSON_GetObjectItemCaseSensitive(payload, "pinIndex");
    const char *uuid = cJSON_IsString(uuid_item) ? uuid_item->valuestring : NULL;
    int pin_index = cJSON_IsNumber(pin_index_item) ? (int)pin_index_item->valuedouble : -1;

    if (!uuid || uuid[0] == '\0') {
        cJSON_Delete(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "UUID required");
    }

    ESP_LOGI(API_TAG, "Deleting keypad user: uuid=%s pinIndex=%d", uuid, pin_index);
    err = pin_index >= 0 ? delete_user_pin_from_flash(uuid, pin_index) : delete_user_from_flash(uuid);
    cJSON_Delete(payload);

    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "User not found");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete user");
    }

    cJSON *users = keypad_users_snapshot();
    return send_json_response(req, users);
}

// POST /api/keypad/users/delete-all - Remove all PIN users
static esp_err_t api_keypad_users_delete_all_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    cJSON_Delete(payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    ESP_LOGI(API_TAG, "Deleting all keypad users");
    err = delete_all_users_from_flash();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete users");
    }

    cJSON *users = keypad_users_snapshot();
    return send_json_response(req, users);
}

// PUT /api/keypad/user - Update PIN user config
static esp_err_t api_keypad_user_put_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const cJSON *uuid_item = cJSON_GetObjectItemCaseSensitive(payload, "uuid");
    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(payload, "name");
    const cJSON *pin_item = cJSON_GetObjectItemCaseSensitive(payload, "pin");
    const cJSON *pin_index_item = cJSON_GetObjectItemCaseSensitive(payload, "pinIndex");
    const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(payload, "mode");
    const cJSON *channel_item = cJSON_GetObjectItemCaseSensitive(payload, "channel_mask");
    const cJSON *keypad_item = cJSON_GetObjectItemCaseSensitive(payload, "keypad_mask");
    const cJSON *exit_item = cJSON_GetObjectItemCaseSensitive(payload, "exit_seconds");
    const cJSON *alert_item = cJSON_GetObjectItemCaseSensitive(payload, "alert");
    const cJSON *alert_target_item = cJSON_GetObjectItemCaseSensitive(payload, "alert_target");
    const cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(payload, "enabled");

    const char *uuid = cJSON_IsString(uuid_item) ? uuid_item->valuestring : NULL;
    const char *name = cJSON_IsString(name_item) ? name_item->valuestring : NULL;
    const char *pin = cJSON_IsString(pin_item) ? pin_item->valuestring : NULL;
    int pin_index = cJSON_IsNumber(pin_index_item) ? (int)pin_index_item->valuedouble : -1;
    const char *mode = cJSON_IsString(mode_item) ? mode_item->valuestring : "momentary";
    int channel_mask = cJSON_IsNumber(channel_item) ? (int)channel_item->valuedouble : 1;
    int keypad_mask = cJSON_IsNumber(keypad_item) ? (int)keypad_item->valuedouble : 3;
    int exit_seconds = cJSON_IsNumber(exit_item) ? (int)exit_item->valuedouble : 4;
    bool alert = alert_item ? cJSON_IsTrue(alert_item) : true;
    int alert_target = alert_target_from_bool(alert);
    if (cJSON_IsString(alert_target_item) && alert_target_item->valuestring) {
        alert_target = alert_target_from_string(alert_target_item->valuestring, alert);
    } else if (cJSON_IsNumber(alert_target_item)) {
        alert_target = alert_target_normalize(alert_target_item->valueint, alert);
    }
    bool enabled = enabled_item ? cJSON_IsTrue(enabled_item) : true;

    if (!uuid || !name) {
        cJSON_Delete(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "UUID and name required");
    }
    if (pin && pin[0] != '\0' && !pin_string_is_valid(pin)) {
        cJSON_Delete(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "PIN must be 4-8 digits");
    }
    if (!pin_mode_is_valid(mode) || channel_mask <= 0 || channel_mask > 3 || keypad_mask <= 0 || keypad_mask > 3) {
        cJSON_Delete(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mode/channel");
    }
    if (exit_seconds <= 0) {
        exit_seconds = 4;
    }

    ESP_LOGI(API_TAG, "Updating keypad user: uuid=%s, name=%s, mode=%s, channel_mask=%d keypad_mask=%d alert_target=%s",
             uuid, name, mode, channel_mask, keypad_mask, alert_target_to_string(alert_target));
    err = update_pin_user_in_flash(uuid, name, pin, pin_index, mode, channel_mask, keypad_mask, exit_seconds, alert, alert_target, enabled);
    cJSON_Delete(payload);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "User not found");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid PIN user config");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to update user");
    }

    cJSON *users = keypad_users_snapshot();
    return send_json_response(req, users);
}

// DELETE /api/wiegand/delete - Remove Wiegand user
static esp_err_t api_wiegand_delete_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(payload, "id");
    const char *id = cJSON_IsString(id_item) ? id_item->valuestring : NULL;

    if (!id || id[0] == '\0') {
        cJSON_Delete(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "User ID required");
    }

    ESP_LOGI(API_TAG, "Deleting Wiegand user: id=%s", id);
    err = wiegand_registry_remove(id);
    cJSON_Delete(payload);

    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "User not found");
    }

    return send_wiegand_state_response(req);
}

// POST /api/wiegand/delete-all - Remove all Wiegand users
static esp_err_t api_wiegand_delete_all_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    cJSON_Delete(payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    ESP_LOGI(API_TAG, "Deleting all Wiegand users");
    err = wiegand_registry_clear();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete Wiegand users");
    }
    return send_wiegand_state_response(req);
}

/* RF remote fobs */
static esp_err_t send_rf_state_response(httpd_req_t *req) {
    cJSON *rf = rf_state_snapshot();
    if (!rf) {
        rf = cJSON_CreateObject();
    }
    if (rf) {
        cJSON *receiver = rf_receiver_diagnostics_snapshot();
        if (receiver) {
            cJSON_AddItemToObject(rf, "receiver", receiver);
        }
    }
    return send_json_response(req, rf);
}

static esp_err_t api_rf_get_handler(httpd_req_t *req) {
    ESP_LOGI(API_TAG, "RF state requested");
    return send_rf_state_response(req);
}

static esp_err_t api_rf_line_test_get_handler(httpd_req_t *req) {
    ESP_LOGI(API_TAG, "RF line test requested");
    return send_json_response(req, rf_receiver_line_test_snapshot());
}

static esp_err_t api_rf_delete_all_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    cJSON_Delete(payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    ESP_LOGI(API_TAG, "Deleting all RF codes");
    err = rf_registry_clear();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete RF codes");
    }
    return send_rf_state_response(req);
}

static esp_err_t api_rf_register_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    cJSON_Delete(payload);

    err = rf_registration_start();
    if (err == ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Registration already active");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start registration");
    }
    return send_rf_state_response(req);
}

static esp_err_t api_rf_stop_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    cJSON_Delete(payload);

    err = rf_registration_stop();
    if (err == ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Registration is not active");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to stop registration");
    }
    return send_rf_state_response(req);
}

static esp_err_t api_rf_rename_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(payload, "id");
    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(payload, "name");
    const char *id = cJSON_IsString(id_item) ? id_item->valuestring : NULL;
    const char *name = cJSON_IsString(name_item) ? name_item->valuestring : NULL;

    if (!id || !name || name[0] == '\0') {
        cJSON_Delete(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id and name required");
    }

    err = rf_registry_update_name(id, name);
    cJSON_Delete(payload);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "RF code not found");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to update name");
    }
    return send_rf_state_response(req);
}

static esp_err_t api_rf_delete_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(payload, "id");
    const char *id = cJSON_IsString(id_item) ? id_item->valuestring : NULL;
    if (!id || id[0] == '\0') {
        cJSON_Delete(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id required");
    }

    err = rf_registry_remove(id);
    cJSON_Delete(payload);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "RF code not found");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete RF code");
    }
    return send_rf_state_response(req);
}

static esp_err_t api_rf_config_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }
    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(payload, "id");
    const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(payload, "mode");
    const cJSON *ch_item = cJSON_GetObjectItemCaseSensitive(payload, "channel_mask");
    const cJSON *exit_item = cJSON_GetObjectItemCaseSensitive(payload, "exit_seconds");
    const cJSON *alert_item = cJSON_GetObjectItemCaseSensitive(payload, "alert");
    const cJSON *alert_target_item = cJSON_GetObjectItemCaseSensitive(payload, "alert_target");
    const cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(payload, "enabled");
    const char *id = cJSON_IsString(id_item) ? id_item->valuestring : NULL;
    const char *mode = cJSON_IsString(mode_item) ? mode_item->valuestring : NULL;
    int ch_mask = cJSON_IsNumber(ch_item) ? (int)ch_item->valuedouble : 0;
    int exit_s = cJSON_IsNumber(exit_item) ? (int)exit_item->valuedouble : 0;
    bool alert = alert_item ? cJSON_IsTrue(alert_item) : true;
    int alert_target = alert_target_from_bool(alert);
    if (cJSON_IsString(alert_target_item) && alert_target_item->valuestring) {
        alert_target = alert_target_from_string(alert_target_item->valuestring, alert);
    } else if (cJSON_IsNumber(alert_target_item)) {
        alert_target = alert_target_normalize(alert_target_item->valueint, alert);
    }
    bool enabled = enabled_item ? cJSON_IsTrue(enabled_item) : true;

    if (!id || !mode || ch_mask <= 0) {
        cJSON_Delete(payload);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id, mode, channel_mask required");
    }

    err = rf_registry_update_config(id, mode, ch_mask, exit_s, alert, alert_target, enabled);
    cJSON_Delete(payload);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "RF code not found");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mode/channel");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to update config");
    }
    return send_rf_state_response(req);
}

static esp_err_t api_enrollment_get_handler(httpd_req_t *req) {
    return send_json_response(req, enrollment_state_snapshot());
}

static cJSON *build_enrollment_update_snapshot(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON *enrollment = enrollment_state_snapshot();
    cJSON_AddItemToObject(root, "enrollment", enrollment ? enrollment : cJSON_CreateObject());

    cJSON *wiegand = wiegand_state_snapshot();
    cJSON_AddItemToObject(root, "wiegand", wiegand ? wiegand : cJSON_CreateObject());

    cJSON *rf = rf_state_summary_snapshot();
    if (rf) {
        cJSON *receiver = rf_receiver_diagnostics_summary_snapshot();
        if (receiver) {
            cJSON_AddItemToObject(rf, "receiver", receiver);
        }
    }
    cJSON_AddItemToObject(root, "rf", rf ? rf : cJSON_CreateObject());

    cJSON *keypad_users = keypad_users_snapshot();
    cJSON_AddItemToObject(root, "keypadUsers", keypad_users ? keypad_users : cJSON_CreateArray());

    return root;
}

static esp_err_t api_enrollment_start_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const cJSON *uuid_item = cJSON_GetObjectItemCaseSensitive(payload, "userUuid");
    const char *user_uuid = cJSON_IsString(uuid_item) ? uuid_item->valuestring : NULL;
    err = enrollment_start(user_uuid);
    cJSON_Delete(payload);

    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "User not found");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start enrollment");
    }
    return send_json_response(req, build_enrollment_update_snapshot());
}

static esp_err_t api_enrollment_stop_post_handler(httpd_req_t *req) {
    cJSON *payload = NULL;
    esp_err_t err = read_json_body(req, &payload);
    cJSON_Delete(payload);

    err = enrollment_stop();
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_response(req, build_enrollment_update_snapshot());
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to stop enrollment");
    }
    return send_json_response(req, build_enrollment_update_snapshot());
}

void register_api_routes(httpd_handle_t server) {
    httpd_uri_t state = {
        .uri = "/api/state",
        .method = HTTP_GET,
        .handler = api_state_get_handler,
    };
    httpd_register_uri_handler(server, &state);

    httpd_uri_t signals = {
        .uri = "/api/signals",
        .method = HTTP_GET,
        .handler = api_signals_get_handler,
    };
    httpd_register_uri_handler(server, &signals);

    httpd_uri_t discovery = {
        .uri = "/api/discovery",
        .method = HTTP_GET,
        .handler = api_discovery_get_handler,
    };
    httpd_register_uri_handler(server, &discovery);

    httpd_uri_t well_known_discovery = {
        .uri = "/.well-known/access-controller.json",
        .method = HTTP_GET,
        .handler = api_discovery_get_handler,
    };
    httpd_register_uri_handler(server, &well_known_discovery);

    httpd_uri_t enrollment_get = {
        .uri = "/api/enrollment",
        .method = HTTP_GET,
        .handler = api_enrollment_get_handler,
    };
    httpd_register_uri_handler(server, &enrollment_get);

    httpd_uri_t enrollment_start_post = {
        .uri = "/api/enrollment/start",
        .method = HTTP_POST,
        .handler = api_enrollment_start_post_handler,
    };
    httpd_register_uri_handler(server, &enrollment_start_post);

    httpd_uri_t enrollment_stop_post = {
        .uri = "/api/enrollment/stop",
        .method = HTTP_POST,
        .handler = api_enrollment_stop_post_handler,
    };
    httpd_register_uri_handler(server, &enrollment_stop_post);

    httpd_uri_t wiegand_get = {
        .uri = "/api/wiegand",
        .method = HTTP_GET,
        .handler = api_wiegand_get_handler,
    };
    httpd_register_uri_handler(server, &wiegand_get);

    httpd_uri_t wiegand_register_post = {
        .uri = "/api/wiegand/register",
        .method = HTTP_POST,
        .handler = api_wiegand_register_post_handler,
    };
    httpd_register_uri_handler(server, &wiegand_register_post);

    httpd_uri_t wiegand_stop_post = {
        .uri = "/api/wiegand/stop",
        .method = HTTP_POST,
        .handler = api_wiegand_stop_post_handler,
    };
    httpd_register_uri_handler(server, &wiegand_stop_post);

    httpd_uri_t wiegand_rename_post = {
        .uri = "/api/wiegand/rename",
        .method = HTTP_POST,
        .handler = api_wiegand_rename_post_handler,
    };
    httpd_register_uri_handler(server, &wiegand_rename_post);

    httpd_uri_t wiegand_delete_post = {
        .uri = "/api/wiegand/delete",
        .method = HTTP_POST,
        .handler = api_wiegand_delete_post_handler,
    };
    httpd_register_uri_handler(server, &wiegand_delete_post);

    httpd_uri_t wiegand_delete_all_post = {
        .uri = "/api/wiegand/delete-all",
        .method = HTTP_POST,
        .handler = api_wiegand_delete_all_post_handler,
    };
    httpd_register_uri_handler(server, &wiegand_delete_all_post);

    httpd_uri_t rf_get = {
        .uri = "/api/rf",
        .method = HTTP_GET,
        .handler = api_rf_get_handler,
    };
    httpd_register_uri_handler(server, &rf_get);

    httpd_uri_t rf_line_test_get = {
        .uri = "/api/rf/line-test",
        .method = HTTP_GET,
        .handler = api_rf_line_test_get_handler,
    };
    httpd_register_uri_handler(server, &rf_line_test_get);

    httpd_uri_t rf_register_post = {
        .uri = "/api/rf/register",
        .method = HTTP_POST,
        .handler = api_rf_register_post_handler,
    };
    httpd_register_uri_handler(server, &rf_register_post);

    httpd_uri_t rf_stop_post = {
        .uri = "/api/rf/stop",
        .method = HTTP_POST,
        .handler = api_rf_stop_post_handler,
    };
    httpd_register_uri_handler(server, &rf_stop_post);

    httpd_uri_t rf_rename_post = {
        .uri = "/api/rf/rename",
        .method = HTTP_POST,
        .handler = api_rf_rename_post_handler,
    };
    httpd_register_uri_handler(server, &rf_rename_post);

    httpd_uri_t rf_delete_post = {
        .uri = "/api/rf/delete",
        .method = HTTP_POST,
        .handler = api_rf_delete_post_handler,
    };
    httpd_register_uri_handler(server, &rf_delete_post);

    httpd_uri_t rf_delete_all_post = {
        .uri = "/api/rf/delete-all",
        .method = HTTP_POST,
        .handler = api_rf_delete_all_post_handler,
    };
    httpd_register_uri_handler(server, &rf_delete_all_post);

    httpd_uri_t rf_config_post = {
        .uri = "/api/rf/config",
        .method = HTTP_POST,
        .handler = api_rf_config_post_handler,
    };
    httpd_register_uri_handler(server, &rf_config_post);

    httpd_uri_t keypad_user_post = {
        .uri = "/api/keypad/user",
        .method = HTTP_POST,
        .handler = api_keypad_user_post_handler,
    };
    httpd_register_uri_handler(server, &keypad_user_post);

    httpd_uri_t keypad_users_get = {
        .uri = "/api/keypad/users",
        .method = HTTP_GET,
        .handler = api_keypad_users_get_handler,
    };
    httpd_register_uri_handler(server, &keypad_users_get);

    httpd_uri_t keypad_users_delete_all_post = {
        .uri = "/api/keypad/users/delete-all",
        .method = HTTP_POST,
        .handler = api_keypad_users_delete_all_post_handler,
    };
    httpd_register_uri_handler(server, &keypad_users_delete_all_post);

    httpd_uri_t logs_get = {
        .uri = "/api/logs",
        .method = HTTP_GET,
        .handler = api_logs_get_handler,
    };
    httpd_register_uri_handler(server, &logs_get);

    httpd_uri_t ota_upload_post = {
        .uri = "/api/ota/upload",
        .method = HTTP_POST,
        .handler = api_ota_upload_post_handler,
    };
    httpd_register_uri_handler(server, &ota_upload_post);

    httpd_uri_t keypad_user_delete = {
        .uri = "/api/keypad/user",
        .method = HTTP_DELETE,
        .handler = api_keypad_user_delete_handler,
    };
    httpd_register_uri_handler(server, &keypad_user_delete);

    httpd_uri_t keypad_user_put = {
        .uri = "/api/keypad/user",
        .method = HTTP_PUT,
        .handler = api_keypad_user_put_handler,
    };
    httpd_register_uri_handler(server, &keypad_user_put);

    httpd_uri_t lock_post = {
        .uri = "/api/lock",
        .method = HTTP_POST,
        .handler = api_lock_post_handler,
    };
    httpd_register_uri_handler(server, &lock_post);

    httpd_uri_t exit_post = {
        .uri = "/api/exit",
        .method = HTTP_POST,
        .handler = api_exit_post_handler,
    };
    httpd_register_uri_handler(server, &exit_post);

    httpd_uri_t fob_post = {
        .uri = "/api/fob",
        .method = HTTP_POST,
        .handler = api_fob_post_handler,
    };
    httpd_register_uri_handler(server, &fob_post);

    httpd_uri_t keypad_post = {
        .uri = "/api/keypad",
        .method = HTTP_POST,
        .handler = api_keypad_post_handler,
    };
    httpd_register_uri_handler(server, &keypad_post);

    httpd_uri_t motion_post = {
        .uri = "/api/motion",
        .method = HTTP_POST,
        .handler = api_motion_post_handler,
    };
    httpd_register_uri_handler(server, &motion_post);

    httpd_uri_t buzzer_quiet_post = {
        .uri = "/api/buzzer/quiet",
        .method = HTTP_POST,
        .handler = api_buzzer_quiet_post_handler,
    };
    httpd_register_uri_handler(server, &buzzer_quiet_post);

    httpd_uri_t buzzer_error_beep_post = {
        .uri = "/api/buzzer/error-beep",
        .method = HTTP_POST,
        .handler = api_buzzer_error_beep_post_handler,
    };
    httpd_register_uri_handler(server, &buzzer_error_beep_post);

    httpd_uri_t keypad_push_test_post = {
        .uri = "/api/keypad/push-test",
        .method = HTTP_POST,
        .handler = api_keypad_push_test_post_handler,
    };
    httpd_register_uri_handler(server, &keypad_push_test_post);

    httpd_uri_t wifi_post = {
        .uri = "/api/wifi",
        .method = HTTP_POST,
        .handler = api_wifi_post_handler,
    };
    httpd_register_uri_handler(server, &wifi_post);

    httpd_uri_t wifi_get = {
        .uri = "/api/wifi",
        .method = HTTP_GET,
        .handler = api_wifi_get_handler,
    };
    httpd_register_uri_handler(server, &wifi_get);

    httpd_uri_t wifi_list_get = {
        .uri = "/api/wifi/list",
        .method = HTTP_GET,
        .handler = api_wifi_list_get_handler,
    };
    httpd_register_uri_handler(server, &wifi_list_get);

    httpd_uri_t wifi_scan_get = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = api_wifi_scan_get_handler,
    };
    httpd_register_uri_handler(server, &wifi_scan_get);

    httpd_uri_t wifi_add = {
        .uri = "/api/wifi/add",
        .method = HTTP_POST,
        .handler = api_wifi_add_post_handler,
    };
    httpd_register_uri_handler(server, &wifi_add);

    httpd_uri_t wifi_delete = {
        .uri = "/api/wifi/delete",
        .method = HTTP_POST,
        .handler = api_wifi_delete_post_handler,
    };
    httpd_register_uri_handler(server, &wifi_delete);

    httpd_uri_t wifi_connect = {
        .uri = "/api/wifi/connect",
        .method = HTTP_POST,
        .handler = api_wifi_connect_post_handler,
    };
    httpd_register_uri_handler(server, &wifi_connect);

    httpd_uri_t server_post = {
        .uri = "/api/server",
        .method = HTTP_POST,
        .handler = api_server_post_handler,
    };
    httpd_register_uri_handler(server, &server_post);

    httpd_uri_t favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = api_favicon_handler,
    };
    httpd_register_uri_handler(server, &favicon);
}
