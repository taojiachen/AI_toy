#include "audio.h"
#include "audio_private.h"
#include "esp_spiffs.h"
#include "driver/i2s.h"
#include <string.h>
#include "esp_board_init.h"
#include "esp_log.h"

static const char *TAG = "audio";

// int16_t zero_buffer[4800 * 2] = {0};

// 配置参数：根据实际需求调整
#define PLAYBACK_TIMEOUT_MS 5000 // 播放超时时间

extern void decoder_ops_register(audio_decoder_t *decoder);

// // 弱定义解码器注册函数，由具体解码器实现覆盖
// __attribute__((weak)) void decoder_ops_register(audio_decoder_t *decoder)
// {
//     ESP_LOGE(TAG, "No decoder implementation registered!");
// }

/**
 * @brief 注册并初始化音频解码器
 */
static audio_decoder_t *audio_decoder_register(void)
{
    audio_decoder_t *decoder = malloc(sizeof(audio_decoder_t));
    if (!decoder)
    {
        ESP_LOGE(TAG, "Failed to allocate decoder memory");
        return NULL;
    }

    memset(decoder, 0, sizeof(audio_decoder_t));
    decoder_ops_register(decoder); // 绑定具体解码器实现

    // 检查解码器接口完整性
    if (!decoder->init || !decoder->decode_frame || !decoder->deinit)
    {
        ESP_LOGE(TAG, "Incomplete decoder implementation");
        free(decoder);
        return NULL;
    }
    // ESP_LOGE(TAG, "Decoder registered successfully");

    return decoder;
}

/**
 * @brief 释放解码器资源
 */
static void audio_decoder_deinit(audio_decoder_t *decoder)
{
    if (decoder)
    {
        if (decoder->deinit)
        {
            decoder->deinit(decoder);
        }
        free(decoder);
    }
}

/**
 * @brief 核心播放函数：读取SPiffs文件并通过I2S播放
 */
static void aud_player_file(const char *path)
{
    if (!path)
    {
        ESP_LOGE(TAG, "Invalid parameters");
        return;
    }
    audio_decoder_t *decoder = NULL;
    FILE *file = NULL;
    int16_t decode_buffer[960];
    uint32_t samples_decoded = 0;
    bool playback_active = true;
    // 1. 注册解码器
    decoder = audio_decoder_register();
    if (!decoder)
    {
        ESP_LOGE(TAG, "Failed to register decoder");
        return;
    }
    // 2. 初始化解码器
    if (decoder->init(decoder) != DECODER_OK)
    {
        ESP_LOGE(TAG, "Decoder initialization failed");
        goto cleanup;
    }
    // 3. 打开SPiffs文件
    file = fopen(path, "rb");
    if (!file)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        goto cleanup;
    }
    ESP_LOGI(TAG, "Successfully opened file: %s", path);

    while (playback_active)
    {
        // 解码一帧音频
        decoder_result_t result = decoder->decode_frame(
            decoder,
            file,
            decode_buffer,
            &samples_decoded);

        switch (result)
        {
        case DECODER_OK:
            if (samples_decoded > 0)
            {
                // 发送数据到I2S（注意：esp_i2s_write参数为字节数时需×2）
                esp_err_t i2s_ret = esp_i2s_write(
                    decode_buffer,
                    samples_decoded * 2 // 此处假设函数参数为采样点数，若为字节数需×2
                );
                if (i2s_ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "I2S write failed: %d", i2s_ret);
                    playback_active = false;
                }
            }
            break;

        case DECODER_HEADER_ONLY:
            // 跳过头部信息
            ESP_LOGI(TAG, "Skipping header data");
            break;

        case DECODER_EOF:
            ESP_LOGI(TAG, "Reached end of file");
            playback_active = false;
            break;

        case DECODER_ERROR:
        default:
            // ESP_LOGE(TAG, "Decode error: %d", result);
            playback_active = false;
            break;
        }
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }

cleanup:
    // 释放资源
    if (file != NULL)
    {
        fclose(file);
    }
    // esp_err_t i2s_ret = esp_i2s_write(
    //     zero_buffer,
    //     samples_decoded * 2 // 此处假设函数参数为采样点数，若为字节数需×2
    // );
    audio_decoder_deinit(decoder);
    ESP_LOGI(TAG, "Playback finished");
}

/**
 * @brief 设置音量（通过设备控制接口）
 */

/**
 * @brief 启动音频播放
 * @param dev: 音频设备结构体
 * @param src: SPiffs中的文件路径
 * @param volume: 播放音量
 */
void audio_task(void *pvParameters)
{
    while (1)
    {
        // 启动播放
        aud_player_file("/spiffs/turn_on.opus");
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

TaskHandle_t audio_task_handle = NULL;

void audio_init()
{
    BaseType_t ret_val = xTaskCreatePinnedToCore(audio_task, "audio_task", 10 * 1024, NULL, 3, &audio_task_handle, 1);
    // ESP_RETURN_ON_FALSE(pdPASS == ret_val, ESP_FAIL, TAG, "Failed create audio task");
}