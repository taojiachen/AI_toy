#include "audio.h"
#include "esp_spiffs.h"
#include "driver/i2s.h"
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "cJSON.h" // 添加JSON解析头文件

#include "app_sr.h"
#include "esp_board_init.h"
#include "websocket.h"

#include "opus.h"

// 配置参数
#define PLAYBACK_TIMEOUT_MS 5000
#define SETTING_FILE_PATH "/spiffs/setting.json" // 设置文件路径
#define DEFAULT_MUSIC "turn_on"                  // 默认播放文件（无配置时）
#define MAX_MUSIC_NAME_LEN 64                    // 最大音乐名长度

#define BUFFER_SIZE (1024 * 3) // 环形缓冲区大小（字节）
#define WS_RECV_BUFFER_SIZE (480 * 40) // 环形缓冲区大小（字节）

// opus编码数据包大小
#define OPUS_ENCODED_PACKET_SIZE 300 // 每个OPUS编码包最大字节数

// 帧头长度定义（需放在函数外，确保可见）
#define FRAME_HEADER_SIZE 6                         // 帧头固定6字节
#define OPUS_DATA_MAX_LEN (OPUS_ENCODED_PACKET_SIZE - FRAME_HEADER_SIZE) // 预留帧头后，OPUS数据最大长度

// 公共缓冲区：存储OPUS编码后的字节数据
static uint8_t opus_output_buffer[BUFFER_SIZE] = {0};
static size_t buffer_write_pos = 0; // 环形缓冲区写入位置（按字节计）

// 公共缓冲区：存储websocket接收到的OPUS字节数据
static uint8_t ws_recv_opus_buffer[WS_RECV_BUFFER_SIZE] = {0};
static size_t ws_recv_buffer_write_pos = 0; // 环形缓冲区写入位置（按字节计）
static const char *TAG = "Audio";

TaskHandle_t audio_encoder_task_handle = NULL;
TaskHandle_t audio_decoder_test_task_handle = NULL;
TaskHandle_t audio_decoder_task_handle = NULL;

QueueHandle_t ws_send_queue = NULL;

static SemaphoreHandle_t opus_buffer_mutex = NULL;
static SemaphoreHandle_t ws_recv_opus_buffer_mutex = NULL;

extern QueueHandle_t audio_encode_queue;

extern void decoder_ops_register(audio_decoder_t *decoder);
extern void encoder_ops_register(audio_encoder_t *encoder);

static audio_decoder_t *decoder = NULL;

uint16_t audio_packet_seq = 0; // 音频包序列号

enum {
    AUDIO_EVENT_START = 0,  // 音频开始事件
    AUDIO_EVENT_END = 1,    // 音频结束事件
    AUDIO_EVENT_NONE = 2,   // 无音频事件
    AUDIO_EVENT_PLAYING =3  // 音频播放中
};

uint8_t audio_event = AUDIO_EVENT_START;

/**
 * @brief 注册并初始化音频解码器
 */
static audio_decoder_t *audio_decoder_register(void)
{
    decoder = malloc(sizeof(audio_decoder_t));
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

    int16_t decode_buffer[1152 * 2];
    uint32_t samples_decoded = 0;
    bool playback_active = true;
    data_source_t source;
    source.type = AUDIO_DATA_TYPE_FILE;
    source.data.file = NULL;

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
    source.data.file = fopen(music_path, "rb");
    if (!source.data.file)
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
            &source,
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
    if (source.data.file != NULL)
        fclose(source.data.file);
    audio_decoder_deinit(decoder);
    ESP_LOGI(TAG, "Music playback finished");
}

/**
 * @brief 从公共缓冲区读取指定长度的OPUS数据（供其他文件调用）
 * @param out_buf 输出缓冲区（存储读取到的OPUS数据）
 * @param req_len 需要读取的字节数（从ws_send_queue获取的encoded_bytes）
 * @return ESP_OK: 读取成功; ESP_FAIL: 数据不足/参数错误
 */
