#define EXIT_BUTTON_MCP_IO_1         A5
#define EXIT_BUTTON_MCP_IO_2         B5
#define NUM_OF_EXITS						 2
#define EXIT_SETTINGS_STORE_DEBOUNCE_MS 5000

char exit_service_message[2000];
bool exit_service_message_ready = false;
cJSON * exit_payload = NULL;

struct exitButton
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

struct exitButton exits[NUM_OF_EXITS];
static bool exit_settings_dirty = false;
static TickType_t exit_settings_due = 0;

int storeExitSettings(void);

static bool exit_json_bool(const cJSON *item)
{
	if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
	if (cJSON_IsNumber(item)) return item->valueint != 0;
	if (cJSON_IsString(item) && item->valuestring) {
		return strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0;
	}
	return false;
}

static const char *exit_mode_from_latch(bool latch)
{
	return latch ? "latch" : "momentary";
}

static bool exit_mode_is_valid(const char *mode)
{
	return mode &&
		(strcmp(mode, "momentary") == 0 ||
		 strcmp(mode, "toggle") == 0 ||
		 strcmp(mode, "latch") == 0);
}

static void exit_set_mode(struct exitButton *ext, const char *mode)
{
	const char *next = exit_mode_is_valid(mode) ? mode : exit_mode_from_latch(ext->latch);
	if (strcmp(next, "toggle") != 0) {
		ext->toggleState = false;
	}
	strlcpy(ext->mode, next, sizeof(ext->mode));
	ext->latch = strcmp(ext->mode, "latch") == 0;
}

static const char *exit_current_mode(struct exitButton *ext)
{
	if (!exit_mode_is_valid(ext->mode)) {
		exit_set_mode(ext, exit_mode_from_latch(ext->latch));
	}
	return ext->mode;
}

static void scheduleExitSettingsStore(void)
{
	exit_settings_dirty = true;
	exit_settings_due = xTaskGetTickCount() + pdMS_TO_TICKS(EXIT_SETTINGS_STORE_DEBOUNCE_MS);
}

static void flushExitSettingsIfDue(void)
{
	if (!exit_settings_dirty) {
		return;
	}
	if ((int32_t)(xTaskGetTickCount() - exit_settings_due) < 0) {
		return;
	}
	exit_settings_dirty = false;
	storeExitSettings();
}

void start_exit_timer (struct exitButton *ext, bool val)
{
  if (val) {
    ext->expired = false;
    ext->count = 0;
  } else {
    ext->expired = true;
  }
}

void exit_timer_func(struct exitButton *ext)
{
	if (ext->count >= ext->delay && !ext->expired) {
		ESP_LOGI(TAG, "Re-arming lock from button %d service.", ext->channel);
		lock_set_action_source("exit_auto");
		arm_lock(ext->channel, true, ext->alert);
		ext->expired = true;
	} else {
		ext->count++;
	}
}

static void
exit_timer (void *pvParameter)
{
  while (1) {
		for (int i=0; i < NUM_OF_EXITS; i++)
			exit_timer_func(&exits[i]);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

int storeExitSettings()
{

	for (uint8_t i=0; i < NUM_OF_EXITS; i++) {
		char type[25] = "";
		strcpy(type, exits[i].type);
		sprintf(exits[i].settings,
			"{\"eventType\":\"%s\", "
			"\"payload\":{\"channel\":%d, \"enable\": %s, \"alert\": %s, \"delay\": %d, \"latch\": %s, \"mode\": \"%s\"}}",
			type,
			i+1,
			(exits[i].enable) ? "true" : "false",
			(exits[i].alert) ? "true" : "false",
			exits[i].delay,
			(exits[i].latch) ? "true" : "false",
			exit_current_mode(&exits[i]));

		sprintf(exits[i].key, "%s%d", type, i);
		storeSetting(exits[i].key, cJSON_Parse(exits[i].settings));
		// printf("storeExitSettings\t%s\n", exits[i].settings);
	}
  return 0;
}


int restoreExitSettings()
{
	for (uint8_t i=0; i < NUM_OF_EXITS; i++) {
		char type[25] = "";
		strcpy(type, exits[i].type);
		sprintf(exits[i].key, "%s%d", type, i);
		restoreSetting(exits[i].key);
    	vTaskDelay(SERVICE_LOOP / portTICK_PERIOD_MS);
	}
	return 0;
}

void sendExitState(void) {
    for (int i=0; i < NUM_OF_EXITS; i++) {
        if (strlen(exits[i].settings) > 2) {
            cJSON *json_msg = cJSON_Parse(exits[i].settings);
            addClientMessageToQueue(json_msg);
            cJSON_Delete(json_msg);
        }
    }
}

cJSON *exit_state_snapshot(void) {
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        return NULL;
    }

    for (int i = 0; i < NUM_OF_EXITS; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry) {
            continue;
        }
        cJSON_AddNumberToObject(entry, "channel", exits[i].channel);
        cJSON_AddBoolToObject(entry, "enable", exits[i].enable);
        cJSON_AddBoolToObject(entry, "alert", exits[i].alert);
        cJSON_AddNumberToObject(entry, "delay", exits[i].delay);
        cJSON_AddBoolToObject(entry, "latch", exits[i].latch);
        cJSON_AddStringToObject(entry, "mode", exit_current_mode(&exits[i]));
        cJSON_AddBoolToObject(entry, "signal", exits[i].isPressed);
        cJSON_AddItemToArray(array, entry);
    }

    return array;
}


int handle_exit_property (char * prop)
{
  printf("exit property: %s\n",prop);

	if (strcmp(prop,"exit")==0) {
	}

	return 0;
}

void enableExit (int ch, bool val)
{
	for (int i=0; i < NUM_OF_EXITS; i++)
		if (exits[i].channel == ch) exits[i].enable = val;
}

