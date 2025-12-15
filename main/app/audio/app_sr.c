#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "app_sr.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "model_path.h"
#include "esp_mn_speech_commands.h"
#include "esp_process_sdkconfig.h"
#include <esp_board_init.h>
#include "websocket.h"

static const char *TAG = "app_sr";

static const esp_afe_sr_iface_t *afe_handle = NULL;

TaskHandle_t audio_feed_task_handle = NULL;
TaskHandle_t audio_detect_task_handle = NULL;

QueueHandle_t audio_encode_queue = NULL;

static bool is_recording = false;           // 是否正在采集音频
static int recording_duration_ms = 30000;   // 采集时长（30秒，可自定义）
static TickType_t recording_start_tick = 0; // 采集开始时间戳

static int16_t pcm_outptu_ring_buffer[BYTES_PER_FRAME * 2] = {0}; // 环形缓冲区用于存储音频数据（2倍帧大小）
static size_t ring_buffer_write_pos = 0; // 环形缓冲区写入位置（按样本数计）

// 环形缓冲区辅助宏定义
#define RING_BUFFER_TOTAL_SAMPLES (sizeof(pcm_outptu_ring_buffer) / sizeof(pcm_outptu_ring_buffer[0]))  // 总样本数（int16_t）
#define FRAME_SAMPLES (BYTES_PER_FRAME / sizeof(int16_t))                         // 每帧需要的样本数

static void audio_feed_task(void *pvParam)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)pvParam;
    // audio_chunksize：音频时间 512->32ms 256->16ms
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    ESP_LOGI(TAG, "audio_chunksize=%d, feed_channel=%d", audio_chunksize, 1);

    int16_t *audio_buffer = heap_caps_malloc(audio_chunksize * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (NULL == audio_buffer)
    {
        esp_system_abort("No mem for audio buffer");
    }

    while (true)
    {
        esp_err_t read_result = esp_i2s_read(audio_buffer, audio_chunksize * sizeof(int16_t));
        if (read_result != ESP_OK)
        {
            ESP_LOGE(TAG, "======== bsp_extra_i2s_read failed ==========");
        }
        afe_handle->feed(afe_data, audio_buffer);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    afe_handle->destroy(afe_data);
    vTaskDelete(NULL);
}

static void audio_detect_task(void *pvParam)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)pvParam;
    ESP_LOGI(TAG, "------------detect start------------\n");

    while (true)
    {
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        // ESP_LOGI(TAG, "fetch data size: %d", res->data_size);  //fetch data size: 1024字节

        if (res && res->ringbuff_free_pct > 0)
        {
            ESP_LOGD(TAG, "AFE缓冲区状态: %f%% 空闲", res->ringbuff_free_pct);
        }

        if (res->wakeup_state == WAKENET_DETECTED)
        {
            ESP_LOGI(TAG, LOG_BOLD(LOG_COLOR_GREEN) "Wakeword detected");
            // 进入录音状态，记录开始时间
            is_recording = true;
            recording_start_tick = xTaskGetTickCount();
            afe_handle->disable_wakenet(afe_data);

            // 通知服务器已检测到唤醒词
            const char *wakeup_msg = "{\"type\":\"wakeup\"}";
            ws_send_text(wakeup_msg, strlen(wakeup_msg));
            continue;
        }

        // 2. 音频采集逻辑
        if (is_recording)
        {
            // 检查是否超时（超过设定的采集时长）
            TickType_t current_tick = xTaskGetTickCount();
            if (pdMS_TO_TICKS(recording_duration_ms) < (current_tick - recording_start_tick))
            {
                ESP_LOGI(TAG, "采集超时，停止采集");
                is_recording = false;
                afe_handle->enable_wakenet(afe_data); // 恢复唤醒词检测
                continue;
            }

            // 使用AFE处理后的音频数据，res->data_size = 1024字节
            if (res->data && res->data_size > 0)
            {
                // ========== 核心逻辑：写入环形缓冲区并推送至编码队列 ==========
                // 转换为16位PCM数据（AFE输出为int16_t格式PCM）
                int16_t *pcm_data = (int16_t *)res->data;
                // 计算要写入的样本数（字节数转样本数，int16_t占2字节）
                size_t sample_count = res->data_size / sizeof(int16_t);
                
                if (sample_count == 0)
                {
                    ESP_LOGD(TAG, "No valid PCM samples to write to ring buffer");
                    continue;
                }

                // 计算环形缓冲区剩余空间（到缓冲区末尾的样本数）
                size_t remaining = RING_BUFFER_TOTAL_SAMPLES - ring_buffer_write_pos;

                // 分两部分写入环形缓冲区（处理缓冲区绕回）
                if (remaining >= sample_count)
                {
                    // 剩余空间足够，直接拷贝
                    memcpy(&pcm_outptu_ring_buffer[ring_buffer_write_pos], pcm_data, sample_count * sizeof(int16_t));
                }
                else
                {
                    // 剩余空间不足，先写缓冲区末尾，再写开头
                    memcpy(&pcm_outptu_ring_buffer[ring_buffer_write_pos], pcm_data, remaining * sizeof(int16_t));
                    memcpy(pcm_outptu_ring_buffer, pcm_data + remaining, (sample_count - remaining) * sizeof(int16_t));
                }

                // 更新环形缓冲区写入位置（环形取模）
                ring_buffer_write_pos = (ring_buffer_write_pos + sample_count) % RING_BUFFER_TOTAL_SAMPLES;
                ESP_LOGD(TAG, "Ring buffer write pos: %zu/%zu (samples)", ring_buffer_write_pos, RING_BUFFER_TOTAL_SAMPLES);

                // 检查是否达到单帧数据量要求，满足则推送至编码队列
                while (ring_buffer_write_pos >= FRAME_SAMPLES)
                {
                    // 临时缓冲区存储要发送的一帧数据
                    int16_t frame_data[FRAME_SAMPLES] = {0};
                    // 从缓冲区开头拷贝一帧数据
                    memcpy(frame_data, pcm_outptu_ring_buffer, FRAME_SAMPLES * sizeof(int16_t));

                    // 非阻塞方式发送到队列，队列满则丢弃
                    BaseType_t send_status = xQueueSend(audio_encode_queue, frame_data, 0);
                    if (send_status == pdPASS)
                    {
                        ESP_LOGD(TAG, "Sent %zu bytes to encode queue (frame count: %zu)", 
                                FRAME_SAMPLES * sizeof(int16_t), ring_buffer_write_pos / FRAME_SAMPLES);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Audio encode queue full, drop current frame");
                    }

                    // 将缓冲区剩余数据前移，覆盖已发送的帧数据
                    memmove(pcm_outptu_ring_buffer, &pcm_outptu_ring_buffer[FRAME_SAMPLES], 
                            (RING_BUFFER_TOTAL_SAMPLES - FRAME_SAMPLES) * sizeof(int16_t));
                    // 更新写入位置（减去已发送的样本数）
                    ring_buffer_write_pos -= FRAME_SAMPLES;

                    // 避免单次循环发送过多帧，退出循环
                    break;
                }
            }
        }
    }

    /* Clean up if audio feed ends */
    afe_handle->destroy(afe_data);

    /* Task never returns */
    vTaskDelete(NULL);
}

