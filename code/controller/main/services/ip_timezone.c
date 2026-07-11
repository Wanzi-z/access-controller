#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "ip_timezone.h"

static const char *IPTZ_TAG = "ip_timezone";

// ip-api.com's free tier auto-detects the caller's public IP server-side (no key, HTTP only on
// the free tier) and returns a ready-to-use UTC offset in seconds -- including the current DST
// adjustment -- so the device never needs an IANA timezone database of its own.
static const char *IPTZ_URL = "http://ip-api.com/json/?fields=status,offset,timezone";

#define IPTZ_HTTP_BUF_MAX 512
#define IPTZ_RETRY_DELAY_MS (5 * 60 * 1000)        // 5 min backoff after a failed lookup
#define IPTZ_REFRESH_DELAY_MS (12 * 60 * 60 * 1000) // refresh twice a day to track DST changes

static int32_t s_offset_seconds = 0;
static bool s_resolved = false;
static bool s_task_started = false;

typedef struct {
    char buf[IPTZ_HTTP_BUF_MAX];
    int len;
} iptz_http_ctx_t;

static esp_err_t iptz_http_event_handler(esp_http_client_event_t *evt) {
    iptz_http_ctx_t *ctx = (iptz_http_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && evt->data && evt->data_len > 0) {
        int copy_len = evt->data_len;
        int space = IPTZ_HTTP_BUF_MAX - ctx->len - 1;
        if (copy_len > space) copy_len = space;
        if (copy_len > 0) {
            memcpy(ctx->buf + ctx->len, evt->data, copy_len);
            ctx->len += copy_len;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool iptz_fetch_once(void) {
    iptz_http_ctx_t ctx = { .len = 0 };
    ctx.buf[0] = '\0';

    esp_http_client_config_t config = {
        .url = IPTZ_URL,
        .event_handler = iptz_http_event_handler,
        .user_data = &ctx,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(IPTZ_TAG, "Failed to init HTTP client");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || ctx.len == 0) {
        ESP_LOGW(IPTZ_TAG, "IP geolocation request failed (err=%s, status=%d)", esp_err_to_name(err), status);
        return false;
    }

    cJSON *root = cJSON_Parse(ctx.buf);
    if (!root) {
        ESP_LOGW(IPTZ_TAG, "IP geolocation response was not valid JSON");
        return false;
    }

    const cJSON *status_item = cJSON_GetObjectItemCaseSensitive(root, "status");
    const cJSON *offset_item = cJSON_GetObjectItemCaseSensitive(root, "offset");
    const cJSON *tz_item = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    bool ok = cJSON_IsString(status_item) && status_item->valuestring
        && strcmp(status_item->valuestring, "success") == 0
        && cJSON_IsNumber(offset_item);

    if (ok) {
        s_offset_seconds = (int32_t)offset_item->valuedouble;
        s_resolved = true;
        ESP_LOGI(IPTZ_TAG, "Resolved UTC offset %ld s from IP geolocation (timezone=%s)",
                 (long)s_offset_seconds, (cJSON_IsString(tz_item) && tz_item->valuestring) ? tz_item->valuestring : "?");
    } else {
        ESP_LOGW(IPTZ_TAG, "IP geolocation response missing status=success/offset");
    }

    cJSON_Delete(root);
    return ok;
}

static void ip_timezone_task(void *arg) {
    (void)arg;
    for (;;) {
        bool ok = iptz_fetch_once();
        vTaskDelay((ok ? IPTZ_REFRESH_DELAY_MS : IPTZ_RETRY_DELAY_MS) / portTICK_PERIOD_MS);
    }
}

void ip_timezone_start(void) {
    if (s_task_started) return;
    s_task_started = true;
    xTaskCreate(ip_timezone_task, "ip_timezone_task", 6 * 1024, NULL, 4, NULL);
}

int32_t ip_timezone_offset_seconds(void) {
    return s_offset_seconds;
}

bool ip_timezone_is_resolved(void) {
    return s_resolved;
}
