/**
 * Simple registry for 433MHz remote fobs (RF codes).
 * Supports registration mode, listing, renaming, and deleting stored codes.
 */
#ifndef RF_REGISTRY_H
#define RF_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

void rf_registry_init(void);
bool rf_registry_is_active(void);
esp_err_t rf_registration_start(void);
esp_err_t rf_registration_stop(void);
void rf_registry_on_code(uint32_t code, size_t pulse_count);
esp_err_t rf_registry_add_for_user(uint32_t code, size_t pulse_count, const char *user_uuid, const char *name);
esp_err_t rf_registry_update_name(const char *id, const char *name);
esp_err_t rf_registry_remove(const char *id);
esp_err_t rf_registry_clear(void);
cJSON *rf_state_snapshot(void);
bool rf_registry_handle_code(uint32_t code);
esp_err_t rf_registry_update_config(const char *id, const char *mode, int channel_mask, int exit_seconds, bool alert, bool enabled);

typedef struct {
    uint64_t received_ms;
    int64_t unix_time;
    size_t pulse_count;
    uint32_t quality_score;
    char quality_label[16];
    uint32_t short_us;
    uint32_t long_us;
    double jitter_percent;
    uint32_t repeat_count;
    uint32_t noise_percent;
    uint32_t noise_rate_per_second;
    uint32_t edge_rate_per_second;
    uint32_t decode_ok_count;
    uint32_t capture_count;
    uint32_t decode_success_rate_percent;
    uint32_t last_capture_pulses;
    uint32_t sync_count;
    bool had_sync;
} rf_rx_metrics_t;

void rf_registry_record_rx(uint32_t code, const rf_rx_metrics_t *metrics);

#endif /* RF_REGISTRY_H */
