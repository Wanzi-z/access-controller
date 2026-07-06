#ifndef ENROLLMENT_H
#define ENROLLMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#define ENROLLMENT_USER_UUID_MAX 40
#define ENROLLMENT_USER_NAME_MAX 64

esp_err_t enrollment_start(const char *user_uuid);
esp_err_t enrollment_stop(void);
bool enrollment_is_active(void);
bool enrollment_on_wiegand(const char *code, int channel);
bool enrollment_on_pin(const char *pin, int channel);
bool enrollment_on_rf(uint32_t code, size_t pulse_count);
cJSON *enrollment_state_snapshot(void);

#endif /* ENROLLMENT_H */
