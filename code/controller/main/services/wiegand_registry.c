#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "automation.h"
#include "wiegand_registry.h"
#include "store.h"

static const char *LOG_TAG_WIEGAND_REGISTRY = "wiegand_registry";
static const char *NVS_KEY = "wiegand_users";
static const char *REGISTRY_FILE_PATH = "/spiffs/wiegand_users.json";
static const char *REGISTRY_TMP_FILE_PATH = "/spiffs/wiegand_users.tmp";

static wiegand_user_t *s_users = NULL;
static size_t s_user_count = 0;
static size_t s_user_capacity = 0;
static bool s_initialised = false;
static SemaphoreHandle_t s_mutex = NULL;

static uint64_t current_time_ms(void) {
    return esp_timer_get_time() / 1000ULL;
}

static void ensure_mutex(void) {
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

static void clear_users(void) {
    if (s_users && s_user_capacity > 0) {
        memset(s_users, 0, s_user_capacity * sizeof(*s_users));
    }
    s_user_count = 0;
}

static bool ensure_capacity_locked(size_t required) {
    if (required <= s_user_capacity) {
        return true;
    }

    size_t new_capacity = s_user_capacity ? s_user_capacity : 16;
    while (new_capacity < required && new_capacity < WIEGAND_USER_MAX_COUNT) {
        new_capacity *= 2;
    }

    if (new_capacity > WIEGAND_USER_MAX_COUNT) {
        new_capacity = WIEGAND_USER_MAX_COUNT;
    }

    if (new_capacity < required) {
        ESP_LOGW(LOG_TAG_WIEGAND_REGISTRY, "Registry capacity limit reached (%u)", WIEGAND_USER_MAX_COUNT);
        return false;
    }

    wiegand_user_t *new_users = realloc(s_users, new_capacity * sizeof(*s_users));
    if (!new_users) {
        ESP_LOGE(LOG_TAG_WIEGAND_REGISTRY, "Failed to expand registry to %zu entries", required);
        return false;
    }

    size_t old_capacity = s_user_capacity;
    s_users = new_users;
    s_user_capacity = new_capacity;
    if (s_user_capacity > old_capacity) {
        memset(s_users + old_capacity, 0, (s_user_capacity - old_capacity) * sizeof(*s_users));
    }
    return true;
}

static void assign_defaults(wiegand_user_t *user) {
    if (!user) return;
    if (user->name[0] == '\0') {
        snprintf(user->name, sizeof(user->name), "User %lu", (unsigned long)(user->sequence + 1));
    }
    if (user->mode[0] == '\0' ||
        (strcmp(user->mode, "momentary") != 0 &&
         strcmp(user->mode, "toggle") != 0 &&
         strcmp(user->mode, "latch") != 0)) {
        strlcpy(user->mode, "momentary", sizeof(user->mode));
    }
    if (user->status != WIEGAND_USER_STATUS_ACTIVE &&
        user->status != WIEGAND_USER_STATUS_DISABLED &&
        user->status != WIEGAND_USER_STATUS_PENDING) {
        user->status = WIEGAND_USER_STATUS_ACTIVE;
    }
    if (user->channel_mask == 0 || user->channel_mask > 3) {
        if (user->channel >= 1 && user->channel <= 2) {
            user->channel_mask = (uint8_t)(1 << (user->channel - 1));
        } else {
            user->channel_mask = 3;
        }
    }
    user->alert_target = alert_target_normalize(user->alert_target, user->alert);
    user->alert = user->alert_target != ALERT_TARGET_NONE;
}

static cJSON *serialize_user(const wiegand_user_t *user) {
    if (!user) return NULL;
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddStringToObject(obj, "id", user->id);
    cJSON_AddStringToObject(obj, "code", user->code);
    cJSON_AddStringToObject(obj, "name", user->name);
    cJSON_AddStringToObject(obj, "mode", user->mode);
    cJSON_AddStringToObject(obj, "userUuid", user->user_uuid);
    cJSON_AddNumberToObject(obj, "channel", user->channel);
    cJSON_AddNumberToObject(obj, "channel_mask", user->channel_mask);
    cJSON_AddNumberToObject(obj, "status", user->status);
    cJSON_AddBoolToObject(obj, "alert", user->alert);
    cJSON_AddStringToObject(obj, "alert_target", alert_target_to_string(user->alert_target));
    cJSON_AddNumberToObject(obj, "sequence", user->sequence);
    cJSON_AddNumberToObject(obj, "created_at_ms", (double)user->created_at_ms);
    cJSON_AddNumberToObject(obj, "updated_at_ms", (double)user->updated_at_ms);
    cJSON_AddNumberToObject(obj, "last_used_ms", (double)user->last_used_ms);
    cJSON_AddNumberToObject(obj, "last_used_unix_time", (double)user->last_used_unix_time);
    if (user->last_used_ms > 0) {
        cJSON *last_used = cJSON_CreateObject();
        if (last_used) {
            bool age_known = false;
            uint64_t age_ms = 0;
            uint64_t now = current_time_ms();
            int64_t unix_time = user->last_used_unix_time;
            int64_t now_unix_time = automation_unix_time_for_timestamp_ms(now);

            if (unix_time > 0 && now_unix_time > 0 && now_unix_time >= unix_time) {
                age_ms = (uint64_t)(now_unix_time - unix_time) * 1000ULL;
                age_known = true;
            } else if (now >= user->last_used_ms) {
                age_ms = now - user->last_used_ms;
                age_known = true;
            }

            if (unix_time <= 0) {
                unix_time = automation_unix_time_for_timestamp_ms(user->last_used_ms);
            }

            if (age_known) {
                cJSON_AddNumberToObject(last_used, "used_ms", (double)user->last_used_ms);
                cJSON_AddNumberToObject(last_used, "unixTime", (double)unix_time);
                cJSON_AddNumberToObject(last_used, "age_ms", (double)age_ms);
                cJSON_AddItemToObject(obj, "lastUsed", last_used);
            } else {
                cJSON_Delete(last_used);
            }
        }
    }
    return obj;
}

static bool parse_string_field(const cJSON *obj, const char *key, char *out, size_t len) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    strlcpy(out, item->valuestring, len);
    return true;
}

