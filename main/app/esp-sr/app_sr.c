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
#include "websocket.h"  // 添加WebSocket头文件
#include "ws_mang.h"    // 添加WebSocket管理头文件

// 修改全局常量，调整WebSocket传输大小为标准的20ms音频（6400字节）
#define I2S_CHANNEL_NUM (1)
#define BUFFER_SIZE (640)
#define WS_TRANSFER_SIZE (6400) // 20ms音频数据大小（16kHz采样率，16bit位深，单声道）

// 添加音频数据收集缓冲区全局变量
static int16_t *audio_collect_buffer = NULL;
static int audio_collect_index = 0;
static const int AUDIO_COLLECT_BUFFER_SIZE = WS_TRANSFER_SIZE * 2; // 增加缓冲以避免数据丢失

static const char *TAG = "app_sr";
static const esp_afe_sr_iface_t *afe_handle = NULL;

TaskHandle_t audio_feed_task_handle = NULL;
TaskHandle_t audio_detect_task_handle = NULL;

static bool is_recording = false;               // 是否正在采集音频
static int recording_duration_ms = 60000;       // 采集时长（60秒，可自定义）
static TickType_t recording_start_tick = 0;     // 采集开始时间戳

static void audio_feed_task(void *pvParam)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)pvParam;
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    ESP_LOGI(TAG, "audio_chunksize=%d, feed_channel=%d", audio_chunksize, 1);

    /* Allocate audio buffer and check for result */
    int16_t *audio_buffer = heap_caps_malloc(audio_chunksize * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (NULL == audio_buffer)
    {
        esp_system_abort("No mem for audio buffer");
    }

    while (true)
    {
        /* Read audio data from I2S bus */
        esp_err_t read_result = esp_i2s_read(audio_buffer, audio_chunksize * sizeof(int16_t));
        if (read_result != ESP_OK)
        {
            ESP_LOGE(TAG, "======== bsp_extra_i2s_read failed ==========");
        }
        /* Feed samples of an audio stream to the AFE_SR */
        afe_handle->feed(afe_data, audio_buffer);
        
        // 添加小延时，减少CPU占用并给其他任务执行机会
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    /* Clean up if audio feed ends */
    afe_handle->destroy(afe_data);
    /* Task never returns */
    vTaskDelete(NULL);
}

static void audio_detect_task(void *pvParam)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)pvParam;
    ESP_LOGI(TAG, "------------detect start------------\n");

    // 为WebSocket发送准备音频缓冲区
    int16_t *websocket_audio_buffer = heap_caps_malloc(WS_TRANSFER_SIZE, 
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (websocket_audio_buffer == NULL) {
        ESP_LOGE(TAG, "无法为WebSocket分配音频缓冲区");
        vTaskDelete(NULL);
        return;
    }

    // 初始化数据收集缓冲区
    audio_collect_buffer = heap_caps_malloc(AUDIO_COLLECT_BUFFER_SIZE, 
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (audio_collect_buffer == NULL) {
        ESP_LOGE(TAG, "无法分配音频收集缓冲区");
        free(websocket_audio_buffer);
        vTaskDelete(NULL);
        return;
    }
    audio_collect_index = 0;

    while (true)
    {
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        // ESP_LOGI(TAG, "fetch data size: %d", res->data_size);

        if (!res || res->ret_value == ESP_FAIL)
        {
            ESP_LOGE(TAG, "fetch error!");
            continue;
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
            ws_send_json(wakeup_msg, strlen(wakeup_msg));
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
                afe_handle->enable_wakenet(afe_data);  // 恢复唤醒词检测
                continue;
            }

            // 使用AFE处理后的音频数据（不再直接从I2S读取）
            if (res->data != NULL && res->data_size > 0) {  
                // 将AFE处理后的数据存入收集缓冲区
                int available_space = AUDIO_COLLECT_BUFFER_SIZE / sizeof(int16_t) - audio_collect_index;
                int copy_size = (res->data_size < available_space) ? res->data_size : available_space;  
                
                if (copy_size > 0) {
                    memcpy(&audio_collect_buffer[audio_collect_index], res->data, copy_size * sizeof(int16_t));
                    audio_collect_index += copy_size;
                }

                // 当收集到足够的数据时，通过WebSocket发送
                if (audio_collect_index * sizeof(int16_t) >= WS_TRANSFER_SIZE) {
                    // 检查WebSocket连接状态和缓冲区指针有效性
                    if (is_ws_connected() && websocket_audio_buffer != NULL && audio_collect_buffer != NULL) {
                        // 复制数据到WebSocket发送缓冲区
                        memcpy(websocket_audio_buffer, audio_collect_buffer, WS_TRANSFER_SIZE);
                        
                        // 通过WebSocket发送音频数据
                        ESP_LOGI(TAG, "准备发送音频数据，缓冲区大小: %d 字节", WS_TRANSFER_SIZE);
                        esp_err_t send_ret = ws_send_binary(websocket_audio_buffer, WS_TRANSFER_SIZE);
                        if (send_ret != ESP_OK) {
                            ESP_LOGE(TAG, "WebSocket发送音频数据失败: %s", esp_err_to_name(send_ret));
                              
                            // 如果发送失败，尝试重连
                            if (!is_ws_connected()) {
                                ESP_LOGI(TAG, "WebSocket已断开，尝试重连");
                                ws_reconnect_now();
                            }
                        }
                    } else {
                        if (!is_ws_connected()) {
                            ESP_LOGW(TAG, "WebSocket未连接，跳过发送音频数据");
                              
                            // 尝试重连WebSocket
                            ws_reconnect_now();
                        }
                        if (websocket_audio_buffer == NULL) {
                            ESP_LOGE(TAG, "WebSocket音频缓冲区为NULL");
                            // 重新分配缓冲区
                            websocket_audio_buffer = heap_caps_malloc(WS_TRANSFER_SIZE, 
                                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                            if (websocket_audio_buffer == NULL) {
                                ESP_LOGE(TAG, "无法重新分配WebSocket音频缓冲区");
                            }
                        }
                        if (audio_collect_buffer == NULL) {
                            ESP_LOGE(TAG, "音频收集缓冲区为NULL");
                            // 重新分配缓冲区
                            audio_collect_buffer = heap_caps_malloc(AUDIO_COLLECT_BUFFER_SIZE, 
                                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                            if (audio_collect_buffer != NULL) {
                                audio_collect_index = 0;
                            } else {
                                ESP_LOGE(TAG, "无法重新分配音频收集缓冲区");
                            }
                        }
                    }
                    
                    // 移动剩余数据到缓冲区开头
                    if (audio_collect_buffer != NULL && audio_collect_index * sizeof(int16_t) > WS_TRANSFER_SIZE) {
                        int remaining_size = audio_collect_index - (WS_TRANSFER_SIZE / sizeof(int16_t));
                        memmove(audio_collect_buffer, &audio_collect_buffer[WS_TRANSFER_SIZE / sizeof(int16_t)], 
                               remaining_size * sizeof(int16_t));
                        audio_collect_index = remaining_size;
                    } else {
                        audio_collect_index = 0;
                    }
                }
            }
        }
    }

    /* 释放音频缓冲区 */
    if (websocket_audio_buffer != NULL) {
        free(websocket_audio_buffer);
        websocket_audio_buffer = NULL;
    }
    
    if (audio_collect_buffer != NULL) {
        free(audio_collect_buffer);
        audio_collect_buffer = NULL;
    }

    /* Clean up if audio feed ends */
    afe_handle->destroy(afe_data);

    /* Task never returns */
    vTaskDelete(NULL);
}

esp_err_t app_sr_start(void)
{
    // 1. 初始化模型列表（从"model"分区加载模型）
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init failed!");
        return ESP_FAIL;
    }

    // 2. 初始化afe_config（通过库函数确保基础配置正确）
    afe_config_t *afe_config = afe_config_init(
        "MR",               // 输入格式（单麦克风）
        models,             // 模型列表
        AFE_TYPE_SR,        // 语音识别场景
        AFE_MODE_LOW_COST   // 低功耗模式
    );
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed!");
        esp_srmodel_deinit(models); // 释放模型列表
        return ESP_FAIL;
    }

    // 3. 在初始化后的结构体上修改自定义参数
    afe_config->aec_init = true;                  // 开启AEC
    afe_config->se_init = true;                   // 启用语音增强
    afe_config->vad_init = true;                  // 启用VAD
    afe_config->wakenet_init = true;              // 启用唤醒词引擎

    afe_config->vad_mode = VAD_MODE_3;            // VAD模式（中等灵敏度）

    // 唤醒词模型名称（从模型列表中过滤获取）
    afe_config->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    afe_config->wakenet_model_name_2 = NULL;      // 不使用第二个唤醒模型
    afe_config->wakenet_mode = DET_MODE_90;       // 唤醒检测模式（Normal）

    afe_config->afe_mode = AFE_MODE_LOW_COST;     // 修正枚举值
    afe_config->afe_perferred_core = 0;           // 偏好核心0
    afe_config->afe_perferred_priority = 5;       // 任务优先级5
    afe_config->afe_ringbuf_size = 50;           // 进一步增大环形缓冲区大小到50
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM; // 优先使用PSRAM
    afe_config->afe_linear_gain = 1.0;            // 线性增益1.0

    afe_config->agc_init = true;                 // 显式启用AGC
    afe_config->agc_mode = AFE_MN_PEAK_AGC_MODE_2; // AGC模式

    // PCM配置（单麦克风，无参考音，16kHz采样率）
    afe_config->pcm_config.total_ch_num = 1;
    afe_config->pcm_config.mic_num = 1;
    afe_config->pcm_config.ref_num = 0;
    afe_config->pcm_config.sample_rate = 16000;

    afe_config->debug_init = false;                // 禁用调试
    afe_config->afe_ns_mode = AFE_NS_MODE_WEBRTC;  // NS模式（WebRTC）
    afe_config->ns_model_name = "WEBRTC";          // NS模型名称
    afe_config->fixed_first_channel = true;        // 固定第一个麦克风通道

    // 修复：afe_config是指针，直接传入无需取地址
    afe_handle = esp_afe_handle_from_config(afe_config);
    if (afe_handle == NULL) {
        ESP_LOGE(TAG, "Failed create afe handle");
        esp_srmodel_deinit(models);
        return ESP_FAIL;
    }

    // 修复：参数为指针本身，无需取地址
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(afe_config);
    afe_config_print(afe_config); // 修复：同上
    if (afe_data == NULL) {
        ESP_LOGE(TAG, "Failed create afe data");
        esp_srmodel_deinit(models);
        return ESP_FAIL;
    }

    // 修复：指针访问成员用->而非.
    ESP_LOGI(TAG, "load wakenet:%s", afe_config->wakenet_model_name);

    BaseType_t ret_val = xTaskCreatePinnedToCore(audio_feed_task, "Feed Task", 8 * 1024, afe_data, 4, &audio_feed_task_handle, 1);
    ESP_RETURN_ON_FALSE(pdPASS == ret_val, ESP_FAIL, TAG, "Failed create audio feed task");

    ret_val = xTaskCreatePinnedToCore(audio_detect_task, "Detect Task", 6 * 1024, afe_data, 5, &audio_detect_task_handle, 0);
    ESP_RETURN_ON_FALSE(pdPASS == ret_val, ESP_FAIL, TAG, "Failed create audio detect task");

    return ESP_OK;
}