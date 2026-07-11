#define MOTION_MCP_IO_1 A6  // Reverted back to correct pin
#define MOTION_MCP_IO_2 B6  // Reverted back to correct pin
#define NUM_OF_MOTIONS				  2
#define MOTION_SETTINGS_STORE_DEBOUNCE_MS 5000

// MCP23017 constants and function declarations
#define MCP_OUTPUT 0
#define MCP_INPUT  1
void set_mcp_io_dir(uint8_t io, bool dir);

char motion_service_message[2000];
bool motion_service_message_ready = false;
cJSON * motion_payload = NULL;

struct motionButton
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

struct motionButton motions[NUM_OF_MOTIONS];
static bool motion_settings_dirty = false;
static TickType_t motion_settings_due = 0;

int storeMotionSettings(void);

static bool motion_json_bool(const cJSON *item)
{
	if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
	if (cJSON_IsNumber(item)) return item->valueint != 0;
	if (cJSON_IsString(item) && item->valuestring) {
		return strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0;
	}
	return false;
}

static int motion_json_int(const cJSON *item, int fallback)
{
	if (cJSON_IsNumber(item)) return item->valueint;
	if (cJSON_IsString(item) && item->valuestring) return atoi(item->valuestring);
	return fallback;
}

static int motion_alert_channel(struct motionButton *mot)
{
	int mask = mot->channel_mask;
	if (mask <= 0 || mask > 3) mask = 1 << (mot->channel - 1);
	return mask == 3 ? 0 : ((mask & 1) ? 1 : 2);
}

static void motion_apply_locks(struct motionButton *mot, bool arm, const char *source, bool do_alert)
{
	int mask = mot->channel_mask;
	if (mask <= 0 || mask > 3) mask = 1 << (mot->channel - 1);
	for (int bit = 0; bit < 2; bit++) {
		if ((mask & (1 << bit)) == 0) continue;
		lock_set_action_source(source);
		arm_lock(bit + 1, arm, false);
	}
	if (do_alert && mot->alert) {
		alert_output_signal_force(1, motion_alert_channel(mot), mot->alert_target);
	}
}

static const char *motion_mode_from_latch(bool latch)
{
	return latch ? "latch" : "momentary";
}

static bool motion_mode_is_valid(const char *mode)
{
	return mode &&
		(strcmp(mode, "momentary") == 0 ||
		 strcmp(mode, "toggle") == 0 ||
		 strcmp(mode, "latch") == 0);
}

static void motion_set_mode(struct motionButton *mot, const char *mode)
{
	const char *next = motion_mode_is_valid(mode) ? mode : motion_mode_from_latch(mot->latch);
	if (strcmp(next, "toggle") != 0) {
		mot->toggleState = false;
	}
	strlcpy(mot->mode, next, sizeof(mot->mode));
	mot->latch = strcmp(mot->mode, "latch") == 0;
}

static const char *motion_current_mode(struct motionButton *mot)
{
	if (!motion_mode_is_valid(mot->mode)) {
		motion_set_mode(mot, motion_mode_from_latch(mot->latch));
	}
	return mot->mode;
}

static void scheduleMotionSettingsStore(void)
{
	motion_settings_dirty = true;
	motion_settings_due = xTaskGetTickCount() + pdMS_TO_TICKS(MOTION_SETTINGS_STORE_DEBOUNCE_MS);
}

static void flushMotionSettingsIfDue(void)
{
	if (!motion_settings_dirty) {
		return;
	}
	if ((int32_t)(xTaskGetTickCount() - motion_settings_due) < 0) {
		return;
	}
	motion_settings_dirty = false;
	storeMotionSettings();
}

