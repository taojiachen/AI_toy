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
#include "websocket.h" // 添加WebSocket头文件

// 修改全局常量，调整WebSocket传输大小为1024字节
#define WS_TRANSFER_SIZE (1024)
#define SAMPLES_PER_BUFFER (WS_TRANSFER_SIZE / sizeof(int16_t))  // 每个缓冲区的样本数

// 全局变量声明
static int16_t *audio_buffer_A = NULL;  // 缓冲区A
static int16_t *audio_buffer_B = NULL;  // 缓冲区B
static int16_t *current_buffer = NULL;  // 当前正在填充的缓冲区
static bool buffer_ready_to_send = false;  // 标记是否有缓冲区准备好发送
static int16_t *buffer_to_send = NULL;    // 准备发送的缓冲区

static const char *TAG = "app_sr";
static const esp_afe_sr_iface_t *afe_handle = NULL;

TaskHandle_t audio_feed_task_handle = NULL;
TaskHandle_t audio_detect_task_handle = NULL;

static bool is_recording = false;           // 是否正在采集音频
static int recording_duration_ms = 30000;   // 采集时长（30秒，可自定义）
static TickType_t recording_start_tick = 0; // 采集开始时间戳

// 重置缓冲区状态
static void reset_buffer_state(void)
{
    current_buffer = audio_buffer_A;
    buffer_ready_to_send = false;
    buffer_to_send = NULL;
}

