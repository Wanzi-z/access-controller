/**
 * RF Receiver Driver for 433MHz RXB6 Module
 * 
 * Decodes EV1527/PT2262 protocol commonly used by 433MHz key fobs.
 * 
 * Protocol: 24-bit code transmitted as:
 *   - Sync: Long HIGH pulse (~11ms) followed by short LOW
 *   - Data: 24 bits, each bit is a HIGH+LOW pair
 *     - Bit 0: Short HIGH (~350us), Long LOW (~1100us)
 *     - Bit 1: Long HIGH (~1100us), Short LOW (~350us)
 *   - Ratio is approximately 3:1
 * 
 * The RXB6 outputs noise when idle - we filter this by requiring
 * a valid sync pulse before capturing data.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "cJSON.h"
#include "automation.h"
#include "enrollment.h"
#include "rf_registry.h"

/* GPIO pin for RF DATA input */
#define RF_DATA_GPIO        15

/*
 * The controller PCB routes the RXB6 DATA pin through an NPN inverter:
 * J22 pin 2 -> R83 -> Q4 base, Q4 collector -> DATA_IO/GPIO15 with R84 pull-up.
 * A HIGH pulse from the RF module is therefore seen as LOW on GPIO15.
 */
#define RF_SIGNAL_INVERTED  1
#define RF_SYNC_LEVEL       (RF_SIGNAL_INVERTED ? 0 : 1)

/* Timing thresholds (microseconds) */
#define RF_MIN_PULSE_US     80      /* Ignore noise spikes < 80us */
#define RF_MAX_PULSE_US     18000   /* Max reasonable pulse length */
#define RF_SYNC_MIN_US      9000    /* Min sync pulse length */
#define RF_SYNC_MAX_US      14000   /* Max sync pulse length */
#define RF_SILENCE_US       25000   /* Period of silence to detect signal end */
#define RF_DEBOUNCE_US      500000  /* 500ms debounce between same code */

/* Expected timing for EV1527/PT2262 (we will auto-tune per capture) */
#define RF_SHORT_MIN_US     180
#define RF_SHORT_MAX_US     750
#define RF_LONG_MIN_US      900
#define RF_LONG_MAX_US      1800
#define RF_RATIO_MIN        2.2f
#define RF_RATIO_MAX        4.5f
#define RF_MIN_VALID_PULSES 45
#define RF_MAX_VALID_PULSES 70

/* Capture buffer */
#define RF_CAPTURE_SIZE     256
#define RF_REPEAT_WINDOW_US 2000000
static const char *RF_TAG = "rf_receiver";

/* Pulse capture buffer - STATIC to avoid stack usage */
typedef struct {
    uint16_t duration_us;
    uint8_t level;
} rf_pulse_t;

static rf_pulse_t rf_pulses[RF_CAPTURE_SIZE];
static volatile size_t rf_pulse_count = 0;
static volatile int64_t rf_last_edge_us = 0;
static volatile bool rf_capturing = false;
static volatile bool rf_sync_detected = false;
static volatile bool rf_capture_ready = false;

/* Debounce */
static uint32_t rf_last_code = 0;
static int64_t rf_last_code_time_us = 0;

/* Diagnostics exposed through /api/rf and /api/state. */
static volatile uint32_t rf_diag_edge_count = 0;
static volatile uint32_t rf_diag_noise_count = 0;
static volatile uint32_t rf_diag_sync_count = 0;
static volatile uint32_t rf_diag_capture_count = 0;
static volatile uint32_t rf_diag_decode_ok_count = 0;
static volatile uint32_t rf_diag_decode_fail_count = 0;
static volatile uint32_t rf_diag_discard_count = 0;
static volatile uint32_t rf_diag_last_pulse_us = 0;
static volatile uint32_t rf_diag_last_capture_pulses = 0;
static volatile uint32_t rf_diag_last_code = 0;
static volatile uint32_t rf_diag_last_decode_start = 0;
static volatile uint32_t rf_diag_last_short_us = 0;
static volatile uint32_t rf_diag_last_long_us = 0;
static volatile uint32_t rf_diag_last_jitter_pct_x10 = 0;
static volatile int64_t rf_diag_last_decode_us = 0;
static volatile uint32_t rf_diag_last_repeat_count = 0;
static volatile uint8_t rf_diag_last_level = 0;
static volatile bool rf_diag_last_had_sync = false;
static volatile uint32_t rf_diag_over_max_count = 0;
static volatile uint32_t rf_diag_last_over_max_pulse_us = 0;
static volatile uint32_t rf_diag_sync_level_pulse_count = 0;
static volatile uint32_t rf_diag_other_level_pulse_count = 0;
static volatile uint32_t rf_diag_max_pulse_us = 0;
static volatile uint32_t rf_diag_bins[2][10] = {{0}};
static volatile uint32_t rf_diag_sample_count = 0;
static volatile uint32_t rf_diag_high_sample_count = 0;
static volatile uint32_t rf_diag_low_sample_count = 0;
static volatile uint8_t rf_diag_sample_level = 0;
static volatile esp_err_t rf_diag_gpio_reset_result = ESP_OK;
static volatile esp_err_t rf_diag_gpio_hold_dis_result = ESP_OK;
static volatile esp_err_t rf_diag_gpio_pull_mode_result = ESP_OK;
static volatile esp_err_t rf_diag_isr_service_result = ESP_OK;
static volatile esp_err_t rf_diag_isr_add_result = ESP_OK;

static uint32_t rf_sample_high_count(int samples, uint32_t delay_us)
{
    uint32_t high = 0;
    for (int i = 0; i < samples; i++) {
        high += gpio_get_level(RF_DATA_GPIO) ? 1U : 0U;
        if (delay_us > 0) {
            esp_rom_delay_us(delay_us);
        }
    }
    return high;
}

