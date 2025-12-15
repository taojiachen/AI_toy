#include "audio.h"
#include "audio_private.h"
#include "esp_spiffs.h"
#include "driver/i2s.h"
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "cJSON.h" // 添加JSON解析头文件

#include "app_sr.h"
#include "esp_board_init.h"

// 配置参数
#define PLAYBACK_TIMEOUT_MS 5000
#define SETTING_FILE_PATH "/spiffs/setting.json" // 设置文件路径
#define DEFAULT_MUSIC "turn_on"                  // 默认播放文件（无配置时）
#define MAX_MUSIC_NAME_LEN 64                    // 最大音乐名长度

#define BUFFER_SIZE (1024 * 2) // 环形缓冲区大小（字节）

// 公共缓冲区：存储OPUS编码后的字节数据
uint8_t opus_output_buffer[BUFFER_SIZE];
static size_t buffer_write_pos = 0; // 环形缓冲区写入位置（按字节计）

static const char *TAG = "audio";

TaskHandle_t audio_encoder_task_handle = NULL;
TaskHandle_t audio_decoder_task_handle = NULL;

QueueHandle_t ws_send_queue = NULL;

static SemaphoreHandle_t opus_buffer_mutex = NULL;

extern QueueHandle_t audio_encode_queue;

extern void decoder_ops_register(audio_decoder_t *decoder);
extern void encoder_ops_register(audio_encoder_t *encoder);

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
    decoder_ops_register(decoder);

    if (!decoder->init || !decoder->decode_frame || !decoder->deinit)
    {
        ESP_LOGE(TAG, "Incomplete decoder implementation");
        free(decoder);
        return NULL;
    }

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
 * @brief 核心播放函数：播放指定名称的MP3文件（自动拼接路径）
 */
static void audio_play_file(const char *music_name)
{
    if (!music_name || strlen(music_name) == 0)
    {
        ESP_LOGE(TAG, "Invalid music name");
        return;
    }

    // 拼接完整路径：/spiffs/xxx.mp3
    char music_path[128] = {0};
    snprintf(music_path, sizeof(music_path), "/spiffs/%s.mp3", music_name);

    audio_decoder_t *decoder = NULL;
    FILE *file = NULL;
    int16_t decode_buffer[1152 * 2];
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

    // 3. 打开MP3文件
    file = fopen(music_path, "rb");
    if (!file)
    {
        ESP_LOGE(TAG, "Failed to open music file: %s", music_path);
        goto cleanup;
    }
    ESP_LOGI(TAG, "Successfully opened music file: %s", music_path);

    // 4. 循环解码播放
    while (playback_active)
    {
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
                esp_err_t i2s_ret = esp_i2s_write(
                    decode_buffer,
                    samples_decoded * 2 // 采样点转字节数（int16_t占2字节）
                );
                if (i2s_ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "I2S write failed: %d", i2s_ret);
                    playback_active = false;
                }
            }
            break;

        case DECODER_HEADER_ONLY:
            ESP_LOGI(TAG, "Skipping audio header");
            break;

        case DECODER_EOF:
            ESP_LOGI(TAG, "Reached end of music file");
            playback_active = false;
            break;

        case DECODER_ERROR:
        default:
            ESP_LOGE(TAG, "Decode error: %d", result);
            playback_active = false;
            break;
        }

        vTaskDelay(20 / portTICK_PERIOD_MS);
    }

cleanup:
    if (file != NULL)
        fclose(file);
    audio_decoder_deinit(decoder);
    ESP_LOGI(TAG, "Music playback finished");
}

/**
 * @brief 从公共缓冲区读取指定长度的OPUS数据（供其他文件调用）
 * @param out_buf 输出缓冲区（存储读取到的OPUS数据）
 * @param req_len 需要读取的字节数（从ws_send_queue获取的encoded_bytes）
 * @return ESP_OK: 读取成功; ESP_FAIL: 数据不足/参数错误
 */
