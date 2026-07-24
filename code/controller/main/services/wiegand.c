#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "wiegand_registry.h"
#include "wiegand_format.h"
#include "wiegand.h"
#include "automation.h"
#include "enrollment.h"
#include "schedule.h"

/* Forward declarations */
struct wiegand;
void start_wiegand_timer(struct wiegand *ctx, bool val);
void start_keypress_timer(struct wiegand *ctx, bool val);
static bool handleKeyCode(struct wiegand *ctx);
int is_pin_authorized(const char *incomingPin, pin_user_match_t *matched_user);
void arm_lock(int channel, bool arm, bool alert);
void beep_keypad(int beeps, int channel);

#define WIEGAND_FRAME_TIMEOUT_MS 25
#define WIEGAND_MIN_FRAME_BITS   24
#define WIEGAND_REPEAT_SUPPRESS_MS 1000
#define WIEGAND_MIN_PULSE_GAP_US 500
#define NUM_OF_WIEGANDS         2
#define NUM_OF_KEYS             12
#define KEYCODE_LENGTH          8
#define WIEGAND_SESSION_MAX     64

typedef struct {
    uint32_t gpio_num;
    int wg_index;
    int64_t time_us;
} gpio_event_t;

struct wiegand {
	uint8_t pin0;
	uint8_t pin1;
	uint8_t pin_push;
	char code[25];
	uint8_t bitCount;
	char name[100];
	int count;
	int keypressCount;
	bool expired;
	bool keypressExpired;
    // Some installations have DATA0/DATA1 effectively inverted for keypad frames
    // (e.g., wiring swap or reader variant). When detected, we invert 4-bit nibbles
    // before decoding so digits like '2' aren't treated as invalid.
    bool keypad_nibble_inverted;
	bool enable;
	bool newKey;
	bool newCode;
	char incomingCode[50];
	char fingerCode[50];
	uint8_t incomingCodeCount;
	int keyCount;
	bool alert;
	int delay;
	int keypressTimeout;
	int channel;
	int rearm_channel;
	bool rearm_alert;
	char bit_buffer[WIEGAND_USER_CODE_MAX];
	size_t bit_buffer_len;
	int64_t last_bit_time_us;
	char last_card_code[WIEGAND_USER_CODE_MAX];
	int64_t last_card_time_us;
	volatile uint32_t data0_pulse_count;
	volatile uint32_t data1_pulse_count;
	volatile uint32_t filtered_pulse_count;
	volatile uint32_t queue_drop_count;
	volatile int64_t last_raw_pulse_time_us;
	esp_err_t data0_isr_status;
	esp_err_t data1_isr_status;
	uint32_t valid_frame_count;
	uint32_t invalid_frame_count;
	uint32_t repeated_frame_count;
	uint8_t last_frame_bits;
	uint8_t last_frame_zero_bits;
	uint8_t last_frame_one_bits;
	bool last_frame_valid;
};

typedef struct {
    bool active;
    uint8_t channel;
    size_t added_count;
    char added_ids[WIEGAND_SESSION_MAX][WIEGAND_USER_ID_MAX];
    char last_duplicate[WIEGAND_USER_CODE_MAX];
} wiegand_registration_session_t;

static const char *LOG_TAG_WIEGAND = "wiegand";
static struct wiegand wg[NUM_OF_WIEGANDS];
static bool wiegand_card_toggle_state[NUM_OF_WIEGANDS] = {false, false};
static bool pin_user_toggle_state[NUM_OF_WIEGANDS] = {false, false};
static TimerHandle_t pin_user_momentary_timers[NUM_OF_WIEGANDS] = {0};
static TimerHandle_t pin_user_exit_timers[NUM_OF_WIEGANDS] = {0};
static QueueHandle_t gpio_evt_queue = NULL;
static portMUX_TYPE registrationMutex = portMUX_INITIALIZER_UNLOCKED;
static wiegand_registration_session_t registration_session = {0};

// True while user is actively typing a PIN (digits/*) and before # is pressed or timeout clears it.
bool wiegand_pin_entry_active(int channel) {
    if (channel < 1 || channel > NUM_OF_WIEGANDS) {
        return false;
    }
    struct wiegand *ctx = &wg[channel - 1];
    if (!ctx->enable) {
        return false;
    }
    return !ctx->keypressExpired && ctx->code[0] != '\0';
}

static void wiegand_bits_to_hex(const char *bit_string, size_t bit_len, char *hex_buf, size_t hex_buf_size) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t out = 0;
    uint8_t acc = 0;
    int acc_bits = 0;

    if (!bit_string || bit_len == 0 || !hex_buf || hex_buf_size < 2) {
        if (hex_buf && hex_buf_size > 0) hex_buf[0] = '\0';
        return;
    }

    for (size_t i = 0; i < bit_len && out < hex_buf_size - 1; i++) {
        acc = (acc << 1) | (bit_string[i] == '1');
        acc_bits++;
        if (acc_bits == 4) {
            hex_buf[out++] = HEX[acc & 0x0F];
            acc = 0;
            acc_bits = 0;
        }
    }
    if (acc_bits > 0 && out < hex_buf_size - 1) {
        acc <<= (4 - acc_bits);
        hex_buf[out++] = HEX[acc & 0x0F];
    }
    hex_buf[out] = '\0';
}

static void wiegand_log_frame_hex(const char *bit_string, size_t bit_len, int channel, bool short_frame) {
    if (!bit_string || bit_len == 0) {
        return;
    }
    (void)short_frame;  // No longer used for logging

    char hex_buf[WIEGAND_USER_CODE_MAX / 4 + 4];
    wiegand_bits_to_hex(bit_string, bit_len, hex_buf, sizeof(hex_buf));

    ESP_LOGI(LOG_TAG_WIEGAND, "Ch%d: 0x%s (%u bits)", channel, hex_buf, (unsigned)bit_len);
}

static int IRAM_ATTR wiegand_index_for_gpio(uint32_t gpio_num) {
    for (int i = 0; i < NUM_OF_WIEGANDS; i++) {
        if (wg[i].pin0 == gpio_num || wg[i].pin1 == gpio_num) {
            return i;
        }
    }
    return -1;
}

static bool registration_channel_matches(uint8_t configured_channel, int channel) {
    return configured_channel == 0 || configured_channel == (uint8_t)channel;
}

static void registration_clear_session_locked(void) {
    registration_session.active = false;
    registration_session.channel = 0;
    registration_session.added_count = 0;
    registration_session.last_duplicate[0] = '\0';
    memset(registration_session.added_ids, 0, sizeof(registration_session.added_ids));
}

static void registration_track_new_user(const wiegand_user_t *user) {
    if (!user) {
        return;
    }
    portENTER_CRITICAL(&registrationMutex);
    if (registration_session.added_count < WIEGAND_SESSION_MAX) {
        size_t index = registration_session.added_count;
        snprintf(registration_session.added_ids[index], WIEGAND_USER_ID_MAX, "%s", user->id);
        registration_session.added_count++;
    }
    registration_session.last_duplicate[0] = '\0';
    portEXIT_CRITICAL(&registrationMutex);
}