static bool deserialize_user(const cJSON *obj, wiegand_user_t *out_user) {
    if (!cJSON_IsObject(obj) || !out_user) {
        return false;
    }

    memset(out_user, 0, sizeof(*out_user));

    if (!parse_string_field(obj, "id", out_user->id, sizeof(out_user->id))) {
        ESP_LOGW(LOG_TAG_WIEGAND_REGISTRY, "User missing id");
        return false;
    }
    if (!parse_string_field(obj, "code", out_user->code, sizeof(out_user->code))) {
        ESP_LOGW(LOG_TAG_WIEGAND_REGISTRY, "User missing code");
        return false;
    }
    parse_string_field(obj, "name", out_user->name, sizeof(out_user->name));
    parse_string_field(obj, "mode", out_user->mode, sizeof(out_user->mode));
    parse_string_field(obj, "userUuid", out_user->user_uuid, sizeof(out_user->user_uuid));

    const cJSON *channel = cJSON_GetObjectItemCaseSensitive(obj, "channel");
    const cJSON *channel_mask = cJSON_GetObjectItemCaseSensitive(obj, "channel_mask");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(obj, "status");
    const cJSON *alert = cJSON_GetObjectItemCaseSensitive(obj, "alert");
    const cJSON *alert_target = cJSON_GetObjectItemCaseSensitive(obj, "alert_target");
    const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(obj, "sequence");
    const cJSON *created_at = cJSON_GetObjectItemCaseSensitive(obj, "created_at_ms");
    const cJSON *updated_at = cJSON_GetObjectItemCaseSensitive(obj, "updated_at_ms");
    const cJSON *last_used_ms = cJSON_GetObjectItemCaseSensitive(obj, "last_used_ms");
    const cJSON *last_used_unix_time = cJSON_GetObjectItemCaseSensitive(obj, "last_used_unix_time");

    out_user->channel = cJSON_IsNumber(channel) ? (uint8_t)channel->valuedouble : 0;
    out_user->channel_mask = cJSON_IsNumber(channel_mask) ? (uint8_t)channel_mask->valuedouble : 0;
    out_user->status = cJSON_IsNumber(status) ? (wiegand_user_status_t)status->valuedouble : WIEGAND_USER_STATUS_ACTIVE;
    out_user->alert = cJSON_IsBool(alert) ? cJSON_IsTrue(alert) : true;
    if (cJSON_IsString(alert_target) && alert_target->valuestring) {
        out_user->alert_target = alert_target_from_string(alert_target->valuestring, out_user->alert);
    } else if (cJSON_IsNumber(alert_target)) {
        out_user->alert_target = alert_target_normalize(alert_target->valueint, out_user->alert);
    } else {
        out_user->alert_target = alert_target_from_bool(out_user->alert);
    }
    out_user->sequence = cJSON_IsNumber(sequence) ? (uint32_t)sequence->valuedouble : 0;
    out_user->created_at_ms = cJSON_IsNumber(created_at) ? (uint64_t)created_at->valuedouble : current_time_ms();
    out_user->updated_at_ms = cJSON_IsNumber(updated_at) ? (uint64_t)updated_at->valuedouble : out_user->created_at_ms;
    out_user->last_used_ms = cJSON_IsNumber(last_used_ms) ? (uint64_t)last_used_ms->valuedouble : 0;
    out_user->last_used_unix_time = cJSON_IsNumber(last_used_unix_time) ? (int64_t)last_used_unix_time->valuedouble : 0;

    assign_defaults(out_user);
    return true;
}