esp_err_t app_sr_start(void)
{   
    // 创建消息队列用于传递PCM音频数据给编码任务
    audio_encode_queue = xQueueCreate(10, BYTES_PER_FRAME);
    ESP_RETURN_ON_FALSE(audio_encode_queue != NULL, ESP_FAIL, TAG, "Failed create audio encode queue");

    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL)
    {
        ESP_LOGE(TAG, "esp_srmodel_init failed!");
        return ESP_FAIL;
    }

    afe_config_t *afe_config = afe_config_init(
        "MR",             // 输入格式（单麦克风）
        models,           // 模型列表
        AFE_TYPE_SR,      // 语音识别场景
        AFE_MODE_HIGH_PERF // 高功耗模式
    );
    if (afe_config == NULL)
    {
        ESP_LOGE(TAG, "afe_config_init failed!");
        esp_srmodel_deinit(models); // 释放模型列表
        return ESP_FAIL;
    }

    afe_config->aec_init = true;     // 开启AEC
    afe_config->se_init = true;      // 启用语音增强
    afe_config->vad_init = true;     // 启用VAD
    afe_config->wakenet_init = true; // 启用唤醒词引擎

    afe_config->vad_mode = VAD_MODE_4; // VAD模式（中等灵敏度）

    afe_config->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    afe_config->wakenet_model_name_2 = NULL; // 不使用第二个唤醒模型
    afe_config->wakenet_mode = DET_MODE_90;  // 唤醒检测模式（Normal）

    afe_config->afe_mode = AFE_MODE_HIGH_PERF;                   // 修正枚举值
    afe_config->afe_perferred_core = 0;                          // 偏好核心0
    afe_config->afe_perferred_priority = 5;                      // 任务优先级5
    afe_config->afe_ringbuf_size = 150;                          // 增大环形缓冲区大小到300，解决"ringbuffer full"问题
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM; // 优先使用PSRAM
    afe_config->afe_linear_gain = 1.0;                           // 线性增益1.0

    afe_config->agc_init = true;                   // 显式启用AGC
    afe_config->agc_mode = AFE_MN_PEAK_AGC_MODE_2; // AGC模式

    afe_config->pcm_config.total_ch_num = 1;
    afe_config->pcm_config.mic_num = 1;
    afe_config->pcm_config.ref_num = 0;
    afe_config->pcm_config.sample_rate = 16000;

    afe_config->debug_init = false;               // 禁用调试
    afe_config->afe_ns_mode = AFE_NS_MODE_WEBRTC; // NS模式（WebRTC）
    afe_config->ns_model_name = "WEBRTC";         // NS模型名称
    afe_config->fixed_first_channel = true;       // 固定第一个麦克风通道

    afe_handle = esp_afe_handle_from_config(afe_config);
    if (afe_handle == NULL)
    {
        ESP_LOGE(TAG, "Failed create afe handle");
        esp_srmodel_deinit(models);
        return ESP_FAIL;
    }

    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(afe_config);
    if (afe_data == NULL)
    {
        ESP_LOGE(TAG, "Failed create afe data");
        esp_srmodel_deinit(models);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "load wakenet:%s", afe_config->wakenet_model_name);

    BaseType_t ret_val = xTaskCreatePinnedToCore(audio_feed_task, "Feed Task", 8 * 1024, afe_data, 3, &audio_feed_task_handle, 1);
    ESP_RETURN_ON_FALSE(pdPASS == ret_val, ESP_FAIL, TAG, "Failed create audio feed task");

    ret_val = xTaskCreatePinnedToCore(audio_detect_task, "Detect Task", 6 * 1024, afe_data, 5, &audio_detect_task_handle, 0);
    ESP_RETURN_ON_FALSE(pdPASS == ret_val, ESP_FAIL, TAG, "Failed create audio detect task");

    return ESP_OK;
}