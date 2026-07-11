#include <stdbool.h>
#include <string.h>

#define ALERT_TARGET_NONE       0
#define ALERT_TARGET_CONTROLLER 1
#define ALERT_TARGET_WG1        2
#define ALERT_TARGET_WG2        4
#define ALERT_TARGET_KEYPAD     (ALERT_TARGET_WG1 | ALERT_TARGET_WG2)
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
    if (strcmp(value, "keypad") == 0 || strcmp(value, "keypads") == 0 || strcmp(value, "wg") == 0) {
        return ALERT_TARGET_KEYPAD;
    }
    int mask = ALERT_TARGET_NONE;
    char buf[48];
    strlcpy(buf, value, sizeof(buf));
    char *save = NULL;
    char *token = strtok_r(buf, ",|+ ", &save);
    while (token) {
        if (strcmp(token, "controller") == 0 || strcmp(token, "buzzer") == 0) {
            mask |= ALERT_TARGET_CONTROLLER;
        } else if (strcmp(token, "wg1") == 0 || strcmp(token, "wiegand1") == 0 || strcmp(token, "keypad1") == 0) {
            mask |= ALERT_TARGET_WG1;
        } else if (strcmp(token, "wg2") == 0 || strcmp(token, "wiegand2") == 0 || strcmp(token, "keypad2") == 0) {
            mask |= ALERT_TARGET_WG2;
        }
        token = strtok_r(NULL, ",|+ ", &save);
    }
    if (mask != ALERT_TARGET_NONE) {
        return mask;
    }
    if (strcmp(value, "both") == 0) {
        return ALERT_TARGET_BOTH;
    }
    return alert_target_from_bool(fallback_alert);
}

const char *alert_target_to_string(int target) {
    static char buf[32];
    int normalized = alert_target_normalize(target, true);
    if (normalized != ALERT_TARGET_NONE &&
        normalized != ALERT_TARGET_CONTROLLER &&
        normalized != ALERT_TARGET_KEYPAD &&
        normalized != ALERT_TARGET_BOTH) {
        buf[0] = '\0';
        if (normalized & ALERT_TARGET_CONTROLLER) {
            strlcat(buf, "controller", sizeof(buf));
        }
        if (normalized & ALERT_TARGET_WG1) {
            if (buf[0]) strlcat(buf, ",", sizeof(buf));
            strlcat(buf, "wg1", sizeof(buf));
        }
        if (normalized & ALERT_TARGET_WG2) {
            if (buf[0]) strlcat(buf, ",", sizeof(buf));
            strlcat(buf, "wg2", sizeof(buf));
        }
        return buf;
    }
    switch (normalized) {
        case ALERT_TARGET_NONE: return "none";
        case ALERT_TARGET_CONTROLLER: return "controller";
        case ALERT_TARGET_KEYPAD: return "keypad";
        default: return "both";
    }
}