static void sort_users(void) {
    if (s_user_count < 2) return;
    for (size_t i = 0; i < s_user_count - 1; i++) {
        for (size_t j = i + 1; j < s_user_count; j++) {
            if (s_users[i].sequence > s_users[j].sequence) {
                wiegand_user_t tmp = s_users[i];
                s_users[i] = s_users[j];
                s_users[j] = tmp;
            }
        }
    }
}

static uint32_t next_sequence(void) {
    uint32_t max_seq = 0;
    for (size_t i = 0; i < s_user_count; i++) {
        if (s_users[i].sequence > max_seq) {
            max_seq = s_users[i].sequence;
        }
    }
    return max_seq + 1;
}

static esp_err_t persist_locked(void);

void wiegand_registry_init(void) {
    ensure_mutex();
    if (s_initialised) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    free(s_users);
    s_users = NULL;
    s_user_capacity = 0;
    clear_users();
    s_initialised = true;
    xSemaphoreGive(s_mutex);
    if (wiegand_registry_reload() != ESP_OK) {
        ESP_LOGW(LOG_TAG_WIEGAND_REGISTRY, "Initial load failed; continuing with empty registry");
    }
}

bool wiegand_registry_is_initialised(void) {
    return s_initialised;
}

size_t wiegand_registry_count(void) {
    return s_user_count;
}

const wiegand_user_t *wiegand_registry_get(size_t index) {
    if (index >= s_user_count || !s_users) {
        return NULL;
    }
    return &s_users[index];
}

