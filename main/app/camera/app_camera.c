/**
 * This example takes a picture every 5s and print its size on serial monitor.
 */

// =============================== SETUP ======================================

// 1. Board setup (Uncomment):
// #define BOARD_WROVER_KIT
// #define BOARD_ESP32CAM_AITHINKER
// #define BOARD_ESP32S3_WROOM
// #define BOARD_ESP32S3_XIAO
// #define BOARD_ESP32S3_GOOUUU
// #define BOARD_ESP32S3_XIAO

/**
 * 2. Kconfig setup
 *
 * If you have a Kconfig file, copy the content from
 *  https://github.com/espressif/esp32-camera/blob/master/Kconfig into it.
 * In case you haven't, copy and paste this Kconfig file inside the src directory.
 * This Kconfig file has definitions that allows more control over the camera and
 * how it will be initialized.
 */

/**
 * 3. Enable PSRAM on sdkconfig:
 *
 * CONFIG_ESP32_SPIRAM_SUPPORT=y
 *
 * More info on
 * https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/kconfig.html#config-esp32-spiram-support
 */

// ================================ CODE ======================================

#include "sdkconfig.h"

#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// support IDF 5.x
#ifndef portTICK_RATE_MS
#define portTICK_RATE_MS portTICK_PERIOD_MS
#endif

#if defined(CONFIG_CAMERA_AF_SUPPORT) && CONFIG_CAMERA_AF_SUPPORT
#include "esp_camera_af.h"
#endif

#define BOARD_ESP32S3_AI_TOY 1

#include "camera_pinout.h"
#include "app_camera.h"
#include "img_converters.h" // 提供 frame2jpg

#include "websocket.h" // 提供 ws_send_jpeg_binary
#include "esp_board_init.h"
#include "app_sr.h" // 提供 stop_audio_feed 和 start_audio_feed

static const char *TAG = "take_picture";

#if ESP_CAMERA_SUPPORTED
static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,

    // XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
    .xclk_freq_hz = 10000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_RGB565, // YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size = FRAMESIZE_QVGA,     // QQVGA-UXGA, For ESP32, do not use sizes above QVGA when not JPEG. The performance of the ESP32-S series has improved a lot, but JPEG mode always gives better frame rates.

    .jpeg_quality = 1, // 0-63, for OV series camera sensors, lower number means higher quality
    .fb_count = 1,      // When jpeg mode is used, if fb_count more than one, the driver will work in continuous mode.
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

esp_err_t init_camera(void)
{
    // initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera Init Failed");
        return err;
    }

    return ESP_OK;
}

esp_err_t deinit_camera(void)
{
    esp_err_t err = esp_camera_deinit();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera Deinit Failed");
        return err;
    }
    return ESP_OK;
}

#if defined(CONFIG_CAMERA_AF_SUPPORT) && CONFIG_CAMERA_AF_SUPPORT
static void maybe_init_autofocus(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s)
    {
        ESP_LOGW(TAG, "AF: no sensor handle");
        return;
    }

    if (!esp_camera_af_is_supported(s))
    {
        ESP_LOGI(TAG, "AF: not supported by this sensor");
        return;
    }

    esp_camera_af_config_t af_cfg = {
        .mode = ESP_CAMERA_AF_MODE_AUTO,
        .timeout_ms = CONFIG_CAMERA_AF_DEFAULT_TIMEOUT_MS,
    };

    esp_err_t ret = esp_camera_af_init(s, &af_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "AF init failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "AF initialized (AUTO mode)");
}
#endif
#endif

/**
 * 将 RGB565 帧转换为 JPEG 数据（动态分配内存）
 *
 * @param fb        摄像头帧（必须是 PIXFORMAT_RGB565）
 * @param quality   JPEG 质量 (0-100，推荐 80)
 * @param out_jpg   输出指针（函数内部分配，调用者需 free）
 * @param out_len   输出 JPEG 数据长度
 * @return          true 成功，false 失败
 */
bool convert_rgb565_to_jpeg(camera_fb_t *fb, uint8_t quality, uint8_t **out_jpg, size_t *out_len)
{
    if (!fb || fb->format != PIXFORMAT_RGB565)
    {
        ESP_LOGE("JPEG_CONV", "Invalid input: not RGB565");
        return false;
    }
    if (!out_jpg || !out_len)
    {
        return false;
    }

    // frame2jpg 会分配内存，调用者负责释放
    bool success = frame2jpg(fb, quality, out_jpg, out_len);
    if (!success)
    {
        ESP_LOGE("JPEG_CONV", "JPEG compression failed");
    }
    else
    {
        ESP_LOGI("JPEG_CONV", "Converted to JPEG, size: %zu bytes", *out_len);
    }
    return success;
}

camera_fb_t *take_picture(void)
{
    // init_camera();
    ESP_LOGI(TAG, "Taking picture...");
    camera_fb_t *pic = esp_camera_fb_get();
    if (pic)
    {
        ESP_LOGI(TAG, "Picture taken! RGB565 size: %zu bytes", pic->len);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to get picture");
    }
    // deinit_camera();
    return pic;
}

void send_camera_image(void)
{
    camera_fb_t *fb = take_picture();
    // 直接发送原始 JPEG（如果摄像头输出已经是 JPEG）
    if (fb->format == PIXFORMAT_JPEG)
    {
        if (ws_is_connected())
        {
            ws_send_jpeg_binary(fb->buf, fb->len);
        }
    }
    // 如果是 RGB565，先压缩为 JPEG
    else if (fb->format == PIXFORMAT_RGB565)
    {
        uint8_t *jpg_buf = NULL;
        size_t jpg_len = 0;
        if (frame2jpg(fb, 80, &jpg_buf, &jpg_len))
        {
            if (ws_is_connected())
            {
                ws_send_jpeg_binary(jpg_buf, jpg_len);
            }
            free(jpg_buf);
        }
        else
        {
            ESP_LOGE(TAG, "JPEG压缩失败");
        }
    }
    else
    {
        ESP_LOGW(TAG, "不支持的格式: %d", fb->format);
    }

    esp_camera_fb_return(fb);
}