static int rf_diag_bin_for_pulse(uint32_t pulse_us)
{
    if (pulse_us < RF_MIN_PULSE_US) return 0;
    if (pulse_us < RF_SHORT_MIN_US) return 1;
    if (pulse_us <= RF_SHORT_MAX_US) return 2;
    if (pulse_us < RF_LONG_MIN_US) return 3;
    if (pulse_us <= RF_LONG_MAX_US) return 4;
    if (pulse_us < RF_SILENCE_US) return 5;
    if (pulse_us < RF_SYNC_MIN_US) return 6;
    if (pulse_us <= RF_SYNC_MAX_US) return 7;
    if (pulse_us <= RF_MAX_PULSE_US) return 8;
    return 9;
}

static inline void rf_diag_record_pulse(uint32_t pulse_us, uint8_t level)
{
    if (level <= 1) {
        rf_diag_bins[level][rf_diag_bin_for_pulse(pulse_us)]++;
    }
    if (pulse_us > rf_diag_max_pulse_us) {
        rf_diag_max_pulse_us = pulse_us;
    }
    if (pulse_us >= RF_SYNC_MIN_US && pulse_us <= RF_SYNC_MAX_US) {
        if (level == RF_SYNC_LEVEL) {
            rf_diag_sync_level_pulse_count++;
        } else {
            rf_diag_other_level_pulse_count++;
        }
    }
}

/**
 * GPIO ISR handler - capture edges with sync detection
 */
static void IRAM_ATTR rf_isr_handler(void *arg)
{
    int64_t now_us = esp_timer_get_time();
    uint8_t current_level = gpio_get_level(RF_DATA_GPIO);
    rf_diag_edge_count++;
    rf_diag_last_level = current_level;
    
    if (rf_last_edge_us > 0) {
        uint32_t pulse_us = (uint32_t)(now_us - rf_last_edge_us);
        uint8_t prev_level = !current_level;
        rf_diag_last_pulse_us = pulse_us;
        rf_diag_record_pulse(pulse_us, prev_level);
        
        /* Filter obvious noise */
        if (pulse_us < RF_MIN_PULSE_US || pulse_us > RF_MAX_PULSE_US) {
            rf_diag_noise_count++;
            if (pulse_us > RF_MAX_PULSE_US) {
                rf_diag_over_max_count++;
                rf_diag_last_over_max_pulse_us = pulse_us;
            }
            rf_last_edge_us = now_us;
            return;
        }
        
        if (prev_level == RF_SYNC_LEVEL &&
            pulse_us >= RF_SYNC_MIN_US && pulse_us <= RF_SYNC_MAX_US) {
            rf_diag_sync_count++;
            rf_sync_detected = true;
        }

        /*
         * RXB6 modules are noisy while idle. On this controller revision the
         * DATA path is inverted by Q4, so only the known sync polarity should
         * start a capture. Accepting either polarity lets idle noise drive
         * constant decode work and can starve HTTP/tunnel traffic.
         */
        bool is_sync_width = (pulse_us >= RF_SYNC_MIN_US && pulse_us <= RF_SYNC_MAX_US);
        bool is_frame_sync = is_sync_width && (prev_level == RF_SYNC_LEVEL);
        if (!rf_capturing && is_frame_sync) {
            rf_capturing = true;
            rf_sync_detected = true;
            rf_capture_ready = false;
            rf_pulse_count = 0;
            rf_pulses[rf_pulse_count].duration_us = (uint16_t)pulse_us;
            rf_pulses[rf_pulse_count].level = prev_level;
            rf_pulse_count++;
        }
        else if (rf_capturing && is_frame_sync && rf_pulse_count >= RF_MIN_VALID_PULSES) {
            rf_capture_ready = true;
        }
        else if (rf_capturing && rf_pulse_count < RF_CAPTURE_SIZE) {
            /* Capture data pulses after sync */
            rf_pulses[rf_pulse_count].duration_us = (pulse_us > 65535) ? 65535 : (uint16_t)pulse_us;
            rf_pulses[rf_pulse_count].level = prev_level;
            rf_pulse_count++;
        }
    }
    
    rf_last_edge_us = now_us;
}

/**
 * Check if a pulse duration is "short" (~300-500us)
 */
static inline bool is_short_pulse(uint16_t us, uint16_t short_us)
{
    /* Accept within +/-30% of measured short_us */
    uint16_t min = (uint16_t)(short_us * 7 / 10);
    uint16_t max = (uint16_t)(short_us * 13 / 10);
    return us >= min && us <= max;
}

static inline bool is_long_pulse(uint16_t us, uint16_t long_us)
{
    /* Accept within +/-30% of measured long_us */
    uint16_t min = (uint16_t)(long_us * 7 / 10);
    uint16_t max = (uint16_t)(long_us * 13 / 10);
    return us >= min && us <= max;
}

/**
 * Try to decode captured pulses as EV1527/PT2262 from a candidate data offset.
 * Returns 24-bit code or 0 if invalid
 */
typedef struct {
    uint32_t code;
    size_t start_index;
    uint16_t short_us;
    uint16_t long_us;
    uint16_t jitter_pct_x10;
} rf_decode_result_t;