static void registration_note_duplicate(const char *code) {
    portENTER_CRITICAL(&registrationMutex);
    if (code && code[0] != '\0') {
        snprintf(registration_session.last_duplicate, sizeof(registration_session.last_duplicate), "%s", code);
    } else {
        registration_session.last_duplicate[0] = '\0';
    }
    portEXIT_CRITICAL(&registrationMutex);
}

static bool registration_snapshot(uint8_t *channel_out, size_t *pending_out) {
    bool active;
    portENTER_CRITICAL(&registrationMutex);
    active = registration_session.active;
    if (channel_out) {
        *channel_out = registration_session.channel;
    }
    if (pending_out) {
        *pending_out = registration_session.added_count;
    }
    portEXIT_CRITICAL(&registrationMutex);
    return active;
}

static size_t registration_collect_ids(char ids[][WIEGAND_USER_ID_MAX], size_t max_ids, uint8_t *channel_out, bool *was_active_out) {
    size_t count = 0;
    bool was_active = false;
    portENTER_CRITICAL(&registrationMutex);
    was_active = registration_session.active;
    if (channel_out) {
        *channel_out = registration_session.channel;
    }
    if (was_active && registration_session.added_count > 0) {
        count = registration_session.added_count;
        if (count > max_ids) {
            count = max_ids;
        }
        for (size_t i = 0; i < count; i++) {
            snprintf(ids[i], WIEGAND_USER_ID_MAX, "%s", registration_session.added_ids[i]);
        }
    }
    registration_clear_session_locked();
    portEXIT_CRITICAL(&registrationMutex);
    if (was_active_out) {
        *was_active_out = was_active;
    }
    return was_active ? count : 0;
}

static size_t registration_pending_count(void) {
    size_t count;
    portENTER_CRITICAL(&registrationMutex);
    count = registration_session.added_count;
    portEXIT_CRITICAL(&registrationMutex);
    return count;
}

static void wiegand_reset_bit_buffer(struct wiegand *ctx) {
    if (!ctx) {
        return;
    }
    ctx->bit_buffer_len = 0;
    ctx->bit_buffer[0] = '\0';
    ctx->last_bit_time_us = 0;
}

static bool wiegand_card_channel_matches(const wiegand_user_t *user, int reader_channel) {
    (void)reader_channel;
    // RFID credentials are controller-wide. `channel` records where a tag was
    // enrolled for diagnostics; it is not a reader-access restriction. PIN
    // credentials have an explicit keypad_mask when restriction is desired.
    return user != NULL;
}

static void wiegand_remember_polarity(struct wiegand *ctx, wiegand_code_polarity_t polarity)
{
    if (!ctx || polarity == WIEGAND_CODE_POLARITY_UNKNOWN) {
        return;
    }

    bool inverted = polarity == WIEGAND_CODE_POLARITY_INVERTED;
    if (ctx->keypad_nibble_inverted != inverted) {
        ctx->keypad_nibble_inverted = inverted;
        memset(ctx->code, 0, sizeof(ctx->code));
        start_keypress_timer(ctx, false);
        ESP_LOGI(LOG_TAG_WIEGAND,
                 "Channel %d DATA0/DATA1 polarity learned as %s; cleared partial PIN",
                 ctx->channel,
                 inverted ? "inverted" : "normal");
    }

    char key[24];
    snprintf(key, sizeof(key), "wiegand_%u_pol", (unsigned)ctx->channel);
    if (get_u32(key, WIEGAND_CODE_POLARITY_UNKNOWN) != (uint32_t)polarity) {
        store_u32(key, (uint32_t)polarity);
    }
}

static void pin_user_start_rearm_timer(TimerHandle_t *timer_slot, int channel, int seconds);

static int alert_channel_from_mask(int mask) {
    if (mask <= 0 || mask > 3) mask = 1;
    return mask == 3 ? 0 : ((mask & 1) ? 1 : 2);
}

static void apply_wiegand_card_action(struct wiegand *wg_entry,
                                      const wiegand_user_t *user,
                                      const char *bit_string) {
    if (!wg_entry || !user) {
        return;
    }

    if (!schedule_allows_access(user->user_uuid, (uint64_t)(esp_timer_get_time() / 1000LL))) {
        ESP_LOGI(LOG_TAG_WIEGAND, "Wiegand code %s denied by schedule (user=%s)",
                 bit_string, user->name[0] != '\0' ? user->name : "Wiegand User");
        return;
    }

    int channel_mask = user->channel_mask;
    if (channel_mask <= 0 || channel_mask > 3) {
        channel_mask = 1 << (wg_entry->channel - 1);
    }
    const char *mode = user->mode[0] != '\0' ? user->mode : "momentary";
    bool did_action = false;
    for (int bit = 0; bit < NUM_OF_WIEGANDS; bit++) {
        if ((channel_mask & (1 << bit)) == 0) continue;
        int action_channel = bit + 1;
        if (strcmp(mode, "toggle") == 0) {
            wiegand_card_toggle_state[bit] = !wiegand_card_toggle_state[bit];
            lock_set_action_source("wg_toggle");
            arm_lock(action_channel, wiegand_card_toggle_state[bit], false);
            ESP_LOGI(LOG_TAG_WIEGAND,
                     "Wiegand code %s toggle mode on channel %d -> %s",
                     bit_string,
                     action_channel,
                     wiegand_card_toggle_state[bit] ? "armed" : "disarmed");
        } else if (strcmp(mode, "latch") == 0) {
            lock_set_action_source("wg_latch");
            arm_lock(action_channel, false, false);
            ESP_LOGI(LOG_TAG_WIEGAND,
                     "Wiegand code %s latch mode disarmed channel %d",
                     bit_string,
                     action_channel);
        } else {
            lock_set_action_source("wg_code");
            arm_lock(action_channel, false, false);
            pin_user_start_rearm_timer(pin_user_momentary_timers, action_channel, wg_entry->delay);
        }
        did_action = true;
    }

    wiegand_registry_record_use(user->id);
    if (did_action && user->alert) {
        alert_output_signal(1, alert_channel_from_mask(channel_mask), user->alert_target);
    }
}

