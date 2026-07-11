#define FOB_MCP_IO_1         A7
#define FOB_MCP_IO_2         B7
#define NUM_OF_FOBS			    2
#define MOMENTARY		  1
#define FOB_SETTINGS_STORE_DEBOUNCE_MS 5000

char fob_service_message[2000];
bool fob_service_message_ready = false;
cJSON * fob_payload = NULL;
bool FOB_ALERT = true;

struct fob
{
  	int pin;
	bool alert;
	bool isPressed;
	bool prevPress;
	int count;
	bool expired;
	bool enable;
	bool latch;  // New field for latch mode (false = momentary, true = latch)
	bool toggleState;
	int delay;
	int channel;
	int channel_mask;
	int alert_target;
	char mode[12];
	cJSON *payload;
	char settings[1000];
	char key[50];
	char type[40];
};

struct fob fobs[NUM_OF_FOBS];
static bool fob_settings_dirty = false;
static TickType_t fob_settings_due = 0;

int storeFobSettings(void);

static bool fob_json_bool(const cJSON *item)
{
	if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
	if (cJSON_IsNumber(item)) return item->valueint != 0;
	if (cJSON_IsString(item) && item->valuestring) {
		return strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0;
	}
	return false;
}

static int fob_json_int(const cJSON *item, int fallback)
{
	if (cJSON_IsNumber(item)) return item->valueint;
	if (cJSON_IsString(item) && item->valuestring) return atoi(item->valuestring);
	return fallback;
}

static int fob_alert_channel(struct fob *fb)
{
	int mask = fb->channel_mask;
	if (mask <= 0 || mask > 3) mask = 1 << (fb->channel - 1);
	return mask == 3 ? 0 : ((mask & 1) ? 1 : 2);
}

static void fob_apply_locks(struct fob *fb, bool arm, const char *source, bool do_alert)
{
	int mask = fb->channel_mask;
	if (mask <= 0 || mask > 3) mask = 1 << (fb->channel - 1);
	for (int bit = 0; bit < 2; bit++) {
		if ((mask & (1 << bit)) == 0) continue;
		lock_set_action_source(source);
		arm_lock(bit + 1, arm, false);
	}
	if (do_alert && fb->alert) {
		alert_output_signal_force(1, fob_alert_channel(fb), fb->alert_target);
	}
}

static const char *fob_mode_from_latch(bool latch)
{
	return latch ? "latch" : "momentary";
}

static bool fob_mode_is_valid(const char *mode)
{
	return mode &&
		(strcmp(mode, "momentary") == 0 ||
		 strcmp(mode, "toggle") == 0 ||
		 strcmp(mode, "latch") == 0);
}

static void fob_set_mode(struct fob *fb, const char *mode)
{
	const char *next = fob_mode_is_valid(mode) ? mode : fob_mode_from_latch(fb->latch);
	if (strcmp(next, "toggle") != 0) {
		fb->toggleState = false;
	}
	strlcpy(fb->mode, next, sizeof(fb->mode));
	fb->latch = strcmp(fb->mode, "latch") == 0;
}

static const char *fob_current_mode(struct fob *fb)
{
	if (!fob_mode_is_valid(fb->mode)) {
		fob_set_mode(fb, fob_mode_from_latch(fb->latch));
	}
	return fb->mode;
}

static void scheduleFobSettingsStore(void)
{
	fob_settings_dirty = true;
	fob_settings_due = xTaskGetTickCount() + pdMS_TO_TICKS(FOB_SETTINGS_STORE_DEBOUNCE_MS);
}

static void flushFobSettingsIfDue(void)
{
	if (!fob_settings_dirty) {
		return;
	}
	if ((int32_t)(xTaskGetTickCount() - fob_settings_due) < 0) {
		return;
	}
	fob_settings_dirty = false;
	storeFobSettings();
}

void start_fob_timer (struct fob *fb, bool val)
{
  if (val) {
    fb->expired = false;
    fb->count = 0;
  } else {
    fb->expired = true;
  }
}

void check_fob_timer (struct fob *fb)
{
  if (fb->count >= fb->delay && !fb->expired) {
			ESP_LOGI(TAG, "Re-arming lock from fob %d service.", fb->channel);
			fob_apply_locks(fb, true, "fob_auto", false);
			fb->expired = true;
  } else fb->count++;
}