static uint32_t rf_abs_diff_u32(uint32_t a, uint32_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

static uint32_t rf_min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

static uint32_t rf_quality_score(uint32_t *success_rate_pct,
                                 uint32_t *noise_pct,
                                 uint32_t *last_age_ms,
                                 uint32_t *edge_rate_hz,
                                 uint32_t *noise_rate_hz)
{
    int64_t now_us = esp_timer_get_time();
    uint32_t frames = rf_diag_decode_ok_count + rf_diag_decode_fail_count + rf_diag_discard_count;
    uint32_t success_pct = frames ? (rf_diag_decode_ok_count * 100U) / frames : 0;
    uint32_t noise = rf_diag_edge_count ? (rf_diag_noise_count * 100U) / rf_diag_edge_count : 0;
    uint32_t age_ms = rf_diag_last_decode_us > 0 ? (uint32_t)((now_us - rf_diag_last_decode_us) / 1000) : 0;
    uint32_t uptime_s = (uint32_t)(now_us / 1000000);
    if (uptime_s == 0) {
        uptime_s = 1;
    }

    if (success_rate_pct) *success_rate_pct = success_pct;
    if (noise_pct) *noise_pct = noise;
    if (last_age_ms) *last_age_ms = age_ms;
    if (edge_rate_hz) *edge_rate_hz = rf_diag_edge_count / uptime_s;
    if (noise_rate_hz) *noise_rate_hz = rf_diag_noise_count / uptime_s;

    if (rf_diag_last_code == 0 || rf_diag_decode_ok_count == 0) {
        return 0;
    }

    int32_t score = 30;
    score += (int32_t)(success_pct / 4U);
    score += (int32_t)(rf_min_u32(rf_diag_last_repeat_count, 5U) * 5U);

    uint32_t jitter_pct = rf_diag_last_jitter_pct_x10 / 10U;
    score += (int32_t)((jitter_pct >= 20U) ? 0U : (20U - jitter_pct));
    if (rf_diag_last_had_sync) {
        score += 10;
    }

    if (noise > 30U) {
        score -= (int32_t)rf_min_u32((noise - 30U) / 2U, 20U);
    }
    if (age_ms > 10000U) {
        score -= (int32_t)rf_min_u32((age_ms - 10000U) / 5000U, 20U);
    }

    if (score < 0) return 0;
    if (score > 100) return 100;
    return (uint32_t)score;
}

static const char *rf_quality_label(uint32_t score, uint32_t noise_pct)
{
    if (rf_diag_last_code == 0 || rf_diag_decode_ok_count == 0) return "No signal";
    if (score >= 85U) return "Excellent";
    if (score >= 70U) return "Good";
    if (score >= 50U) return "Fair";
    if (noise_pct >= 60U) return "Noisy";
    return "Weak";
}

static void rf_record_code_metrics(uint32_t code, size_t count, int64_t now_us)
{
    uint32_t success_rate_pct = 0;
    uint32_t noise_pct = 0;
    uint32_t last_age_ms = 0;
    uint32_t edge_rate_hz = 0;
    uint32_t noise_rate_hz = 0;
    uint32_t quality_score = rf_quality_score(&success_rate_pct, &noise_pct, &last_age_ms,
                                              &edge_rate_hz, &noise_rate_hz);
    const char *quality_label = rf_quality_label(quality_score, noise_pct);

    rf_rx_metrics_t metrics = {
        .received_ms = (uint64_t)(now_us / 1000),
        .unix_time = automation_unix_time_for_timestamp_ms((uint64_t)(now_us / 1000)),
        .pulse_count = count,
        .quality_score = quality_score,
        .short_us = rf_diag_last_short_us,
        .long_us = rf_diag_last_long_us,
        .jitter_percent = (double)rf_diag_last_jitter_pct_x10 / 10.0,
        .repeat_count = rf_diag_last_repeat_count,
        .noise_percent = noise_pct,
        .noise_rate_per_second = noise_rate_hz,
        .edge_rate_per_second = edge_rate_hz,
        .decode_ok_count = rf_diag_decode_ok_count,
        .capture_count = rf_diag_capture_count,
        .decode_success_rate_percent = success_rate_pct,
        .last_capture_pulses = rf_diag_last_capture_pulses,
        .sync_count = rf_diag_sync_count,
        .had_sync = rf_diag_last_had_sync,
    };
    strlcpy(metrics.quality_label, quality_label, sizeof(metrics.quality_label));
    rf_registry_record_rx(code, &metrics);
}

static bool rf_try_decode_window(size_t count, size_t start_index, rf_decode_result_t *result)
{
    if (count < start_index + 48) {
        return false;
    }

    uint16_t min_us = UINT16_MAX;
    uint16_t max_us = 0;

    for (size_t i = start_index; i < start_index + 48; i++) {
        uint16_t d = rf_pulses[i].duration_us;
        if (d < RF_SHORT_MIN_US || d > RF_LONG_MAX_US) {
            return false;
        }
        if (d < min_us) min_us = d;
        if (d > max_us) max_us = d;
    }

    if (min_us == 0 || max_us < min_us) {
        return false;
    }

    float initial_ratio = (float)max_us / (float)min_us;
    if (initial_ratio < (RF_RATIO_MIN - 0.4f) || initial_ratio > (RF_RATIO_MAX + 1.0f)) {
        return false;
    }

    uint16_t midpoint = (uint16_t)((min_us + max_us) / 2);
    uint32_t short_sum = 0;
    uint32_t long_sum = 0;
    int short_cnt = 0;
    int long_cnt = 0;

    for (size_t i = start_index; i < start_index + 48; i++) {
        uint16_t d = rf_pulses[i].duration_us;
        if (d <= midpoint) {
            short_sum += d;
            short_cnt++;
        } else {
            long_sum += d;
            long_cnt++;
        }
    }

    if (short_cnt < 12 || long_cnt < 12) {
        return false;
    }

    uint16_t short_us = (uint16_t)(short_sum / short_cnt);
    uint16_t long_us = (uint16_t)(long_sum / long_cnt);
    float ratio = (float)long_us / (float)short_us;
    if (ratio < (RF_RATIO_MIN - 0.4f) || ratio > (RF_RATIO_MAX + 1.0f)) {
        return false;
    }

    uint32_t code = 0;
    uint32_t error_pct_x10_sum = 0;
    for (size_t i = start_index; i < start_index + 48; i += 2) {
        uint16_t first_us = rf_pulses[i].duration_us;
        uint16_t second_us = rf_pulses[i + 1].duration_us;

        bool first_short = is_short_pulse(first_us, short_us);
        bool first_long = is_long_pulse(first_us, long_us);
        bool second_short = is_short_pulse(second_us, short_us);
        bool second_long = is_long_pulse(second_us, long_us);

        int bit = -1;
        if (first_short && second_long) {
            bit = 0;
            error_pct_x10_sum += (rf_abs_diff_u32(first_us, short_us) * 1000U) / short_us;
            error_pct_x10_sum += (rf_abs_diff_u32(second_us, long_us) * 1000U) / long_us;
        } else if (first_long && second_short) {
            bit = 1;
            error_pct_x10_sum += (rf_abs_diff_u32(first_us, long_us) * 1000U) / long_us;
            error_pct_x10_sum += (rf_abs_diff_u32(second_us, short_us) * 1000U) / short_us;
        }

        if (bit < 0) {
            return false;
        }

        code = (code << 1) | (uint32_t)bit;
    }

    if (code == 0 || code == 0xFFFFFF) {
        return false;
    }

    if (result) {
        result->code = code;
        result->start_index = start_index;
        result->short_us = short_us;
        result->long_us = long_us;
        result->jitter_pct_x10 = (uint16_t)(error_pct_x10_sum / 48U);
    }
    return true;
}

static uint32_t rf_try_decode_scan(size_t count, uint32_t *decode_start)
{
    rf_decode_result_t result = {0};

    for (size_t start = 0; start + 48 <= count; start++) {
        if (rf_try_decode_window(count, start, &result)) {
            if (decode_start) *decode_start = (uint32_t)result.start_index;
            rf_diag_last_short_us = result.short_us;
            rf_diag_last_long_us = result.long_us;
            rf_diag_last_jitter_pct_x10 = result.jitter_pct_x10;
            return result.code;
        }
    }

    return 0;
}

static uint32_t rf_try_decode_from(size_t count, size_t start_index)
{
    /* Need sync + at least 48 pulses (24 bit pairs) */
    if (count < start_index + 48) {
        return 0;
    }
    
    /* First pulse should be sync (already validated in ISR, but double-check) */
    if (rf_pulses[0].level != RF_SYNC_LEVEL ||
        rf_pulses[0].duration_us < RF_SYNC_MIN_US ||
        rf_pulses[0].duration_us > RF_SYNC_MAX_US) {
        return 0;
    }

    /* Estimate short/long from the following pulses */
    uint32_t short_sum = 0, long_sum = 0;
    int short_cnt = 0, long_cnt = 0;

    /* Use up to the first 60 pulses after sync to estimate */
    size_t max_estimate = (count > start_index + 60) ? start_index + 60 : count;
    for (size_t i = start_index; i < max_estimate; i++) {
        uint16_t d = rf_pulses[i].duration_us;
        if (d >= RF_SHORT_MIN_US && d <= RF_SHORT_MAX_US) {
            short_sum += d;
            short_cnt++;
        } else if (d >= RF_LONG_MIN_US && d <= RF_LONG_MAX_US) {
            long_sum += d;
            long_cnt++;
        }
    }

    if (short_cnt < 5 || long_cnt < 5) {
        /* Not enough data to estimate */
        return 0;
    }

    uint16_t short_us = (uint16_t)(short_sum / short_cnt);
    uint16_t long_us  = (uint16_t)(long_sum  / long_cnt);
    rf_diag_last_short_us = short_us;
    rf_diag_last_long_us = long_us;

    /* Validate ratio */
    float ratio = (float)long_us / (float)short_us;
    if (ratio < RF_RATIO_MIN || ratio > RF_RATIO_MAX) {
        return 0;
    }

    /* Decode 24 bits starting after sync/gap. */
    uint32_t code = 0;
    uint32_t error_pct_x10_sum = 0;
    int bits = 0;
    int errors = 0;
    for (size_t i = start_index; i + 1 < count && bits < 24; i += 2) {
        uint16_t first_us = rf_pulses[i].duration_us;
        uint16_t second_us = rf_pulses[i + 1].duration_us;

        int bit = -1;

        /* Bit 0: Short + Long */
        if (is_short_pulse(first_us, short_us) && is_long_pulse(second_us, long_us)) {
            bit = 0;
            error_pct_x10_sum += (rf_abs_diff_u32(first_us, short_us) * 1000U) / short_us;
            error_pct_x10_sum += (rf_abs_diff_u32(second_us, long_us) * 1000U) / long_us;
        }
        /* Bit 1: Long + Short */
        else if (is_long_pulse(first_us, long_us) && is_short_pulse(second_us, short_us)) {
            bit = 1;
            error_pct_x10_sum += (rf_abs_diff_u32(first_us, long_us) * 1000U) / long_us;
            error_pct_x10_sum += (rf_abs_diff_u32(second_us, short_us) * 1000U) / short_us;
        }
        /* Fallback: ratio-based check */
        else if (first_us < second_us && (float)second_us / (float)first_us > (RF_RATIO_MIN - 0.2f)) {
            bit = 0;
        }
        else if (first_us > second_us && (float)first_us / (float)second_us > (RF_RATIO_MIN - 0.2f)) {
            bit = 1;
        }

        if (bit >= 0) {
            code = (code << 1) | bit;
            bits++;
        } else {
            errors++;
            if (errors > 2) {
                return 0;
            }
        }
    }

    if (bits == 24) {
        rf_diag_last_jitter_pct_x10 = error_pct_x10_sum / 48U;
        return code;
    }
    return 0;
}

static uint32_t rf_try_decode(size_t count, uint32_t *decode_start)
{
    /*
     * Some receivers include the short post-sync gap in the captured stream,
     * while others present the first data pulse immediately. Try both offsets.
     */
    uint32_t code = rf_try_decode_from(count, 1);
    if (code != 0) {
        if (decode_start) *decode_start = 1;
        return code;
    }

    code = rf_try_decode_from(count, 2);
    if (code != 0) {
        if (decode_start) *decode_start = 2;
        return code;
    }

    code = rf_try_decode_scan(count, decode_start);
    if (code != 0) {
        return code;
    }

    if (decode_start) *decode_start = 0;
    return 0;
}

static void rf_process_capture(size_t count, bool had_sync, int64_t now_us)
{
    rf_diag_capture_count++;
    rf_diag_last_capture_pulses = (uint32_t)count;
    rf_diag_last_had_sync = had_sync;

    /*
     * Only process complete, single EV1527/PT2262-sized captures. Letting the
     * decoder scan a full noise buffer can synthesize a 24-bit code from
     * unrelated GPIO activity, which shows up as phantom remote fobs.
     */
    if (had_sync && count >= RF_MIN_VALID_PULSES && count <= RF_MAX_VALID_PULSES) {
        uint32_t decode_start = 0;
        uint32_t code = rf_try_decode(count, &decode_start);

        if (code != 0) {
            rf_diag_decode_ok_count++;
            if (code == rf_diag_last_code &&
                rf_diag_last_decode_us > 0 &&
                (now_us - rf_diag_last_decode_us) <= RF_REPEAT_WINDOW_US) {
                rf_diag_last_repeat_count++;
            } else {
                rf_diag_last_repeat_count = 1;
            }
            rf_diag_last_code = code;
            rf_diag_last_decode_start = decode_start;
            rf_diag_last_decode_us = now_us;
            rf_record_code_metrics(code, count, now_us);
            /* Debounce: ignore if same code within 500ms */
            bool is_duplicate = (code == rf_last_code) &&
                                ((now_us - rf_last_code_time_us) < RF_DEBOUNCE_US);

            if (!is_duplicate) {
                ESP_LOGI(RF_TAG,
                         "RF code received 0x%06lX pulses=%d short=%luus long=%luus jitter=%lu.%lu%%",
                         (unsigned long)code,
                         (int)count,
                         (unsigned long)rf_diag_last_short_us,
                         (unsigned long)rf_diag_last_long_us,
                         (unsigned long)(rf_diag_last_jitter_pct_x10 / 10U),
                         (unsigned long)(rf_diag_last_jitter_pct_x10 % 10U));

                rf_last_code = code;
                rf_last_code_time_us = now_us;

                /* Registration path vs live action */
                if (enrollment_on_rf(code, count)) {
                    rf_record_code_metrics(code, count, now_us);
                    ESP_LOGI(RF_TAG, "RF code captured for unified enrollment");
                } else if (rf_registry_is_active()) {
                    rf_registry_on_code(code, count);
                    rf_record_code_metrics(code, count, now_us);
                } else {
                    rf_registry_handle_code(code);
                }
            }
        } else {
            rf_diag_decode_fail_count++;
            ESP_LOGD(RF_TAG, "Decode failed (count=%d, sync=%d)", (int)count, had_sync);
        }
    } else {
        rf_diag_discard_count++;
        ESP_LOGD(RF_TAG, "Discard capture (sync=%d, count=%d)", had_sync, (int)count);
    }
}

/**
 * Process captured RF signals - runs in task context
 */
static void rf_process_task(void *pvParameter)
{
    ESP_LOGI(RF_TAG, "RF receiver task started on GPIO%d", RF_DATA_GPIO);
    ESP_LOGI(RF_TAG, "Sync: %d-%dus, Short: %d-%dus, Long: %d-%dus",
             RF_SYNC_MIN_US, RF_SYNC_MAX_US,
             RF_SHORT_MIN_US, RF_SHORT_MAX_US,
             RF_LONG_MIN_US, RF_LONG_MAX_US);
    
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15));
        
        int64_t now_us = esp_timer_get_time();
        uint8_t sample_level = gpio_get_level(RF_DATA_GPIO);
        rf_diag_sample_level = sample_level;
        rf_diag_sample_count++;
        if (sample_level) {
            rf_diag_high_sample_count++;
        } else {
            rf_diag_low_sample_count++;
        }
        
        /* Check for signal end (silence) */
        if (rf_capturing && rf_pulse_count > 0) {
            int64_t since_last = now_us - rf_last_edge_us;
            
            if (since_last > RF_SILENCE_US || rf_capture_ready) {
                /* Signal ended - process capture */
                size_t count = rf_pulse_count;
                bool had_sync = rf_sync_detected;
                
                /* Stop capturing */
                rf_capturing = false;
                rf_sync_detected = false;
                rf_capture_ready = false;
                rf_pulse_count = 0;
                rf_process_capture(count, had_sync, now_us);
            }
        }
        
        /* Process instead of dropping if idle noise fills the buffer. */
        if (rf_pulse_count >= RF_CAPTURE_SIZE - 2) {
            size_t count = rf_pulse_count;
            bool had_sync = rf_sync_detected;
            rf_capturing = false;
            rf_sync_detected = false;
            rf_capture_ready = false;
            rf_pulse_count = 0;
            rf_process_capture(count, had_sync, now_us);
        }
    }
}