int storeMotionSettings()
{
	for (uint8_t i=0; i < NUM_OF_MOTIONS; i++) {
		char type[25] = "";
		strcpy(type, motions[i].type);
		sprintf(motions[i].settings,
			"{\"eventType\":\"%s\", "
			"\"payload\":{\"channel\":%d, \"enable\": %s, \"alert\": %s, \"alert_target\": \"%s\", \"channel_mask\": %d, \"delay\": %d, \"latch\": %s, \"mode\": \"%s\"}}",
			type,
			i+1,
			(motions[i].enable) ? "true" : "false",
			(motions[i].alert) ? "true" : "false",
			alert_target_to_string(motions[i].alert_target),
			motions[i].channel_mask,
			motions[i].delay,
			(motions[i].latch) ? "true" : "false",
			motion_current_mode(&motions[i]));

		sprintf(motions[i].key, "%s%d", type, i);
		storeSetting(motions[i].key, cJSON_Parse(motions[i].settings));
		// printf("storeMotionSettings\t%s\n", motions[i].settings);
	}
  return 0;
}

int restoreMotionSettings()
{
	for (uint8_t i=0; i < NUM_OF_MOTIONS; i++) {
		char type[25] = "";
		strcpy(type, motions[i].type);
		sprintf(motions[i].key, "%s%d", type, i);
		restoreSetting(motions[i].key);
    	vTaskDelay(SERVICE_LOOP / portTICK_PERIOD_MS);
	}
	return 0;
}

void
sendMotionEventToServer()
{
	for (uint8_t i=0; i < NUM_OF_MOTIONS; i++) {
		char state_str[300];
		char msg[600];

		snprintf(state_str, sizeof(state_str), "{\"presence\":%s, \"exit\":false, \"keypad\":false, \"uptime\":1}", motions[i].isPressed ? "true" : "false");
		snprintf(msg, sizeof(msg),"{\"event_type\":\"load\", \"payload\":{\"services\":"
			"[{\"id\":\"ac_1\", \"type\":\"access-control\",\"state\":%s}]}}", state_str);

		addServerMessageToQueue(msg);
		printf("sendMotionEventToServer: %s\n", msg);
	}
}

void sendMotionEventToClient(int channel, bool state) {
    for (int i=0; i<NUM_OF_MOTIONS; i++) {
        if (motions[i].channel == channel) {
            if (strlen(motions[i].settings) > 2) {
                cJSON *json_msg = cJSON_Parse(motions[i].settings);
                addClientMessageToQueue(json_msg);
                cJSON_Delete(json_msg);
            }
        }
    }
}

void enableMotion (int ch, bool val)
{
	for (int i=0; i < NUM_OF_MOTIONS; i++)
		if (motions[i].channel == ch) motions[i].enable = val;
}

void alertOnMotion (int ch, bool val)
{
	for (int i=0; i < NUM_OF_MOTIONS; i++)
		if (motions[i].channel == ch) {
			motions[i].alert = val;
			motions[i].alert_target = alert_target_from_bool(val);
		}
}

void setMotionAlertTarget (int ch, int val)
{
	for (int i=0; i < NUM_OF_MOTIONS; i++)
		if (motions[i].channel == ch) {
			motions[i].alert_target = alert_target_normalize(val, motions[i].alert);
			motions[i].alert = motions[i].alert_target != ALERT_TARGET_NONE;
		}
}

void setMotionChannelMask (int ch, int val)
{
	if (val <= 0 || val > 3) return;
	for (int i=0; i < NUM_OF_MOTIONS; i++)
		if (motions[i].channel == ch) motions[i].channel_mask = val;
}

void setMotionArmDelay (int ch, int val)
{
	for (int i=0; i < NUM_OF_MOTIONS; i++)
		if (motions[i].channel == ch) motions[i].delay = val;
}

void latchMotion (int ch, bool val)
{
	for (int i=0; i < NUM_OF_MOTIONS; i++)
		if (motions[i].channel == ch) motion_set_mode(&motions[i], motion_mode_from_latch(val));
}

void modeMotion (int ch, const char *mode)
{
	for (int i=0; i < NUM_OF_MOTIONS; i++)
		if (motions[i].channel == ch) motion_set_mode(&motions[i], mode);
}

void start_motion_timer (struct motionButton *mot, bool val)
{
  if (val) {
    mot->expired = false;
    mot->count = 0;
  } else {
    mot->expired = true;
  }
}

