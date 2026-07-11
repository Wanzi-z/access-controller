#ifndef SCHEDULE_H
#define SCHEDULE_H

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

// Loads persisted state from SPIFFS (if any) and logs a startup summary. Safe to call once at boot;
// every other function here also lazy-loads on first use, so calling this is an optimization/log
// line, not a correctness requirement.
void schedule_init(void);

// Full state for the web UI: {"profiles":[{id,name,days:{sun:{enabled,start,end},...}}],
// "assignments":{"<user_uuid>":"<schedule_id>"}}. Caller owns the returned object (cJSON_Delete).
cJSON *schedule_state_snapshot(void);

// Create a new custom profile (default name/window if name is NULL/empty). Fills *out_snapshot
// with the full state (see schedule_state_snapshot) on success.
esp_err_t schedule_profile_create(const char *name, cJSON **out_snapshot);

// Update an existing custom profile's name and/or day windows. `days` is a borrowed cJSON object
// shaped like {"sun":{"enabled":true,"start":"06:00","end":"18:00"}, "mon":{...}, ..., "sat":{...}};
// pass NULL to leave the day windows unchanged (name-only rename).
esp_err_t schedule_profile_update(const char *id, const char *name, const cJSON *days, cJSON **out_snapshot);

// Delete a custom profile. Any user currently assigned to it falls back to unrestricted access.
esp_err_t schedule_profile_delete(const char *id, cJSON **out_snapshot);

// Assign a schedule to a user uuid. schedule_id may be "day", "night", a custom profile id, or ""
// (clears the assignment -> unrestricted). Returns ESP_ERR_INVALID_ARG if schedule_id names a
// custom profile that doesn't exist.
esp_err_t schedule_assign_user(const char *user_uuid, const char *schedule_id, cJSON **out_snapshot);

// The enforcement check: does this user have access right now? now_ms is a device-uptime
// millisecond timestamp (matching automation_unix_time_for_timestamp_ms's expected input).
// Fails OPEN (returns true) whenever: uuid is empty/unset, the uuid has no assignment, the
// assigned schedule_id no longer resolves to a profile, or the device clock isn't NTP-synced yet.
// Intentionally conservative: a schedule bug should never lock someone out of their own door.
bool schedule_allows_access(const char *user_uuid, uint64_t now_ms);

#endif // SCHEDULE_H