static void wiegand_process_code(struct wiegand *wg_entry, const char *bit_string) {
    if (!wg_entry || !bit_string || bit_string[0] == '\0') {
        return;
    }

    char normalized_code[WIEGAND_USER_CODE_MAX];
    wiegand_code_polarity_t polarity = WIEGAND_CODE_POLARITY_UNKNOWN;
    if (!wiegand_code_normalize(bit_string, normalized_code, sizeof(normalized_code), &polarity)) {
        wg_entry->invalid_frame_count++;
        wg_entry->last_frame_valid = false;
        ESP_LOGW(LOG_TAG_WIEGAND,
                 "Ignoring invalid Wiegand frame on channel %d (bits=%u, DATA0=%u, DATA1=%u)",
                 wg_entry->channel,
                 (unsigned)wg_entry->last_frame_bits,
                 (unsigned)wg_entry->last_frame_zero_bits,
                 (unsigned)wg_entry->last_frame_one_bits);
        return;
    }
    wiegand_remember_polarity(wg_entry, polarity);
    bit_string = normalized_code;

    int64_t now_us = esp_timer_get_time();
    if (wg_entry->last_card_code[0] != '\0' &&
        strcmp(wg_entry->last_card_code, bit_string) == 0 &&
        wg_entry->last_card_time_us > 0 &&
        now_us - wg_entry->last_card_time_us < (int64_t)WIEGAND_REPEAT_SUPPRESS_MS * 1000) {
        wg_entry->repeated_frame_count++;
        wg_entry->last_frame_valid = true;
        ESP_LOGI(LOG_TAG_WIEGAND,
                 "Ignoring repeated presentation on channel %d",
                 wg_entry->channel);
        return;
    }
    strlcpy(wg_entry->last_card_code, bit_string, sizeof(wg_entry->last_card_code));
    wg_entry->last_card_time_us = now_us;
    wg_entry->valid_frame_count++;
    wg_entry->last_frame_valid = true;

    size_t bit_len = strlen(bit_string);
    (void)bit_len;  // Used below in registration

    if (enrollment_on_wiegand(bit_string, wg_entry->channel)) {
        ESP_LOGI(LOG_TAG_WIEGAND, "Captured Wiegand code for unified enrollment on channel %d", wg_entry->channel);
        return;
    }

    uint8_t configured_channel = 0;
    size_t pending = 0;
    bool active = registration_snapshot(&configured_channel, &pending);
    // If registration mode is active, still allow ACTIVE users to unlock.
    // This prevents the system from feeling "broken" while enrolling new tags.
    const wiegand_user_t *existing = wiegand_registry_find_by_code(bit_string);
    if (active && existing &&
        existing->status == WIEGAND_USER_STATUS_ACTIVE &&
        wiegand_card_channel_matches(existing, wg_entry->channel)) {
        const char *display_name = (existing->name[0] != '\0') ? existing->name : "Wiegand User";
        ESP_LOGI(LOG_TAG_WIEGAND,
                 "Authorized Wiegand code %s (user=%s, mode=%s, lock_mask=0x%x, alert=%d) while registering",
                 bit_string,
                 display_name,
                 existing->mode[0] != '\0' ? existing->mode : "momentary",
                 existing->channel_mask,
                 existing->alert ? 1 : 0);
        apply_wiegand_card_action(wg_entry, existing, bit_string);
        return;
    }

    if (active && registration_channel_matches(configured_channel, wg_entry->channel)) {
        wiegand_user_t new_user;
        esp_err_t err = wiegand_registry_add(bit_string, wg_entry->channel, &new_user);
        if (err == ESP_OK) {
            if (new_user.status != WIEGAND_USER_STATUS_PENDING) {
                esp_err_t status_err = wiegand_registry_update_status(new_user.id, WIEGAND_USER_STATUS_PENDING);
                if (status_err == ESP_OK) {
                    new_user.status = WIEGAND_USER_STATUS_PENDING;
                } else {
                    ESP_LOGW(LOG_TAG_WIEGAND,
                             "Failed to mark Wiegand user %s as pending (%s)",
                             new_user.id,
                             esp_err_to_name(status_err));
                }
            }
            registration_track_new_user(&new_user);
            ESP_LOGI(LOG_TAG_WIEGAND,
                     "Captured Wiegand code %s on channel %d (user_id=%s, pending=%u)",
                     bit_string,
                     wg_entry->channel,
                     new_user.id,
                     (unsigned)(pending + 1));
        } else if (err == ESP_ERR_INVALID_STATE) {
            registration_note_duplicate(bit_string);
            ESP_LOGW(LOG_TAG_WIEGAND, "Duplicate Wiegand code %s on channel %d", bit_string, wg_entry->channel);
        } else {
            ESP_LOGE(LOG_TAG_WIEGAND, "Failed to record Wiegand code %s (%s)", bit_string, esp_err_to_name(err));
        }
        return;
    }
    if (active) {
        ESP_LOGD(LOG_TAG_WIEGAND,
                 "Registration active for channel %u but ignoring frame on channel %d",
                 (unsigned)configured_channel,
                 wg_entry->channel);
    }

    const wiegand_user_t *user = existing ? existing : wiegand_registry_find_by_code(bit_string);
    if (user &&
        user->status == WIEGAND_USER_STATUS_ACTIVE &&
        wiegand_card_channel_matches(user, wg_entry->channel)) {
        const char *display_name = (user->name[0] != '\0') ? user->name : "Wiegand User";
        ESP_LOGI(LOG_TAG_WIEGAND,
                 "Authorized Wiegand code %s (user=%s, mode=%s, lock_mask=0x%x, alert=%d)",
                 bit_string,
                 display_name,
                 user->mode[0] != '\0' ? user->mode : "momentary",
                 user->channel_mask,
                 user->alert ? 1 : 0);
        apply_wiegand_card_action(wg_entry, user, bit_string);
    } else {
        ESP_LOGW(LOG_TAG_WIEGAND, "Unauthorized Wiegand code %s on channel %d", bit_string, wg_entry->channel);
    }
}

static void IRAM_ATTR wiegand_isr_handler(void *arg) {
	gpio_event_t event;
    event.gpio_num = (uint32_t)arg;
    event.wg_index = wiegand_index_for_gpio(event.gpio_num);
	event.time_us = esp_timer_get_time();
	if (event.wg_index >= 0 && event.wg_index < NUM_OF_WIEGANDS) {
		if (event.gpio_num == wg[event.wg_index].pin0) {
			wg[event.wg_index].data0_pulse_count++;
		} else if (event.gpio_num == wg[event.wg_index].pin1) {
			wg[event.wg_index].data1_pulse_count++;
		}
		if (!wg[event.wg_index].enable) {
			return;
		}

		int64_t previous_us = wg[event.wg_index].last_raw_pulse_time_us;
		wg[event.wg_index].last_raw_pulse_time_us = event.time_us;
		if (previous_us > 0 &&
		    event.time_us - previous_us < WIEGAND_MIN_PULSE_GAP_US) {
			wg[event.wg_index].filtered_pulse_count++;
			return;
		}
	}
	
	if (xQueueSendFromISR(gpio_evt_queue, &event, NULL) != pdTRUE &&
	    event.wg_index >= 0 && event.wg_index < NUM_OF_WIEGANDS) {
		wg[event.wg_index].queue_drop_count++;
	}
}

void start_keypress_timer(struct wiegand *ctx, bool val) {
	if (val) {
        ctx->keypressExpired = false;
        ctx->keypressCount = 0;
	} else {
        ctx->keypressExpired = true;
	}
}

