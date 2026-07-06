#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "automation.h"
#include "enrollment.h"
#include "rf_registry.h"
#include "store.h"
#include "wiegand_registry.h"

#define ENROLLMENT_VALUE_MAX 80

static const char *ENROLLMENT_TAG = "enrollment";

typedef struct {
    bool active;
    char user_uuid[ENROLLMENT_USER_UUID_MAX];
    char user_name[ENROLLMENT_USER_NAME_MAX];
    uint32_t rfid_count;
    uint32_t pin_count;
    uint32_t remote_count;
    char last_source[16];
    char last_value[ENROLLMENT_VALUE_MAX];
    char last_status[32];
    uint64_t updated_ms;
} enrollment_session_t;

static enrollment_session_t s_enrollment = {0};
static SemaphoreHandle_t s_enrollment_mutex = NULL;

static void enrollment_ensure_mutex(void) {
    if (!s_enrollment_mutex) {
        s_enrollment_mutex = xSemaphoreCreateMutex();
    }
}

static uint64_t enrollment_now_ms(void) {
    return esp_timer_get_time() / 1000ULL;
}

static void enrollment_generate_uuid(char *buf, size_t len) {
    static const char hex[] = "0123456789abcdef";
    if (!buf || len == 0) {
        return;
    }
    for (size_t i = 0; i + 1 < len; i++) {
        buf[i] = hex[esp_random() % 16];
    }
    buf[len - 1] = '\0';
}

static bool enrollment_load_user_name(const char *user_uuid, char *name, size_t name_size) {
    if (!user_uuid || user_uuid[0] == '\0' || !name || name_size == 0) {
        return false;
    }
    name[0] = '\0';

    uint32_t count = get_u32("auth_user_count", 0);
    for (uint32_t i = 0; i < count; i++) {
        cJSON *user = load_user_from_flash(i + 1);
        if (!user) {
            continue;
        }

        const cJSON *uuid_item = cJSON_GetObjectItemCaseSensitive(user, "uuid");
        if (cJSON_IsString(uuid_item) && uuid_item->valuestring &&
            strcmp(uuid_item->valuestring, user_uuid) == 0) {
            const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(user, "name");
            if (cJSON_IsString(name_item) && name_item->valuestring) {
                strlcpy(name, name_item->valuestring, name_size);
            }
            cJSON_Delete(user);
            return true;
        }
        cJSON_Delete(user);
    }

    return false;
}

static esp_err_t enrollment_default_user(char *uuid, size_t uuid_size, char *name, size_t name_size) {
    uint32_t count = get_u32("auth_user_count", 0);
    if (count > 0) {
        cJSON *user = load_user_from_flash(1);
        if (user) {
            const cJSON *uuid_item = cJSON_GetObjectItemCaseSensitive(user, "uuid");
            const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(user, "name");
            if (cJSON_IsString(uuid_item) && uuid_item->valuestring) {
                strlcpy(uuid, uuid_item->valuestring, uuid_size);
                strlcpy(name,
                        (cJSON_IsString(name_item) && name_item->valuestring) ? name_item->valuestring : "Default User",
                        name_size);
                cJSON_Delete(user);
                return ESP_OK;
            }
            cJSON_Delete(user);
        }
    }

    enrollment_generate_uuid(uuid, uuid_size);
    strlcpy(name, "Default User", name_size);
    store_user_to_flash(uuid, name, "");
    return ESP_OK;
}

static void enrollment_set_last_locked(const char *source, const char *value, const char *status) {
    strlcpy(s_enrollment.last_source, source ? source : "", sizeof(s_enrollment.last_source));
    strlcpy(s_enrollment.last_value, value ? value : "", sizeof(s_enrollment.last_value));
    strlcpy(s_enrollment.last_status, status ? status : "", sizeof(s_enrollment.last_status));
    s_enrollment.updated_ms = enrollment_now_ms();
}