/**
 * Initialize the RF receiver
 */
void rf_receiver_init(void)
{
    ESP_LOGI(RF_TAG, "Initializing RF receiver on GPIO%d", RF_DATA_GPIO);

    /*
     * DATA_IO has a 10K external pull-up to 3V3 and Q4 pulls it down.
     * Reset the pad first so no prior boot/peripheral state can leave GPIO15
     * driving or held. Keep the ESP's weak internal pull-up enabled as a
     * fallback: if R84/continuity is weak or open, GPIO15 otherwise floats low
     * and no RF edges can be captured.
     */
    rf_diag_gpio_reset_result = gpio_reset_pin(RF_DATA_GPIO);
    rf_diag_gpio_hold_dis_result = gpio_hold_dis(RF_DATA_GPIO);
    rf_diag_gpio_pull_mode_result = gpio_set_pull_mode(RF_DATA_GPIO, GPIO_PULLUP_ONLY);
    
    /* Configure GPIO as input with pull-up fallback, interrupt on any edge. */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RF_DATA_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    
    ESP_LOGI(RF_TAG, "GPIO%d initial level: %d", RF_DATA_GPIO, gpio_get_level(RF_DATA_GPIO));
    
    /*
     * gpio_main() normally installs the ISR service before Wiegand and RF setup.
     * Calling this defensively makes RF robust if init ordering changes; an
     * already-installed service is fine.
     */
    esp_err_t service_err = gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    rf_diag_isr_service_result = service_err;
    if (service_err != ESP_OK && service_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(RF_TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(service_err));
        return;
    }

    /* Install ISR handler */
    esp_err_t isr_err = gpio_isr_handler_add(RF_DATA_GPIO, rf_isr_handler, NULL);
    rf_diag_isr_add_result = isr_err;
    if (isr_err != ESP_OK) {
        ESP_LOGE(RF_TAG, "Failed to add RF GPIO ISR handler on GPIO%d: %s",
                 RF_DATA_GPIO, esp_err_to_name(isr_err));
        return;
    }
    
    /* Start processing task */
    xTaskCreate(rf_process_task, "rf_rx", 8192, NULL, 3, NULL);
    
    ESP_LOGI(RF_TAG, "RF receiver ready - waiting for fob signals...");
}