static ssize_t find_index_by_code(const char *code) {
    if (!code) return -1;
    for (size_t i = 0; i < s_user_count; i++) {
        if (strcmp(s_users[i].code, code) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static ssize_t find_index_by_id(const char *id) {
    if (!id) return -1;
    for (size_t i = 0; i < s_user_count; i++) {
        if (strcmp(s_users[i].id, id) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

const wiegand_user_t *wiegand_registry_find_by_code(const char *code) {
    ssize_t idx = find_index_by_code(code);
    if (idx < 0) return NULL;
    return &s_users[idx];
}

const wiegand_user_t *wiegand_registry_find_by_id(const char *id) {
    ssize_t idx = find_index_by_id(id);
    if (idx < 0) return NULL;
    return &s_users[idx];
}

static esp_err_t read_registry_file(char **out_json) {
    if (!out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    FILE *file = fopen(REGISTRY_FILE_PATH, "r");
    if (!file) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long length = ftell(file);
    if (length < 0) {
        fclose(file);
        return ESP_FAIL;
    }
    rewind(file);

    char *buffer = malloc((size_t)length + 1);
    if (!buffer) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buffer, 1, (size_t)length, file);
    fclose(file);
    buffer[read] = '\0';
    *out_json = buffer;
    return ESP_OK;
}

static esp_err_t write_registry_file(const char *json) {
    if (!json) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(REGISTRY_TMP_FILE_PATH, "w");
    if (!file) {
        ESP_LOGE(LOG_TAG_WIEGAND_REGISTRY,
                 "Failed to open %s for writing: errno=%d",
                 REGISTRY_TMP_FILE_PATH,
                 errno);
        return ESP_FAIL;
    }

    size_t len = strlen(json);
    size_t written = fwrite(json, 1, len, file);
    int close_result = fclose(file);
    if (written != len || close_result != 0) {
        ESP_LOGE(LOG_TAG_WIEGAND_REGISTRY,
                 "Failed to write registry file (%u/%u bytes, close=%d)",
                 (unsigned)written,
                 (unsigned)len,
                 close_result);
        remove(REGISTRY_TMP_FILE_PATH);
        return ESP_FAIL;
    }

    remove(REGISTRY_FILE_PATH);
    if (rename(REGISTRY_TMP_FILE_PATH, REGISTRY_FILE_PATH) != 0) {
        ESP_LOGE(LOG_TAG_WIEGAND_REGISTRY,
                 "Failed to replace %s: errno=%d",
                 REGISTRY_FILE_PATH,
                 errno);
        remove(REGISTRY_TMP_FILE_PATH);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t persist_locked(void) {
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        ESP_LOGE(LOG_TAG_WIEGAND_REGISTRY, "Failed to allocate array for persistence");
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < s_user_count; i++) {
        cJSON *entry = serialize_user(&s_users[i]);
        if (entry) {
            cJSON_AddItemToArray(array, entry);
        }
    }

    char *json = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    if (!json) {
        ESP_LOGE(LOG_TAG_WIEGAND_REGISTRY, "Failed to serialise registry");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = write_registry_file(json);
    if (err == ESP_OK) {
        ESP_LOGI(LOG_TAG_WIEGAND_REGISTRY,
                 "Persisted %u Wiegand users to SPIFFS (%u bytes)",
                 (unsigned)s_user_count,
                 (unsigned)strlen(json));
    } else {
        ESP_LOGE(LOG_TAG_WIEGAND_REGISTRY,
                 "Failed to persist Wiegand registry to SPIFFS (%s)",
                 esp_err_to_name(err));
    }
    free(json);
    return err;
}

static esp_err_t load_locked(void) {
    bool loaded_from_legacy_nvs = false;
    char *json_str = NULL;
    esp_err_t read_err = read_registry_file(&json_str);
    if (read_err == ESP_ERR_NOT_FOUND) {
        json_str = get_char(NVS_KEY);
        loaded_from_legacy_nvs = true;
    } else if (read_err != ESP_OK) {
        ESP_LOGW(LOG_TAG_WIEGAND_REGISTRY,
                 "Failed to read Wiegand registry file (%s)",
                 esp_err_to_name(read_err));
        return read_err;
    }

    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    if (json_str[0] == '\0') {
        free(json_str);
        clear_users();
        return ESP_OK;
    }

    cJSON *array = cJSON_Parse(json_str);
    free(json_str);
    if (!array || !cJSON_IsArray(array)) {
        ESP_LOGW(LOG_TAG_WIEGAND_REGISTRY, "Stored wiegand_users is not an array");
        cJSON_Delete(array);
        return ESP_FAIL;
    }

    size_t count = cJSON_GetArraySize(array);
    if (count > WIEGAND_USER_MAX_COUNT) {
        ESP_LOGW(LOG_TAG_WIEGAND_REGISTRY, "Stored users exceed maximum (%zu > %d); truncating", count, WIEGAND_USER_MAX_COUNT);
        count = WIEGAND_USER_MAX_COUNT;
    }

    clear_users();
    if (!ensure_capacity_locked(count)) {
        cJSON_Delete(array);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(array, i);
        if (item) {
            wiegand_user_t user;
            if (deserialize_user(item, &user)) {
                if (s_user_count < WIEGAND_USER_MAX_COUNT) {
                    if (!ensure_capacity_locked(s_user_count + 1)) {
                        cJSON_Delete(array);
                        return ESP_ERR_NO_MEM;
                    }
                    s_users[s_user_count++] = user;
                } else {
                    ESP_LOGW(LOG_TAG_WIEGAND_REGISTRY, "Registry full while loading; truncating");
                    break;
                }
            }
        }
    }

    cJSON_Delete(array);
    sort_users();
    if (loaded_from_legacy_nvs && s_user_count > 0) {
        esp_err_t migrate_err = persist_locked();
        if (migrate_err == ESP_OK) {
            ESP_LOGI(LOG_TAG_WIEGAND_REGISTRY,
                     "Migrated %u Wiegand users from NVS string to SPIFFS",
                     (unsigned)s_user_count);
        } else {
            ESP_LOGW(LOG_TAG_WIEGAND_REGISTRY,
                     "Failed to migrate Wiegand users to SPIFFS (%s)",
                     esp_err_to_name(migrate_err));
        }
    }
    return ESP_OK;
}

esp_err_t wiegand_registry_reload(void) {
    ensure_mutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = load_locked();
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t wiegand_registry_save(void) {
    ensure_mutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = persist_locked();
    xSemaphoreGive(s_mutex);
    return result;
}

static void generate_id(char *buffer, size_t len) {
    const char *alphabet = "0123456789abcdef";
    for (size_t i = 0; i + 1 < len; i++) {
        uint32_t rnd = esp_random();
        buffer[i] = alphabet[rnd % 16];
    }
    buffer[len - 1] = '\0';
}

esp_err_t wiegand_registry_add_for_user(const char *code,
                                        uint8_t channel,
                                        const char *user_uuid,
                                        const char *name,
                                        bool active,
                                        wiegand_user_t *out_user) {
    if (!code || code[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ensure_mutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_initialised) {
        ESP_LOGE(LOG_TAG_WIEGAND_REGISTRY, "Registry not initialised");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_user_count >= WIEGAND_USER_MAX_COUNT) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    if (find_index_by_code(code) >= 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (!ensure_capacity_locked(s_user_count + 1)) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    wiegand_user_t user = {
        .channel = channel,
        .channel_mask = (channel >= 1 && channel <= 2) ? (uint8_t)(1 << (channel - 1)) : 3,
        .status = active ? WIEGAND_USER_STATUS_ACTIVE : WIEGAND_USER_STATUS_PENDING,
        .alert = true,
        .alert_target = ALERT_TARGET_BOTH,
        .sequence = next_sequence(),
        .created_at_ms = current_time_ms(),
        .updated_at_ms = current_time_ms(),
    };
    strlcpy(user.code, code, sizeof(user.code));
    strlcpy(user.mode, "momentary", sizeof(user.mode));
    generate_id(user.id, sizeof(user.id));
    if (user_uuid) {
        strlcpy(user.user_uuid, user_uuid, sizeof(user.user_uuid));
    }
    if (name && name[0] != '\0') {
        strlcpy(user.name, name, sizeof(user.name));
    }
    assign_defaults(&user);

    s_users[s_user_count++] = user;
    sort_users();
    esp_err_t persist_err = persist_locked();
    if (persist_err != ESP_OK) {
        ssize_t rollback_idx = find_index_by_id(user.id);
        if (rollback_idx >= 0) {
            for (size_t i = (size_t)rollback_idx; i + 1 < s_user_count; i++) {
                s_users[i] = s_users[i + 1];
            }
            if (s_user_count > 0) {
                s_user_count--;
            }
        }
        ESP_LOGE(LOG_TAG_WIEGAND_REGISTRY,
                 "Rejected Wiegand user %s because persistence failed (%s)",
                 user.id,
                 esp_err_to_name(persist_err));
        xSemaphoreGive(s_mutex);
        return persist_err;
    }
    xSemaphoreGive(s_mutex);

    if (out_user) {
        *out_user = user;
    }
    return ESP_OK;
}

esp_err_t wiegand_registry_add(const char *code, uint8_t channel, wiegand_user_t *out_user) {
    return wiegand_registry_add_for_user(code, channel, NULL, NULL, false, out_user);
}

static esp_err_t update_user_locked(size_t idx, const wiegand_user_t *replacement) {
    if (idx >= s_user_count || !replacement) {
        return ESP_ERR_INVALID_ARG;
    }
    wiegand_user_t previous = s_users[idx];
    s_users[idx] = *replacement;
    s_users[idx].updated_at_ms = current_time_ms();
    esp_err_t err = persist_locked();
    if (err != ESP_OK) {
        s_users[idx] = previous;
    }
    return err;
}

esp_err_t wiegand_registry_update_name(const char *id, const char *name) {
    if (!id || !name) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_mutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ssize_t idx = find_index_by_id(id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    wiegand_user_t updated = s_users[idx];
    strlcpy(updated.name, name, sizeof(updated.name));
    assign_defaults(&updated);
    esp_err_t result = update_user_locked((size_t)idx, &updated);
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t wiegand_registry_update_config(const char *id, const char *name, const char *mode, uint8_t channel, uint8_t channel_mask, bool alert, int alert_target) {
    if (!id || !name || !mode || channel > 2 || channel_mask == 0 || channel_mask > 3) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(mode, "momentary") != 0 && strcmp(mode, "toggle") != 0 && strcmp(mode, "latch") != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_mutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ssize_t idx = find_index_by_id(id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    wiegand_user_t updated = s_users[idx];
    strlcpy(updated.name, name, sizeof(updated.name));
    strlcpy(updated.mode, mode, sizeof(updated.mode));
    updated.channel = channel;
    updated.channel_mask = channel_mask;
    updated.alert_target = alert_target_normalize(alert_target, alert);
    updated.alert = updated.alert_target != ALERT_TARGET_NONE;
    assign_defaults(&updated);
    esp_err_t result = update_user_locked((size_t)idx, &updated);
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t wiegand_registry_record_use(const char *id) {
    if (!id || id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_mutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ssize_t idx = find_index_by_id(id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    uint64_t used_ms = current_time_ms();
    wiegand_user_t updated = s_users[idx];
    updated.last_used_ms = used_ms;
    updated.last_used_unix_time = automation_unix_time_for_timestamp_ms(used_ms);
    updated.updated_at_ms = used_ms;
    esp_err_t result = update_user_locked((size_t)idx, &updated);
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t wiegand_registry_update_status(const char *id, wiegand_user_status_t status) {
    ensure_mutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ssize_t idx = find_index_by_id(id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    wiegand_user_t updated = s_users[idx];
    updated.status = status;
    assign_defaults(&updated);
    esp_err_t result = update_user_locked((size_t)idx, &updated);
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t wiegand_registry_remove(const char *id) {
    if (!id) return ESP_ERR_INVALID_ARG;
    ensure_mutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ssize_t idx = find_index_by_id(id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    for (size_t i = (size_t)idx; i + 1 < s_user_count; i++) {
        s_users[i] = s_users[i + 1];
    }
    if (s_user_count > 0) {
        s_user_count--;
    }
    esp_err_t result = persist_locked();
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t wiegand_registry_clear(void) {
    ensure_mutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_user_count = 0;
    esp_err_t result = persist_locked();
    xSemaphoreGive(s_mutex);
    return result;
}

cJSON *wiegand_registry_snapshot(void) {
    if (!s_initialised) {
        ESP_LOGE(LOG_TAG_WIEGAND_REGISTRY, "Registry not initialised");
        return NULL;
    }

    cJSON *array = cJSON_CreateArray();
    if (!array) {
        ESP_LOGE(LOG_TAG_WIEGAND_REGISTRY, "Failed to create snapshot array");
        return NULL;
    }

    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(LOG_TAG_WIEGAND_REGISTRY, "Timed out waiting for registry snapshot lock");
        cJSON_Delete(array);
        return NULL;
    }

    for (size_t i = 0; i < s_user_count; i++) {
        cJSON *obj = serialize_user(&s_users[i]);
        if (obj) {
            cJSON_AddItemToArray(array, obj);
        }
    }

    xSemaphoreGive(s_mutex);
    return array;
}

esp_err_t wiegand_registry_promote_all_pending(size_t *out_promoted) {
    ensure_mutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t promoted = 0;
    for (size_t i = 0; i < s_user_count; i++) {
        if (s_users[i].status == WIEGAND_USER_STATUS_PENDING) {
            s_users[i].status = WIEGAND_USER_STATUS_ACTIVE;
            assign_defaults(&s_users[i]);
            s_users[i].updated_at_ms = current_time_ms();
            promoted++;
        }
    }
    esp_err_t result = ESP_OK;
    if (promoted > 0) {
        result = persist_locked();
    }

    xSemaphoreGive(s_mutex);
    if (out_promoted) {
        *out_promoted = promoted;
    }
    return result;
}