esp_err_t audio_get_opus_data(uint8_t *out_buf, uint16_t req_len)
{
    // 参数校验
    if (!out_buf || req_len == 0)
    {
        ESP_LOGE(TAG, "Invalid read param: req_len=%u", req_len);
        return ESP_FAIL;
    }

    // ========== 加锁：访问缓冲区前获取互斥锁 ==========
    if (xSemaphoreTake(opus_buffer_mutex, pdMS_TO_TICKS(10)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take buffer mutex (reader)");
        return ESP_FAIL;
    }

    // 二次校验：读取长度不能超过已存储数据
    if (req_len > buffer_write_pos)
    {
        ESP_LOGE(TAG, "Read failed: req_len=%u, stored_len=%u", req_len, buffer_write_pos);
        xSemaphoreGive(opus_buffer_mutex); // 解锁
        return ESP_FAIL;
    }

    // 读取数据 + 挪动缓冲区
    memcpy(out_buf, opus_output_buffer, req_len);
    size_t remain_bytes = buffer_write_pos - req_len;
    if (remain_bytes > 0)
    {
        memmove(opus_output_buffer, opus_output_buffer + req_len, remain_bytes);
    }
    buffer_write_pos = remain_bytes;
    ESP_LOGD(TAG, "Read OPUS data: %u bytes, remain: %u bytes", req_len, remain_bytes);

    // ========== 解锁：释放互斥锁 ==========
    xSemaphoreGive(opus_buffer_mutex);

    return ESP_OK;
}

/**
 * @brief 注册并初始化音频编码器
 */
static audio_encoder_t *audio_encoder_register(void)
{
    audio_encoder_t *encoder = malloc(sizeof(audio_encoder_t));
    if (!encoder)
    {
        ESP_LOGE(TAG, "Failed to allocate encoder memory");
        return NULL;
    }

    memset(encoder, 0, sizeof(audio_encoder_t));
    encoder_ops_register(encoder);

    if (!encoder->init || !encoder->encode_frame || !encoder->deinit)
    {
        ESP_LOGE(TAG, "Incomplete encoder implementation");
        free(encoder);
        return NULL;
    }

    return encoder;
}

/**
 * @brief 释放编码器资源
 */
static void audio_encoder_deinit(audio_encoder_t *encoder)
{
    if (encoder)
    {
        if (encoder->deinit)
        {
            encoder->deinit(encoder);
        }
        free(encoder);
    }
}

/**
 * @brief 音频播放任务（动态读取配置文件）
 */
void audio_decoder_task(void *pvParameters)
{
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void audio_encoder_task(void *pvParameters)
{
    BaseType_t xStatus;
    int16_t pcm_buf[960] = {0};
    uint8_t opus_buf[300] = {0};
    uint16_t encoded_bytes = 0;
    audio_encoder_t *encoder = NULL;

    // 前置校验：队列句柄不能为空（避免空指针调用）
    if (audio_encode_queue == NULL || ws_send_queue == NULL)
    {
        ESP_LOGE(TAG, "Fatal error: queue handle is NULL");
        return;
    }

    // 1. 注册编码器
    encoder = audio_encoder_register();
    if (!encoder)
    {
        ESP_LOGE(TAG, "Failed to register encoder");
        return;
    }

    // 2. 初始化编码器
    if (encoder->init(encoder) != ENCODER_OK)
    {
        ESP_LOGE(TAG, "Encoder initialization failed");
        audio_encoder_deinit(encoder);
        return;
    }

    for (;;)
    {
        // 从队列取PCM帧：永久阻塞（直到队列有数据）
        xStatus = xQueueReceive(
            audio_encode_queue, // 目标队列句柄
            &pcm_buf,           // 接收缓冲区（存储取出的帧）
            portMAX_DELAY       // 永久阻塞（无超时）
        );

        // 处理取数结果
        if (xStatus == pdPASS)
        {
            // 调用你的opus_encode_frame编码PCM数据
            encoder_result_t ret = encoder->encode_frame(
                encoder,          // 编码器实例
                pcm_buf,          // PCM输入数据
                960,              // 采样数(样本数)
                opus_buf,         // OPUS输出缓冲区
                sizeof(opus_buf), // 缓冲区大小
                &encoded_bytes    // 实际编码字节数
            );

            if (ret == ENCODER_OK && encoded_bytes > 0)
            {
                // ESP_LOGI("ENCODE", "Encoded PCM to OPUS: %u", encoded_bytes);
                // 编码成功后，可将opus_buf通过WebSocket发送等
                // ========== 加锁：访问缓冲区前获取互斥锁 ==========
                if (xSemaphoreTake(opus_buffer_mutex, pdMS_TO_TICKS(10)) != pdTRUE)
                {
                    ESP_LOGE(TAG, "Failed to take buffer mutex (encoder)");
                    continue; // 拿不到锁，丢弃当前帧
                }
                // ========== 核心逻辑：检查缓冲区剩余空间 ==========
                size_t remain_space = BUFFER_SIZE - buffer_write_pos;
                if (encoded_bytes > remain_space)
                {
                    // 剩余空间不足，丢弃当前帧
                    ESP_LOGW(TAG, "Buffer full! Need %u bytes, remain %u bytes",
                             encoded_bytes, remain_space);
                    xSemaphoreGive(opus_buffer_mutex); // 解锁
                    continue;
                }
                // 空间足够：拷贝OPUS数据到公共缓冲区
                memcpy(opus_output_buffer + buffer_write_pos, opus_buf, encoded_bytes);

                // 更新写入位置
                buffer_write_pos += encoded_bytes;
                ESP_LOGI(TAG, "Store OPUS to buffer: %u bytes, write_pos=%u",
                         encoded_bytes, buffer_write_pos);

                // 发送encoded_bytes到ws_send_queue（非阻塞，避免任务卡死）
                BaseType_t send_ret = xQueueSend(
                    ws_send_queue,
                    &encoded_bytes,
                    0 // 0超时=非阻塞，队列满则丢弃
                );
                if (send_ret != pdPASS)
                {
                    ESP_LOGE(TAG, "ws_send_queue full! Drop frame");
                    // 队列满时回滚写入位置（丢弃当前帧，避免缓冲区数据无效）
                    buffer_write_pos -= encoded_bytes;
                }
                // ========== 解锁：释放互斥锁 ==========
                xSemaphoreGive(opus_buffer_mutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t audio_init()
{
    // 1. 初始化公共缓冲区
    memset(opus_output_buffer, 0, BUFFER_SIZE);
    buffer_write_pos = 0;

    // 2. 创建互斥锁（核心：保护缓冲区）
    opus_buffer_mutex = xSemaphoreCreateMutex();
    if (opus_buffer_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create opus buffer mutex");
        return ESP_FAIL;
    }

    // 3. 创建消息队列
    ws_send_queue = xQueueCreate(20, sizeof(uint16_t));
    ESP_RETURN_ON_FALSE(ws_send_queue != NULL, ESP_FAIL, TAG, "Failed create ws send queue");

    BaseType_t ret_val = xTaskCreatePinnedToCore(
        audio_decoder_task,
        "audio_decoder_task",
        12 * 1024,
        NULL,
        3,
        &audio_decoder_task_handle,
        1);

    if (ret_val != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create audio decoder task");
    }
    else
    {
        ESP_LOGI(TAG, "Audio decoder task created successfully");
    }

    ret_val = xTaskCreatePinnedToCore(
        audio_encoder_task,
        "audio_encoder_task",
        12 * 1024,
        NULL,
        3,
        &audio_encoder_task_handle,
        1);

    if (ret_val != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create audio encoder task");
    }
    else
    {
        ESP_LOGI(TAG, "Audio encoder task created successfully");
    }

    return ESP_OK;
}