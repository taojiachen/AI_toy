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

static const esp_afe_sr_iface_t *g_afe_handle = NULL;
static esp_afe_sr_data_t *g_afe_data = NULL;

TaskHandle_t audio_feed_task_handle = NULL;
TaskHandle_t audio_detect_task_handle = NULL;

QueueHandle_t audio_encode_queue = NULL;

// 录音状态标志
static bool wakeup_recording = false;      // 唤醒词触发的录音
static bool api_recording = false;         // API 触发的录音
static int recording_duration_ms = 30000;  // 当前录音的有效时长（动态修改）
static TickType_t recording_start_tick = 0;

static int16_t pcm_output_ring_buffer[BYTES_PER_FRAME * 2] = {0}; // 环形缓冲区用于存储音频数据（2倍帧大小）
static size_t ring_buffer_write_pos = 0; // 环形缓冲区写入位置（按样本数计）

// 环形缓冲区辅助宏定义
#define RING_BUFFER_TOTAL_SAMPLES (sizeof(pcm_output_ring_buffer) / sizeof(pcm_output_ring_buffer[0]))  // 总样本数（int16_t）
#define FRAME_SAMPLES (BYTES_PER_FRAME / sizeof(int16_t))                         // 每帧需要的样本数

static void audio_feed_task(void *pvParam)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)pvParam;
    // audio_chunksize：音频时间 512->32ms 256->16ms
    int audio_chunksize = g_afe_handle->get_feed_chunksize(afe_data);
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
        g_afe_handle->feed(afe_data, audio_buffer);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    g_afe_handle->destroy(afe_data);
    vTaskDelete(NULL);
}

