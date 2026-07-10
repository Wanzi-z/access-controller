#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#define EXAMPLE_MAX_STA_CONN       CONFIG_ESP_MAX_STA_CONN

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

void ap_main(char *ssid, char *password)
{
    static bool ap_netif_created = false;
    if (!ap_netif_created) {
        esp_netif_create_default_wifi_ap();
        ap_netif_created = true;
    }

    ESP_ERROR_CHECK(wifi_driver_ensure_initialized());
    esp_err_t storage_err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (storage_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set AP WiFi storage to RAM (%s)", esp_err_to_name(storage_err));
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "",
            .ssid_len = 0, // This will be set after copying SSID
            .channel = CONFIG_ESP_WIFI_CHANNEL,
            .password = "",
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                    .required = false,
            },
        },
    };

    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid[sizeof(wifi_config.ap.ssid) - 1] = '\0';
    wifi_config.ap.ssid_len = strlen(ssid);
    strncpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.password[sizeof(wifi_config.ap.password) - 1] = '\0';
    printf("ap_main\t%s\t%s\n", wifi_config.ap.ssid, wifi_config.ap.password);

    if (strlen(password) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AP WiFi mode (%s)", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AP WiFi config (%s)", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "Failed to start AP WiFi (%s)", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             ssid, password, CONFIG_ESP_WIFI_CHANNEL);
}