esp_err_t audio_get_opus_encode_data(uint8_t *out_buf, uint16_t req_len)
{
    // 参数校验
    if (!out_buf || req_len == 0)
    {
        ESP_LOGE(TAG, "Invalid read param: req_len=%u", req_len);
        return ESP_FAIL;
    }

    // ========== 加锁：访问缓冲区前获取互斥锁 ==========
    if (xSemaphoreTake(opus_buffer_mutex, pdMS_TO_TICKS(1)) != pdTRUE)
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
 * @brief 音频播放测试任务（自测i2s_rx->pcm->opus_encode->opus_decode->pcm->i2s_tx）
 */
void audio_decoder_test_task(void *pvParameters)
{
    BaseType_t xStatus;
    uint16_t packet_len = 0;
    uint8_t opus_packet_buffer[300] = {0}; // 存储从缓冲区读取的完整数据包（包含帧头）
    uint8_t *opus_data_ptr = NULL;         // 指向OPUS数据部分（跳过帧头）
    uint16_t opus_data_len = 0;            // OPUS数据长度（不含帧头）
    int16_t pcm_output[960] = {0};         // PCM输出缓冲区（16000Hz * 0.06s = 960采样点）
    int decoded_samples = 0;               // 解码得到的采样点数

    // OPUS解码器相关变量
    OpusDecoder *opus_decoder = NULL;
    int opus_error = 0;

    // 前置校验：队列句柄不能为空
    if (ws_send_queue == NULL)
    {
        ESP_LOGE(TAG, "Fatal error: ws_send_queue is NULL");
        return;
    }

    // 1. 创建OPUS解码器
    opus_decoder = opus_decoder_create(
        16000,      // 采样率：16kHz
        1,          // 声道数：单声道
        &opus_error // 错误码输出
    );

    if (opus_error != OPUS_OK || opus_decoder == NULL)
    {
        ESP_LOGE(TAG, "Failed to create OPUS decoder, error: %d", opus_error);
        return;
    }

    ESP_LOGI(TAG, "OPUS decoder created successfully (16kHz, mono, 16bit)");

    // 2. 主循环：等待队列数据并解码播放
    for (;;)
    {
        // 从ws_send_queue获取数据包长度（永久阻塞）
        xStatus = xQueueReceive(
            ws_send_queue, // 队列句柄
            &packet_len,   // 接收缓冲区（数据包总长度）
            portMAX_DELAY  // 永久阻塞
        );

        if (xStatus != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to receive from ws_send_queue");
            continue;
        }

        // 3. 从公共缓冲区读取完整数据包
        esp_err_t read_ret = audio_get_opus_encode_data(opus_packet_buffer, packet_len);
        if (read_ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to read OPUS packet from buffer");
            continue;
        }

        // printf("Received OPUS packet: %u bytes\n", packet_len);
        // for (int i = 0; i < packet_len; i++)
        // {
        //     printf("%02X ", opus_packet_buffer[i]);
        // }
        // printf("\n");

        // 4. 解析帧头并提取OPUS数据
        if (packet_len < FRAME_HEADER_SIZE)
        {
            ESP_LOGE(TAG, "Invalid packet: too short (%u bytes)", packet_len);
            continue;
        }

        // 验证帧头标识（0xAA55）
        if (opus_packet_buffer[0] != 0xAA || opus_packet_buffer[1] != 0x55)
        {
            ESP_LOGE(TAG, "Invalid frame header: 0x%02X%02X",
                     opus_packet_buffer[0], opus_packet_buffer[1]);
            continue;
        }

        // 提取包序号（用于调试，可选）
        uint16_t packet_seq = (opus_packet_buffer[2] << 8) | opus_packet_buffer[3];

        // 提取OPUS数据长度（大端序）
        opus_data_len = (opus_packet_buffer[4] << 8) | opus_packet_buffer[5];

        // 校验OPUS数据长度
        if (opus_data_len == 0 || opus_data_len > (packet_len - FRAME_HEADER_SIZE))
        {
            ESP_LOGE(TAG, "Invalid OPUS data length: %u (packet_len=%u)",
                     opus_data_len, packet_len);
            continue;
        }

        // 指向OPUS数据部分（跳过6字节帧头）
        opus_data_ptr = opus_packet_buffer + FRAME_HEADER_SIZE;

        ESP_LOGD(TAG, "Received OPUS packet: seq=%u, opus_len=%u", packet_seq, opus_data_len);

        // 5. 调用OPUS解码器解码
        decoded_samples = opus_decode(
            opus_decoder,  // 解码器实例
            opus_data_ptr, // OPUS数据指针
            opus_data_len, // OPUS数据长度
            pcm_output,    // PCM输出缓冲区
            960,           // 最大输出采样点数（16kHz * 60ms = 960）
            0              // FEC标志（0=正常解码）
        );

        // 6. 检查解码结果
        if (decoded_samples < 0)
        {
            ESP_LOGE(TAG, "OPUS decode failed, error: %d", decoded_samples);
            continue;
        }

        if (decoded_samples == 0)
        {
            ESP_LOGW(TAG, "OPUS decode returned 0 samples");
            continue;
        }

        ESP_LOGD(TAG, "OPUS decoded successfully: %d samples", decoded_samples);

        // 7. 通过I2S播放PCM数据
        esp_err_t i2s_ret = esp_i2s_write(
            pcm_output,         // PCM数据缓冲区
            decoded_samples * 2 // 字节数（int16_t占2字节）
        );

        if (i2s_ret != ESP_OK)
        {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(i2s_ret));
            continue;
        }

        ESP_LOGD(TAG, "PCM data written to I2S: %d samples (%d bytes)",
                 decoded_samples, decoded_samples * 2);

        // 8. 可选：添加延时避免任务占用过多CPU
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // 9. 清理资源（正常情况下不会执行到这里）
    if (opus_decoder != NULL)
    {
        opus_decoder_destroy(opus_decoder);
        ESP_LOGI(TAG, "OPUS decoder destroyed");
    }
}


void ws_recv_data_handler(const char *data, size_t len) {
    // ESP_LOGI(TAG, "Received data written to buffer: %u bytes, total stored: %u bytes", len, ws_recv_buffer_write_pos);
    if (xSemaphoreTake(ws_recv_opus_buffer_mutex, pdMS_TO_TICKS(1)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take buffer mutex (writer)");
    }
    if (len == 0 || data == NULL)
    {
        ESP_LOGE(TAG, "Received empty data");
    }
    if( ws_recv_buffer_write_pos + len > WS_RECV_BUFFER_SIZE )
    {
        ESP_LOGE(TAG, "Received data too large: %u bytes", len);
    }
    else {
        // 写入数据到公共缓冲区
        memcpy(ws_recv_opus_buffer + ws_recv_buffer_write_pos, data, len);
        ws_recv_buffer_write_pos += len;
    }

    // 释放缓冲区互斥锁
    xSemaphoreGive(ws_recv_opus_buffer_mutex);
}

static data_source_t source;

size_t mread(char *buf, uint16_t len) {
    size_t bytes_read = 0;
    static uint8_t flag = 0;
    source.type = AUDIO_DATA_TYPE_BUFFER;
    if (xSemaphoreTake(ws_recv_opus_buffer_mutex, pdMS_TO_TICKS(1)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take buffer mutex (reader)");
        return 0;
    }

    if (ws_recv_buffer_write_pos >= 4000 || flag) {
        flag = 1;
        // 读取数据到本地缓冲区
        if(ws_recv_buffer_write_pos < len) {
            len = ws_recv_buffer_write_pos;
        }
        memcpy(buf, ws_recv_opus_buffer, len);
        // 移动指针
        ws_recv_buffer_write_pos -= len;
        // 移动数据
        if(ws_recv_buffer_write_pos > 0) {
            memmove(ws_recv_opus_buffer, ws_recv_opus_buffer + len, ws_recv_buffer_write_pos);
        }
        bytes_read = len;
        if(bytes_read == 0) {
            flag = 0;
        }
    }
    // 释放缓冲区互斥锁
    xSemaphoreGive(ws_recv_opus_buffer_mutex);
    return bytes_read;
}

void audio_start_event() {
    audio_event = AUDIO_EVENT_START;
}

void audio_end_event() {
    audio_event = AUDIO_EVENT_END;
}

void audio_decoder_task(void *pvParameters)
{
    int16_t decode_buffer[960];
    uint32_t samples_decoded = 0;
    source.type = AUDIO_DATA_TYPE_BUFFER;
    int i = 0;

    for (;;)
    {   
        if(i++ == 2000) {
            ESP_LOGW(TAG, "ws_recv_buffer_write_pos: %d", ws_recv_buffer_write_pos);
            i = 0;
        }
        
        if(audio_event == AUDIO_EVENT_START) {
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
                audio_decoder_deinit(decoder);
                return;
            }
            audio_event = AUDIO_EVENT_PLAYING;
            continue;
        }

        decoder_result_t result = decoder->decode_frame(
            decoder,
            &source,
            decode_buffer,
            &samples_decoded);

        switch (result) {
            case DECODER_OK:
                if (samples_decoded > 0)
                {
                    esp_err_t i2s_ret = esp_i2s_write(
                        decode_buffer,
                        samples_decoded * 2 // 采样点转字节数（int16_t占2字节）
                    );
                    ESP_LOGI(TAG, "Decoded and played %lu bytes", samples_decoded * 2);
                    if (i2s_ret != ESP_OK)
                    {
                        ESP_LOGE(TAG, "I2S write failed: %d", i2s_ret);
                    }
                }
                break;

            case DECODER_HEADER_ONLY:
                ESP_LOGI(TAG, "Skipping audio header");
                break;

            case DECODER_EOF:
                if(audio_event == AUDIO_EVENT_END && ws_recv_buffer_write_pos == 0) {
                    decoder->deinit(decoder);
                    audio_event = AUDIO_EVENT_START;
                }
                // ESP_LOGI(TAG, "Reached end of music file");
                break;

            case DECODER_ERROR:
            default:
                ESP_LOGE(TAG, "Decode error: %d", result);
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void audio_encoder_task(void *pvParameters)
{
    BaseType_t xStatus;
    int16_t pcm_buf[960] = {0};
    uint8_t opus_buf[OPUS_ENCODED_PACKET_SIZE] = {0};
    uint16_t encoded_bytes = 0;
    uint16_t total_packet_len = 0; // 总数据包长度（帧头6字节 + OPUS数据长度）
    audio_encoder_t *encoder = NULL;
    opus_buf[0] = 0xAA;
    opus_buf[1] = 0x55;

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
            pcm_buf,            // 接收缓冲区（存储取出的帧）
            portMAX_DELAY       // 永久阻塞（无超时）
        );

        // 处理取数结果
        if (xStatus == pdPASS)
        {
            memset(opus_buf + 2, 0, OPUS_ENCODED_PACKET_SIZE - 2); // 清空除帧头外的缓冲区部分
            // // 通过I2S播放PCM数据，用于测试
            // esp_err_t i2s_ret = esp_i2s_write(
            //     pcm_buf,                 // PCM数据缓冲区
            //     960 * 2         // 字节数（int16_t占2字节）
            // );

            // if (i2s_ret != ESP_OK)
            // {
            //     ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(i2s_ret));
            //     continue;
            // }

            // 调用你的opus_encode_frame编码PCM数据
            encoder_result_t ret = encoder->encode_frame(
                encoder,                      // 编码器实例
                pcm_buf,                      // PCM输入数据
                960,                          // 采样数(样本数)
                opus_buf + FRAME_HEADER_SIZE, // OPUS输出缓冲区
                OPUS_DATA_MAX_LEN,            // 缓冲区大小
                &encoded_bytes                // 实际编码字节数
            );
            // printf("编码 OPUS packet: %u bytes\n", encoded_bytes);
            // for (int i = 6; i < 6 + encoded_bytes; i++)
            // {
            //     printf("%02X ", opus_buf[i]);
            // }
            // printf("\n");

            if (ret == ENCODER_OK && encoded_bytes > 0)
            {
                // ========== 加锁：访问缓冲区前获取互斥锁 ==========
                if (xSemaphoreTake(opus_buffer_mutex, pdMS_TO_TICKS(1)) != pdTRUE)
                {
                    ESP_LOGE(TAG, "Failed to take buffer mutex (encoder)");
                    continue; // 拿不到锁，丢弃当前帧
                }
                // 包序号（16位大端）
                opus_buf[2] = (audio_packet_seq >> 8) & 0xFF; // 高8位
                opus_buf[3] = audio_packet_seq & 0xFF;        // 低8位
                // OPUS数据长度（16位大端）
                opus_buf[4] = (encoded_bytes >> 8) & 0xFF;            // 高8位
                opus_buf[5] = encoded_bytes & 0xFF;                   // 低8位
                total_packet_len = FRAME_HEADER_SIZE + encoded_bytes; // 计算总数据包长度（帧头+OPUS数据）
                                                                      // 序号递增（0-65535循环）
                audio_packet_seq = (audio_packet_seq + 1) % 65536;
                // ESP_LOGI("ENCODE", "Encoded PCM to OPUS: %u", encoded_bytes);
                // 编码成功后，可将opus_buf通过WebSocket发送等

                // ========== 核心逻辑：检查缓冲区剩余空间 ==========
                size_t remain_space = BUFFER_SIZE - buffer_write_pos;
                if (total_packet_len > remain_space)
                {
                    // 剩余空间不足，丢弃当前帧
                    // ESP_LOGW(TAG, "Buffer full! Need %u bytes, remain %u bytes",
                    //          total_packet_len, remain_space);
                    xSemaphoreGive(opus_buffer_mutex); // 解锁
                    continue;
                }

                // printf("发送 OPUS packet: %u bytes\n", total_packet_len);
                // for (int i = 0; i < total_packet_len; i++)
                // {
                //     printf("%02X ", opus_buf[i]);
                // }
                // printf("\n");

                // 空间足够：拷贝OPUS数据到公共缓冲区
                memcpy(opus_output_buffer + buffer_write_pos, opus_buf, total_packet_len);

                // 更新写入位置
                buffer_write_pos += total_packet_len;
                // ESP_LOGI(TAG, "Store OPUS to buffer: %u bytes, write_pos=%u",
                //          total_packet_len, buffer_write_pos);

                // 发送encoded_bytes到ws_send_queue（非阻塞，避免任务卡死）
                // ESP_LOGE(TAG, "total_packet_len: %u", total_packet_len);
                BaseType_t send_ret = xQueueSend(
                    ws_send_queue,
                    &total_packet_len,
                    0 // 0超时=非阻塞，队列满则丢弃
                );
                if (send_ret != pdPASS)
                {
                    ESP_LOGE(TAG, "ws_send_queue full! Drop frame");
                    // 队列满时回滚写入位置（丢弃当前帧，避免缓冲区数据无效）
                    buffer_write_pos -= total_packet_len;
                }
                // ========== 解锁：释放互斥锁 ==========
                xSemaphoreGive(opus_buffer_mutex);
            }
            else
            {
                ESP_LOGE(TAG, "OPUS encode failed: %d", ret);
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

    ws_recv_opus_buffer_mutex = xSemaphoreCreateMutex();
    if (ws_recv_opus_buffer_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create ws recv opus buffer mutex");
        return ESP_FAIL;
    }

    // 3. 创建消息队列
    ws_send_queue = xQueueCreate(20, sizeof(uint16_t));
    ESP_RETURN_ON_FALSE(ws_send_queue != NULL, ESP_FAIL, TAG, "Failed create ws send queue");

    BaseType_t ret_val = xTaskCreatePinnedToCore(
        audio_encoder_task,
        "audio_encoder_task",
        18 * 1024,
        NULL,
        4,
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

    // ret_val = xTaskCreatePinnedToCore(
    //     audio_decoder_test_task,
    //     "audio_decoder_test",
    //     12 * 1024,
    //     NULL,
    //     5,
    //     &audio_decoder_test_task_handle,
    //     1);

    // if (ret_val != pdPASS)
    // {
    //     ESP_LOGE(TAG, "Failed to create audio decoder task");
    // }
    // else
    // {
    //     ESP_LOGI(TAG, "Audio decoder task created successfully");
    // }

    ret_val = xTaskCreatePinnedToCore(
        audio_decoder_task,
        "audio_decoder_task",
        16 * 1024,
        NULL,
        5,
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

    return ESP_OK;
}