static void audio_feed_task(void *pvParam)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)pvParam;
    // audio_chunksize：音频时间 512->32ms 256->16ms
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

    // 为WebSocket发送准备两个音频缓冲区（乒乓缓冲区）
    audio_buffer_A = heap_caps_malloc(WS_TRANSFER_SIZE, 
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    audio_buffer_B = heap_caps_malloc(WS_TRANSFER_SIZE, 
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (audio_buffer_A == NULL || audio_buffer_B == NULL) {
        ESP_LOGE(TAG, "无法分配音频缓冲区");
        if (audio_buffer_A) free(audio_buffer_A);
        if (audio_buffer_B) free(audio_buffer_B);
        vTaskDelete(NULL);
        return;
    }
    // 初始化缓冲区状态
    reset_buffer_state();

    while (true)
    {
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        // ESP_LOGI(TAG, "fetch data size: %d", res->data_size);  //fetch data size: 1024

        // 检查缓冲区状态
        if (res && res->ringbuff_free_pct > 0) {
            ESP_LOGD(TAG, "AFE缓冲区状态: %f%% 空闲", res->ringbuff_free_pct);
        }
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "fetch error!");
            vTaskDelay(pdMS_TO_TICKS(10));
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
            // 重置缓冲区状态
            reset_buffer_state();
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

            // 检查是否有缓冲区准备好发送，如果有则尝试发送
            if (buffer_ready_to_send && is_ws_connected()) {
                esp_err_t send_ret = ws_send_binary(buffer_to_send, WS_TRANSFER_SIZE);
                if (send_ret == ESP_OK) {
                    ESP_LOGD(TAG, "WebSocket成功发送音频数据");
                    buffer_ready_to_send = false;
                    buffer_to_send = NULL;
                } else {
                    ESP_LOGE(TAG, "WebSocket发送音频数据失败: %s", esp_err_to_name(send_ret));
                }
            }

            // 使用AFE处理后的音频数据，res->data_size = 1024字节
            if (res->data && res->data_size > 0 && res->data_size == WS_TRANSFER_SIZE) {
                // 由于res->data_size正好等于WS_TRANSFER_SIZE，我们可以直接复制整个数据块
                memcpy(current_buffer, res->data, WS_TRANSFER_SIZE);
                
                // 交换缓冲区：当前缓冲区变为待发送缓冲区，下一个缓冲区变为当前缓冲区
                buffer_to_send = current_buffer;
                buffer_ready_to_send = true;
                
                // 切换当前缓冲区（乒乓机制）
                current_buffer = (current_buffer == audio_buffer_A) ? audio_buffer_B : audio_buffer_A;
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
    // 1. 初始化模型列表（从"model"分区加载模型）
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL)
    {
        ESP_LOGE(TAG, "esp_srmodel_init failed!");
        return ESP_FAIL;
    }

    // 2. 初始化afe_config（通过库函数确保基础配置正确）
    afe_config_t *afe_config = afe_config_init(
        "MR",             // 输入格式（单麦克风）
        models,           // 模型列表
        AFE_TYPE_SR,      // 语音识别场景
        AFE_MODE_LOW_COST // 低功耗模式
    );
    if (afe_config == NULL)
    {
        ESP_LOGE(TAG, "afe_config_init failed!");
        esp_srmodel_deinit(models); // 释放模型列表
        return ESP_FAIL;
    }

    // 3. 在初始化后的结构体上修改自定义参数
    afe_config->aec_init = true;     // 开启AEC
    afe_config->se_init = true;      // 启用语音增强
    afe_config->vad_init = true;     // 启用VAD
    afe_config->wakenet_init = true; // 启用唤醒词引擎

    afe_config->vad_mode = VAD_MODE_3; // VAD模式（中等灵敏度）

    // 唤醒词模型名称（从模型列表中过滤获取）
    afe_config->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    afe_config->wakenet_model_name_2 = NULL; // 不使用第二个唤醒模型
    afe_config->wakenet_mode = DET_MODE_90;  // 唤醒检测模式（Normal）

    afe_config->afe_mode = AFE_MODE_LOW_COST;                    // 修正枚举值
    afe_config->afe_perferred_core = 0;                          // 偏好核心0
    afe_config->afe_perferred_priority = 5;                      // 任务优先级5
    afe_config->afe_ringbuf_size = 150;                          // 增大环形缓冲区大小到300，解决"ringbuffer full"问题
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM; // 优先使用PSRAM
    afe_config->afe_linear_gain = 1.0;                           // 线性增益1.0

    afe_config->agc_init = true;                   // 显式启用AGC
    afe_config->agc_mode = AFE_MN_PEAK_AGC_MODE_2; // AGC模式

    // PCM配置（单麦克风，无参考音，16kHz采样率）
    afe_config->pcm_config.total_ch_num = 1;
    afe_config->pcm_config.mic_num = 1;
    afe_config->pcm_config.ref_num = 0;
    afe_config->pcm_config.sample_rate = 16000;

    afe_config->debug_init = false;               // 禁用调试
    afe_config->afe_ns_mode = AFE_NS_MODE_WEBRTC; // NS模式（WebRTC）
    afe_config->ns_model_name = "WEBRTC";         // NS模型名称
    afe_config->fixed_first_channel = true;       // 固定第一个麦克风通道

    // 修复：afe_config是指针，直接传入无需取地址
    afe_handle = esp_afe_handle_from_config(afe_config);
    if (afe_handle == NULL)
    {
        ESP_LOGE(TAG, "Failed create afe handle");
        esp_srmodel_deinit(models);
        return ESP_FAIL;
    }

    // 修复：参数为指针本身，无需取地址
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(afe_config);
    afe_config_print(afe_config); // 修复：同上
    if (afe_data == NULL)
    {
        ESP_LOGE(TAG, "Failed create afe data");
        esp_srmodel_deinit(models);
        return ESP_FAIL;
    }

    // 修复：指针访问成员用->而非.
    ESP_LOGI(TAG, "load wakenet:%s", afe_config->wakenet_model_name);

    // 降低audio_feed_task优先级，确保audio_detect_task有足够资源处理数据
    BaseType_t ret_val = xTaskCreatePinnedToCore(audio_feed_task, "Feed Task", 8 * 1024, afe_data, 3, &audio_feed_task_handle, 1);
    ESP_RETURN_ON_FALSE(pdPASS == ret_val, ESP_FAIL, TAG, "Failed create audio feed task");

    // audio_detect_task保持较高优先级
    ret_val = xTaskCreatePinnedToCore(audio_detect_task, "Detect Task", 6 * 1024, afe_data, 5, &audio_detect_task_handle, 0);
    ESP_RETURN_ON_FALSE(pdPASS == ret_val, ESP_FAIL, TAG, "Failed create audio detect task");

    return ESP_OK;
}