static void test_motion_signal(struct motionButton *mot)
{
	if (!mot) return;
	const char *mode = motion_current_mode(mot);
	if (strcmp(mode, "latch") == 0) {
		ESP_LOGI(TAG, "Motion channel %d test - latch active", mot->channel);
		motion_apply_locks(mot, true, "motion_test", true);
		start_motion_timer(mot, false);
	} else if (strcmp(mode, "toggle") == 0) {
		mot->toggleState = !mot->toggleState;
		ESP_LOGI(TAG, "Motion channel %d test toggled lock to %s", mot->channel, mot->toggleState ? "armed" : "disarmed");
		motion_apply_locks(mot, mot->toggleState, "motion_test", true);
		start_motion_timer(mot, false);
	} else {
		ESP_LOGI(TAG, "Motion channel %d test - disarming lock", mot->channel);
		motion_apply_locks(mot, false, "motion_test", true);
		start_motion_timer(mot, true);
	}
}

void check_motion (struct motionButton *mot)
{
	mot->isPressed = !get_mcp_io(mot->pin);
	if (!mot->enable) {
		mot->prevPress = mot->isPressed;
		return;
	}

	const char *mode = motion_current_mode(mot);
	if (strcmp(mode, "latch") == 0 && mot->isPressed != mot->prevPress) {
		ESP_LOGI(TAG, "Motion channel %d state changed to %s (latch mode)", mot->channel, mot->isPressed ? "active" : "inactive");
		motion_apply_locks(mot, mot->isPressed, "motion_latch", true);
		start_motion_timer(mot, false);
	} else if (strcmp(mode, "toggle") == 0 && mot->isPressed && !mot->prevPress) {
		mot->toggleState = !mot->toggleState;
		ESP_LOGI(TAG, "Motion channel %d toggled lock to %s", mot->channel, mot->toggleState ? "armed" : "disarmed");
		motion_apply_locks(mot, mot->toggleState, "motion_toggle", true);
		start_motion_timer(mot, false);
	} else if (strcmp(mode, "momentary") == 0 && mot->isPressed && !mot->prevPress) {
		ESP_LOGI(TAG, "Motion detected on channel %d - disarming lock", mot->channel);
		motion_apply_locks(mot, false, "motion", true);
		start_motion_timer(mot, true);
	}

	mot->prevPress = mot->isPressed;
}

void handle_motion_message(cJSON * payload)
{
	int ch=0;
	bool tmp = 0;

	if (payload == NULL) return;

	if (cJSON_GetObjectItem(payload,"getState")) {
		sendMotionEventToClient(motions[0].channel, motions[0].isPressed);
		sendMotionEventToServer();
	}

	if (cJSON_GetObjectItem(payload,"channel")) {
		 ch = cJSON_GetObjectItem(payload,"channel")->valueint;
		 if (ch < 1 || ch > NUM_OF_MOTIONS) {
			 cJSON_Delete(payload);
			 return;
		 }

		 if (cJSON_IsTrue(cJSON_GetObjectItem(payload, "test"))) {
			test_motion_signal(&motions[ch - 1]);
		 }

		 if (cJSON_GetObjectItem(payload,"alert")) {
			tmp = motion_json_bool(cJSON_GetObjectItem(payload,"alert"));
	 		alertOnMotion(ch, tmp);
	 	}

		cJSON *alert_target = cJSON_GetObjectItem(payload, "alert_target");
		if (cJSON_IsString(alert_target) && alert_target->valuestring) {
			setMotionAlertTarget(ch, alert_target_from_string(alert_target->valuestring, motions[ch - 1].alert));
		} else if (cJSON_IsNumber(alert_target)) {
			setMotionAlertTarget(ch, alert_target->valueint);
		}

		if (cJSON_GetObjectItem(payload,"channel_mask")) {
			setMotionChannelMask(ch, motion_json_int(cJSON_GetObjectItem(payload,"channel_mask"), motions[ch - 1].channel_mask));
		}

	 	if (cJSON_GetObjectItem(payload,"enable")) {
			tmp = motion_json_bool(cJSON_GetObjectItem(payload,"enable"));
	 		enableMotion(ch, tmp);
	 	}

		if (cJSON_GetObjectItem(payload,"delay")) {
			setMotionArmDelay(ch, cJSON_GetObjectItem(payload,"delay")->valueint);
		}

		if (cJSON_GetObjectItem(payload,"latch")) {
			tmp = motion_json_bool(cJSON_GetObjectItem(payload,"latch"));
			latchMotion(ch, tmp);
	 	}

		cJSON *mode = cJSON_GetObjectItem(payload, "mode");
		if (cJSON_IsString(mode) && mode->valuestring) {
			modeMotion(ch, mode->valuestring);
		}
		scheduleMotionSettingsStore();
	}

	cJSON_Delete(payload);
}

