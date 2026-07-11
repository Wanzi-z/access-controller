#include <stdio.h>
#include "esp_log.h"
#include "cJSON.h"
#include "automation.h"

#define KEYPAD_MCP_IO_1         A3
#define KEYPAD_MCP_IO_2         B3
#define NUM_OF_KEYPADS          2
#define KEYPAD_SETTINGS_STORE_DEBOUNCE_MS 5000

char keypad_service_message[2000];
bool keypad_service_message_ready = false;
cJSON * keypad_payload = NULL;

struct keypadButton
{
  int pin;
	bool alert;
	bool isPressed;
	bool prevPress;
	int count;
	bool expired;
	bool enable;
	bool latch;
	bool toggleState;
	int delay;
	int channel;
	int channel_mask;
	int alert_target;
	char mode[12];
	char settings[1000];
	char key[50];
	char type[40];
	cJSON *payload;
};

struct keypadButton keypads[NUM_OF_KEYPADS];
static bool keypad_settings_dirty = false;
static TickType_t keypad_settings_due = 0;

void sendKeypadState(void);
cJSON *keypad_state_snapshot(void);
int storeKeypadSettings(void);

static bool keypad_json_bool(const cJSON *item)
{
	if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
	if (cJSON_IsNumber(item)) return item->valueint != 0;
	if (cJSON_IsString(item) && item->valuestring) {
		return strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0;
	}
	return false;
}

static int keypad_json_int(const cJSON *item, int fallback)
{
	if (cJSON_IsNumber(item)) return item->valueint;
	if (cJSON_IsString(item) && item->valuestring) return atoi(item->valuestring);
	return fallback;
}

static int keypad_alert_channel(struct keypadButton *pad)
{
	int mask = pad->channel_mask;
	if (mask <= 0 || mask > 3) mask = 1 << (pad->channel - 1);
	return mask == 3 ? 0 : ((mask & 1) ? 1 : 2);
}

static void keypad_apply_locks(struct keypadButton *pad, bool arm, const char *source, bool do_alert)
{
	int mask = pad->channel_mask;
	if (mask <= 0 || mask > 3) mask = 1 << (pad->channel - 1);
	for (int bit = 0; bit < 2; bit++) {
		if ((mask & (1 << bit)) == 0) continue;
		lock_set_action_source(source);
		arm_lock(bit + 1, arm, false);
	}
	if (do_alert && pad->alert) {
		alert_output_signal_force(1, keypad_alert_channel(pad), pad->alert_target);
	}
}

static const char *keypad_mode_from_latch(bool latch)
{
	return latch ? "latch" : "momentary";
}

static bool keypad_mode_is_valid(const char *mode)
{
	return mode &&
		(strcmp(mode, "momentary") == 0 ||
		 strcmp(mode, "toggle") == 0 ||
		 strcmp(mode, "latch") == 0);
}

static void keypad_set_mode(struct keypadButton *pad, const char *mode)
{
	const char *next = keypad_mode_is_valid(mode) ? mode : keypad_mode_from_latch(pad->latch);
	if (strcmp(next, "toggle") != 0) {
		pad->toggleState = false;
	}
	strlcpy(pad->mode, next, sizeof(pad->mode));
	pad->latch = strcmp(pad->mode, "latch") == 0;
}

static const char *keypad_current_mode(struct keypadButton *pad)
{
	if (!keypad_mode_is_valid(pad->mode)) {
		keypad_set_mode(pad, keypad_mode_from_latch(pad->latch));
	}
	return pad->mode;
}

static void scheduleKeypadSettingsStore(void)
{
	keypad_settings_dirty = true;
	keypad_settings_due = xTaskGetTickCount() + pdMS_TO_TICKS(KEYPAD_SETTINGS_STORE_DEBOUNCE_MS);
}

static void flushKeypadSettingsIfDue(void)
{
	if (!keypad_settings_dirty) {
		return;
	}
	if ((int32_t)(xTaskGetTickCount() - keypad_settings_due) < 0) {
		return;
	}
	keypad_settings_dirty = false;
	storeKeypadSettings();
}

void start_keypad_timer (struct keypadButton *pad, bool val)
{
  if (val) {
    pad->expired = false;
    pad->count = 0;
  } else {
    pad->expired = true;
  }
}