void check_keypress_timer(struct wiegand *ctx) {
    if (ctx->keypressCount >= ctx->keypressTimeout && !ctx->keypressExpired) {
        ESP_LOGW(LOG_TAG_WIEGAND, "Keypress timer expired for wg %d (clearing buffer, wait for # to submit)", ctx->channel);
        memset(ctx->code, 0, sizeof(ctx->code));
        memset(ctx->incomingCode, 0, sizeof(ctx->incomingCode));
        ctx->incomingCodeCount = 0;
        ctx->keypressExpired = true;
        ctx->keypressCount = 0;
        /* Do not error-beep on timeout; only reject when user presses # with wrong PIN */
    } else {
        ctx->keypressCount++;
    }
}

static void keypress_timer(void *pvParameter) {
  while (1) {
        for (int i = 0; i < NUM_OF_WIEGANDS; i++) {
		check_keypress_timer(&wg[i]);
        }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void start_wiegand_timer(struct wiegand *ctx, bool val) {
    start_keypress_timer(ctx, false);
  if (val) {
        ctx->rearm_channel = ctx->channel;
        ctx->rearm_alert = ctx->alert;
        ctx->expired = false;
        ctx->count = 0;
  } else {
        ctx->expired = true;
        ctx->incomingCodeCount = 0;
  }
}

void check_wiegand_timer(struct wiegand *ctx) {
    if (!ctx->enable) {
        return;
    }
    if (ctx->count >= ctx->delay && !ctx->expired) {
        int channel = (ctx->rearm_channel >= 1 && ctx->rearm_channel <= NUM_OF_WIEGANDS)
            ? ctx->rearm_channel
            : ctx->channel;
        ESP_LOGI(LOG_TAG_WIEGAND, "Re-arming lock from wg %d service on channel %d. Alert %d", ctx->channel, channel, ctx->rearm_alert);
        lock_set_action_source("wg_auto");
        arm_lock(channel, true, ctx->rearm_alert);
        ctx->expired = true;
    } else {
        ctx->count++;
    }
}

static void wiegand_timer(void *pvParameter) {
  while (1) {
        for (int i = 0; i < NUM_OF_WIEGANDS; i++) {
			check_wiegand_timer(&wg[i]);
        }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

static void pin_user_rearm_callback(TimerHandle_t timer) {
    intptr_t ch = (intptr_t)pvTimerGetTimerID(timer);
    int channel = (int)ch;
    if (channel < 1 || channel > NUM_OF_WIEGANDS) {
        return;
    }
    lock_set_action_source("wg_pin_auto");
    arm_lock(channel, true, false);
    ESP_LOGI(LOG_TAG_WIEGAND, "PIN user re-armed channel %d after timer", channel);
}

static void pin_user_start_rearm_timer(TimerHandle_t *timer_slot, int channel, int seconds) {
    if (channel < 1 || channel > NUM_OF_WIEGANDS) {
        return;
    }
    if (seconds <= 0) {
        seconds = 4;
    }
    int idx = channel - 1;
    if (!timer_slot[idx]) {
        timer_slot[idx] = xTimerCreate("pin_rearm",
                                       pdMS_TO_TICKS(seconds * 1000),
                                       pdFALSE,
                                       (void *)(intptr_t)channel,
                                       pin_user_rearm_callback);
    }
    if (timer_slot[idx]) {
        xTimerStop(timer_slot[idx], 0);
        xTimerChangePeriod(timer_slot[idx], pdMS_TO_TICKS(seconds * 1000), 0);
        xTimerStart(timer_slot[idx], 0);
    }
}

static bool apply_pin_user_action(const pin_user_match_t *user, int reader_channel) {
    if (!user) {
        return false;
    }
    if (reader_channel < 1 || reader_channel > NUM_OF_WIEGANDS) {
        return false;
    }
    if ((user->keypad_mask & (1 << (reader_channel - 1))) == 0) {
        ESP_LOGW(LOG_TAG_WIEGAND, "PIN user %s not allowed from keypad channel %d", user->name, reader_channel);
        return false;
    }

    bool did_action = false;
    for (int bit = 0; bit < NUM_OF_WIEGANDS; bit++) {
        if ((user->channel_mask & (1 << bit)) == 0) {
            continue;
        }
        int channel = bit + 1;

        if (strcmp(user->mode, "toggle") == 0) {
            pin_user_toggle_state[bit] = !pin_user_toggle_state[bit];
            lock_set_action_source("wg_pin_toggle");
            arm_lock(channel, pin_user_toggle_state[bit], false);
            ESP_LOGI(LOG_TAG_WIEGAND, "PIN user %s toggled channel %d -> %s",
                     user->name, channel, pin_user_toggle_state[bit] ? "armed" : "disarmed");
        } else if (strcmp(user->mode, "latch") == 0) {
            lock_set_action_source("wg_pin_latch");
            arm_lock(channel, false, false);
            ESP_LOGI(LOG_TAG_WIEGAND, "PIN user %s latched channel %d off", user->name, channel);
        } else if (strcmp(user->mode, "exit") == 0) {
            lock_set_action_source("wg_pin_exit");
            arm_lock(channel, false, false);
            pin_user_start_rearm_timer(pin_user_exit_timers, channel, user->exit_seconds);
            ESP_LOGI(LOG_TAG_WIEGAND, "PIN user %s exit pulse on channel %d for %ds",
                     user->name, channel, user->exit_seconds);
        } else if (strcmp(user->mode, "power_on") == 0) {
            lock_set_action_source("wg_pin_on");
            arm_lock(channel, true, false);
            ESP_LOGI(LOG_TAG_WIEGAND, "PIN user %s powered channel %d on", user->name, channel);
        } else if (strcmp(user->mode, "power_off") == 0) {
            lock_set_action_source("wg_pin_off");
            arm_lock(channel, false, false);
            ESP_LOGI(LOG_TAG_WIEGAND, "PIN user %s powered channel %d off", user->name, channel);
        } else {
            lock_set_action_source("wg_pin");
            arm_lock(channel, false, false);
            pin_user_start_rearm_timer(pin_user_momentary_timers, channel, user->exit_seconds);
            ESP_LOGI(LOG_TAG_WIEGAND, "PIN user %s momentary unlock on channel %d for %ds",
                     user->name, channel, user->exit_seconds);
        }

        did_action = true;
    }

    if (did_action && user->alert) {
        alert_output_signal(1, reader_channel, user->alert_target);
    }

    char msg[160];
    snprintf(msg, sizeof(msg), "PIN %s mode=%s chmask=0x%x keypad=0x%x", user->name, user->mode, user->channel_mask, user->keypad_mask);
    automation_record_log(msg);
    return did_action;
}

static bool handleKeyCode(struct wiegand *ctx) {
    static const uint8_t key[NUM_OF_KEYS] = {
        0b00000000, 0b11000000, 0b00110000, 0b11110000,
        0b00001100, 0b11001100, 0b00111100, 0b11111100,
        0b00000011, 0b11000011, 0b00110011, 0b11110011
    };

    if (!ctx) {
        return false;
    }

    const size_t bit_len = strlen(ctx->incomingCode);
    int keyIndex = -1;

    if (bit_len == 8) {
        uint8_t incomingByte = (uint8_t)strtol(ctx->incomingCode, NULL, 2);
        for (int i = 0; i < NUM_OF_KEYS; i++) {
            if (incomingByte == key[i]) {
                keyIndex = i;
                break;
            }
        }
    } else if (bit_len == 4) {
        // This keypad uses inverted 4-bit encoding: nibble = 0xF - digit
        // Confirmed mapping from pressing 1,2,3,4,5,6,7,8,9,0,*,# in order:
        //   1→0xE  2→0xD  3→0xC  4→0xB  5→0xA
        //   6→0x9  7→0x8  8→0x7  9→0x6  0→0xF
        //   *→0x5  #→0x4
        uint8_t raw_nibble = (uint8_t)strtol(ctx->incomingCode, NULL, 2) & 0x0F;

        // Nibbles 0x0-0x3 exist only when DATA0/DATA1 are swapped; 0xC-0xF
        // exist only in normal polarity. Persist definitive observations so
        // PIN decoding stays consistent across reboots and both keypads.
        if (raw_nibble <= 0x03) {
            ESP_LOGW(LOG_TAG_WIEGAND,
                     "Ch%d: Detected inverted keypad nibble encoding (raw=0x%X)",
                     ctx->channel,
                     (unsigned)raw_nibble);
            wiegand_remember_polarity(ctx, WIEGAND_CODE_POLARITY_INVERTED);
        } else if (raw_nibble >= 0x0C) {
            wiegand_remember_polarity(ctx, WIEGAND_CODE_POLARITY_NORMAL);
        }

        uint8_t nibble = raw_nibble;
        if (ctx->keypad_nibble_inverted) {
            nibble = (uint8_t)(nibble ^ 0x0F);
        }

        uint8_t decoded = 0xF - nibble;  // inverted encoding
        if (decoded <= 9) {
            keyIndex = (int)decoded;       // digits 0-9
        } else if (decoded == 10) {
            keyIndex = 10;                 // '*'
        } else if (decoded == 11) {
            keyIndex = 11;                 // '#' (submit)
        } else {
            keyIndex = -1;                 // invalid (nibbles 0x0-0x3)
        }

        ESP_LOGI(LOG_TAG_WIEGAND,
                 "Keypad 4-bit frame on channel %d: bits=%s nibble=0x%X%s mapped=%d",
                 ctx->channel,
                 ctx->incomingCode,
                 (unsigned)raw_nibble,
                 ctx->keypad_nibble_inverted ? " (inverted)" : "",
                 keyIndex);
    } else {
        ESP_LOGD(LOG_TAG_WIEGAND, "Unexpected keypad frame length %u bits (%s) on channel %d",
                 (unsigned)bit_len, ctx->incomingCode, ctx->channel);
    }

    if (keyIndex != -1) {
        start_keypress_timer(ctx, true);
    }

    if (keyIndex >= 0 && keyIndex <= 9) {
        size_t len = strlen(ctx->code);
        if (len >= KEYCODE_LENGTH) {
            // Do not error-beep or reset; just ignore extra digits until # is pressed.
            ESP_LOGI(LOG_TAG_WIEGAND,
                     "Ignoring extra keypad digit '%d' on channel %d (PIN already %u chars)",
                     keyIndex,
                     ctx->channel,
                     (unsigned)KEYCODE_LENGTH);
        } else if (len + 1 < sizeof(ctx->code)) {
            ctx->code[len] = (char)('0' + keyIndex);
            ctx->code[len + 1] = '\0';
            ESP_LOGI(LOG_TAG_WIEGAND, "Keypad digit '%d' received on channel %d (PIN=%s)", keyIndex, ctx->channel, ctx->code);
        }
    } else if (keyIndex == 10) {
        size_t len = strlen(ctx->code);
        if (len >= KEYCODE_LENGTH) {
            ESP_LOGI(LOG_TAG_WIEGAND,
                     "Ignoring extra keypad '*' on channel %d (PIN already %u chars)",
                     ctx->channel,
                     (unsigned)KEYCODE_LENGTH);
        } else if (len + 1 < sizeof(ctx->code)) {
            ctx->code[len] = '*';
            ctx->code[len + 1] = '\0';
            ESP_LOGI(LOG_TAG_WIEGAND, "Keypad '*' received on channel %d", ctx->channel);
        }
    } else if (keyIndex == 11) {
        // Only attempt authorization if PIN is not empty
        if (strlen(ctx->code) > 0) {
            if (enrollment_on_pin(ctx->code, ctx->channel)) {
                beep_keypad(1, ctx->channel);
                ESP_LOGI(LOG_TAG_WIEGAND, "PIN captured for unified enrollment on channel %d", ctx->channel);
            } else {
                pin_user_match_t pin_user;
                if (is_pin_authorized(ctx->code, &pin_user)) {
                    if (apply_pin_user_action(&pin_user, ctx->channel)) {
                        ESP_LOGI(LOG_TAG_WIEGAND, "PIN accepted on reader channel %d (%s, user=%s)",
                                 ctx->channel, ctx->code, pin_user.name);
                    } else {
                        // No beep: rejected PINs stay silent (only success/enrollment-capture beep).
                        ESP_LOGW(LOG_TAG_WIEGAND, "PIN user %s rejected on keypad channel %d", pin_user.name, ctx->channel);
                    }
                } else {
                    // No beep: wrong/disabled/schedule-denied PINs stay silent.
                    ESP_LOGW(LOG_TAG_WIEGAND, "PIN rejected on channel %d (%s)", ctx->channel, ctx->code);
                }
            }
        } else {
            ESP_LOGD(LOG_TAG_WIEGAND, "Ignoring # key with empty PIN on channel %d", ctx->channel);
        }
        memset(ctx->code, 0, sizeof(ctx->code));
        start_keypress_timer(ctx, false);
        return true;
    }

    if (keyIndex == -1) {
        ESP_LOGW(LOG_TAG_WIEGAND,
                 "Unknown keypad code bits=%s (len=%u) on channel %d",
                 ctx->incomingCode,
                 (unsigned)strlen(ctx->incomingCode),
                 ctx->channel);
    }

    memset(ctx->incomingCode, 0, sizeof(ctx->incomingCode));
    ctx->incomingCodeCount = 0;
    return keyIndex != -1;
}

static void wiegand_finalize_frame(struct wiegand *ctx) {
    if (!ctx || ctx->bit_buffer_len == 0) {
        return;
    }

    ESP_LOGI(LOG_TAG_WIEGAND, "Ch%d: Frame complete - %u bits received",
             ctx->channel, (unsigned)ctx->bit_buffer_len);

    if (ctx->bit_buffer_len < WIEGAND_MIN_FRAME_BITS) {
        if (ctx->bit_buffer_len >= 4 && ctx->bit_buffer_len <= 8) {
            memset(ctx->incomingCode, 0, sizeof(ctx->incomingCode));
            memcpy(ctx->incomingCode, ctx->bit_buffer, ctx->bit_buffer_len);
            ctx->incomingCode[ctx->bit_buffer_len] = '\0';
            ctx->incomingCodeCount = (int)ctx->bit_buffer_len;
            handleKeyCode(ctx);
        } else {
            ESP_LOGD(LOG_TAG_WIEGAND, "Ch%d: Ignoring %u-bit short frame (not keypad)",
                     ctx->channel, (unsigned)ctx->bit_buffer_len);
        }
    } else {
        char captured_code[WIEGAND_USER_CODE_MAX];
        snprintf(captured_code, sizeof(captured_code), "%s", ctx->bit_buffer);
        size_t zero_bits = 0;
        size_t one_bits = 0;
        for (size_t bit_index = 0; bit_index < ctx->bit_buffer_len; bit_index++) {
            if (ctx->bit_buffer[bit_index] == '0') zero_bits++;
            else if (ctx->bit_buffer[bit_index] == '1') one_bits++;
        }
        ctx->last_frame_bits = (uint8_t)ctx->bit_buffer_len;
        ctx->last_frame_zero_bits = (uint8_t)zero_bits;
        ctx->last_frame_one_bits = (uint8_t)one_bits;
        wiegand_log_frame_hex(captured_code, ctx->bit_buffer_len, ctx->channel, false);
        wiegand_process_code(ctx, captured_code);
    }

    wiegand_reset_bit_buffer(ctx);
    ctx->incomingCodeCount = 0;
    memset(ctx->incomingCode, 0, sizeof(ctx->incomingCode));
}

static void wiegand_finalize_frames_before(int64_t reference_time_us) {
    for (int i = 0; i < NUM_OF_WIEGANDS; i++) {
        struct wiegand *ctx = &wg[i];
        if (!ctx->enable || ctx->bit_buffer_len == 0 || ctx->last_bit_time_us == 0) {
            continue;
        }
        if (reference_time_us - ctx->last_bit_time_us >
            (int64_t)WIEGAND_FRAME_TIMEOUT_MS * 1000) {
            wiegand_finalize_frame(ctx);
        }
    }
}

static void wiegand_task(void *pvParameter) {
    gpio_event_t event;
    const TickType_t wait_ticks = pdMS_TO_TICKS(WIEGAND_FRAME_TIMEOUT_MS);

    for (;;) {
        bool received = xQueueReceive(gpio_evt_queue, &event, wait_ticks) == pdTRUE;
        if (!received) {
            wiegand_finalize_frames_before(esp_timer_get_time());
            continue;
        }

        ESP_LOGD(LOG_TAG_WIEGAND,
                 "RAW GPIO falling edge: GPIO=%u, wg_index=%d",
                 (unsigned)event.gpio_num, event.wg_index);

        if (event.wg_index < 0 || event.wg_index >= NUM_OF_WIEGANDS) {
            ESP_LOGD(LOG_TAG_WIEGAND, "Unknown Wiegand index (%d) on GPIO %u",
                     event.wg_index, (unsigned)event.gpio_num);
            continue;
        }

        // Use the ISR timestamp as a queue-wide chronological watermark. This
        // preserves real frame gaps even when a noisy line has queued a burst.
        wiegand_finalize_frames_before(event.time_us);

        struct wiegand *current_wg = &wg[event.wg_index];
        if (!current_wg->enable) {
            continue;
        }

        char bit = '\0';
        if (event.gpio_num == current_wg->pin0) {
            bit = '0';
        } else if (event.gpio_num == current_wg->pin1) {
            bit = '1';
        } else {
            continue;
        }

        if (current_wg->bit_buffer_len + 1 >= sizeof(current_wg->bit_buffer)) {
            current_wg->invalid_frame_count++;
            current_wg->last_frame_valid = false;
            ESP_LOGW(LOG_TAG_WIEGAND, "Discarding overflowing frame on channel %d",
                     current_wg->channel);
            wiegand_reset_bit_buffer(current_wg);
            current_wg->incomingCodeCount = 0;
            memset(current_wg->incomingCode, 0, sizeof(current_wg->incomingCode));
            continue;
        }

        current_wg->bit_buffer[current_wg->bit_buffer_len++] = bit;
        current_wg->bit_buffer[current_wg->bit_buffer_len] = '\0';
        current_wg->last_bit_time_us = event.time_us;

        if (current_wg->bit_buffer_len == 1) {
            ESP_LOGI(LOG_TAG_WIEGAND, "Ch%d: Frame started (bit=%c)",
                     current_wg->channel, bit);
        }
        current_wg->incomingCodeCount++;
    }
}

void enableWiegand(int ch, bool val) {
    for (int i = 0; i < NUM_OF_WIEGANDS; i++) {
        if (wg[i].channel == ch) {
            wg[i].enable = val;
        }
    }
}

bool wiegand_is_enabled(uint8_t channel) {
    if (channel < 1 || channel > NUM_OF_WIEGANDS) {
        return false;
    }
    return wg[channel - 1].enable;
}

esp_err_t wiegand_set_enabled(uint8_t channel, bool enabled) {
    if (channel < 1 || channel > NUM_OF_WIEGANDS) {
        return ESP_ERR_INVALID_ARG;
    }
    wg[channel - 1].enable = enabled;
    char key[24];
    snprintf(key, sizeof(key), "wiegand_%u_enable", (unsigned)channel);
    set_bool(key, enabled);
    return ESP_OK;
}

static void restore_wiegand_settings(void) {
    for (uint8_t channel = 1; channel <= NUM_OF_WIEGANDS; channel++) {
        char key[24];
        snprintf(key, sizeof(key), "wiegand_%u_enable", (unsigned)channel);
        wg[channel - 1].enable = get_bool(key, true);
        snprintf(key, sizeof(key), "wiegand_%u_pol", (unsigned)channel);
        uint32_t polarity = get_u32(key, WIEGAND_CODE_POLARITY_UNKNOWN);
        wg[channel - 1].keypad_nibble_inverted = polarity == WIEGAND_CODE_POLARITY_INVERTED;
    }
}

static cJSON *wiegand_devices_snapshot(void) {
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        return NULL;
    }
    for (uint8_t i = 0; i < NUM_OF_WIEGANDS; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry) {
            continue;
        }
        cJSON_AddNumberToObject(entry, "channel", wg[i].channel);
        cJSON_AddStringToObject(entry, "name", wg[i].name);
        cJSON_AddBoolToObject(entry, "enable", wg[i].enable);
        cJSON_AddBoolToObject(entry, "dataInverted", wg[i].keypad_nibble_inverted);
        cJSON_AddNumberToObject(entry, "data0PulseCount", wg[i].data0_pulse_count);
        cJSON_AddNumberToObject(entry, "data1PulseCount", wg[i].data1_pulse_count);
        cJSON_AddNumberToObject(entry, "filteredPulseCount", wg[i].filtered_pulse_count);
        cJSON_AddNumberToObject(entry, "queueDropCount", wg[i].queue_drop_count);
        cJSON_AddNumberToObject(entry, "data0Level", gpio_get_level(wg[i].pin0));
        cJSON_AddNumberToObject(entry, "data1Level", gpio_get_level(wg[i].pin1));
        cJSON_AddBoolToObject(entry, "data0IsrReady", wg[i].data0_isr_status == ESP_OK);
        cJSON_AddBoolToObject(entry, "data1IsrReady", wg[i].data1_isr_status == ESP_OK);
        cJSON_AddNumberToObject(entry, "validFrameCount", wg[i].valid_frame_count);
        cJSON_AddNumberToObject(entry, "invalidFrameCount", wg[i].invalid_frame_count);
        cJSON_AddNumberToObject(entry, "repeatedFrameCount", wg[i].repeated_frame_count);
        cJSON_AddNumberToObject(entry, "lastFrameBits", wg[i].last_frame_bits);
        cJSON_AddNumberToObject(entry, "lastFrameData0Bits", wg[i].last_frame_zero_bits);
        cJSON_AddNumberToObject(entry, "lastFrameData1Bits", wg[i].last_frame_one_bits);
        cJSON_AddBoolToObject(entry, "lastFrameValid", wg[i].last_frame_valid);
        cJSON_AddItemToArray(array, entry);
    }
    return array;
}


void wiegand_main(void) {
	wg[0].pin0 = WG0_DATA0_IO;
	wg[0].pin1 = WG0_DATA1_IO;
	wg[0].pin_push = OPEN_IO_1;
	wg[0].delay = 4;
	wg[0].keypressTimeout = 10;  /* seconds before buffer clear; PIN only evaluated on # key */
	wg[0].channel = 1;
	wg[0].rearm_channel = wg[0].channel;
	wg[0].enable = true;
	wg[0].alert = true;
	wg[0].rearm_alert = wg[0].alert;
	wg[0].newKey = false;
	wg[0].keypressExpired = true;  // Start as expired to prevent false timeout at boot
	wg[0].keypressCount = 0;
	wg[0].expired = true;
	wg[0].count = 0;
    wg[0].keypad_nibble_inverted = false;
    memset(wg[0].incomingCode, 0, sizeof(wg[0].incomingCode));
    memset(wg[0].code, 0, sizeof(wg[0].code));
    wg[0].incomingCodeCount = 0;
    memset(wg[0].last_card_code, 0, sizeof(wg[0].last_card_code));
    wg[0].last_card_time_us = 0;
    wg[0].data0_pulse_count = 0;
    wg[0].data1_pulse_count = 0;
    wg[0].filtered_pulse_count = 0;
    wg[0].queue_drop_count = 0;
    wg[0].last_raw_pulse_time_us = 0;
    wg[0].data0_isr_status = ESP_ERR_INVALID_STATE;
    wg[0].data1_isr_status = ESP_ERR_INVALID_STATE;
    wg[0].valid_frame_count = 0;
    wg[0].invalid_frame_count = 0;
    wg[0].repeated_frame_count = 0;
    wg[0].last_frame_bits = 0;
    wg[0].last_frame_zero_bits = 0;
    wg[0].last_frame_one_bits = 0;
    wg[0].last_frame_valid = false;
    wiegand_reset_bit_buffer(&wg[0]);
	strcpy(wg[0].name, "Wiegand0");

	wg[1].pin0 = WG1_DATA0_IO;
	wg[1].pin1 = WG1_DATA1_IO;
	wg[1].pin_push = OPEN_IO_1;
	wg[1].delay = 4;
	wg[1].keypressTimeout = 10;  /* seconds before buffer clear; PIN only evaluated on # key */
	wg[1].channel = 2;
	wg[1].rearm_channel = wg[1].channel;
	wg[1].enable = true;
	wg[1].alert = true;
	wg[1].rearm_alert = wg[1].alert;
	wg[1].newKey = false;
	wg[1].keypressExpired = true;  // Start as expired to prevent false timeout at boot
	wg[1].keypressCount = 0;
	wg[1].expired = true;
	wg[1].count = 0;
    wg[1].keypad_nibble_inverted = false;
    memset(wg[1].incomingCode, 0, sizeof(wg[1].incomingCode));
    memset(wg[1].code, 0, sizeof(wg[1].code));
    wg[1].incomingCodeCount = 0;
    memset(wg[1].last_card_code, 0, sizeof(wg[1].last_card_code));
    wg[1].last_card_time_us = 0;
    wg[1].data0_pulse_count = 0;
    wg[1].data1_pulse_count = 0;
    wg[1].filtered_pulse_count = 0;
    wg[1].queue_drop_count = 0;
    wg[1].last_raw_pulse_time_us = 0;
    wg[1].data0_isr_status = ESP_ERR_INVALID_STATE;
    wg[1].data1_isr_status = ESP_ERR_INVALID_STATE;
    wg[1].valid_frame_count = 0;
    wg[1].invalid_frame_count = 0;
    wg[1].repeated_frame_count = 0;
    wg[1].last_frame_bits = 0;
    wg[1].last_frame_zero_bits = 0;
    wg[1].last_frame_one_bits = 0;
    wg[1].last_frame_valid = false;
    wiegand_reset_bit_buffer(&wg[1]);
	strcpy(wg[1].name, "Wiegand1");

    restore_wiegand_settings();

    ESP_LOGI(LOG_TAG_WIEGAND, "Initializing Wiegand: WG0 (channel 1) GPIO%d/DATA0, GPIO%d/DATA1, enabled=%d",
             wg[0].pin0, wg[0].pin1, wg[0].enable);
    ESP_LOGI(LOG_TAG_WIEGAND, "Initializing Wiegand: WG1 (channel 2) GPIO%d/DATA0, GPIO%d/DATA1, enabled=%d",
             wg[1].pin0, wg[1].pin1, wg[1].enable);

    gpio_evt_queue = xQueueCreate(128, sizeof(gpio_event_t));

	xTaskCreate(wiegand_timer, "wigand_timer", 3072, NULL, 10, NULL);
	xTaskCreate(keypress_timer, "keypress_timer", 3072, NULL, 10, NULL);
    xTaskCreate(wiegand_task, "wiegand_task", 4096, NULL, 10, NULL);

    for (int i = 0; i < NUM_OF_WIEGANDS; i++) {
        ESP_LOGI(LOG_TAG_WIEGAND, "Registering ISR handlers for WG%d: GPIO%d and GPIO%d",
                 i, wg[i].pin0, wg[i].pin1);
		gpio_intr_disable(wg[i].pin0);
		gpio_intr_disable(wg[i].pin1);
		esp_err_t pin0_add_err = gpio_isr_handler_add(wg[i].pin0, wiegand_isr_handler, (void *)(uintptr_t)wg[i].pin0);
		esp_err_t pin1_add_err = gpio_isr_handler_add(wg[i].pin1, wiegand_isr_handler, (void *)(uintptr_t)wg[i].pin1);
		esp_err_t pin0_type_err = gpio_set_intr_type(wg[i].pin0, GPIO_INTR_NEGEDGE);
		esp_err_t pin1_type_err = gpio_set_intr_type(wg[i].pin1, GPIO_INTR_NEGEDGE);
		esp_err_t pin0_enable_err = (pin0_add_err == ESP_OK && pin0_type_err == ESP_OK)
		    ? gpio_intr_enable(wg[i].pin0)
		    : ESP_ERR_INVALID_STATE;
		esp_err_t pin1_enable_err = (pin1_add_err == ESP_OK && pin1_type_err == ESP_OK)
		    ? gpio_intr_enable(wg[i].pin1)
		    : ESP_ERR_INVALID_STATE;
		wg[i].data0_isr_status = pin0_add_err != ESP_OK ? pin0_add_err
		    : (pin0_type_err != ESP_OK ? pin0_type_err : pin0_enable_err);
		wg[i].data1_isr_status = pin1_add_err != ESP_OK ? pin1_add_err
		    : (pin1_type_err != ESP_OK ? pin1_type_err : pin1_enable_err);
		if (wg[i].data0_isr_status != ESP_OK || wg[i].data1_isr_status != ESP_OK) {
			ESP_LOGE(LOG_TAG_WIEGAND,
			         "Failed to initialize Wiegand interrupts for channel %d (DATA0=%s, DATA1=%s)",
			         wg[i].channel,
			         esp_err_to_name(wg[i].data0_isr_status),
			         esp_err_to_name(wg[i].data1_isr_status));
		}
	}
}

esp_err_t wiegand_registration_start(uint8_t channel) {
    if (channel > NUM_OF_WIEGANDS) {
        channel = 0;
    }

    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&registrationMutex);
    if (registration_session.active) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        registration_clear_session_locked();
        registration_session.active = true;
        registration_session.channel = channel;
    }
    portEXIT_CRITICAL(&registrationMutex);

    if (result == ESP_OK) {
        ESP_LOGI(LOG_TAG_WIEGAND, "Wiegand registration started (channel=%u)", (unsigned)channel);
    }
    return result;
}

