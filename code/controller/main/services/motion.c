#define MOTION_MCP_IO_1 A6  // Reverted back to correct pin
#define MOTION_MCP_IO_2 B6  // Reverted back to correct pin
#define NUM_OF_MOTIONS				  2

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
	char mode[12];
	char settings[1000];
	char key[50];
	char type[40];
	cJSON *payload;
};

struct motionButton motions[NUM_OF_MOTIONS];

static bool motion_json_bool(const cJSON *item)
{
	if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
	if (cJSON_IsNumber(item)) return item->valueint != 0;
	if (cJSON_IsString(item) && item->valuestring) {
		return strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0;
	}
	return false;
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

int storeMotionSettings()
{
	for (uint8_t i=0; i < NUM_OF_MOTIONS; i++) {
		char type[25] = "";
		strcpy(type, motions[i].type);
		sprintf(motions[i].settings,
			"{\"eventType\":\"%s\", "
			"\"payload\":{\"channel\":%d, \"enable\": %s, \"alert\": %s, \"delay\": %d, \"latch\": %s, \"mode\": \"%s\"}}",
			type,
			i+1,
			(motions[i].enable) ? "true" : "false",
			(motions[i].alert) ? "true" : "false",
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
		if (motions[i].channel == ch) motions[i].alert = val;
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
        lock_set_action_source("motion_latch");
		arm_lock(mot->channel, mot->isPressed, mot->alert);
		start_motion_timer(mot, false);
	} else if (strcmp(mode, "toggle") == 0 && mot->isPressed && !mot->prevPress) {
		mot->toggleState = !mot->toggleState;
		ESP_LOGI(TAG, "Motion channel %d toggled lock to %s", mot->channel, mot->toggleState ? "armed" : "disarmed");
        lock_set_action_source("motion_toggle");
		arm_lock(mot->channel, mot->toggleState, mot->alert);
		start_motion_timer(mot, false);
	} else if (strcmp(mode, "momentary") == 0 && mot->isPressed && !mot->prevPress) {
		ESP_LOGI(TAG, "Motion detected on channel %d - disarming lock", mot->channel);
        lock_set_action_source("motion");
		arm_lock(mot->channel, false, mot->alert);
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

		 if (cJSON_GetObjectItem(payload,"alert")) {
			tmp = motion_json_bool(cJSON_GetObjectItem(payload,"alert"));
	 		alertOnMotion(ch, tmp);
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
		storeMotionSettings();
	}

	cJSON_Delete(payload);
}

void motion_timer_func(struct motionButton *mot)
{
	if (mot->count >= mot->delay && !mot->expired) {
		ESP_LOGI(TAG, "Re-arming lock from motion %d service.", mot->channel);
        lock_set_action_source("motion_auto");
		arm_lock(mot->channel, true, mot->alert);
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
	motions[0].alert = true;
	motions[0].enable = true;
	motions[0].latch = false;
	motions[0].toggleState = false;
	motion_set_mode(&motions[0], "momentary");
	strcpy(motions[0].type, "motion");

	motions[1].pin = MOTION_MCP_IO_2;
	motions[1].delay = 4;
	motions[1].channel = 2;
	motions[1].alert = true;
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