void keypad_timer_func(struct keypadButton *pad)
{
	if (pad->count >= pad->delay && !pad->expired) {
		ESP_LOGI(TAG, "Re-arming lock from pad %d service. Alert %d", pad->channel, pad->alert);
		keypad_apply_locks(pad, true, "kp_auto", false);
		pad->expired = true;
	} else {
		pad->count++;
	}
}

static void
keypad_timer (void *pvParameter)
{
  while (1) {
		for (int i=0; i < NUM_OF_KEYPADS; i++)
			keypad_timer_func(&keypads[i]);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

int storeKeypadSettings()
{
	for (uint8_t i=0; i < NUM_OF_KEYPADS; i++) {
		char type[25] = "";
		strcpy(type, keypads[i].type);
		sprintf(keypads[i].settings,
			"{\"eventType\":\"%s\", "
			"\"payload\":{\"channel\":%d, \"enable\": %s, \"alert\": %s, \"alert_target\": \"%s\", \"channel_mask\": %d, \"delay\": %d, \"latch\": %s, \"mode\": \"%s\"}}",
			type,
			i+1,
			(keypads[i].enable) ? "true" : "false",
			(keypads[i].alert) ? "true" : "false",
			alert_target_to_string(keypads[i].alert_target),
			keypads[i].channel_mask,
			keypads[i].delay,
			(keypads[i].latch) ? "true" : "false",
			keypad_current_mode(&keypads[i]));

		sprintf(keypads[i].key, "%s%d", type, i);
		storeSetting(keypads[i].key, cJSON_Parse(keypads[i].settings));
		printf("storeKeypadSettings\t%s\n", keypads[i].settings);
	}
  return 0;
}

void sendKeypadState(void) {
    for (int i = 0; i < NUM_OF_KEYPADS; i++) {
        if (strlen(keypads[i].settings) > 2) {
            cJSON *json_msg = cJSON_Parse(keypads[i].settings);
            if (json_msg) {
                addClientMessageToQueue(json_msg);
                cJSON_Delete(json_msg);
            }
        }
    }
}

cJSON *keypad_state_snapshot(void) {
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        return NULL;
    }

    for (int i = 0; i < NUM_OF_KEYPADS; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry) {
            continue;
        }
        cJSON_AddNumberToObject(entry, "channel", keypads[i].channel);
        cJSON_AddBoolToObject(entry, "enable", keypads[i].enable);
        cJSON_AddBoolToObject(entry, "alert", keypads[i].alert);
        cJSON_AddStringToObject(entry, "alert_target", alert_target_to_string(keypads[i].alert_target));
        cJSON_AddNumberToObject(entry, "channel_mask", keypads[i].channel_mask);
        cJSON_AddNumberToObject(entry, "delay", keypads[i].delay);
        cJSON_AddBoolToObject(entry, "latch", keypads[i].latch);
        cJSON_AddStringToObject(entry, "mode", keypad_current_mode(&keypads[i]));
        cJSON_AddBoolToObject(entry, "signal", keypads[i].isPressed);
        cJSON_AddItemToArray(array, entry);
    }

    return array;
}

int restoreKeypadSettings()
{
	for (uint8_t i=0; i < NUM_OF_KEYPADS; i++) {
		char type[25] = "";
		strcpy(type, keypads[i].type);
		vTaskDelay(100 / portTICK_PERIOD_MS);
		sprintf(keypads[i].key, "%s%d", type, i);
		restoreSetting(keypads[i].key);
	}
	return 0;
}

int load_keypad_state_from_flash()
{
  char *state_str = get_char("keypad");
  if (strcmp(state_str,"")==0) {
    printf("Lock state not found in flash.\n");
    return 1;
  }

  // Need JSON validation
  cJSON *keypad_payload = cJSON_Parse(state_str);
  printf("Loaded keypad state from flash. %s\n", state_str);
  return 0;
}

int handle_keypad_property (char * prop)
{
	printf("keypad property: %s\n",prop);

	if (strcmp(prop,"keypad")==0) {
	}

	return 0;
}

void enableKeypad (int ch, bool val)
{
	for (int i=0; i < NUM_OF_KEYPADS; i++)
		if (keypads[i].channel == ch) keypads[i].enable = val;
}

