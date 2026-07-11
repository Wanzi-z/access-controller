struct buzzer
{
	uint8_t pin;
	uint8_t beepCount;
	uint8_t longBeepCount;
	bool enable;
	bool quietTestMode;
	bool contactAlert;
};

struct buzzer bzr;

// MCP23017 pins for keypad push/beep signal.
// Hardware: MCP pin drives an NPN transistor base (via resistor). So:
// - MCP HIGH  => transistor ON  => keypad PUSH line pulled LOW (active at the connector)
// - MCP LOW   => transistor OFF => keypad PUSH line released (idle)
// From schematic: PUSH0_IO = GPA4 (index 4), PUSH1_IO = GPB4 (index 12)
#define MCP_PUSH0_IO  4   // Port A, pin 4
#define MCP_PUSH1_IO  12  // Port B, pin 4 (8 + 4 = 12)
#define KEYPAD_PUSH_ACTIVE_MS 750
#define KEYPAD_PUSH_IDLE_MS 100

// External MCP23017 functions
extern void set_mcp_io(uint8_t io, bool val);
extern void set_mcp_io_dir(uint8_t io, bool dir);

static const char *BUZZER_TAG = "buzzer";

static void keypad_push_set(uint8_t push_pin, bool active) {
    if (USE_MCP23017) {
        ESP_LOGI(BUZZER_TAG, "MCP PUSH pin %d -> %s", push_pin, active ? "HIGH (active)" : "LOW (idle)");
        set_mcp_io(push_pin, active);
    } else {
        gpio_set_level(OPEN_IO_1, active ? 1 : 0);
    }
}

static void alert_output_internal(int beeps, int channel, int target, bool force) {
    if (!bzr.enable) return;
    if (bzr.quietTestMode && !force) return;
    target = alert_target_normalize(target, true);
    if (target == ALERT_TARGET_NONE) return;

    bool push_wg1 = (target & ALERT_TARGET_WG1) != 0;
    bool push_wg2 = (target & ALERT_TARGET_WG2) != 0;

    ESP_LOGI(BUZZER_TAG, "alert_output: beeps=%d, channel=%d, target=%s, wg1=%d wg2=%d",
             beeps, channel, alert_target_to_string(target), push_wg1 ? 1 : 0, push_wg2 ? 1 : 0);

    for (int i = 0; i < beeps; i++) {
        if (target & ALERT_TARGET_CONTROLLER) {
            gpio_set_level(bzr.pin, 1);
        }
        
        if (push_wg1) {
            keypad_push_set(MCP_PUSH0_IO, true);
        }
        if (push_wg2) {
            keypad_push_set(MCP_PUSH1_IO, true);
        }
        
        vTaskDelay(pdMS_TO_TICKS(KEYPAD_PUSH_ACTIVE_MS));
        
        if (target & ALERT_TARGET_CONTROLLER) {
            gpio_set_level(bzr.pin, 0);
        }
        
        if (push_wg1) {
            keypad_push_set(MCP_PUSH0_IO, false);
        }
        if (push_wg2) {
            keypad_push_set(MCP_PUSH1_IO, false);
        }
        
        vTaskDelay(pdMS_TO_TICKS(KEYPAD_PUSH_IDLE_MS));
    }
}

void alert_output_signal(int beeps, int channel, int target) {
    alert_output_internal(beeps, channel, target, false);
}

void alert_output_signal_force(int beeps, int channel, int target) {
    alert_output_internal(beeps, channel, target, true);
}

void beep_keypad(int beeps, int channel) {
    alert_output_internal(beeps, channel, ALERT_TARGET_BOTH, false);
}

void beep_keypad_force(int beeps, int channel) {
    alert_output_internal(beeps, channel, ALERT_TARGET_BOTH, true);
}