esp_err_t enrollment_start(const char *user_uuid) {
    enrollment_ensure_mutex();
    if (!s_enrollment_mutex) {
        return ESP_ERR_NO_MEM;
    }

    char selected_uuid[ENROLLMENT_USER_UUID_MAX] = {0};
    char selected_name[ENROLLMENT_USER_NAME_MAX] = {0};

    if (user_uuid && user_uuid[0] != '\0') {
        if (!enrollment_load_user_name(user_uuid, selected_name, sizeof(selected_name))) {
            return ESP_ERR_NOT_FOUND;
        }
        strlcpy(selected_uuid, user_uuid, sizeof(selected_uuid));
    } else {
        esp_err_t err = enrollment_default_user(selected_uuid, sizeof(selected_uuid), selected_name, sizeof(selected_name));
        if (err != ESP_OK) {
            return err;
        }
    }

    xSemaphoreTake(s_enrollment_mutex, portMAX_DELAY);
    memset(&s_enrollment, 0, sizeof(s_enrollment));
    s_enrollment.active = true;
    strlcpy(s_enrollment.user_uuid, selected_uuid, sizeof(s_enrollment.user_uuid));
    strlcpy(s_enrollment.user_name, selected_name[0] ? selected_name : "Default User", sizeof(s_enrollment.user_name));
    enrollment_set_last_locked("session", s_enrollment.user_name, "listening");
    xSemaphoreGive(s_enrollment_mutex);

    ESP_LOGI(ENROLLMENT_TAG, "Enrollment started for user %s (%s)", selected_name, selected_uuid);
    automation_record_log("Credential enrollment started");
    return ESP_OK;
}

