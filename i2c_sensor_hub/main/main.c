/* i2c - Example

   For other examples please check:
   https://github.com/espressif/esp-idf/tree/master/examples

   See README.md file to get detailed usage of this example.

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// WIFI
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_event.h"

// I2C
#include "driver/i2c.h"
#include "sdkconfig.h"

#define NO_DATA_TIMEOUT_SEC 5

static const char *TAG = "i2c_sensor_hub";

#define _I2C_NUMBER(num) I2C_NUM_##num
#define I2C_NUMBER(num) _I2C_NUMBER(num)

#define DATA_LENGTH 512                  /*!< Data buffer length of test buffer */
#define RW_TEST_LENGTH 128               /*!< Data length for r/w test, [0,DATA_LENGTH] */
#define DELAY_TIME_BETWEEN_ITEMS_MS 1000 /*!< delay time between different test items */

#define I2C_RESPONDER_SCL_IO CONFIG_I2C_RESPONDER_SCL               /*!< gpio number for i2c RESPONDER clock */
#define I2C_RESPONDER_SDA_IO CONFIG_I2C_RESPONDER_SDA               /*!< gpio number for i2c RESPONDER data */
#define I2C_RESPONDER_NUM I2C_NUMBER(CONFIG_I2C_RESPONDER_PORT_NUM) /*!< I2C port number for RESPONDER dev */
#define I2C_RESPONDER_TX_BUF_LEN (2 * DATA_LENGTH)              /*!< I2C RESPONDER tx buffer size */
#define I2C_RESPONDER_RX_BUF_LEN (2 * DATA_LENGTH)              /*!< I2C RESPONDER rx buffer size */

#define I2C_CONTROLLER_SCL_IO CONFIG_I2C_CONTROLLER_SCL               /*!< gpio number for I2C CONTROLLER clock */
#define I2C_CONTROLLER_SDA_IO CONFIG_I2C_CONTROLLER_SDA               /*!< gpio number for I2C CONTROLLER data  */
#define I2C_CONTROLLER_NUM I2C_NUMBER(CONFIG_I2C_CONTROLLER_PORT_NUM) /*!< I2C port number for CONTROLLER dev */
#define I2C_CONTROLLER_FREQ_HZ CONFIG_I2C_CONTROLLER_FREQUENCY        /*!< I2C CONTROLLER clock frequency */
#define I2C_CONTROLLER_TX_BUF_DISABLE 0                           /*!< I2C CONTROLLER doesn't need buffer */
#define I2C_CONTROLLER_RX_BUF_DISABLE 0                           /*!< I2C CONTROLLER doesn't need buffer */

#define BH1750_CMD_START CONFIG_BH1750_OPMODE   /*!< Operation mode */
#define ESP_RESPONDER_ADDR CONFIG_I2C_RESPONDER_ADDRESS /*!< ESP32 RESPONDER address, you can set any 7bit value */
#define WRITE_BIT I2C_CONTROLLER_WRITE              /*!< I2C CONTROLLER write */
#define READ_BIT I2C_CONTROLLER_READ                /*!< I2C CONTROLLER read */
#define ACK_CHECK_EN 0x1                        /*!< I2C CONTROLLER will check ack from RESPONDER*/
#define ACK_CHECK_DIS 0x0                       /*!< I2C CONTROLLER will not check ack from RESPONDER */
#define ACK_VAL 0x0                             /*!< I2C ack value */
#define NACK_VAL 0x1                            /*!< I2C nack value */

#define MCP9808_SENSOR_ADDR CONFIG_MCP9808_ADDR

// idk a queue
static QueueHandle_t sensor_queue;
static QueueHandle_t websocket_queue;

//WIFI
static TimerHandle_t shutdown_signal_timer;
static SemaphoreHandle_t shutdown_sema;