void motion_timer_func(struct motionButton *mot)
{
	if (mot->count >= mot->delay && !mot->expired) {
		ESP_LOGI(TAG, "Re-arming lock from motion %d service.", mot->channel);
		motion_apply_locks(mot, true, "motion_auto", false);
		mot->expired = true;
	} else {
		mot->count++;
	}
}

static void
motion_timer (void *pvParameter)
{
  while (1) {
		for (int i=0; i < NUM_OF_MOTIONS; i++)
			motion_timer_func(&motions[i]);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

static void
motion_service (void *pvParameter)
{
  while (1) {
		for (int i=0; i < NUM_OF_MOTIONS; i++) {
			check_motion(&motions[i]);
		}

		handle_motion_message(checkServiceMessageByType("motion"));
		flushMotionSettingsIfDue();
    vTaskDelay(SERVICE_LOOP / portTICK_PERIOD_MS);
  }
}

cJSON *motion_state_snapshot(void)
{
	cJSON *array = cJSON_CreateArray();
	if (!array) return NULL;

	for (int i = 0; i < NUM_OF_MOTIONS; i++) {
		cJSON *item = cJSON_CreateObject();
		if (!item) continue;
		cJSON_AddNumberToObject(item, "channel", motions[i].channel);
		cJSON_AddBoolToObject(item, "enable", motions[i].enable);
		cJSON_AddBoolToObject(item, "alert", motions[i].alert);
		cJSON_AddStringToObject(item, "alert_target", alert_target_to_string(motions[i].alert_target));
		cJSON_AddNumberToObject(item, "channel_mask", motions[i].channel_mask);
		cJSON_AddNumberToObject(item, "delay", motions[i].delay);
		cJSON_AddBoolToObject(item, "latch", motions[i].latch);
		cJSON_AddStringToObject(item, "mode", motion_current_mode(&motions[i]));
		cJSON_AddBoolToObject(item, "signal", motions[i].isPressed);
		cJSON_AddItemToArray(array, item);
	}
	return array;
}

void motion_main()
{
  ESP_LOGI(TAG, "Starting motion service.");

	motions[0].pin = MOTION_MCP_IO_1;
	motions[0].delay = 4;
	motions[0].channel = 1;
	motions[0].channel_mask = 1;
	motions[0].alert = true;
	motions[0].alert_target = ALERT_TARGET_BOTH;
	motions[0].enable = true;
	motions[0].latch = false;
	motions[0].toggleState = false;
	motion_set_mode(&motions[0], "momentary");
	strcpy(motions[0].type, "motion");

	motions[1].pin = MOTION_MCP_IO_2;
	motions[1].delay = 4;
	motions[1].channel = 2;
	motions[1].channel_mask = 2;
	motions[1].alert = true;
	motions[1].alert_target = ALERT_TARGET_BOTH;
	motions[1].enable = true;
	motions[1].latch = false;
	motions[1].toggleState = false;
	motion_set_mode(&motions[1], "momentary");
	strcpy(motions[1].type, "motion");

	restoreMotionSettings();

	// Configure motion pins as inputs
	if (USE_MCP23017) {
		set_mcp_io_dir(motions[0].pin, MCP_INPUT);
		set_mcp_io_dir(motions[1].pin, MCP_INPUT);
		ESP_LOGI(TAG, "Motion pins configured as inputs: pin %d, pin %d", motions[0].pin, motions[1].pin);
	} else {
		gpio_set_direction(motions[0].pin, GPIO_MODE_INPUT);
		gpio_set_direction(motions[1].pin, GPIO_MODE_INPUT);
	}

  xTaskCreate(motion_timer, "motion_timer", 4096, NULL, 10, NULL);
	xTaskCreate(motion_service, "motion_service", 5000, NULL, 10, NULL);
}