/**
 * Get the last received RF code
 */
uint32_t rf_get_last_code(void)
{
    return rf_last_code;
}

cJSON *rf_receiver_diagnostics_snapshot(void)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }

    uint32_t success_rate_pct = 0;
    uint32_t noise_pct = 0;
    uint32_t last_age_ms = 0;
    uint32_t edge_rate_hz = 0;
    uint32_t noise_rate_hz = 0;
    uint32_t quality_score = rf_quality_score(&success_rate_pct, &noise_pct, &last_age_ms,
                                              &edge_rate_hz, &noise_rate_hz);
    const char *quality_label = rf_quality_label(quality_score, noise_pct);
    int64_t now_us = esp_timer_get_time();
    uint32_t last_edge_age_ms = 0;
    if (rf_last_edge_us > 0 && now_us >= rf_last_edge_us) {
        last_edge_age_ms = (uint32_t)((now_us - rf_last_edge_us) / 1000);
    }
    uint32_t sample_count = rf_diag_sample_count;
    uint32_t high_sample_pct = sample_count ? (rf_diag_high_sample_count * 100U) / sample_count : 0;

    cJSON_AddNumberToObject(obj, "gpio", RF_DATA_GPIO);
    cJSON_AddStringToObject(obj, "gpioResetResult", esp_err_to_name(rf_diag_gpio_reset_result));
    cJSON_AddStringToObject(obj, "gpioHoldDisResult", esp_err_to_name(rf_diag_gpio_hold_dis_result));
    cJSON_AddStringToObject(obj, "gpioPullModeResult", esp_err_to_name(rf_diag_gpio_pull_mode_result));
    cJSON_AddStringToObject(obj, "configuredPullMode", "internal-pullup-plus-external-10k-pullup");
    cJSON_AddStringToObject(obj, "isrServiceResult", esp_err_to_name(rf_diag_isr_service_result));
    cJSON_AddStringToObject(obj, "isrAddResult", esp_err_to_name(rf_diag_isr_add_result));
    cJSON_AddNumberToObject(obj, "qualityScore", quality_score);
    cJSON_AddStringToObject(obj, "qualityLabel", quality_label);
    cJSON_AddBoolToObject(obj, "signalInverted", RF_SIGNAL_INVERTED);
    cJSON_AddNumberToObject(obj, "syncLevel", RF_SYNC_LEVEL);
    cJSON_AddNumberToObject(obj, "currentLevel", gpio_get_level(RF_DATA_GPIO));
    cJSON_AddNumberToObject(obj, "sampleLevel", rf_diag_sample_level);
    cJSON_AddNumberToObject(obj, "sampleCount", sample_count);
    cJSON_AddNumberToObject(obj, "highSampleCount", rf_diag_high_sample_count);
    cJSON_AddNumberToObject(obj, "lowSampleCount", rf_diag_low_sample_count);
    cJSON_AddNumberToObject(obj, "highSamplePercent", high_sample_pct);
    cJSON_AddNumberToObject(obj, "lastLevel", rf_diag_last_level);
    cJSON_AddNumberToObject(obj, "edgeCount", rf_diag_edge_count);
    cJSON_AddNumberToObject(obj, "edgeRatePerSecond", edge_rate_hz);
    cJSON_AddNumberToObject(obj, "lastEdgeAgeMs", last_edge_age_ms);
    cJSON_AddNumberToObject(obj, "noiseCount", rf_diag_noise_count);
    cJSON_AddNumberToObject(obj, "noiseRatePerSecond", noise_rate_hz);
    cJSON_AddNumberToObject(obj, "noisePercent", noise_pct);
    cJSON_AddNumberToObject(obj, "syncCount", rf_diag_sync_count);
    cJSON_AddNumberToObject(obj, "captureCount", rf_diag_capture_count);
    cJSON_AddNumberToObject(obj, "decodeOkCount", rf_diag_decode_ok_count);
    cJSON_AddNumberToObject(obj, "decodeFailCount", rf_diag_decode_fail_count);
    cJSON_AddNumberToObject(obj, "decodeSuccessRatePercent", success_rate_pct);
    cJSON_AddNumberToObject(obj, "discardCount", rf_diag_discard_count);
    cJSON_AddNumberToObject(obj, "lastPulseUs", rf_diag_last_pulse_us);
    cJSON_AddNumberToObject(obj, "maxPulseUs", rf_diag_max_pulse_us);
    cJSON_AddNumberToObject(obj, "overMaxCount", rf_diag_over_max_count);
    cJSON_AddNumberToObject(obj, "lastOverMaxPulseUs", rf_diag_last_over_max_pulse_us);
    cJSON_AddNumberToObject(obj, "syncLevelPulseCount", rf_diag_sync_level_pulse_count);
    cJSON_AddNumberToObject(obj, "otherLevelPulseCount", rf_diag_other_level_pulse_count);
    cJSON_AddNumberToObject(obj, "lastCapturePulses", rf_diag_last_capture_pulses);
    cJSON_AddBoolToObject(obj, "lastHadSync", rf_diag_last_had_sync);
    cJSON_AddNumberToObject(obj, "lastDecodeStart", rf_diag_last_decode_start);
    cJSON_AddNumberToObject(obj, "lastShortUs", rf_diag_last_short_us);
    cJSON_AddNumberToObject(obj, "lastLongUs", rf_diag_last_long_us);
    cJSON_AddNumberToObject(obj, "lastJitterPercent", (double)rf_diag_last_jitter_pct_x10 / 10.0);
    cJSON_AddNumberToObject(obj, "lastRepeatCount", rf_diag_last_repeat_count);
    cJSON_AddNumberToObject(obj, "lastDecodeAgeMs", last_age_ms);
    cJSON_AddNumberToObject(obj, "lastCode", rf_diag_last_code);

    cJSON *bins = cJSON_CreateObject();
    if (bins) {
        static const char *labels[] = {
            "lt80", "80_179", "180_750", "751_899", "900_1800",
            "1801_5999", "6000_8999", "9000_14000", "14001_18000", "gt18000"
        };
        for (int level = 0; level <= 1; level++) {
            cJSON *level_obj = cJSON_CreateObject();
            if (!level_obj) {
                continue;
            }
            for (int i = 0; i < 10; i++) {
                cJSON_AddNumberToObject(level_obj, labels[i], rf_diag_bins[level][i]);
            }
            cJSON_AddItemToObject(bins, level == 0 ? "level0" : "level1", level_obj);
        }
        cJSON_AddItemToObject(obj, "pulseBins", bins);
    }

    return obj;
}