static void shutdown_signaler(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "No data received for %d seconds, signaling shutdown", NO_DATA_TIMEOUT_SEC);
    xSemaphoreGive(shutdown_sema);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        break;
    case WEBSOCKET_EVENT_DATA:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DATA");
        ESP_LOGI(TAG, "Received opcode=%d", data->op_code);
        if (data->op_code == 0x08 && data->data_len == 2) {
            ESP_LOGW(TAG, "Received closed message with code=%d", 256*data->data_ptr[0] + data->data_ptr[1]);
        } else {
            ESP_LOGW(TAG, "Received=%.*s", data->data_len, (char *)data->data_ptr);
            xQueueSendToBack(websocket_queue, &data, portMAX_DELAY); //TODO look into different delays
        }
        ESP_LOGW(TAG, "Total payload length=%d, data_len=%d, current payload offset=%d\r\n", data->payload_len, data->data_len, data->payload_offset);

        xTimerReset(shutdown_signal_timer, portMAX_DELAY);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_ERROR");
        break;
    }
}

static void websocket_app_start(void)
{
    esp_websocket_client_config_t websocket_cfg = {};

    shutdown_signal_timer = xTimerCreate("Websocket shutdown timer", NO_DATA_TIMEOUT_SEC * 1000 / portTICK_PERIOD_MS,
                                         pdFALSE, NULL, shutdown_signaler);
    shutdown_sema = xSemaphoreCreateBinary();

    websocket_cfg.uri = CONFIG_WEBSOCKET_URI;

    ESP_LOGI(TAG, "Connecting to %s...", websocket_cfg.uri);

    esp_websocket_client_handle_t client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);
    esp_websocket_client_start(client);

    xTimerStart(shutdown_signal_timer, portMAX_DELAY);
    char data[32];
    char websocket_buffer[32] = "";
    float sensor_buffer = 0;
    const char name[] = "ESP32";
    while (1) {
    	if (xQueueReceive(websocket_queue, &websocket_buffer, portMAX_DELAY) == 1) {
    		int len = sprintf(data, "%s Response:", name);
    		ESP_LOGI(TAG, "websocket_buffer %s", websocket_buffer);
    		esp_websocket_client_send_text(client, data, len, portMAX_DELAY);
    	}

    	if (xQueueReceive(sensor_queue, &sensor_buffer, portMAX_DELAY) == 1) {
            int len = sprintf(data, "received %d", (int) sensor_buffer);
            ESP_LOGI(TAG, "Sending %s", data);
            esp_websocket_client_send_text(client, data, len, portMAX_DELAY);
    	}

    	if (esp_websocket_client_is_connected(client)) {
            int len = sprintf(data, "hello %04d", 3);
            ESP_LOGI(TAG, "Sending %s", data);
            esp_websocket_client_send_text(client, data, len, portMAX_DELAY);
        }
        vTaskDelay(1000 / portTICK_RATE_MS);
    }

    xSemaphoreTake(shutdown_sema, portMAX_DELAY);
    esp_websocket_client_close(client, portMAX_DELAY);
    ESP_LOGI(TAG, "Websocket Stopped");
    esp_websocket_client_destroy(client);
}


// I2C
SemaphoreHandle_t print_mux = NULL;

static esp_err_t i2c_controller_init(void)
{
    int i2c_controller_port = I2C_CONTROLLER_NUM;
    i2c_config_t conf = {
        .mode = I2C_MODE_CONTROLLER,
        .sda_io_num = I2C_CONTROLLER_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_CONTROLLER_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .controller.clk_speed = I2C_CONTROLLER_FREQ_HZ,
        // .clk_flags = 0,          /*!< Optional, you can use I2C_SCLK_SRC_FLAG_* flags to choose i2c source clock here. */
    };
    esp_err_t err = i2c_param_config(i2c_controller_port, &conf);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_driver_install(i2c_controller_port, conf.mode, I2C_CONTROLLER_RX_BUF_DISABLE, I2C_CONTROLLER_TX_BUF_DISABLE, 0);
}