esp_err_t wiegand_registration_stop(bool promote_pending) {
    char ids[WIEGAND_SESSION_MAX][WIEGAND_USER_ID_MAX];
    memset(ids, 0, sizeof(ids));
    uint8_t channel = 0;
    bool was_active = false;

    size_t captured = registration_collect_ids(ids, WIEGAND_SESSION_MAX, &channel, &was_active);
    if (!was_active) {
        return ESP_ERR_INVALID_STATE;
    }

    if (promote_pending) {
        for (size_t i = 0; i < captured; i++) {
            if (ids[i][0] == '\0') {
                continue;
            }
            esp_err_t err = wiegand_registry_update_status(ids[i], WIEGAND_USER_STATUS_ACTIVE);
            if (err != ESP_OK) {
                ESP_LOGW(LOG_TAG_WIEGAND, "Failed to activate Wiegand user %s (%s)", ids[i], esp_err_to_name(err));
            }
        }

        // Also promote any previously-enrolled pending users. This fixes the common case where
        // a tag was captured earlier (or scanned as a duplicate during registration) and stayed pending.
        size_t promoted = 0;
        esp_err_t promote_err = wiegand_registry_promote_all_pending(&promoted);
        if (promote_err != ESP_OK) {
            ESP_LOGW(LOG_TAG_WIEGAND, "Failed to promote pending users (%s)", esp_err_to_name(promote_err));
        } else if (promoted > 0) {
            ESP_LOGI(LOG_TAG_WIEGAND, "Promoted %u pending Wiegand users to ACTIVE", (unsigned)promoted);
        }
    }

    ESP_LOGI(LOG_TAG_WIEGAND, "Wiegand registration stopped; %u new codes captured", (unsigned)captured);
    return ESP_OK;
}