static void audio_detect_task(void *pvParam)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)pvParam;
    if (afe_data == NULL) {
        ESP_LOGE(TAG, "audio_detect_task: afe_data is NULL");
        vTaskDelete(NULL);
        return;
    }

    if (g_afe_handle == NULL) {
        ESP_LOGE(TAG, "audio_detect_task: afe_handle is NULL");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "------------detect start------------\n");

    while (true)
    {
        afe_fetch_result_t *res = g_afe_handle->fetch(afe_data);
        if (res == NULL) {
            ESP_LOGE(TAG, "afe_fetch returned NULL");
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        // ESP_LOGI(TAG, "fetch data size: %d", res->data_size);  //fetch data size: 1024字节

        if (res && res->ringbuff_free_pct > 0)
        {
            ESP_LOGD(TAG, "AFE缓冲区状态: %f%% 空闲", res->ringbuff_free_pct);
        }

        // 唤醒词检测（仅在无任何录音活动时启动唤醒词录音）
        if (res->wakeup_state == WAKENET_DETECTED)
        {
            ESP_LOGI(TAG, LOG_BOLD(LOG_COLOR_GREEN) "Wakeword detected");

            if (!wakeup_recording && !api_recording)
            {
                wakeup_recording = true;
                api_recording = false;
                recording_duration_ms = 30000;               // 唤醒词录音固定 30 秒
                recording_start_tick = xTaskGetTickCount();
                g_afe_handle->disable_wakenet(afe_data);

                // 通知服务器已检测到唤醒词
                if (ws_is_connected()) {
                    const char *wakeup_msg = "{\"type\":\"wakeup\"}";
                    ws_send_text(wakeup_msg, strlen(wakeup_msg));
                } else {
                    ESP_LOGW(TAG, "WebSocket not connected, skip wakeup msg");
                }
            }
            continue;
        }

        // 音频采集逻辑（唤醒词录音或 API 录音）
        if (wakeup_recording || api_recording)
        {
            // 检查超时
            int current_max_duration = wakeup_recording ? 30000 : recording_duration_ms;
            TickType_t current_tick = xTaskGetTickCount();
            if (pdMS_TO_TICKS(current_max_duration) < (current_tick - recording_start_tick))
            {
                ESP_LOGI(TAG, "采集超时，停止录音 (%s)",
                         wakeup_recording ? "wakeup" : "api");
                if (wakeup_recording) {
                    wakeup_recording = false;
                    g_afe_handle->enable_wakenet(afe_data);
                } else if (api_recording) {
                    api_recording = false;
                    g_afe_handle->enable_wakenet(afe_data);
                }
                continue;
            }

            // 处理音频数据（与原有逻辑完全相同）
            if (res->data && res->data_size > 0)
            {
                // 转换为16位PCM数据（AFE输出为int16_t格式PCM）
                int16_t *pcm_data = (int16_t *)res->data;
                size_t sample_count = res->data_size / sizeof(int16_t);
                
                if (sample_count == 0)
                {
                    ESP_LOGD(TAG, "No valid PCM samples to write to ring buffer");
                    continue;
                }

                size_t remaining = RING_BUFFER_TOTAL_SAMPLES - ring_buffer_write_pos;

                if (remaining >= sample_count)
                {
                    memcpy(&pcm_output_ring_buffer[ring_buffer_write_pos], pcm_data, sample_count * sizeof(int16_t));
                }
                else
                {
                    memcpy(&pcm_output_ring_buffer[ring_buffer_write_pos], pcm_data, remaining * sizeof(int16_t));
                    memcpy(pcm_output_ring_buffer, pcm_data + remaining, (sample_count - remaining) * sizeof(int16_t));
                }

                ring_buffer_write_pos = (ring_buffer_write_pos + sample_count) % RING_BUFFER_TOTAL_SAMPLES;
                ESP_LOGD(TAG, "Ring buffer write pos: %zu/%zu (samples)", ring_buffer_write_pos, RING_BUFFER_TOTAL_SAMPLES);

                while (ring_buffer_write_pos >= FRAME_SAMPLES)
                {
                    int16_t frame_data[FRAME_SAMPLES] = {0};
                    memcpy(frame_data, pcm_output_ring_buffer, FRAME_SAMPLES * sizeof(int16_t));

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

                    memmove(pcm_output_ring_buffer, &pcm_output_ring_buffer[FRAME_SAMPLES], 
                            (RING_BUFFER_TOTAL_SAMPLES - FRAME_SAMPLES) * sizeof(int16_t));
                    ring_buffer_write_pos -= FRAME_SAMPLES;
                    break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    /* Clean up if audio feed ends */
    g_afe_handle->destroy(afe_data);
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
        esp_srmodel_deinit(models);
        return ESP_FAIL;
    }

    afe_config->aec_init = true;
    afe_config->se_init = true;
    afe_config->vad_init = true;
    afe_config->wakenet_init = true;
    afe_config->vad_mode = VAD_MODE_2;
    afe_config->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    afe_config->wakenet_model_name_2 = NULL;
    afe_config->wakenet_mode = DET_MODE_90;
    afe_config->afe_mode = AFE_MODE_HIGH_PERF;
    afe_config->afe_perferred_core = 0;
    afe_config->afe_perferred_priority = 5;
    afe_config->afe_ringbuf_size = 50;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_config->afe_linear_gain = 1.0;
    afe_config->agc_init = true;
    afe_config->agc_mode = AFE_MN_PEAK_AGC_MODE_2;
    afe_config->pcm_config.total_ch_num = 1;
    afe_config->pcm_config.mic_num = 1;
    afe_config->pcm_config.ref_num = 0;
    afe_config->pcm_config.sample_rate = 16000;
    afe_config->debug_init = false;
    afe_config->afe_ns_mode = AFE_NS_MODE_WEBRTC;
    afe_config->ns_model_name = "WEBRTC";
    afe_config->fixed_first_channel = true;

    g_afe_handle = esp_afe_handle_from_config(afe_config);
    if (g_afe_handle == NULL)
    {
        ESP_LOGE(TAG, "Failed create afe handle");
        esp_srmodel_deinit(models);
        return ESP_FAIL;
    }

    g_afe_data = g_afe_handle->create_from_config(afe_config);
    if (g_afe_data == NULL)
    {
        ESP_LOGE(TAG, "Failed create afe data");
        esp_srmodel_deinit(models);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "load wakenet:%s", afe_config->wakenet_model_name);

    BaseType_t ret_val = xTaskCreatePinnedToCore(audio_feed_task, "Feed Task", 6 * 1024, g_afe_data, 4, &audio_feed_task_handle, 1);
    ESP_RETURN_ON_FALSE(pdPASS == ret_val, ESP_FAIL, TAG, "Failed create audio feed task");

    ret_val = xTaskCreatePinnedToCore(audio_detect_task, "Detect Task", 4 * 1024, g_afe_data, 3, &audio_detect_task_handle, 0);
    ESP_RETURN_ON_FALSE(pdPASS == ret_val, ESP_FAIL, TAG, "Failed create audio detect task");

    return ESP_OK;
}

// ==================== API 函数实现 ====================
void app_sr_start_api_recording(int duration_ms)
{
    if (wakeup_recording || api_recording) {
        ESP_LOGW(TAG, "Already recording, cannot start API recording");
        return;
    }
    if (!g_afe_handle || !g_afe_data) {
        ESP_LOGE(TAG, "AFE not initialized");
        return;
    }

    api_recording = true;
    wakeup_recording = false;
    recording_duration_ms = duration_ms;
    recording_start_tick = xTaskGetTickCount();

    // 关闭唤醒词，避免干扰
    g_afe_handle->disable_wakenet(g_afe_data);

    ESP_LOGI(TAG, "API recording started, duration=%d ms", duration_ms);
}

void app_sr_stop_api_recording(void)
{
    if (!api_recording) {
        ESP_LOGW(TAG, "No active API recording");
        return;
    }

    api_recording = false;
    // 恢复唤醒词
    if (g_afe_handle && g_afe_data) {
        g_afe_handle->enable_wakenet(g_afe_data);
    }
    ESP_LOGI(TAG, "API recording stopped manually");
}

bool app_sr_is_api_recording(void)
{
    return api_recording;
}