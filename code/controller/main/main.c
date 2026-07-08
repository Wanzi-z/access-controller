// main.c

#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_event.h"
#include "automation.h"
#include "automation.c"
#include "services/station.c"
#include "services/drivers/i2c.c"
#include "services/drivers/mcp23x17.c"
#include "services/gpio.c"
#include "services/store.c"
#include "services/wiegand_registry.c"
#include "services/enrollment.c"
#include "services/authorize.c"
#include "services/buzzer.c"
#include "services/lock.c"
#include "services/wiegand.c"
#include "services/exit.c"
#include "services/motion.c"
#include "services/keypad.c"
#include "services/fob.c"
#include "services/rf_registry.c"
#include "services/rf_receiver.c"
#include "services/server.c"
#include "services/tunnel.c"
#include "services/ap.c"
#include "services/api.c"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include <time.h>
#include "cJSON.h"
#include "esp_timer.h"

char stored_firmware_md5[33];
bool need_to_update_firmware = true;

#define OTA_STARTUP_VALIDATION_DELAY_MS 10000
#define DEVICE_PUNCH_CHECK_TASK_STACK_BYTES 8192
#define CONNECTIVITY_RECOVERY_TASK_STACK_BYTES 8192
#define CONNECTIVITY_RECOVERY_INITIAL_DELAY_MS 30000
#define CONNECTIVITY_RECOVERY_INTERVAL_MS 60000

#define DEVICE_PUNCH_CHECK_DONE_BIT BIT0

static bool running_app_pending_verify(void) {
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    return running &&
           esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
           ota_state == ESP_OTA_IMG_PENDING_VERIFY;
#else
    return false;
#endif
}

static void mark_running_app_valid_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_STARTUP_VALIDATION_DELAY_MS));

#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    if (running_app_pending_verify()) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA image marked valid after successful startup");
            automation_record_log("OTA image marked valid after successful startup");
        } else {
            ESP_LOGE(TAG, "Failed to mark OTA image valid (%s)", esp_err_to_name(err));
        }
    }
#endif

    vTaskDelete(NULL);
}