cJSON *rf_receiver_diagnostics_summary_snapshot(void)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }

    uint32_t success_rate_pct = 0;
    uint32_t noise_pct = 0;
    uint32_t last_age_ms = 0;
    uint32_t edge_rate_hz = 0;
    uint32_t noise_rate_hz = 0;
    uint32_t quality_score = rf_quality_score(&success_rate_pct, &noise_pct, &last_age_ms,
                                              &edge_rate_hz, &noise_rate_hz);
    const char *quality_label = rf_quality_label(quality_score, noise_pct);

    cJSON_AddNumberToObject(obj, "gpio", RF_DATA_GPIO);
    cJSON_AddStringToObject(obj, "isrAddResult", esp_err_to_name(rf_diag_isr_add_result));
    cJSON_AddNumberToObject(obj, "qualityScore", quality_score);
    cJSON_AddStringToObject(obj, "qualityLabel", quality_label);
    cJSON_AddBoolToObject(obj, "signalInverted", RF_SIGNAL_INVERTED);
    cJSON_AddNumberToObject(obj, "syncLevel", RF_SYNC_LEVEL);
    cJSON_AddNumberToObject(obj, "currentLevel", gpio_get_level(RF_DATA_GPIO));
    cJSON_AddNumberToObject(obj, "edgeCount", rf_diag_edge_count);
    cJSON_AddNumberToObject(obj, "edgeRatePerSecond", edge_rate_hz);
    cJSON_AddNumberToObject(obj, "noiseCount", rf_diag_noise_count);
    cJSON_AddNumberToObject(obj, "noiseRatePerSecond", noise_rate_hz);
    cJSON_AddNumberToObject(obj, "noisePercent", noise_pct);
    cJSON_AddNumberToObject(obj, "syncCount", rf_diag_sync_count);
    cJSON_AddNumberToObject(obj, "captureCount", rf_diag_capture_count);
    cJSON_AddNumberToObject(obj, "decodeOkCount", rf_diag_decode_ok_count);
    cJSON_AddNumberToObject(obj, "decodeFailCount", rf_diag_decode_fail_count);
    cJSON_AddNumberToObject(obj, "decodeSuccessRatePercent", success_rate_pct);
    cJSON_AddNumberToObject(obj, "discardCount", rf_diag_discard_count);
    cJSON_AddNumberToObject(obj, "lastCapturePulses", rf_diag_last_capture_pulses);
    cJSON_AddBoolToObject(obj, "lastHadSync", rf_diag_last_had_sync);
    cJSON_AddNumberToObject(obj, "lastDecodeStart", rf_diag_last_decode_start);
    cJSON_AddNumberToObject(obj, "lastShortUs", rf_diag_last_short_us);
    cJSON_AddNumberToObject(obj, "lastLongUs", rf_diag_last_long_us);
    cJSON_AddNumberToObject(obj, "lastJitterPercent", (double)rf_diag_last_jitter_pct_x10 / 10.0);
    cJSON_AddNumberToObject(obj, "lastRepeatCount", rf_diag_last_repeat_count);
    cJSON_AddNumberToObject(obj, "lastDecodeAgeMs", last_age_ms);
    cJSON_AddNumberToObject(obj, "lastCode", rf_diag_last_code);

    return obj;
}