static void
fob_timer (void *pvParameter)
{
  while (1) {
		for (int i=0; i < NUM_OF_FOBS; i++)
			check_fob_timer(&fobs[i]);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

int handle_fob_property (char * prop)
{
  printf("fob property: %s\n",prop);

	if (strcmp(prop,"fob")==0) {
	}

	return 0;
}

void check_fobs (struct fob *fb)
{
	fb->isPressed = !get_mcp_io(fb->pin);
	if (!fb->enable) {
		fb->prevPress = fb->isPressed;
		return;
	}

	const char *mode = fob_current_mode(fb);
	if (strcmp(mode, "latch") == 0 && fb->isPressed != fb->prevPress) {
		// Latch mode: FOB state directly controls lock state
		ESP_LOGI(TAG, "Fob %d state changed to %s (latch mode)", fb->channel, fb->isPressed ? "activated" : "deactivated");
		fob_apply_locks(fb, fb->isPressed, "fob_latch", true);
		enableExit(fb->channel, fb->isPressed);
		enableKeypad(fb->channel, fb->isPressed);
	} else if (strcmp(mode, "toggle") == 0 && fb->isPressed && !fb->prevPress) {
		fb->toggleState = !fb->toggleState;
		ESP_LOGI(TAG, "Fob %d toggled lock to %s", fb->channel, fb->toggleState ? "armed" : "disarmed");
		fob_apply_locks(fb, fb->toggleState, "fob_toggle", true);
		start_fob_timer(fb, false);
	} else if (strcmp(mode, "momentary") == 0 && fb->isPressed && !fb->prevPress) {
		// Momentary mode: active edge triggers unlock and timer
		ESP_LOGI(TAG, "Fob %d activated (momentary mode) - disarming lock", fb->channel);
		fob_apply_locks(fb, false, "fob_active", true);
		start_fob_timer(fb, true);
	}

	fb->prevPress = fb->isPressed;
}

void alertOnFob (int ch, bool val)
{
	for (int i=0; i < NUM_OF_FOBS; i++)
		if (fobs[i].channel == ch) {
			fobs[i].alert = val;
			fobs[i].alert_target = alert_target_from_bool(val);
		}
}

void setFobAlertTarget (int ch, int val)
{
	for (int i=0; i < NUM_OF_FOBS; i++)
		if (fobs[i].channel == ch) {
			fobs[i].alert_target = alert_target_normalize(val, fobs[i].alert);
			fobs[i].alert = fobs[i].alert_target != ALERT_TARGET_NONE;
		}
}

void setFobChannelMask (int ch, int val)
{
	if (val <= 0 || val > 3) return;
	for (int i=0; i < NUM_OF_FOBS; i++)
		if (fobs[i].channel == ch) fobs[i].channel_mask = val;
}

static void test_fob_signal(struct fob *fb)
{
	if (!fb) return;
	const char *mode = fob_current_mode(fb);
	if (strcmp(mode, "latch") == 0) {
		ESP_LOGI(TAG, "Fob %d test - latch active", fb->channel);
		fob_apply_locks(fb, true, "fob_test", true);
		enableExit(fb->channel, true);
		enableKeypad(fb->channel, true);
	} else if (strcmp(mode, "toggle") == 0) {
		fb->toggleState = !fb->toggleState;
		ESP_LOGI(TAG, "Fob %d test toggled lock to %s", fb->channel, fb->toggleState ? "armed" : "disarmed");
		fob_apply_locks(fb, fb->toggleState, "fob_test", true);
		start_fob_timer(fb, false);
	} else {
		ESP_LOGI(TAG, "Fob %d test - disarming lock", fb->channel);
		fob_apply_locks(fb, false, "fob_test", true);
		start_fob_timer(fb, true);
	}
}


int storeFobSettings()
{

	for (uint8_t i=0; i < NUM_OF_FOBS; i++) {
		char type[25] = "";
		strcpy(type, fobs[i].type);
		sprintf(fobs[i].settings,
			"{\"eventType\":\"%s\", "
			"\"payload\":{\"channel\":%d, \"enable\": %s, \"alert\": %s, \"alert_target\": \"%s\", \"channel_mask\": %d, \"delay\": %d, \"latch\": %s, \"mode\": \"%s\"}}",
			type,
			i+1,
			(fobs[i].enable) ? "true" : "false",
			(fobs[i].alert) ? "true" : "false",
			alert_target_to_string(fobs[i].alert_target),
			fobs[i].channel_mask,
			fobs[i].delay,
			(fobs[i].latch) ? "true" : "false",
			fob_current_mode(&fobs[i]));

		sprintf(fobs[i].key, "%s%d", type, i);
		storeSetting(fobs[i].key, cJSON_Parse(fobs[i].settings));
		// printf("storeFobSettings\t%s\n", fobs[i].settings);
	}
  return 0;
}

int restoreFobSettings()
{
	for (uint8_t i=0; i < NUM_OF_FOBS; i++) {
		char type[25] = "";
		strcpy(type, fobs[i].type);
		sprintf(fobs[i].key, "%s%d", type, i);
		restoreSetting(fobs[i].key);
	}
	return 0;
}

void sendFobState(void) {
    for (int i=0; i < NUM_OF_FOBS; i++) {
        if (strlen(fobs[i].settings) > 2) {
            cJSON *json_msg = cJSON_Parse(fobs[i].settings);
            addClientMessageToQueue(json_msg);
            cJSON_Delete(json_msg);
        }
    }
}

cJSON *fob_state_snapshot(void) {
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        return NULL;
    }

    for (int i = 0; i < NUM_OF_FOBS; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry) {
            continue;
        }
        cJSON_AddNumberToObject(entry, "channel", fobs[i].channel);
        cJSON_AddBoolToObject(entry, "enable", fobs[i].enable);
        cJSON_AddBoolToObject(entry, "alert", fobs[i].alert);
        cJSON_AddStringToObject(entry, "alert_target", alert_target_to_string(fobs[i].alert_target));
        cJSON_AddNumberToObject(entry, "channel_mask", fobs[i].channel_mask);
        cJSON_AddNumberToObject(entry, "delay", fobs[i].delay);
        cJSON_AddBoolToObject(entry, "latch", fobs[i].latch);
        cJSON_AddStringToObject(entry, "mode", fob_current_mode(&fobs[i]));
        cJSON_AddBoolToObject(entry, "signal", fobs[i].isPressed);
        cJSON_AddItemToArray(array, entry);
    }

    return array;
}

