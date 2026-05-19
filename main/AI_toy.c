#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_bt.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include <esp_psram.h>
#include "app_wifi_config.h"
#include "websocket.h"
#include "app_sntp.h"
#include "app_aliyun_mqtt.h"
#include "esp_board_init.h"
#include "app_sr.h"
#include "esp_spiffs.h"
#include "audio.h"
#include "app_health.h"
#include "app_RFID.h"
#include "event.h"
#include "app_task_list.h"
#include "app_camera.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // health_init();
    esp_board_init();
    init_camera();
    RFID_start();
    wifi_init();
    // app_sntp_init();
    // app_aliyun_mqtt_init();
    ESP_ERROR_CHECK(app_sr_start());
    ESP_ERROR_CHECK(audio_init());
    ws_start("wss://10.167.90.44:8765");
    // update_value();
    // Update_Nearest_Task();
    event_start();
    // clear_all_tasks();
    vTaskDelay(5000 / portTICK_PERIOD_MS);

    while (1)
    {
        vTaskDelay(50000 / portTICK_PERIOD_MS);
        // send_camera_image();
        // print_all_tasks();
        // update_value();
        ESP_LOGI(TAG, "Free memory after start: %d bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        ESP_LOGI(TAG, "Free PSRAM heap: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