void keypad_push_test(int channel, int pulses, int active_ms, int idle_ms, bool active_high) {
    if (pulses < 1) pulses = 1;
    if (pulses > 10) pulses = 10;
    if (active_ms < 20) active_ms = 20;
    if (active_ms > 3000) active_ms = 3000;
    if (idle_ms < 20) idle_ms = 20;
    if (idle_ms > 3000) idle_ms = 3000;

    uint8_t push_pin = (channel == 2) ? MCP_PUSH1_IO : MCP_PUSH0_IO;
    bool active_level = active_high;
    bool idle_level = !active_level;

    ESP_LOGI(BUZZER_TAG,
             "keypad_push_test: channel=%d MCP_PIN=%d pulses=%d active_ms=%d idle_ms=%d active_level=%d",
             channel, push_pin, pulses, active_ms, idle_ms, active_level ? 1 : 0);

    if (USE_MCP23017) {
        set_mcp_io_dir(push_pin, 0);
        set_mcp_io(push_pin, idle_level);
    } else {
        gpio_set_direction(OPEN_IO_1, GPIO_MODE_OUTPUT);
        gpio_set_level(OPEN_IO_1, idle_level);
    }
    vTaskDelay(pdMS_TO_TICKS(idle_ms));

    for (int i = 0; i < pulses; i++) {
        if (USE_MCP23017) {
            ESP_LOGI(BUZZER_TAG, "PUSH test pin %d -> %d", push_pin, active_level ? 1 : 0);
            set_mcp_io(push_pin, active_level);
        } else {
            gpio_set_level(OPEN_IO_1, active_level);
        }
        vTaskDelay(pdMS_TO_TICKS(active_ms));

        if (USE_MCP23017) {
            ESP_LOGI(BUZZER_TAG, "PUSH test pin %d -> %d", push_pin, idle_level ? 1 : 0);
            set_mcp_io(push_pin, idle_level);
        } else {
            gpio_set_level(OPEN_IO_1, idle_level);
        }
        if (i + 1 < pulses) {
            vTaskDelay(pdMS_TO_TICKS(idle_ms));
        }
    }
}

void buzzer_set_quiet_test_mode(bool enabled) {
    bzr.quietTestMode = enabled;
    ESP_LOGI(BUZZER_TAG, "Quiet test mode %s", enabled ? "enabled" : "disabled");
}

bool buzzer_get_quiet_test_mode(void) {
    return bzr.quietTestMode;
}

static void buzzer_task(void *pvParameter) {
  while (1) {
    // This task can be used for more complex patterns in the future
    vTaskDelay(SERVICE_LOOP / portTICK_PERIOD_MS);
  }
}

void buzzer_main() {
	bzr.pin = BUZZER_IO;
	bzr.enable = true;
	bzr.quietTestMode = false;
	bzr.contactAlert = true;
	bzr.longBeepCount = 0;
	bzr.beepCount = 0;

    gpio_set_direction(bzr.pin, GPIO_MODE_OUTPUT);
    gpio_set_level(bzr.pin, 0);

    // Ensure keypad PUSH output is OFF by default (idle).
    // (OPEN_IO_1 is configured as an output in gpio_main().)
    gpio_set_level(OPEN_IO_1, 0);

    // Initialize MCP PUSH pins as outputs (LOW = idle, HIGH = pulse)
    if (USE_MCP23017) {
        ESP_LOGI(BUZZER_TAG, "Initializing MCP PUSH pins: PUSH0=%d, PUSH1=%d as outputs", MCP_PUSH0_IO, MCP_PUSH1_IO);
        
        // PUSH0 for channel 1
        set_mcp_io_dir(MCP_PUSH0_IO, 0);  // 0 = output
        set_mcp_io(MCP_PUSH0_IO, 0);       // Default low (idle)
        
        // PUSH1 for channel 2
        set_mcp_io_dir(MCP_PUSH1_IO, 0);  // 0 = output
        set_mcp_io(MCP_PUSH1_IO, 0);       // Default low (idle)
        
        ESP_LOGI(BUZZER_TAG, "MCP PUSH pins initialized to LOW (idle)");
    }

    xTaskCreate(buzzer_task, "buzzer_task", 3072, NULL, 10, NULL);
}