cJSON *rf_receiver_line_test_snapshot(void)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }

    const int samples = 40;
    const uint32_t delay_us = 100;
    uint32_t edges_before = rf_diag_edge_count;
    int before_level = gpio_get_level(RF_DATA_GPIO);

    esp_err_t intr_disable_err = gpio_intr_disable(RF_DATA_GPIO);

    esp_err_t float_err = gpio_set_pull_mode(RF_DATA_GPIO, GPIO_FLOATING);
    esp_rom_delay_us(1000);
    uint32_t float_high = rf_sample_high_count(samples, delay_us);
    int float_level = gpio_get_level(RF_DATA_GPIO);

    esp_err_t pullup_err = gpio_set_pull_mode(RF_DATA_GPIO, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(1000);
    uint32_t pullup_high = rf_sample_high_count(samples, delay_us);
    int pullup_level = gpio_get_level(RF_DATA_GPIO);

    esp_err_t pulldown_err = gpio_set_pull_mode(RF_DATA_GPIO, GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(1000);
    uint32_t pulldown_high = rf_sample_high_count(samples, delay_us);
    int pulldown_level = gpio_get_level(RF_DATA_GPIO);

    esp_err_t post_discharge_float_err = gpio_set_pull_mode(RF_DATA_GPIO, GPIO_FLOATING);
    esp_rom_delay_us(100);
    int post_discharge_float_level_100us = gpio_get_level(RF_DATA_GPIO);
    esp_rom_delay_us(900);
    int post_discharge_float_level_1ms = gpio_get_level(RF_DATA_GPIO);
    esp_rom_delay_us(9000);
    int post_discharge_float_level_10ms = gpio_get_level(RF_DATA_GPIO);
    esp_rom_delay_us(90000);
    int post_discharge_float_level_100ms = gpio_get_level(RF_DATA_GPIO);
    uint32_t post_discharge_float_high = rf_sample_high_count(samples, delay_us);

    esp_err_t restore_err = gpio_set_pull_mode(RF_DATA_GPIO, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(1000);
    uint32_t restore_high = rf_sample_high_count(samples, delay_us);
    int restore_level = gpio_get_level(RF_DATA_GPIO);

    esp_err_t intr_enable_err = gpio_intr_enable(RF_DATA_GPIO);
    uint32_t edges_after = rf_diag_edge_count;

    const char *interpretation = "indeterminate";
    if (float_high == 0 && pullup_high == 0 && pulldown_high == 0) {
        interpretation = "GPIO15/DATA_IO is held low by external hardware; check Q4 collector, R84 pull-up, and shorts to ground.";
    } else if (float_high == 0 && pullup_high > 0 && pulldown_high == 0) {
        interpretation = "GPIO15 can be pulled high internally but floats low; external R84 pull-up or DATA_IO continuity is suspect.";
    } else if (float_high > 0 && post_discharge_float_high == 0 && pullup_high > 0 && pulldown_high == 0) {
        interpretation = "GPIO15 can float high initially, but DATA_IO does not recharge after pulldown; external R84 pull-up may be open or very weak.";
    } else if (float_high > 0 && pullup_high > 0 && pulldown_high == 0) {
        interpretation = "GPIO15 is movable and the digital input path works; if fob edges are missing, inspect Q4 base/collector waveform.";
    } else if (pulldown_high > 0) {
        interpretation = "GPIO15 resists the internal pulldown; DATA_IO may be shorted or strongly pulled high.";
    }

    cJSON_AddNumberToObject(obj, "gpio", RF_DATA_GPIO);
    cJSON_AddNumberToObject(obj, "samplesPerMode", samples);
    cJSON_AddNumberToObject(obj, "sampleDelayUs", delay_us);
    cJSON_AddNumberToObject(obj, "beforeLevel", before_level);
    cJSON_AddNumberToObject(obj, "floatingLevel", float_level);
    cJSON_AddNumberToObject(obj, "floatingHighSamples", float_high);
    cJSON_AddNumberToObject(obj, "pullupLevel", pullup_level);
    cJSON_AddNumberToObject(obj, "pullupHighSamples", pullup_high);
    cJSON_AddNumberToObject(obj, "pulldownLevel", pulldown_level);
    cJSON_AddNumberToObject(obj, "pulldownHighSamples", pulldown_high);
    cJSON_AddNumberToObject(obj, "postDischargeFloatingLevel100us", post_discharge_float_level_100us);
    cJSON_AddNumberToObject(obj, "postDischargeFloatingLevel1ms", post_discharge_float_level_1ms);
    cJSON_AddNumberToObject(obj, "postDischargeFloatingLevel10ms", post_discharge_float_level_10ms);
    cJSON_AddNumberToObject(obj, "postDischargeFloatingLevel100ms", post_discharge_float_level_100ms);
    cJSON_AddNumberToObject(obj, "postDischargeFloatingHighSamples", post_discharge_float_high);
    cJSON_AddNumberToObject(obj, "restoredPullupLevel", restore_level);
    cJSON_AddNumberToObject(obj, "restoredPullupHighSamples", restore_high);
    cJSON_AddStringToObject(obj, "interruptDisableResult", esp_err_to_name(intr_disable_err));
    cJSON_AddStringToObject(obj, "floatingPullResult", esp_err_to_name(float_err));
    cJSON_AddStringToObject(obj, "pullupResult", esp_err_to_name(pullup_err));
    cJSON_AddStringToObject(obj, "pulldownResult", esp_err_to_name(pulldown_err));
    cJSON_AddStringToObject(obj, "postDischargeFloatingPullResult", esp_err_to_name(post_discharge_float_err));
    cJSON_AddStringToObject(obj, "restorePullResult", esp_err_to_name(restore_err));
    cJSON_AddStringToObject(obj, "interruptEnableResult", esp_err_to_name(intr_enable_err));
    cJSON_AddNumberToObject(obj, "edgeCountBefore", edges_before);
    cJSON_AddNumberToObject(obj, "edgeCountAfter", edges_after);
    cJSON_AddStringToObject(obj, "normalMode", "internal-pullup-plus-external-10k-pullup");
    cJSON_AddStringToObject(obj, "interpretation", interpretation);

    return obj;
}