bool wiegand_registration_is_active(void) {
    return registration_snapshot(NULL, NULL);
}

uint8_t wiegand_registration_channel(void) {
    uint8_t channel = 0;
    registration_snapshot(&channel, NULL);
    return channel;
}

const char *wiegand_registration_last_duplicate(void) {
    static char duplicate[WIEGAND_USER_CODE_MAX];
    portENTER_CRITICAL(&registrationMutex);
    snprintf(duplicate, sizeof(duplicate), "%s", registration_session.last_duplicate);
    portEXIT_CRITICAL(&registrationMutex);
    return duplicate;
}

size_t wiegand_registration_pending_count(void) {
    return registration_pending_count();
}

static cJSON *wiegand_pin_entries_snapshot(void) {
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        return NULL;
    }

    for (int i = 0; i < NUM_OF_WIEGANDS; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry) {
            continue;
        }
        bool active = wg[i].enable && !wg[i].keypressExpired && wg[i].code[0] != '\0';
        cJSON_AddNumberToObject(entry, "channel", wg[i].channel);
        cJSON_AddBoolToObject(entry, "active", active);
        cJSON_AddStringToObject(entry, "code", active ? wg[i].code : "");
        cJSON_AddNumberToObject(entry, "length", active ? (double)strlen(wg[i].code) : 0);
        cJSON_AddItemToArray(array, entry);
    }

    return array;
}

