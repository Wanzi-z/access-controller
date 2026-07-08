#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "automation.h"
#include <sys/time.h>
#include <stdbool.h>

#include "lwip/err.h"
#include "lwip/sys.h"

#define EXAMPLE_ESP_MAXIMUM_RETRY  5

/* FreeRTOS event group to signal when we are connected */
static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static bool s_sntp_started = false;
static bool s_connected_once = false;
static int s_disconnect_retry = 0;
static bool s_wifi_driver_ready = false;
static bool s_wifi_started = false;
static bool s_sta_netif_ready = false;
static bool s_sta_handlers_ready = false;

static void time_sync_notification_cb(struct timeval *tv) {
    time_t now = 0;
    time(&now);
    automation_update_unix_time((int64_t)now);
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        int reason = disc ? disc->reason : -1;
        ESP_LOGW(TAG, "Station disconnected (reason=%d)", reason);

        char message[96];
        snprintf(message, sizeof(message), "WiFi disconnected (reason=%d)", reason);
        automation_record_log(message);

        if (s_connected_once) {
            if (s_disconnect_retry < EXAMPLE_ESP_MAXIMUM_RETRY) {
                s_disconnect_retry++;
                esp_wifi_connect();
                ESP_LOGI(TAG, "Reconnect attempt %d after disconnect", s_disconnect_retry);
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            return;
        }

        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry %d to connect to the AP", s_retry_num);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_connected_once = true;
        s_disconnect_retry = 0;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        if (!s_sntp_started) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
            esp_sntp_init();
            s_sntp_started = true;
            ESP_LOGI(TAG, "SNTP initialised");
        }
    }
}

esp_err_t wifi_driver_ensure_initialized(void) {
    if (s_wifi_driver_ready) {
        return ESP_OK;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err == ESP_OK || err == ESP_ERR_WIFI_INIT_STATE) {
        s_wifi_driver_ready = true;
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to initialize WiFi driver (%s)", esp_err_to_name(err));
    return err;
}

static esp_err_t station_ensure_initialized(void) {
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
        if (!s_wifi_event_group) {
            ESP_LOGE(TAG, "Failed to create WiFi event group");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_sta_netif_ready) {
        esp_netif_create_default_wifi_sta();
        s_sta_netif_ready = true;
    }

    esp_err_t err = wifi_driver_ensure_initialized();
    if (err != ESP_OK) {
        return err;
    }

    if (s_sta_handlers_ready) {
        return ESP_OK;
    }
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    s_sta_handlers_ready = true;
    return ESP_OK;
}

bool station_connect(char *ssid, char *password, bool keep_ap_enabled) {
    if (!ssid || ssid[0] == '\0') {
        return false;
    }

    esp_err_t err = station_ensure_initialized();
    if (err != ESP_OK) {
        return false;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;
    s_disconnect_retry = 0;
    s_connected_once = false;

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",  // Placeholder values, to be set below.
            .password = "", 
        },
    };

    // Copy ssid and password into wifi_config
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    err = esp_wifi_set_mode(keep_ap_enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode (%s)", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi STA config (%s)", esp_err_to_name(err));
        return false;
    }

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGE(TAG, "Failed to start WiFi (%s)", esp_err_to_name(err));
            return false;
        }
        s_wifi_started = true;
    } else {
        esp_wifi_disconnect();
        esp_wifi_connect();
    }

    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi power save disabled for low-latency controller traffic");
    } else {
        ESP_LOGW(TAG, "Failed to disable WiFi power save (%s)", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "wifi_init_sta finished%s.", keep_ap_enabled ? " in APSTA recovery mode" : "");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", ssid, password);
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to connect to SSID:%s, password:%s", ssid, password);
        if (keep_ap_enabled) {
            esp_wifi_disconnect();
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        return false;
    }
}

void station_disconnect_for_ap_mode(void) {
    s_connected_once = false;
    s_retry_num = EXAMPLE_ESP_MAXIMUM_RETRY;
    s_disconnect_retry = EXAMPLE_ESP_MAXIMUM_RETRY;
    esp_wifi_disconnect();
}

bool station_main(char *ssid, char *password) {
    return station_connect(ssid, password, false);
}