void alertOnExit (int ch, bool val)
{
	for (int i=0; i < NUM_OF_EXITS; i++)
		if (exits[i].channel == ch) exits[i].alert = val;
}

void setArmDelay (int ch, int val)
{
	for (int i=0; i < NUM_OF_EXITS; i++)
		if (exits[i].channel == ch) exits[i].delay = val;
}

void latchExit (int ch, bool val)
{
	for (int i=0; i < NUM_OF_EXITS; i++)
		if (exits[i].channel == ch) exit_set_mode(&exits[i], exit_mode_from_latch(val));
}

void modeExit (int ch, const char *mode)
{
	for (int i=0; i < NUM_OF_EXITS; i++)
		if (exits[i].channel == ch) exit_set_mode(&exits[i], mode);
}

void check_exit (struct exitButton *ext)
{
	ext->isPressed = !get_io(ext->pin);
	if (!ext->enable) {
		ext->prevPress = ext->isPressed;
		return;
	}

	const char *mode = exit_current_mode(ext);
	if (strcmp(mode, "latch") == 0 && ext->isPressed != ext->prevPress) {
		ESP_LOGI(TAG, "Exit button %d state changed to %s (latch mode)", ext->channel, ext->isPressed ? "active" : "inactive");
		lock_set_action_source("exit_latch");
		arm_lock(ext->channel, ext->isPressed, ext->alert);
		start_exit_timer(ext, false);
	} else if (strcmp(mode, "toggle") == 0 && ext->isPressed && !ext->prevPress) {
		ext->toggleState = !ext->toggleState;
		ESP_LOGI(TAG, "Exit button %d toggled lock to %s", ext->channel, ext->toggleState ? "armed" : "disarmed");
		lock_set_action_source("exit_toggle");
		arm_lock(ext->channel, ext->toggleState, ext->alert);
		start_exit_timer(ext, false);
	} else if (strcmp(mode, "momentary") == 0 && ext->isPressed && !ext->prevPress) {
		ESP_LOGI(TAG, "Exit button %d pressed - disarming lock", ext->channel);
		lock_set_action_source("exit_press");
		arm_lock(ext->channel, false, ext->alert);
		start_exit_timer(ext, true);
	}

	ext->prevPress = ext->isPressed;
}

void handle_exit_message(cJSON * payload)
{
	int ch=0;
	bool tmp = 0;

	if (payload == NULL) return;

	if (cJSON_GetObjectItem(payload,"getState")) {
		sendExitState();
	}

	if (cJSON_GetObjectItem(payload,"channel")) {
		 ch = cJSON_GetObjectItem(payload,"channel")->valueint;
		 if (ch < 1 || ch > NUM_OF_EXITS) {
			 cJSON_Delete(payload);
			 return;
		 }

		 if (cJSON_GetObjectItem(payload,"alert")) {
			tmp = exit_json_bool(cJSON_GetObjectItem(payload,"alert"));
	 		alertOnExit(ch, tmp);
	 	}

	 	if (cJSON_GetObjectItem(payload,"enable")) {
			tmp = exit_json_bool(cJSON_GetObjectItem(payload,"enable"));
	 		enableExit(ch, tmp);
	 	}

		if (cJSON_GetObjectItem(payload,"delay")) {
			setArmDelay(ch, cJSON_GetObjectItem(payload,"delay")->valueint);
		}

		if (cJSON_GetObjectItem(payload,"latch")) {
			tmp = exit_json_bool(cJSON_GetObjectItem(payload,"latch"));
			latchExit(ch, tmp);
		}

		cJSON *mode = cJSON_GetObjectItem(payload, "mode");
		if (cJSON_IsString(mode) && mode->valuestring) {
			modeExit(ch, mode->valuestring);
		}
		scheduleExitSettingsStore();
	}

	cJSON_Delete(payload);
}

static void
exit_service (void *pvParameter)
{
  while (1) {
		for (int i=0; i < NUM_OF_EXITS; i++)
			check_exit(&exits[i]);

		                handle_exit_message(checkServiceMessageByType("exit"));
		flushExitSettingsIfDue();
    vTaskDelay(SERVICE_LOOP / portTICK_PERIOD_MS);
  }
}

void exit_main()
{
  printf("Starting exit service.\n");

	exits[0].pin = USE_MCP23017 ? EXIT_BUTTON_MCP_IO_1 : EXIT_BUTTON_IO_1;
	exits[0].delay = 4;
	exits[0].channel = 1;
	exits[0].alert = false;
	exits[0].enable = false;
	exits[0].latch = false;
	exits[0].toggleState = false;
	exit_set_mode(&exits[0], "momentary");
	strcpy(exits[0].type, "exit");

	exits[1].pin = USE_MCP23017 ? EXIT_BUTTON_MCP_IO_2 : EXIT_BUTTON_IO_2;
	exits[1].delay = 4;
	exits[1].channel = 2;
	exits[1].enable = false;
	exits[1].alert = false;
	exits[1].latch = false;
	exits[1].toggleState = false;
	exit_set_mode(&exits[1], "momentary");
	strcpy(exits[1].type, "exit");

	restoreExitSettings();

	if (USE_MCP23017) {
		set_mcp_io_dir(exits[0].pin, MCP_INPUT);
		set_mcp_io_dir(exits[1].pin, MCP_INPUT);
	} else {
		gpio_set_direction(exits[0].pin, GPIO_MODE_INPUT);
		gpio_set_direction(exits[1].pin, GPIO_MODE_INPUT);
	}

  	xTaskCreate(exit_timer, "exit_timer", 4096, NULL, 10, NULL);
	xTaskCreate(exit_service, "exit_service", 5000, NULL, 10, NULL);
}
