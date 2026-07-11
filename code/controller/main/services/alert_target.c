#include <stdbool.h>
#include <string.h>

#define ALERT_TARGET_NONE       0
#define ALERT_TARGET_CONTROLLER 1
#define ALERT_TARGET_KEYPAD     2
#define ALERT_TARGET_BOTH       (ALERT_TARGET_CONTROLLER | ALERT_TARGET_KEYPAD)

static int alert_target_normalize(int target, bool fallback_alert) {
    if (target < ALERT_TARGET_NONE || target > ALERT_TARGET_BOTH) {
        return fallback_alert ? ALERT_TARGET_BOTH : ALERT_TARGET_NONE;
    }
    return target;
}

int alert_target_from_bool(bool alert) {
    return alert ? ALERT_TARGET_BOTH : ALERT_TARGET_NONE;
}

int alert_target_from_string(const char *value, bool fallback_alert) {
    if (!value || value[0] == '\0') {
        return alert_target_from_bool(fallback_alert);
    }
    if (strcmp(value, "none") == 0 || strcmp(value, "off") == 0) {
        return ALERT_TARGET_NONE;
    }
    if (strcmp(value, "controller") == 0 || strcmp(value, "buzzer") == 0) {
        return ALERT_TARGET_CONTROLLER;
    }
    if (strcmp(value, "keypad") == 0) {
        return ALERT_TARGET_KEYPAD;
    }
    if (strcmp(value, "both") == 0) {
        return ALERT_TARGET_BOTH;
    }
    return alert_target_from_bool(fallback_alert);
}

const char *alert_target_to_string(int target) {
    switch (alert_target_normalize(target, true)) {
        case ALERT_TARGET_NONE: return "none";
        case ALERT_TARGET_CONTROLLER: return "controller";
        case ALERT_TARGET_KEYPAD: return "keypad";
        default: return "both";
    }
}