void handle_fob_message(cJSON * payload)
{
	if (payload == NULL) return;

	int ch = 0;
	bool val = false;

	if (cJSON_GetObjectItem(payload,"getState")) {
		sendFobState();
	}

	if (cJSON_GetObjectItem(payload,"channel")) {
		ch = cJSON_GetObjectItem(payload,"channel")->valueint;
		if (ch < 1 || ch > NUM_OF_FOBS) {
			cJSON_Delete(payload);
			return;
		}

		if (cJSON_IsTrue(cJSON_GetObjectItem(payload, "test"))) {
			test_fob_signal(&fobs[ch - 1]);
		}

		if (cJSON_GetObjectItem(payload,"enable")) {
			val = fob_json_bool(cJSON_GetObjectItem(payload,"enable"));
			fobs[ch - 1].enable = val;
		}

		if (cJSON_GetObjectItem(payload,"alert")) {
			val = fob_json_bool(cJSON_GetObjectItem(payload,"alert"));
			alertOnFob(ch, val);
		}

		cJSON *alert_target = cJSON_GetObjectItem(payload, "alert_target");
		if (cJSON_IsString(alert_target) && alert_target->valuestring) {
			setFobAlertTarget(ch, alert_target_from_string(alert_target->valuestring, fobs[ch - 1].alert));
		} else if (cJSON_IsNumber(alert_target)) {
			setFobAlertTarget(ch, alert_target->valueint);
		}

		if (cJSON_GetObjectItem(payload,"channel_mask")) {
			setFobChannelMask(ch, fob_json_int(cJSON_GetObjectItem(payload,"channel_mask"), fobs[ch - 1].channel_mask));
		}

		if (cJSON_GetObjectItem(payload,"delay")) {
			fobs[ch - 1].delay = cJSON_GetObjectItem(payload,"delay")->valueint;
		}

		if (cJSON_GetObjectItem(payload,"latch")) {
			val = fob_json_bool(cJSON_GetObjectItem(payload,"latch"));
			fob_set_mode(&fobs[ch - 1], fob_mode_from_latch(val));
		}

		cJSON *mode = cJSON_GetObjectItem(payload, "mode");
		if (cJSON_IsString(mode) && mode->valuestring) {
			fob_set_mode(&fobs[ch - 1], mode->valuestring);
		}
		scheduleFobSettingsStore();
	}

	cJSON_Delete(payload);
}

static void
fob_service (void *pvParameter)
{
  // load_fob_state_from_flash();

  while (1) {
		for (int i=0; i < NUM_OF_FOBS; i++)
			check_fobs(&fobs[i]);

		                handle_fob_message(checkServiceMessageByType("fob"));
		flushFobSettingsIfDue();
    vTaskDelay(SERVICE_LOOP / portTICK_PERIOD_MS);
  }
}

void fob_main()
{
  printf("Starting fob service.\n");

	fobs[0].pin = FOB_MCP_IO_1;
	fobs[0].delay = 4;
	fobs[0].channel = 1;
	fobs[0].channel_mask = 1;
	fobs[0].enable = false;
	fobs[0].alert = true;
	fobs[0].alert_target = ALERT_TARGET_BOTH;
	fobs[0].latch = false;  // Default to momentary mode
	fobs[0].toggleState = false;
	fob_set_mode(&fobs[0], "momentary");
	strcpy(fobs[0].type, "fob");

	fobs[1].pin = FOB_MCP_IO_2;
	fobs[1].delay = 4;
	fobs[1].channel = 2;
	fobs[1].channel_mask = 2;
	fobs[1].enable = false;
	fobs[1].alert = true;
	fobs[1].alert_target = ALERT_TARGET_BOTH;
	fobs[1].latch = false;  // Default to momentary mode
	fobs[1].toggleState = false;
	fob_set_mode(&fobs[1], "momentary");
	strcpy(fobs[1].type, "fob");

	if (USE_MCP23017) {
		set_mcp_io_dir(fobs[0].pin, MCP_INPUT);
		set_mcp_io_dir(fobs[1].pin, MCP_INPUT);
	} else {
		gpio_set_direction(fobs[0].pin, GPIO_MODE_INPUT);
		gpio_set_direction(fobs[1].pin, GPIO_MODE_INPUT);
	}

  	xTaskCreate(fob_timer, "fob_timer", 4096, NULL, 10, NULL);
	xTaskCreate(fob_service, "fob_service", 5000, NULL, 10, NULL);

	restoreFobSettings();
}