void alertOnKeypad (int ch, bool val)
{
	printf("alertOnKeypad\tch: %d alert: %d.\n", ch, val);
	for (int i=0; i < NUM_OF_KEYPADS; i++)
		if (keypads[i].channel == ch) {
			keypads[i].alert = val;
			keypads[i].alert_target = alert_target_from_bool(val);
		}
}

void setKeypadAlertTarget (int ch, int val)
{
	for (int i=0; i < NUM_OF_KEYPADS; i++)
		if (keypads[i].channel == ch) {
			keypads[i].alert_target = alert_target_normalize(val, keypads[i].alert);
			keypads[i].alert = keypads[i].alert_target != ALERT_TARGET_NONE;
		}
}

void setKeypadChannelMask (int ch, int val)
{
	if (val <= 0 || val > 3) return;
	for (int i=0; i < NUM_OF_KEYPADS; i++)
		if (keypads[i].channel == ch) keypads[i].channel_mask = val;
}

void setKeypadArmDelay (int ch, int val)
{
	for (int i=0; i < NUM_OF_KEYPADS; i++)
		if (keypads[i].channel == ch) keypads[i].delay = val;
}

void latchKeypad (int ch, bool val)
{
	for (int i=0; i < NUM_OF_KEYPADS; i++)
		if (keypads[i].channel == ch) keypad_set_mode(&keypads[i], keypad_mode_from_latch(val));
}

void modeKeypad (int ch, const char *mode)
{
	for (int i=0; i < NUM_OF_KEYPADS; i++)
		if (keypads[i].channel == ch) keypad_set_mode(&keypads[i], mode);
}

static void test_keypad_signal(struct keypadButton *pad)
{
	if (!pad) return;
	const char *mode = keypad_current_mode(pad);
	if (strcmp(mode, "latch") == 0) {
		ESP_LOGI(TAG, "Keypad %d test - latch active", pad->channel);
		keypad_apply_locks(pad, true, "kp_test", true);
		start_keypad_timer(pad, false);
	} else if (strcmp(mode, "toggle") == 0) {
		pad->toggleState = !pad->toggleState;
		ESP_LOGI(TAG, "Keypad %d test toggled lock to %s", pad->channel, pad->toggleState ? "armed" : "disarmed");
		keypad_apply_locks(pad, pad->toggleState, "kp_test", true);
		start_keypad_timer(pad, false);
	} else {
		ESP_LOGI(TAG, "Keypad %d test - disarming lock", pad->channel);
		keypad_apply_locks(pad, false, "kp_test", true);
		start_keypad_timer(pad, true);
	}
}

void check_keypads (struct keypadButton *pad)
{
	pad->isPressed = !get_io(pad->pin);
	if (!pad->enable) {
		pad->prevPress = pad->isPressed;
		return;
	}

	const char *mode = keypad_current_mode(pad);
	if (strcmp(mode, "latch") == 0 && pad->isPressed != pad->prevPress) {
		ESP_LOGI(TAG, "Keypad %d state changed to %s (latch mode)", pad->channel, pad->isPressed ? "active" : "inactive");
		keypad_apply_locks(pad, pad->isPressed, "kp_latch", true);
		start_keypad_timer(pad, false);
	} else if (strcmp(mode, "toggle") == 0 && pad->isPressed && !pad->prevPress) {
		pad->toggleState = !pad->toggleState;
		ESP_LOGI(TAG, "Keypad %d toggled lock to %s", pad->channel, pad->toggleState ? "armed" : "disarmed");
		keypad_apply_locks(pad, pad->toggleState, "kp_toggle", true);
		start_keypad_timer(pad, false);
	} else if (strcmp(mode, "momentary") == 0 && pad->isPressed && !pad->prevPress) {
		ESP_LOGI(TAG, "Keypad %d pressed - disarming lock", pad->channel);
		keypad_apply_locks(pad, false, "kp_press", true);
		start_keypad_timer(pad, true);
	}

	pad->prevPress = pad->isPressed;
}