esp_err_t enrollment_stop(void) {
    enrollment_ensure_mutex();
    if (!s_enrollment_mutex) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_enrollment_mutex, portMAX_DELAY);
    if (!s_enrollment.active) {
        xSemaphoreGive(s_enrollment_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_enrollment.active = false;
    enrollment_set_last_locked("session", s_enrollment.user_name, "stopped");
    xSemaphoreGive(s_enrollment_mutex);

    ESP_LOGI(ENROLLMENT_TAG, "Enrollment stopped");
    automation_record_log("Credential enrollment stopped");
    return ESP_OK;
}

bool enrollment_is_active(void) {
    enrollment_ensure_mutex();
    if (!s_enrollment_mutex) {
        return false;
    }
    xSemaphoreTake(s_enrollment_mutex, portMAX_DELAY);
    bool active = s_enrollment.active;
    xSemaphoreGive(s_enrollment_mutex);
    return active;
}

bool enrollment_on_wiegand(const char *code, int channel) {
    if (!code || code[0] == '\0') {
        return false;
    }
    enrollment_ensure_mutex();
    if (!s_enrollment_mutex) {
        return false;
    }

    char user_uuid[ENROLLMENT_USER_UUID_MAX];
    char user_name[ENROLLMENT_USER_NAME_MAX];
    xSemaphoreTake(s_enrollment_mutex, portMAX_DELAY);
    if (!s_enrollment.active) {
        xSemaphoreGive(s_enrollment_mutex);
        return false;
    }
    strlcpy(user_uuid, s_enrollment.user_uuid, sizeof(user_uuid));
    strlcpy(user_name, s_enrollment.user_name, sizeof(user_name));
    xSemaphoreGive(s_enrollment_mutex);

    wiegand_user_t added;
    esp_err_t err = wiegand_registry_add_for_user(code, (uint8_t)channel, user_uuid, user_name, true, &added);

    xSemaphoreTake(s_enrollment_mutex, portMAX_DELAY);
    if (err == ESP_OK) {
        s_enrollment.rfid_count++;
        enrollment_set_last_locked("rfid", code, "added");
    } else if (err == ESP_ERR_INVALID_STATE) {
        enrollment_set_last_locked("rfid", code, "duplicate");
    } else {
        enrollment_set_last_locked("rfid", code, "failed");
        ESP_LOGW(ENROLLMENT_TAG, "Failed to add RFID credential (%s)", esp_err_to_name(err));
    }
    xSemaphoreGive(s_enrollment_mutex);
    return true;
}

bool enrollment_on_pin(const char *pin, int channel) {
    (void)channel;
    if (!pin || pin[0] == '\0') {
        return false;
    }
    enrollment_ensure_mutex();
    if (!s_enrollment_mutex) {
        return false;
    }

    char user_uuid[ENROLLMENT_USER_UUID_MAX];
    xSemaphoreTake(s_enrollment_mutex, portMAX_DELAY);
    if (!s_enrollment.active) {
        xSemaphoreGive(s_enrollment_mutex);
        return false;
    }
    strlcpy(user_uuid, s_enrollment.user_uuid, sizeof(user_uuid));
    xSemaphoreGive(s_enrollment_mutex);

    bool added = false;
    esp_err_t err = append_user_pin_to_flash(user_uuid, pin, &added);

    xSemaphoreTake(s_enrollment_mutex, portMAX_DELAY);
    if (err == ESP_OK && added) {
        s_enrollment.pin_count++;
        enrollment_set_last_locked("pin", "****", "added");
    } else if (err == ESP_OK) {
        enrollment_set_last_locked("pin", "****", "duplicate");
    } else {
        enrollment_set_last_locked("pin", "****", "failed");
        ESP_LOGW(ENROLLMENT_TAG, "Failed to save PIN credential (%s)", esp_err_to_name(err));
    }
    xSemaphoreGive(s_enrollment_mutex);
    ESP_LOGI(ENROLLMENT_TAG, "PIN credential saved for user %s", user_uuid);
    return true;
}

bool enrollment_on_rf(uint32_t code, size_t pulse_count) {
    enrollment_ensure_mutex();
    if (!s_enrollment_mutex) {
        return false;
    }

    char user_uuid[ENROLLMENT_USER_UUID_MAX];
    char user_name[ENROLLMENT_USER_NAME_MAX];
    xSemaphoreTake(s_enrollment_mutex, portMAX_DELAY);
    if (!s_enrollment.active) {
        xSemaphoreGive(s_enrollment_mutex);
        return false;
    }
    strlcpy(user_uuid, s_enrollment.user_uuid, sizeof(user_uuid));
    strlcpy(user_name, s_enrollment.user_name, sizeof(user_name));
    xSemaphoreGive(s_enrollment_mutex);

    char code_hex[9];
    snprintf(code_hex, sizeof(code_hex), "%06lX", (unsigned long)(code & 0xFFFFFF));
    esp_err_t err = rf_registry_add_for_user(code, pulse_count, user_uuid, user_name);

    xSemaphoreTake(s_enrollment_mutex, portMAX_DELAY);
    if (err == ESP_OK) {
        s_enrollment.remote_count++;
        enrollment_set_last_locked("remote", code_hex, "added");
    } else if (err == ESP_ERR_INVALID_STATE) {
        enrollment_set_last_locked("remote", code_hex, "duplicate");
    } else {
        enrollment_set_last_locked("remote", code_hex, "failed");
        ESP_LOGW(ENROLLMENT_TAG, "Failed to add RF credential (%s)", esp_err_to_name(err));
    }
    xSemaphoreGive(s_enrollment_mutex);
    return true;
}

cJSON *enrollment_state_snapshot(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    enrollment_ensure_mutex();
    if (!s_enrollment_mutex) {
        return root;
    }

    xSemaphoreTake(s_enrollment_mutex, portMAX_DELAY);
    cJSON_AddBoolToObject(root, "active", s_enrollment.active);
    cJSON_AddStringToObject(root, "userUuid", s_enrollment.user_uuid);
    cJSON_AddStringToObject(root, "userName", s_enrollment.user_name);
    cJSON_AddNumberToObject(root, "rfidCount", s_enrollment.rfid_count);
    cJSON_AddNumberToObject(root, "pinCount", s_enrollment.pin_count);
    cJSON_AddNumberToObject(root, "remoteCount", s_enrollment.remote_count);
    cJSON_AddStringToObject(root, "lastSource", s_enrollment.last_source);
    cJSON_AddStringToObject(root, "lastValue", s_enrollment.last_value);
    cJSON_AddStringToObject(root, "lastStatus", s_enrollment.last_status);
    cJSON_AddNumberToObject(root, "updatedMs", (double)s_enrollment.updated_ms);
    xSemaphoreGive(s_enrollment_mutex);

    return root;
}