static esp_err_t i2c_controller_sensor_test(i2c_port_t i2c_num, uint8_t *data_h, uint8_t *data_l)
{
    int ret;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
//    i2c_controller_start(cmd);
//    i2c_controller_write_byte(cmd, MCP9808_SENSOR_ADDR << 1 | WRITE_BIT, ACK_CHECK_EN);
//    i2c_controller_write_byte(cmd, BH1750_CMD_START, ACK_CHECK_EN);
//    i2c_controller_stop(cmd);
//    ret = i2c_controller_cmd_begin(i2c_num, cmd, 1000 / portTICK_RATE_MS);
//    i2c_cmd_link_delete(cmd);
//    if (ret != ESP_OK) {
//        return ret;
//    }
    vTaskDelay(30 / portTICK_RATE_MS);
    cmd = i2c_cmd_link_create();
    i2c_controller_start(cmd);
    i2c_controller_write_byte(cmd, MCP9808_SENSOR_ADDR << 1 | READ_BIT, ACK_CHECK_EN);
    i2c_controller_read_byte(cmd, data_h, ACK_VAL);
    i2c_controller_read_byte(cmd, data_l, NACK_VAL);
    i2c_controller_stop(cmd);
    ret = i2c_controller_cmd_begin(i2c_num, cmd, 1000 / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void i2c_task(void *arg)
{
	int ret;
	uint32_t task_idx = (uint32_t)arg;
	float data = 0;
    uint8_t sensor_data_h, sensor_data_l;
    int cnt = 0;

    while (1) {
    	ESP_LOGI(TAG, "TASK[%d] test cnt: %d", task_idx, cnt++);
    	ret = i2c_controller_sensor_test(I2C_CONTROLLER_NUM, &sensor_data_h, &sensor_data_l);
    	xSemaphoreTake(print_mux, portMAX_DELAY);
    	if (ret == ESP_ERR_TIMEOUT) {
    	            ESP_LOGE(TAG, "I2C Timeout");
    	} else if (ret == ESP_OK) {
    		data = (sensor_data_h << 8 | sensor_data_l) / 1.2;
    		xQueueSendToBack(sensor_queue, &data, portMAX_DELAY); //TODO look into different delays
    		printf("*******************\n");
            printf("TASK[%d]  CONTROLLER READ SENSOR( MCP9808 )\n", task_idx);
            printf("*******************\n");
            printf("data_h: %02x\n", sensor_data_h);
            printf("data_l: %02x\n", sensor_data_l);
            printf("sensor val: %.02f [units]\n", (sensor_data_h << 8 | sensor_data_l) / 1.2);
    	}  else {
            ESP_LOGW(TAG, "%s: No ack, sensor not connected...skip...", esp_err_to_name(ret));
        }
    	xSemaphoreGive(print_mux);
    	vTaskDelay((DELAY_TIME_BETWEEN_ITEMS_MS * (task_idx + 1)) / portTICK_RATE_MS);
    }
    vSemaphoreDelete(print_mux);
    vTaskDelete(NULL);
}


static void wifi_task(void *arg)
{
    websocket_app_start();
    vTaskDelete(NULL);
}

void app_main(void)
{
	sensor_queue = xQueueCreate(2, sizeof(int));
	websocket_queue = xQueueCreate(2, sizeof(char)*32);
    //WIFI
	ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("WEBSOCKET_CLIENT", ESP_LOG_DEBUG);
    esp_log_level_set("TRANSPORT_WS", ESP_LOG_DEBUG);
    esp_log_level_set("TRANS_TCP", ESP_LOG_DEBUG);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    //I2C
    print_mux = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(i2c_controller_init());

    // Starting Tasks
    xTaskCreate(i2c_task, "i2c_task_0", 1024 * 2, (void *)0, 10, NULL);
    xTaskCreate(wifi_task, "wifi_task_0", 1024 * 2, (void *)0, 10, NULL);

}