static void rollback_pending_app_and_reboot(const char *reason) {
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    if (running_app_pending_verify()) {
        ESP_LOGE(TAG, "Startup validation failed: %s. Rolling back OTA image.", reason);
        automation_record_log("Startup validation failed; rolling back OTA image");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
#else
    (void)reason;
#endif
}

static void schedule_running_app_validation(void) {
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    if (running_app_pending_verify()) {
        ESP_LOGI(TAG, "OTA image pending verification; validating after %d ms", OTA_STARTUP_VALIDATION_DELAY_MS);
        if (xTaskCreate(mark_running_app_valid_task, "ota_valid", 3072, NULL, 5, NULL) != pdPASS) {
            rollback_pending_app_and_reboot("failed to start validation task");
        }
    }
#endif
}

static void generate_uuid_v4(char *uuid, size_t size) {
    if (!uuid || size < 37) {
        if (uuid && size > 0) {
            uuid[0] = '\0';
        }
        return;
    }

    uint8_t bytes[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t value = esp_random();
        bytes[i] = (value >> 24) & 0xFF;
        bytes[i + 1] = (value >> 16) & 0xFF;
        bytes[i + 2] = (value >> 8) & 0xFF;
        bytes[i + 3] = value & 0xFF;
    }

    bytes[6] = (bytes[6] & 0x0F) | 0x40; // Version 4
    bytes[8] = (bytes[8] & 0x3F) | 0x80; // RFC 4122 variant

    snprintf(uuid, size,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5],
             bytes[6], bytes[7],
             bytes[8], bytes[9],
             bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

static void ensure_device_identity(void) {
    char *stored_device_id = get_char("device_id");
    bool need_new_device_id = true;
    if (stored_device_id && stored_device_id[0] != '\0') {
        size_t len = strlen(stored_device_id);
        need_new_device_id = len < 8; // basic sanity check
    }

    if (need_new_device_id) {
        generate_uuid_v4(device_id, sizeof(device_id));
        if (store_char("device_id", device_id) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist device UUID");
        }
        ESP_LOGI(TAG, "Generated new device UUID: %s", device_id);
    } else {
        strncpy(device_id, stored_device_id, sizeof(device_id) - 1);
        device_id[sizeof(device_id) - 1] = '\0';
    }

    if (stored_device_id) {
        free(stored_device_id);
    }

    char *stored_token = get_char("token");
    if (stored_token && stored_token[0] != '\0') {
        strncpy(token, stored_token, sizeof(token) - 1);
        token[sizeof(token) - 1] = '\0';
    } else {
        strncpy(token, device_id, sizeof(token) - 1);
        token[sizeof(token) - 1] = '\0';
        if (store_char("token", token) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist token");
        }
        ESP_LOGI(TAG, "Token not found; defaulting to device UUID");
    }

    if (stored_token) {
        free(stored_token);
    }
}

void generate_ssid_from_device_id(char *device_id, char *ssid, size_t size) {
    char suffix[5] = "uuid";
    if (device_id && device_id[0] != '\0') {
        int count = 0;
        for (int i = (int)strlen(device_id) - 1; i >= 0 && count < 4; --i) {
            if (device_id[i] == '-') {
                continue;
            }
            suffix[3 - count] = (char)tolower((unsigned char)device_id[i]);
            count++;
        }
    }

    snprintf(ssid, size, "ac_%s", suffix);
}

void perform_ota_update(const char *ota_url) {
    esp_http_client_config_t http_config = {
        .url = ota_url,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Firmware upgrade failed");
    }
}

esp_err_t http_event_handle(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (strcmp((char*)evt->data, stored_firmware_md5) != 0) {
                need_to_update_firmware = true;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

void fetch_firmware_md5_from_server(char *buffer, size_t buffer_size, const char *server_ip, const char *server_port) {
    if (!buffer || buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';

    esp_http_client_config_t http_config = {
        .url = NULL,
    };

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%s/firmware-md5", server_ip, server_port);
    http_config.url = url;

    esp_http_client_handle_t client = esp_http_client_init(&http_config);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);

        int total_read = 0;
        while (total_read < (int)buffer_size - 1) {
            int read_len = esp_http_client_read(client, buffer + total_read, buffer_size - 1 - total_read);
            if (read_len <= 0) {
                break;
            }
            total_read += read_len;
        }
        buffer[total_read] = '\0';

        for (int i = total_read - 1; i >= 0 && (buffer[i] == '\r' || buffer[i] == '\n' || buffer[i] == ' '); i--) {
            buffer[i] = '\0';
        }

        ESP_LOGI(TAG, "Firmware MD5 status=%d value=%s", esp_http_client_get_status_code(client), buffer);
        esp_http_client_close(client);
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
}

static bool post_device_punch_to_server(void) {
    char server_url[160] = {0};
    load_server_url_from_flash(server_url, sizeof(server_url));
    if (server_url[0] == '\0') {
        ESP_LOGW(TAG, "Skipping device punch: server URL is empty");
        return false;
    }

    bool use_tls = strncmp(server_url, "https://", 8) == 0;
    if (!use_tls && strncmp(server_url, "http://", 7) != 0) {
        ESP_LOGW(TAG, "Skipping device punch: server URL must start with http:// or https:// (%s)", server_url);
        return false;
    }

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{"
             "\"id\":\"%s\","
             "\"name\":\"Access Controller %s\","
             "\"type\":\"access_controller\","
             "\"model\":\"ESP32-S3\","
             "\"version\":\"%s\","
             "\"capabilities\":[\"access-control\",\"esp32\",\"wiegand\",\"rf\",\"keypad\",\"motion\",\"ota-upload\"],"
             "\"metadata\":{\"firmware\":\"controller\",\"gitBranch\":\"%s\",\"gitDirty\":%s}"
             "}",
             device_id,
             device_id,
             BUILD_GIT_COMMIT,
             BUILD_GIT_BRANCH,
             BUILD_GIT_DIRTY ? "true" : "false");

    esp_http_client_config_t http_config = {
        .url = server_url,
        .timeout_ms = 10000,
    };
    if (use_tls) {
        http_config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize device punch HTTP client");
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    ESP_LOGI(TAG, "Posting device punch to %s", server_url);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    bool ok = false;
    if (err == ESP_OK && status >= 200 && status < 300) {
        ESP_LOGI(TAG, "Device punch accepted (status=%d)", status);
        automation_record_log("Device punch accepted by server");
        ok = true;
    } else {
        ESP_LOGW(TAG, "Device punch failed: err=%s status=%d", esp_err_to_name(err), status);
        automation_record_log("Device punch failed");
    }

    esp_http_client_cleanup(client);
    return ok;
}

typedef struct {
    EventGroupHandle_t done;
    bool ok;
} device_punch_check_t;

static void device_punch_check_task(void *arg) {
    device_punch_check_t *check = (device_punch_check_t *)arg;
    if (check) {
        check->ok = post_device_punch_to_server();
        xEventGroupSetBits(check->done, DEVICE_PUNCH_CHECK_DONE_BIT);
    }
    vTaskDelete(NULL);
}

static bool post_device_punch_to_server_for_policy(void) {
    device_punch_check_t check = {
        .done = xEventGroupCreate(),
        .ok = false,
    };
    if (!check.done) {
        ESP_LOGE(TAG, "Failed to create device punch check event group");
        return false;
    }

    BaseType_t result = xTaskCreate(
        device_punch_check_task,
        "punch_check",
        DEVICE_PUNCH_CHECK_TASK_STACK_BYTES,
        &check,
        5,
        NULL);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create device punch check task");
        vEventGroupDelete(check.done);
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        check.done,
        DEVICE_PUNCH_CHECK_DONE_BIT,
        pdTRUE,
        pdFALSE,
        portMAX_DELAY);
    bool ok = (bits & DEVICE_PUNCH_CHECK_DONE_BIT) && check.ok;
    vEventGroupDelete(check.done);
    return ok;
}

static bool server_policy_allows_station_with_check(bool run_check_in_current_task) {
    bool require_server = load_server_require_reachable();
    if (!require_server) {
        ESP_LOGI(TAG, "Server reachability is not required for station mode");
        return true;
    }

    ESP_LOGI(TAG, "Server reachability is required; checking configured server URL");
    if (run_check_in_current_task ? post_device_punch_to_server() : post_device_punch_to_server_for_policy()) {
        return true;
    }

    ESP_LOGW(TAG, "Configured server URL is not reachable; station mode rejected by policy");
    automation_record_log("Server unreachable; falling back to AP mode");
    return false;
}

static bool server_policy_allows_station(void) {
    return server_policy_allows_station_with_check(false);
}

static void start_access_point_mode(void) {
    ESP_LOGI(TAG, "Starting Access Point...");
    automation_record_log("Starting AP provisioning mode");

    char ap_ssid[32];
    generate_ssid_from_device_id(device_id, ap_ssid, sizeof(ap_ssid));

    ap_main(ap_ssid, "pyfitech");
}

static void connectivity_recovery_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(CONNECTIVITY_RECOVERY_INITIAL_DELAY_MS));

    while (1) {
        char wifi_ssid[32] = {0};
        char wifi_password[64] = {0};
        load_wifi_credentials_from_flash(wifi_ssid, wifi_password);

        if (wifi_ssid[0] == '\0') {
            ESP_LOGI(TAG, "AP recovery: no saved WiFi network; staying in AP mode");
            vTaskDelay(pdMS_TO_TICKS(CONNECTIVITY_RECOVERY_INTERVAL_MS));
            continue;
        }

        ESP_LOGI(TAG, "AP recovery: trying saved WiFi SSID '%s'", wifi_ssid);
        automation_record_log("AP recovery trying saved WiFi");
        if (!station_connect(wifi_ssid, wifi_password, true)) {
            ESP_LOGW(TAG, "AP recovery: WiFi connection failed");
            vTaskDelay(pdMS_TO_TICKS(CONNECTIVITY_RECOVERY_INTERVAL_MS));
            continue;
        }

        if (!server_policy_allows_station_with_check(true)) {
            ESP_LOGW(TAG, "AP recovery: server policy failed; keeping AP mode active");
            station_disconnect_for_ap_mode();
            esp_wifi_set_mode(WIFI_MODE_AP);
            vTaskDelay(pdMS_TO_TICKS(CONNECTIVITY_RECOVERY_INTERVAL_MS));
            continue;
        }

        ESP_LOGI(TAG, "AP recovery: network and policy passed; switching to station mode");
        automation_record_log("AP recovery restored station mode");
        esp_wifi_set_mode(WIFI_MODE_STA);
        tunnel_start();
        vTaskDelete(NULL);
    }
}

static void start_connectivity_recovery_task(void) {
    BaseType_t result = xTaskCreate(
        connectivity_recovery_task,
        "net_recover",
        CONNECTIVITY_RECOVERY_TASK_STACK_BYTES,
        NULL,
        4,
        NULL);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create connectivity recovery task");
        automation_record_log("Connectivity recovery task failed to start");
    }
}


void app_main(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    init_automation_queues();
    automation_log_boot_event();

    ensure_device_identity();
    ESP_LOGI(TAG, "Device UUID: %s", device_id);
    
    char wifi_ssid[32];
    char wifi_password[64];
    snprintf(wifi_ssid, sizeof(wifi_ssid), "pyfitech");
    snprintf(wifi_password, sizeof(wifi_password), "pyfitech");

    load_wifi_credentials_from_flash(wifi_ssid, wifi_password);
    
    // Check if we have valid credentials before attempting station mode
    bool has_valid_credentials = (strlen(wifi_ssid) > 0 && strlen(wifi_password) > 0);
    bool station_connected = false;
    ESP_LOGI(TAG, "WiFi credentials check: SSID='%s' (len=%d), Password='%s' (len=%d)", 
             wifi_ssid, (int)strlen(wifi_ssid), wifi_password, (int)strlen(wifi_password));
    
    if (has_valid_credentials && station_main(wifi_ssid, wifi_password)) {
        ESP_LOGI(TAG, "Successfully connected to WiFi in station mode");
        if (server_policy_allows_station()) {
            station_connected = true;
        } else {
            station_disconnect_for_ap_mode();
            start_access_point_mode();
            start_connectivity_recovery_task();
        }
    } else {
        automation_record_log("WiFi STA failed after retries; starting AP mode");
        start_access_point_mode();
        start_connectivity_recovery_task();
    }

    if (station_connected) {
        load_server_info_from_flash(server_ip, server_port);
        char ota_url[256];

        get_md5_from_flash(stored_firmware_md5, sizeof(stored_firmware_md5));
        // Fetch the latest firmware MD5 hash from the server
        char latest_firmware_md5[33];
        fetch_firmware_md5_from_server(latest_firmware_md5, sizeof(latest_firmware_md5), server_ip, server_port);

        snprintf(ota_url, sizeof(ota_url), "http://%s:%s/firmware.bin", server_ip, server_port);
        // Compare the latest firmware MD5 hash with the stored one
        need_to_update_firmware = strcmp(stored_firmware_md5, latest_firmware_md5) != 0;

        if (need_to_update_firmware) {
            if (latest_firmware_md5[0] != '\0') {
                store_char("firmware_md5", latest_firmware_md5);
            }
            perform_ota_update(ota_url);
        }

        if (strcmp(device_id, "") == 0) {
            ESP_LOGE(TAG, "Device ID not found");
        }
    }

    esp_err_t spiffs_result = initialize_spiffs();
    if (spiffs_result == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS Initialized successfully");
    } else {
        rollback_pending_app_and_reboot("SPIFFS failed");
    }

    esp_err_t server_result = server_main();
    if (server_result != ESP_OK) {
        rollback_pending_app_and_reboot("web server failed");
    }

    gpio_main();
    i2c_main();
    mcp23x17_main();
    auth_main();
    buzzer_main();
    wiegand_registry_init();
    wiegand_main();
    exit_main();
    motion_main();
    keypad_main();
    fob_main();
    rf_registry_init();
    rf_receiver_init();
    lock_main();
    if (server_result == ESP_OK && spiffs_result == ESP_OK) {
        schedule_running_app_validation();
    }
    if (station_connected) {
        tunnel_start();
    }
    send_user_count();

    int cnt = 0;
    while (1) {
        // Get system information
        int64_t uptime_us = esp_timer_get_time();
        int64_t uptime_s = uptime_us / 1000000;
        int days = uptime_s / (24 * 3600);
        uptime_s %= (24 * 3600);
        int hours = uptime_s / 3600;
        uptime_s %= 3600;
        int minutes = uptime_s / 60;
        int seconds = uptime_s % 60;

        size_t current_free_heap = esp_get_free_heap_size();
        size_t min_free_heap = esp_get_minimum_free_heap_size();
        size_t largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

        // Get NVS stats for the 'nvs' partition
        nvs_stats_t nvs_stats;
        esp_err_t err = nvs_get_stats("nvs", &nvs_stats);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get NVS stats (%s)", esp_err_to_name(err));
        }

        // Log system status
        ESP_LOGI(TAG, "------ SYSTEM STATUS ------");
        ESP_LOGI(TAG, "Uptime: %d days %d hours %d minutes %d seconds", days, hours, minutes, seconds);
        ESP_LOGI(TAG, "Free Heap: %zu bytes (min: %zu, largest block: %zu)", current_free_heap, min_free_heap, largest_free_block);
        ESP_LOGI(TAG, "NVS Free Entries: %u", nvs_stats.free_entries);
        ESP_LOGI(TAG, "NVS Used Entries: %u", nvs_stats.used_entries);
        ESP_LOGI(TAG, "Loop Count: %d", cnt++);
        ESP_LOGI(TAG, "----------------------------");

        vTaskDelay(60 * 1000 / portTICK_PERIOD_MS);
    }
}