void handle_keypad_message(cJSON * payload)
{
	int ch=0;
	bool tmp;

	if (payload == NULL) return;

	if (cJSON_GetObjectItem(payload,"getState")) {
		sendKeypadState();
	}

	if (cJSON_GetObjectItem(payload,"channel")) {
		 ch = cJSON_GetObjectItem(payload,"channel")->valueint;
		 if (ch < 1 || ch > NUM_OF_KEYPADS) {
			 cJSON_Delete(payload);
			 return;
		 }

		 if (cJSON_IsTrue(cJSON_GetObjectItem(payload, "test"))) {
			 test_keypad_signal(&keypads[ch - 1]);
		 }

		 if (cJSON_GetObjectItem(payload,"alert")) {
			 tmp = keypad_json_bool(cJSON_GetObjectItem(payload,"alert"));
			 alertOnKeypad(ch, tmp);
		 }

		 cJSON *alert_target = cJSON_GetObjectItem(payload, "alert_target");
		 if (cJSON_IsString(alert_target) && alert_target->valuestring) {
			 setKeypadAlertTarget(ch, alert_target_from_string(alert_target->valuestring, keypads[ch - 1].alert));
		 } else if (cJSON_IsNumber(alert_target)) {
			 setKeypadAlertTarget(ch, alert_target->valueint);
		 }

		 if (cJSON_GetObjectItem(payload,"channel_mask")) {
			 setKeypadChannelMask(ch, keypad_json_int(cJSON_GetObjectItem(payload,"channel_mask"), keypads[ch - 1].channel_mask));
		 }

		 if (cJSON_GetObjectItem(payload,"enable")) {
			 tmp = keypad_json_bool(cJSON_GetObjectItem(payload,"enable"));
			 enableKeypad(ch, tmp);
		 }

		 if (cJSON_GetObjectItem(payload,"delay")) {
			 setKeypadArmDelay(ch, cJSON_GetObjectItem(payload,"delay")->valueint);
		 }

		 if (cJSON_GetObjectItem(payload,"latch")) {
			 tmp = keypad_json_bool(cJSON_GetObjectItem(payload,"latch"));
			 latchKeypad(ch, tmp);
		 }

		 cJSON *mode = cJSON_GetObjectItem(payload, "mode");
		 if (cJSON_IsString(mode) && mode->valuestring) {
			 modeKeypad(ch, mode->valuestring);
		 }

		 scheduleKeypadSettingsStore();
	}

	cJSON_Delete(payload);
}

static void
keypad_service (void *pvParameter)
{
  // load_lock_state_from_flash();

  while (1) {
		for (int i=0; i < NUM_OF_KEYPADS; i++)
			check_keypads(&keypads[i]);

		                handle_keypad_message(checkServiceMessageByType("keypad"));
		flushKeypadSettingsIfDue();
    vTaskDelay(SERVICE_LOOP / portTICK_PERIOD_MS);
  }
}

void keypad_main()
{
  printf("Starting keypad service.\n");

	keypads[0].pin = USE_MCP23017 ? KEYPAD_MCP_IO_1 : KEYPAD_IO_1;
	keypads[0].delay = 4;
	keypads[0].channel = 1;
	keypads[0].channel_mask = 1;
	keypads[0].alert = true;
	keypads[0].alert_target = ALERT_TARGET_BOTH;
	keypads[0].enable = true;
	keypads[0].latch = false;
	keypads[0].toggleState = false;
	keypad_set_mode(&keypads[0], "momentary");
	strcpy(keypads[0].type, "keypad");

	keypads[1].pin = USE_MCP23017 ? KEYPAD_MCP_IO_2 : KEYPAD_IO_2;
	keypads[1].delay = 4;
	keypads[1].channel = 2;
	keypads[1].channel_mask = 2;
	keypads[1].enable = true;
	keypads[1].alert = true;
	keypads[1].alert_target = ALERT_TARGET_BOTH;
	keypads[1].latch = false;
	keypads[1].toggleState = false;
	keypad_set_mode(&keypads[1], "momentary");
	strcpy(keypads[1].type, "keypad");

	restoreKeypadSettings();

	
	if (USE_MCP23017) {
		set_mcp_io_dir(keypads[0].pin, MCP_INPUT);
		set_mcp_io_dir(keypads[1].pin, MCP_INPUT);
	} else {
		gpio_set_direction(keypads[0].pin, GPIO_MODE_INPUT);
		gpio_set_direction(keypads[1].pin, GPIO_MODE_INPUT);
	}

  xTaskCreate(keypad_timer, "keypad_timer", 4096, NULL, 10, NULL);
	xTaskCreate(keypad_service, "keypad_service", 4096, NULL, 10, NULL);
}
