#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "automation.h"
#include "ip_timezone.h"
#include "schedule.h"

static const char *SCHEDULE_TAG = "schedule";

static const char *PROFILES_FILE_PATH = "/spiffs/schedule_profiles.json";
static const char *PROFILES_TMP_FILE_PATH = "/spiffs/schedule_profiles.tmp";
static const char *ASSIGNMENTS_FILE_PATH = "/spiffs/schedule_assignments.json";
static const char *ASSIGNMENTS_TMP_FILE_PATH = "/spiffs/schedule_assignments.tmp";

// tm_wday order: 0=Sunday .. 6=Saturday.
static const char *SCHEDULE_DAY_KEYS[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };

static SemaphoreHandle_t s_schedule_mutex = NULL;
static cJSON *s_schedule_profiles = NULL;    // array, custom profiles only ("day"/"night"/"" are built-in constants)
static cJSON *s_schedule_assignments = NULL; // object, { "<user_uuid>": "<schedule_id>" }
static bool s_schedule_loaded = false;

static void ensure_schedule_mutex(void) {
    if (!s_schedule_mutex) {
        s_schedule_mutex = xSemaphoreCreateMutex();
    }
}

static esp_err_t schedule_read_file(const char *path, char **out_json) {
    *out_json = NULL;
    FILE *file = fopen(path, "r");
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

static esp_err_t schedule_write_file(const char *path, const char *tmp_path, const char *json) {
    FILE *file = fopen(tmp_path, "w");
    if (!file) {
        ESP_LOGE(SCHEDULE_TAG, "Failed to open %s for writing: errno=%d", tmp_path, errno);
        return ESP_FAIL;
    }
    size_t len = strlen(json);
    size_t written = fwrite(json, 1, len, file);
    int close_result = fclose(file);
    if (written != len || close_result != 0) {
        ESP_LOGE(SCHEDULE_TAG, "Failed to write %s (%u/%u bytes, close=%d)", tmp_path, (unsigned)written, (unsigned)len, close_result);
        remove(tmp_path);
        return ESP_FAIL;
    }
    remove(path);
    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(SCHEDULE_TAG, "Failed to replace %s: errno=%d", path, errno);
        remove(tmp_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void ensure_schedule_loaded_locked(void) {
    if (s_schedule_loaded) return;

    char *profiles_json = NULL;
    if (schedule_read_file(PROFILES_FILE_PATH, &profiles_json) == ESP_OK && profiles_json) {
        cJSON *parsed = cJSON_Parse(profiles_json);
        free(profiles_json);
        s_schedule_profiles = (parsed && cJSON_IsArray(parsed)) ? parsed : cJSON_CreateArray();
        if (parsed && !cJSON_IsArray(parsed)) cJSON_Delete(parsed);
    } else {
        s_schedule_profiles = cJSON_CreateArray();
    }

    char *assignments_json = NULL;
    if (schedule_read_file(ASSIGNMENTS_FILE_PATH, &assignments_json) == ESP_OK && assignments_json) {
        cJSON *parsed = cJSON_Parse(assignments_json);
        free(assignments_json);
        s_schedule_assignments = (parsed && cJSON_IsObject(parsed)) ? parsed : cJSON_CreateObject();
        if (parsed && !cJSON_IsObject(parsed)) cJSON_Delete(parsed);
    } else {
        s_schedule_assignments = cJSON_CreateObject();
    }

    s_schedule_loaded = true;
}

static esp_err_t schedule_persist_profiles_locked(void) {
    char *json = cJSON_PrintUnformatted(s_schedule_profiles);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err = schedule_write_file(PROFILES_FILE_PATH, PROFILES_TMP_FILE_PATH, json);
    free(json);
    return err;
}

static esp_err_t schedule_persist_assignments_locked(void) {
    char *json = cJSON_PrintUnformatted(s_schedule_assignments);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err = schedule_write_file(ASSIGNMENTS_FILE_PATH, ASSIGNMENTS_TMP_FILE_PATH, json);
    free(json);
    return err;
}

static void generate_schedule_id(char *buf, size_t len) {
    const char *hex = "0123456789abcdef";
    for (size_t i = 0; i + 1 < len; i++) {
        buf[i] = hex[esp_random() % 16];
    }
    buf[len - 1] = '\0';
}

static bool schedule_id_is_builtin(const char *schedule_id) {
    return schedule_id && (strcmp(schedule_id, "day") == 0 || strcmp(schedule_id, "night") == 0);
}

static cJSON *schedule_find_profile_locked(const char *id) {
    if (!id || id[0] == '\0') return NULL;
    int count = cJSON_GetArraySize(s_schedule_profiles);
    for (int i = 0; i < count; i++) {
        cJSON *profile = cJSON_GetArrayItem(s_schedule_profiles, i);
        cJSON *id_item = cJSON_GetObjectItemCaseSensitive(profile, "id");
        if (cJSON_IsString(id_item) && id_item->valuestring && strcmp(id_item->valuestring, id) == 0) {
            return profile;
        }
    }
    return NULL;
}

static cJSON *schedule_build_default_day(bool enabled, const char *start, const char *end) {
    cJSON *day = cJSON_CreateObject();
    cJSON_AddBoolToObject(day, "enabled", enabled);
    cJSON_AddStringToObject(day, "start", start);
    cJSON_AddStringToObject(day, "end", end);
    return day;
}

static cJSON *schedule_build_default_days(void) {
    cJSON *days = cJSON_CreateObject();
    for (int i = 0; i < 7; i++) {
        cJSON_AddItemToObject(days, SCHEDULE_DAY_KEYS[i], schedule_build_default_day(true, "09:00", "17:00"));
    }
    return days;
}

// Accepts a borrowed `days` object from a client request and returns a fresh, fully-populated
// (all 7 keys present, each with sane enabled/start/end) object regardless of what was passed in
// -- malformed or partial input degrades to sensible defaults per-day rather than being rejected,
// since this only controls a UI-facing schedule, not something that should 400 on a typo.
static cJSON *schedule_sanitize_days(const cJSON *days_in, const cJSON *fallback_days) {
    cJSON *days_out = cJSON_CreateObject();
    for (int i = 0; i < 7; i++) {
        const char *key = SCHEDULE_DAY_KEYS[i];
        cJSON *day_in = days_in ? cJSON_GetObjectItemCaseSensitive((cJSON *)days_in, key) : NULL;
        cJSON *day_fallback = fallback_days ? cJSON_GetObjectItemCaseSensitive((cJSON *)fallback_days, key) : NULL;

        bool enabled = true;
        const char *start = "09:00";
        const char *end = "17:00";

        cJSON *source = day_in ? day_in : day_fallback;
        if (source) {
            cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(source, "enabled");
            if (enabled_item) enabled = cJSON_IsTrue(enabled_item);
            cJSON *start_item = cJSON_GetObjectItemCaseSensitive(source, "start");
            if (cJSON_IsString(start_item) && start_item->valuestring && start_item->valuestring[0]) start = start_item->valuestring;
            cJSON *end_item = cJSON_GetObjectItemCaseSensitive(source, "end");
            if (cJSON_IsString(end_item) && end_item->valuestring && end_item->valuestring[0]) end = end_item->valuestring;
        }

        cJSON_AddItemToObject(days_out, key, schedule_build_default_day(enabled, start, end));
    }
    return days_out;
}

static int schedule_parse_hhmm_minutes(cJSON *item, int fallback_minutes) {
    if (!cJSON_IsString(item) || !item->valuestring) return fallback_minutes;
    int hour = 0, minute = 0;
    if (sscanf(item->valuestring, "%d:%d", &hour, &minute) != 2) return fallback_minutes;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return fallback_minutes;
    return hour * 60 + minute;
}

static bool schedule_time_in_window(int minutes_now, int start_min, int end_min) {
    if (start_min == end_min) return true; // degenerate window == no restriction within the day
    if (start_min < end_min) {
        return minutes_now >= start_min && minutes_now < end_min;
    }
    return minutes_now >= start_min || minutes_now < end_min; // wraps midnight (e.g. Night: 18:00-06:00)
}

cJSON *schedule_state_snapshot(void) {
    ensure_schedule_mutex();
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;

    if (s_schedule_mutex && xSemaphoreTake(s_schedule_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        ensure_schedule_loaded_locked();
        cJSON *profiles_copy = cJSON_Duplicate(s_schedule_profiles, true);
        cJSON *assignments_copy = cJSON_Duplicate(s_schedule_assignments, true);
        xSemaphoreGive(s_schedule_mutex);
        cJSON_AddItemToObject(result, "profiles", profiles_copy ? profiles_copy : cJSON_CreateArray());
        cJSON_AddItemToObject(result, "assignments", assignments_copy ? assignments_copy : cJSON_CreateObject());
    } else {
        ESP_LOGW(SCHEDULE_TAG, "Timed out reading schedule state");
        cJSON_AddItemToObject(result, "profiles", cJSON_CreateArray());
        cJSON_AddItemToObject(result, "assignments", cJSON_CreateObject());
    }
    cJSON_AddNumberToObject(result, "utc_offset_seconds", ip_timezone_offset_seconds());
    cJSON_AddBoolToObject(result, "utc_offset_resolved", ip_timezone_is_resolved());
    return result;
}

void schedule_init(void) {
    ensure_schedule_mutex();
    if (!s_schedule_mutex || xSemaphoreTake(s_schedule_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(SCHEDULE_TAG, "Failed to acquire mutex during init");
        return;
    }
    ensure_schedule_loaded_locked();
    int profile_count = cJSON_GetArraySize(s_schedule_profiles);
    int assignment_count = cJSON_GetArraySize(s_schedule_assignments);
    xSemaphoreGive(s_schedule_mutex);
    ESP_LOGI(SCHEDULE_TAG, "Loaded %d custom profile(s), %d user assignment(s)", profile_count, assignment_count);
}

esp_err_t schedule_profile_create(const char *name, cJSON **out_snapshot) {
    ensure_schedule_mutex();
    if (!s_schedule_mutex || xSemaphoreTake(s_schedule_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    ensure_schedule_loaded_locked();

    char id[17];
    generate_schedule_id(id, sizeof(id));

    char default_name[48];
    if (!name || name[0] == '\0') {
        snprintf(default_name, sizeof(default_name), "Profile %d", cJSON_GetArraySize(s_schedule_profiles) + 1);
        name = default_name;
    }

    cJSON *profile = cJSON_CreateObject();
    cJSON_AddStringToObject(profile, "id", id);
    cJSON_AddStringToObject(profile, "name", name);
    cJSON_AddItemToObject(profile, "days", schedule_build_default_days());
    cJSON_AddItemToArray(s_schedule_profiles, profile);

    esp_err_t err = schedule_persist_profiles_locked();
    xSemaphoreGive(s_schedule_mutex);
    // schedule_state_snapshot() acquires s_schedule_mutex itself (it's not a recursive mutex),
    // so it must only be called after we've released it -- otherwise this deadlocks until the
    // snapshot's own 2s timeout expires and it silently returns an empty/stale state.
    if (err == ESP_OK && out_snapshot) {
        *out_snapshot = schedule_state_snapshot();
    }
    return err;
}

esp_err_t schedule_profile_update(const char *id, const char *name, const cJSON *days, cJSON **out_snapshot) {
    if (!id || id[0] == '\0' || schedule_id_is_builtin(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_schedule_mutex();
    if (!s_schedule_mutex || xSemaphoreTake(s_schedule_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    ensure_schedule_loaded_locked();

    cJSON *profile = schedule_find_profile_locked(id);
    if (!profile) {
        xSemaphoreGive(s_schedule_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (name && name[0] != '\0') {
        cJSON_DeleteItemFromObject(profile, "name");
        cJSON_AddStringToObject(profile, "name", name);
    }

    cJSON *existing_days = cJSON_GetObjectItemCaseSensitive(profile, "days");
    cJSON *sanitized = schedule_sanitize_days(days, existing_days);
    cJSON_DeleteItemFromObject(profile, "days");
    cJSON_AddItemToObject(profile, "days", sanitized);

    esp_err_t err = schedule_persist_profiles_locked();
    xSemaphoreGive(s_schedule_mutex);
    if (err == ESP_OK && out_snapshot) {
        *out_snapshot = schedule_state_snapshot();
    }
    return err;
}

esp_err_t schedule_profile_delete(const char *id, cJSON **out_snapshot) {
    if (!id || id[0] == '\0' || schedule_id_is_builtin(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_schedule_mutex();
    if (!s_schedule_mutex || xSemaphoreTake(s_schedule_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    ensure_schedule_loaded_locked();

    int count = cJSON_GetArraySize(s_schedule_profiles);
    int found_index = -1;
    for (int i = 0; i < count; i++) {
        cJSON *profile = cJSON_GetArrayItem(s_schedule_profiles, i);
        cJSON *id_item = cJSON_GetObjectItemCaseSensitive(profile, "id");
        if (cJSON_IsString(id_item) && id_item->valuestring && strcmp(id_item->valuestring, id) == 0) {
            found_index = i;
            break;
        }
    }
    if (found_index < 0) {
        xSemaphoreGive(s_schedule_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    cJSON_DeleteItemFromArray(s_schedule_profiles, found_index);

    // Any user assigned to the deleted profile falls back to unrestricted access.
    bool assignments_changed = false;
    cJSON *entry = s_schedule_assignments->child;
    while (entry) {
        cJSON *next = entry->next;
        if (cJSON_IsString(entry) && entry->valuestring && strcmp(entry->valuestring, id) == 0) {
            char key[64];
            snprintf(key, sizeof(key), "%s", entry->string ? entry->string : "");
            cJSON_DeleteItemFromObject(s_schedule_assignments, key);
            assignments_changed = true;
        }
        entry = next;
    }

    esp_err_t err = schedule_persist_profiles_locked();
    if (err == ESP_OK && assignments_changed) {
        err = schedule_persist_assignments_locked();
    }
    xSemaphoreGive(s_schedule_mutex);
    if (err == ESP_OK && out_snapshot) {
        *out_snapshot = schedule_state_snapshot();
    }
    return err;
}

esp_err_t schedule_assign_user(const char *user_uuid, const char *schedule_id, cJSON **out_snapshot) {
    if (!user_uuid || user_uuid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_schedule_mutex();
    if (!s_schedule_mutex || xSemaphoreTake(s_schedule_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    ensure_schedule_loaded_locked();

    esp_err_t err = ESP_OK;
    if (!schedule_id || schedule_id[0] == '\0') {
        cJSON_DeleteItemFromObject(s_schedule_assignments, user_uuid);
    } else if (schedule_id_is_builtin(schedule_id) || schedule_find_profile_locked(schedule_id)) {
        cJSON_DeleteItemFromObject(s_schedule_assignments, user_uuid);
        cJSON_AddStringToObject(s_schedule_assignments, user_uuid, schedule_id);
    } else {
        err = ESP_ERR_INVALID_ARG;
    }

    if (err == ESP_OK) {
        err = schedule_persist_assignments_locked();
    }
    xSemaphoreGive(s_schedule_mutex);
    if (err == ESP_OK && out_snapshot) {
        *out_snapshot = schedule_state_snapshot();
    }
    return err;
}

bool schedule_allows_access(const char *user_uuid, uint64_t now_ms) {
    if (!user_uuid || user_uuid[0] == '\0') return true;

    ensure_schedule_mutex();
    if (!s_schedule_mutex || xSemaphoreTake(s_schedule_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(SCHEDULE_TAG, "Timed out checking schedule for %s; allowing access", user_uuid);
        return true;
    }
    ensure_schedule_loaded_locked();

    cJSON *assigned = cJSON_GetObjectItemCaseSensitive(s_schedule_assignments, user_uuid);
    if (!cJSON_IsString(assigned) || !assigned->valuestring || assigned->valuestring[0] == '\0') {
        xSemaphoreGive(s_schedule_mutex);
        return true; // no schedule assigned -> unrestricted, same as before this feature existed
    }
    char schedule_id[40];
    snprintf(schedule_id, sizeof(schedule_id), "%s", assigned->valuestring);

    // Every schedule_id (built-in or custom) resolves to a per-day enabled/start/end window, so
    // we need the current LOCAL weekday before we can look anything up -- resolve device time
    // first, then shift it by the IP-geolocated UTC offset. gmtime_r() on a pre-shifted timestamp
    // is the standard portable way to get a local wall-clock breakdown without needing a full
    // IANA timezone/DST database on the device -- ip_timezone.c already resolves the current
    // (DST-aware) offset for us via IP geolocation, so no local DST math is needed here.
    int64_t unix_time = automation_unix_time_for_timestamp_ms(now_ms);
    if (unix_time == 0) {
        xSemaphoreGive(s_schedule_mutex);
        return true; // clock not synced yet -> fail open
    }
    time_t local_t = (time_t)(unix_time + ip_timezone_offset_seconds());
    struct tm tm_local;
    gmtime_r(&local_t, &tm_local);
    int weekday = tm_local.tm_wday; // 0=Sunday..6=Saturday, matches SCHEDULE_DAY_KEYS order
    int minutes_now = tm_local.tm_hour * 60 + tm_local.tm_min;

    bool enabled;
    int start_min;
    int end_min;

    if (strcmp(schedule_id, "day") == 0) {
        enabled = true;
        start_min = 6 * 60;
        end_min = 18 * 60;
    } else if (strcmp(schedule_id, "night") == 0) {
        enabled = true;
        start_min = 18 * 60;
        end_min = 6 * 60;
    } else {
        cJSON *profile = schedule_find_profile_locked(schedule_id);
        cJSON *days = profile ? cJSON_GetObjectItemCaseSensitive(profile, "days") : NULL;
        cJSON *day = days ? cJSON_GetObjectItemCaseSensitive(days, SCHEDULE_DAY_KEYS[weekday]) : NULL;
        if (!day) {
            xSemaphoreGive(s_schedule_mutex);
            return true; // profile was deleted/renamed out from under this assignment -> fail open
        }
        enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(day, "enabled"));
        start_min = schedule_parse_hhmm_minutes(cJSON_GetObjectItemCaseSensitive(day, "start"), 0);
        end_min = schedule_parse_hhmm_minutes(cJSON_GetObjectItemCaseSensitive(day, "end"), 24 * 60);
    }

    xSemaphoreGive(s_schedule_mutex);

    if (!enabled) return false;
    return schedule_time_in_window(minutes_now, start_min, end_min);
}
