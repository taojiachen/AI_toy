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

/**
 * 将图片帧缓冲进行 Base64 编码并分块打印到串口
 * @param fb 摄像头帧缓冲指针（调用者需确保非空）
 */
// static void print_frame_as_base64(camera_fb_t *fb)
// {
//     if (!fb || !fb->buf)
//     {
//         ESP_LOGE(TAG, "Invalid frame buffer");
//         return;
//     }

//     size_t base64_len = 4 * ((fb->len + 2) / 3);
//     size_t dlen = base64_len + 16;
//     char *base64_buf = malloc(dlen + 1);
//     if (!base64_buf)
//     {
//         ESP_LOGE(TAG, "Failed to allocate base64 buffer");
//         return;
//     }

//     size_t out_len = 0;
//     int ret = mbedtls_base64_encode((unsigned char *)base64_buf, dlen,
//                                     &out_len, fb->buf, fb->len);
//     if (ret != 0)
//     {
//         ESP_LOGE(TAG, "Base64 encode failed, ret=%d", ret);
//         free(base64_buf);
//         return;
//     }
//     base64_buf[out_len] = '\0';

//     const size_t CHUNK_SIZE = 512;
//     for (size_t i = 0; i < out_len; i += CHUNK_SIZE)
//     {
//         size_t len = (i + CHUNK_SIZE < out_len) ? CHUNK_SIZE : (out_len - i);
//         printf("%.*s", len, base64_buf + i);
//         vTaskDelay(1); // 关键：喂看门狗并让出 CPU
//     }
//     printf("\n");

//     free(base64_buf);
// }

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    health_init();
    esp_board_init();
    init_camera();
    i2c_bus_init();
    RFID_start();
    wifi_init();
    app_sntp_init();
    app_aliyun_mqtt_init();
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
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        // send_camera_image();
        // print_all_tasks();
        // update_value();
        ESP_LOGI(TAG, "Free memory after start: %d bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        ESP_LOGI(TAG, "Free PSRAM heap: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
