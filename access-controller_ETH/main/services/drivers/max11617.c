
#define AIN0	0
#define AIN1	1
#define AIN2	2
#define AIN3	3
#define AIN4	4
#define AIN5	5
#define AIN6	6
#define AIN7	7
#define AIN8	8
#define AIN9	9
#define AIN10	10
#define AIN11	11

#define MAX11617_ADDR 0x35 /*!< ESP32 slave address, you can set any 7bit value */
#define ADC_CHANNELS 12                  /*!< Data buffer length of test buffer */
#define DATA_LENGTH 2                  /*!< Data buffer length of test buffer */
#define RW_TEST_LENGTH 128               /*!< Data length for r/w test, [0,DATA_LENGTH] */
#define DELAY_TIME_BETWEEN_ITEMS_MS 1000 /*!< delay time between different test items */

uint16_t MAX_ADC_VALUES[ADC_CHANNELS] = {0};

uint16_t * get_max_values()
{
	return MAX_ADC_VALUES;
}

static void max11617_task(void* arg)
{

	int i = 0;
	int ret;
	uint32_t task_idx = (uint32_t)arg;
	uint8_t *data = (uint8_t *)malloc(DATA_LENGTH);
	uint8_t *data_wr = (uint8_t *)malloc(DATA_LENGTH);
	uint8_t *data_rd = (uint8_t *)malloc(ADC_CHANNELS * 2);
	uint8_t sensor_data_h, sensor_data_l;
	int cnt = 0;
	while (1) {
			//------------------------ config ---------------------------//
			data_wr[0] = 0x82;
			data_wr[1] = 0x17;

			ret = i2c_master_write_slave(MAX11617_ADDR, I2C_MASTER_NUM, data_wr, 2);

			if (ret == ESP_ERR_TIMEOUT) {
					ESP_LOGE(TAG, "I2C Timeout");
			} else if (ret == ESP_OK) {
					// printf("Master write to slave\n");
			} else {
					ESP_LOGW(TAG, "TASK[%d] %s: Master write slave error, IO not connected...\n",
									 task_idx, esp_err_to_name(ret));
			}

			//------------------------- read -------------------------//
			ret = i2c_master_read_slave(MAX11617_ADDR, I2C_MASTER_NUM, data_rd, ADC_CHANNELS * 2);

			if (ret == ESP_ERR_TIMEOUT) {
					ESP_LOGE(TAG, "I2C Timeout");
			} else if (ret == ESP_OK) {
					for (int i=0,j=0; i < ADC_CHANNELS * 2; i+=2,j++) {
						uint16_t MSB = (data_rd[i] & 0x0f)<<8;
						uint16_t LSB = data_rd[i+1];
						MAX_ADC_VALUES[j] = MSB | LSB;
					}
			} else {
					ESP_LOGW(TAG, "TASK[%d] %s: Master read slave error, IO not connected...\n",
									 task_idx, esp_err_to_name(ret));
			}
			//--------------------------------------------------//

			vTaskDelay((DELAY_TIME_BETWEEN_ITEMS_MS * (task_idx + 1)) / portTICK_RATE_MS);
	}
	vTaskDelete(NULL);
}

void max11617_main(void)
{
	xTaskCreate(max11617_task, "max11617_task", 2048, NULL, 10, NULL);
}