cJSON *wiegand_state_snapshot(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    bool active = wiegand_registration_is_active();
    uint8_t channel = wiegand_registration_channel();
    size_t pending = registration_pending_count();
    const char *duplicate = wiegand_registration_last_duplicate();

    cJSON_AddBoolToObject(root, "registrationActive", active);
    cJSON_AddNumberToObject(root, "registrationChannel", channel);
    cJSON_AddNumberToObject(root, "registrationPending", (double)pending);
    if (duplicate && duplicate[0] != '\0') {
        cJSON_AddStringToObject(root, "lastDuplicateCode", duplicate);
    }

    cJSON *users = wiegand_registry_snapshot();
    if (!users) {
        users = cJSON_CreateArray();
    }
    cJSON_AddItemToObject(root, "users", users);
    cJSON *pin_entries = wiegand_pin_entries_snapshot();
    cJSON_AddItemToObject(root, "pinEntries", pin_entries ? pin_entries : cJSON_CreateArray());
    cJSON *devices = wiegand_devices_snapshot();
    cJSON_AddItemToObject(root, "devices", devices ? devices : cJSON_CreateArray());
    return root;
}

cJSON *wiegand_state_summary_snapshot(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    bool active = wiegand_registration_is_active();
    uint8_t channel = wiegand_registration_channel();
    size_t pending = registration_pending_count();
    const char *duplicate = wiegand_registration_last_duplicate();

    cJSON_AddBoolToObject(root, "registrationActive", active);
    cJSON_AddNumberToObject(root, "registrationChannel", channel);
    cJSON_AddNumberToObject(root, "registrationPending", (double)pending);
    if (duplicate && duplicate[0] != '\0') {
        cJSON_AddStringToObject(root, "lastDuplicateCode", duplicate);
    }
    cJSON_AddBoolToObject(root, "summary", true);
    cJSON_AddNumberToObject(root, "userCount", (double)wiegand_registry_count());
    cJSON_AddItemToObject(root, "users", cJSON_CreateArray());
    cJSON *pin_entries = wiegand_pin_entries_snapshot();
    cJSON_AddItemToObject(root, "pinEntries", pin_entries ? pin_entries : cJSON_CreateArray());
    cJSON *devices = wiegand_devices_snapshot();
    cJSON_AddItemToObject(root, "devices", devices ? devices : cJSON_CreateArray());
    return root;
}
