#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "cJSON.h"

#include "websocket.h"
#include "audio.h"

// 日志标签
static const char *TAG = "WS_CLIENT";

// 内置默认配置参数（所有配置集中在这里）
#define WS_DEFAULT_PING_INTERVAL 10          // PING间隔(秒)
#define WS_DEFAULT_BUFFER_SIZE (8 * 1024)   // 缓冲区大小
#define WS_DEFAULT_NETWORK_TIMEOUT 20000     // 网络超时(毫秒)
#define WS_DEFAULT_MAX_RECONNECT 15          // 最大重连次数
#define WS_DEFAULT_INIT_RECONNECT_DELAY 2000 // 初始重连延迟(毫秒)
#define WS_DEFAULT_MAX_RECONNECT_DELAY 30000 // 最大重连延迟(毫秒)
#define WS_DEFAULT_SKIP_CERT_CHECK true      // 是否跳过证书检查
#define MAX_OPUS_FRAME_LEN 200
// ==========================================================================================

// WebSocket配置结构体（内部使用）
typedef struct
{
    const char *uri;                // WebSocket服务器地址
    const char *cert_pem;           // SSL证书
    int ping_interval_sec;          // PING间隔(秒)
    int buffer_size;                // 缓冲区大小
    int network_timeout_ms;         // 网络超时(毫秒)
    int max_reconnect_attempts;     // 最大重连次数
    int initial_reconnect_delay_ms; // 初始重连延迟(毫秒)
    int max_reconnect_delay_ms;     // 最大重连延迟(毫秒)
    bool skip_cert_check;           // 是否跳过证书检查
} ws_config_t;

// WebSocket客户端上下文
typedef struct
{
    esp_websocket_client_handle_t client;
    ws_config_t config;
    ws_state_t state;

    // 回调函数
    ws_data_handler_t data_handler;
    ws_state_handler_t state_handler;

    // 重连相关
    TaskHandle_t reconnect_task;
    TimerHandle_t reconnect_timer;
    int reconnect_attempts;
    int reconnect_success;
    int current_delay;
    bool manual_disconnect;

    // 同步对象
    SemaphoreHandle_t mutex;

    // 状态标志
    bool initialized;
} ws_context_t;

static ws_context_t g_ws_ctx = {0};

TaskHandle_t ws_send_opus_task_handle = NULL;

extern QueueHandle_t ws_send_queue;

// 前向声明
static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void ws_reconnect_task(void *pvParameters);
static void ws_reconnect_timer_cb(TimerHandle_t timer);
static void ws_set_state(ws_state_t new_state);
static esp_err_t ws_create_client(void);
static void ws_destroy_client(void);
static void ws_start_reconnect_timer(void);
static void ws_stop_reconnect_timer(void);
static void ws_send_device_info(void);
static esp_err_t ws_init_internal(const ws_config_t *config);

/**
 * @brief 设置WebSocket状态并通知回调
 */
static void ws_set_state(ws_state_t new_state)
{
    if (xSemaphoreTake(g_ws_ctx.mutex, portMAX_DELAY) == pdTRUE)
    {
        ws_state_t old_state = g_ws_ctx.state;
        g_ws_ctx.state = new_state;
        xSemaphoreGive(g_ws_ctx.mutex);

        // 状态变化时调用回调
        if (old_state != new_state && g_ws_ctx.state_handler)
        {
            g_ws_ctx.state_handler(old_state, new_state);
        }

        ESP_LOGI(TAG, "状态变化: %d -> %d", old_state, new_state);
    }
}

/**
 * @brief 发送设备信息到服务器
 */
static void ws_send_device_info(void)
{
    uint8_t mac_addr[6];
    char mac_str[18];

    if (esp_wifi_get_mac(WIFI_IF_STA, mac_addr) == ESP_OK)
    {
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
    }
    else
    {
        strcpy(mac_str, "unknown");
    }

    time_t now = time(NULL);
    char device_info[256];
    snprintf(device_info, sizeof(device_info),
             "{\"type\":\"device_info\",\"mac\":\"%s\",\"timestamp\":%lld}",
             mac_str, (long long)now);

    ws_send_text(device_info, strlen(device_info));
}

/**
 * @brief WebSocket事件处理器
 */
static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket连接成功");
        ws_set_state(WS_STATE_CONNECTED);

        // 重置重连参数
        if (xSemaphoreTake(g_ws_ctx.mutex, portMAX_DELAY) == pdTRUE)
        {
            g_ws_ctx.current_delay = g_ws_ctx.config.initial_reconnect_delay_ms;
            if (g_ws_ctx.reconnect_attempts > 0)
            {
                g_ws_ctx.reconnect_success++;
            }
            g_ws_ctx.reconnect_attempts = 0;
            xSemaphoreGive(g_ws_ctx.mutex);
        }

        // 停止重连定时器
        ws_stop_reconnect_timer();

        // 发送设备信息
        ws_send_device_info();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket连接断开");

        // 检查是否手动断开
        bool manual = false;
        if (xSemaphoreTake(g_ws_ctx.mutex, portMAX_DELAY) == pdTRUE)
        {
            manual = g_ws_ctx.manual_disconnect;
            xSemaphoreGive(g_ws_ctx.mutex);
        }

        if (!manual)
        {
            ws_set_state(WS_STATE_RECONNECTING);
            ws_start_reconnect_timer();
        }
        else
        {
            ws_set_state(WS_STATE_DISCONNECTED);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if(data->op_code == 2) // 二进制数据
        {
            ESP_LOGI(TAG, "收到二进制数据，长度: %d", data->data_len);

            // 调用数据处理回调
            if (g_ws_ctx.data_handler)
            {
                // ESP_LOGI(TAG, "调用回调函数");
                g_ws_ctx.data_handler(data->data_ptr, data->data_len);
            } else {
                ESP_LOGW(TAG, "未注册数据处理回调");
            }
        } else if(data->op_code == 1) // 文本数据
        {
            ESP_LOGI(TAG, "收到文本数据: %.*s", data->data_len, (char *)data->data_ptr);
            // 处理JSON文本数据，当type = audio_end时，重置opus解码器  I (18398) WS_CLIENT: 收到文本数据: {"type": "audio_end", "timestamp": 1769880620, "message": "\u97f3\u9891\u6570\u636e\u63a5\u6536\u5b8c\u6210"}
            // 解析JSON字符串
            cJSON *json = cJSON_Parse((char *)data->data_ptr);
            if (json)
            {
                // 提取type字段
                cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
                if (type && cJSON_IsString(type))
                {
                    if (strcmp(type->valuestring, "audio_start") == 0) {
                        audio_start_event();
                    } else if(strcmp(type->valuestring, "audio_end") == 0) {
                        audio_end_event();
                    }
                }
                cJSON_Delete(json);
            }
        } else {
            // ESP_LOGW(TAG, "收到未知数据类型: op_code=%d, length=%d", data->op_code, data->data_len);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket错误");
        if (data && data->error_handle.error_type != WEBSOCKET_ERROR_TYPE_NONE)
        {
            ESP_LOGE(TAG, "错误类型: %d, ESP错误: %s",
                     data->error_handle.error_type,
                     esp_err_to_name(data->error_handle.esp_tls_last_esp_err));
            if (data->error_handle.esp_transport_sock_errno != 0)
            {
                ESP_LOGE(TAG, "Socket错误: %d (%s)",
                         data->error_handle.esp_transport_sock_errno,
                         strerror(data->error_handle.esp_transport_sock_errno));
            }
        }
        ws_set_state(WS_STATE_ERROR);
        ws_start_reconnect_timer();
        break;

    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "WebSocket连接已关闭");
        break;

    case WEBSOCKET_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "准备建立WebSocket连接");
        ws_set_state(WS_STATE_CONNECTING);
        break;

    default:
        ESP_LOGD(TAG, "未知事件: %d", (int)event_id);
        break;
    }
}

/**
 * @brief 重连定时器回调
 */
static void ws_reconnect_timer_cb(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "重连定时器触发");

    // 创建重连任务
    if (xTaskCreate(ws_reconnect_task, "ws_reconnect", 1024 * 3, NULL, 4, &g_ws_ctx.reconnect_task) != pdPASS)
    {
        ESP_LOGE(TAG, "创建重连任务失败");
        // 重新启动定时器
        ws_start_reconnect_timer();
    }
}

/**
 * @brief 重连任务
 */
static void ws_reconnect_task(void *pvParameters)
{
    ESP_LOGI(TAG, "开始重连任务");

    bool should_retry = true;

    if (xSemaphoreTake(g_ws_ctx.mutex, portMAX_DELAY) == pdTRUE)
    {
        g_ws_ctx.reconnect_attempts++;

        // 检查是否超过最大重连次数
        if (g_ws_ctx.reconnect_attempts > g_ws_ctx.config.max_reconnect_attempts)
        {
            ESP_LOGE(TAG, "达到最大重连次数 (%d)，停止重连",
                     g_ws_ctx.config.max_reconnect_attempts);
            should_retry = false;
            g_ws_ctx.state = WS_STATE_ERROR;
        }

        xSemaphoreGive(g_ws_ctx.mutex);
    }

    if (should_retry)
    {
        ESP_LOGI(TAG, "尝试重连 (%d/%d)",
                 g_ws_ctx.reconnect_attempts, g_ws_ctx.config.max_reconnect_attempts);

        // 销毁旧客户端
        ws_destroy_client();

        // 短暂延迟
        vTaskDelay(pdMS_TO_TICKS(1000));

        // 创建新客户端并连接
        if (ws_create_client() == ESP_OK)
        {
            if (esp_websocket_client_start(g_ws_ctx.client) == ESP_OK)
            {
                ESP_LOGI(TAG, "重连启动成功");
            }
            else
            {
                ESP_LOGE(TAG, "重连启动失败");
                ws_start_reconnect_timer();
            }
        }
        else
        {
            ESP_LOGE(TAG, "重连创建客户端失败");
            ws_start_reconnect_timer();
        }
    }

    // 清理任务句柄
    if (xSemaphoreTake(g_ws_ctx.mutex, portMAX_DELAY) == pdTRUE)
    {
        g_ws_ctx.reconnect_task = NULL;
        xSemaphoreGive(g_ws_ctx.mutex);
    }

    vTaskDelete(NULL);
}

/**
 * @brief 启动重连定时器
 */
static void ws_start_reconnect_timer(void)
{
    if (g_ws_ctx.reconnect_timer && xTimerIsTimerActive(g_ws_ctx.reconnect_timer) == pdFALSE)
    {
        int delay = g_ws_ctx.current_delay;

        // 指数退避：每次失败后延迟时间乘以1.5
        g_ws_ctx.current_delay = (g_ws_ctx.current_delay * 3) / 2;
        if (g_ws_ctx.current_delay > g_ws_ctx.config.max_reconnect_delay_ms)
        {
            g_ws_ctx.current_delay = g_ws_ctx.config.max_reconnect_delay_ms;
        }

        // 添加随机抖动（±500ms）
        delay += (rand() % 1000) - 500;
        if (delay < 1000)
            delay = 1000;

        ESP_LOGI(TAG, "启动重连定时器，延迟: %d ms", delay);
        xTimerChangePeriod(g_ws_ctx.reconnect_timer, pdMS_TO_TICKS(delay), 0);
        xTimerStart(g_ws_ctx.reconnect_timer, 0);
    }
}

/**
 * @brief 停止重连定时器
 */
static void ws_stop_reconnect_timer(void)
{
    if (g_ws_ctx.reconnect_timer && xTimerIsTimerActive(g_ws_ctx.reconnect_timer) == pdTRUE)
    {
        xTimerStop(g_ws_ctx.reconnect_timer, 0);
        ESP_LOGI(TAG, "停止重连定时器");
    }
}

/**
 * @brief 创建WebSocket客户端
 */
static esp_err_t ws_create_client(void)
{
    if (g_ws_ctx.client)
    {
        ESP_LOGW(TAG, "客户端已存在");
        return ESP_OK;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = g_ws_ctx.config.uri,
        .transport = WEBSOCKET_TRANSPORT_OVER_SSL,
        .cert_pem = g_ws_ctx.config.cert_pem,
        .skip_cert_common_name_check = g_ws_ctx.config.skip_cert_check,
        .disable_auto_reconnect = true, // 使用自定义重连逻辑
        .task_prio = 3,
        .buffer_size = g_ws_ctx.config.buffer_size,
        .ping_interval_sec = g_ws_ctx.config.ping_interval_sec,
        .network_timeout_ms = g_ws_ctx.config.network_timeout_ms,
    };

    g_ws_ctx.client = esp_websocket_client_init(&ws_cfg);
    if (!g_ws_ctx.client)
    {
        ESP_LOGE(TAG, "创建WebSocket客户端失败");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_websocket_register_events(g_ws_ctx.client, WEBSOCKET_EVENT_ANY,
                                                  ws_event_handler, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "注册事件处理器失败: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(g_ws_ctx.client);
        g_ws_ctx.client = NULL;
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief 销毁WebSocket客户端
 */
static void ws_destroy_client(void)
{
    if (g_ws_ctx.client)
    {
        esp_websocket_client_stop(g_ws_ctx.client);
        esp_websocket_client_destroy(g_ws_ctx.client);
        g_ws_ctx.client = NULL;
        ESP_LOGI(TAG, "WebSocket客户端已销毁");
    }
}

/**
 * @brief 内部初始化函数（用户无需调用）
 */
static esp_err_t ws_init_internal(const ws_config_t *config)
{
    if (g_ws_ctx.initialized)
    {
        ESP_LOGW(TAG, "WebSocket已经初始化");
        return ESP_OK;
    }

    if (!config || !config->uri)
    {
        ESP_LOGE(TAG, "无效的配置参数");
        return ESP_ERR_INVALID_ARG;
    }

    // 清零上下文
    memset(&g_ws_ctx, 0, sizeof(g_ws_ctx));

    // 复制配置
    g_ws_ctx.config = *config;

    // 填充默认值（确保配置完整性）
    if (g_ws_ctx.config.ping_interval_sec <= 0)
    {
        g_ws_ctx.config.ping_interval_sec = WS_DEFAULT_PING_INTERVAL;
    }
    if (g_ws_ctx.config.buffer_size <= 0)
    {
        g_ws_ctx.config.buffer_size = WS_DEFAULT_BUFFER_SIZE;
    }
    if (g_ws_ctx.config.network_timeout_ms <= 0)
    {
        g_ws_ctx.config.network_timeout_ms = WS_DEFAULT_NETWORK_TIMEOUT;
    }
    if (g_ws_ctx.config.max_reconnect_attempts <= 0)
    {
        g_ws_ctx.config.max_reconnect_attempts = WS_DEFAULT_MAX_RECONNECT;
    }
    if (g_ws_ctx.config.initial_reconnect_delay_ms <= 0)
    {
        g_ws_ctx.config.initial_reconnect_delay_ms = WS_DEFAULT_INIT_RECONNECT_DELAY;
    }
    if (g_ws_ctx.config.max_reconnect_delay_ms <= 0)
    {
        g_ws_ctx.config.max_reconnect_delay_ms = WS_DEFAULT_MAX_RECONNECT_DELAY;
    }
    if (g_ws_ctx.config.cert_pem == NULL)
    {
        g_ws_ctx.config.cert_pem = server_cert_pem; // 使用内置证书
    }

    g_ws_ctx.current_delay = g_ws_ctx.config.initial_reconnect_delay_ms;
    g_ws_ctx.state = WS_STATE_DISCONNECTED;

    // 创建互斥锁
    g_ws_ctx.mutex = xSemaphoreCreateMutex();
    if (!g_ws_ctx.mutex)
    {
        ESP_LOGE(TAG, "创建互斥锁失败");
        return ESP_FAIL;
    }

    // 创建重连定时器
    g_ws_ctx.reconnect_timer = xTimerCreate("ws_reconnect_timer",
                                            pdMS_TO_TICKS(1000),
                                            pdFALSE,
                                            NULL,
                                            ws_reconnect_timer_cb);
    if (!g_ws_ctx.reconnect_timer)
    {
        ESP_LOGE(TAG, "创建重连定时器失败");
        vSemaphoreDelete(g_ws_ctx.mutex);
        return ESP_FAIL;
    }

    g_ws_ctx.initialized = true;
    ESP_LOGI(TAG, "WebSocket客户端初始化成功");

    return ESP_OK;
}

esp_err_t ws_send_opus_task(void *pvParameters)
{
    // 1. 初始化临时缓冲区（存储从opus_output_buffer读取的OPUS数据）
    uint8_t opus_data_buf[MAX_OPUS_FRAME_LEN] = {0};
    // 2. 队列接收的变量：存储需要读取的OPUS数据长度
    uint16_t req_len = 0;
    while (1)
    {
        // 3. 从队列中接收「需要读取的字节数」（阻塞等待，直到有数据）
        if (xQueueReceive(ws_send_queue, &req_len, portMAX_DELAY) == pdTRUE)
        {
            // 校验：读取长度不能超过缓冲区最大容量（避免缓冲区溢出）
            if (req_len > MAX_OPUS_FRAME_LEN)
            {
                ESP_LOGE(TAG, "请求读取长度超过缓冲区上限: req_len=%u, max=%u",
                         req_len, MAX_OPUS_FRAME_LEN);
                req_len = 0; // 重置，避免后续错误
                continue;
            }

            // 4. 提前检查WebSocket连接状态（减少无效的读操作）
            if (!ws_is_connected())
            {
                ESP_LOGE(TAG, "WebSocket未连接，跳过OPUS数据发送");
                req_len = 0;
                continue;
            }

            // ESP_LOGE(TAG, "请求读取长度: req_len=%u",req_len);

            // 5. 从公共缓冲区读取指定长度的OPUS数据
            esp_err_t read_ret = audio_get_opus_encode_data(opus_data_buf, req_len);
            if (read_ret != ESP_OK)
            {
                ESP_LOGE(TAG, "读取OPUS数据失败: %s", esp_err_to_name(read_ret));
                req_len = 0;
                continue;
            }

            // 6. 发送二进制OPUS数据（通过WebSocket）
            esp_err_t send_ret = ws_send_binary(opus_data_buf, req_len);
            if (send_ret != ESP_OK)
            {
                ESP_LOGE(TAG, "发送OPUS数据失败: %s (长度=%u)",
                         esp_err_to_name(send_ret), req_len);
            }
            else
            {
                ESP_LOGD(TAG, "成功发送OPUS数据，长度=%u字节，数据内容第1字节=%02x, 数据内容第二字节=%02x", req_len, opus_data_buf[0], opus_data_buf[1]);
            }

            // 重置变量，避免残留数据
            req_len = 0;
            memset(opus_data_buf, 0, req_len); // 仅清空已使用的部分，提升效率
        }
        else
        {
            // 队列接收失败（仅调试用，portMAX_DELAY下基本不会触发）
            ESP_LOGW(TAG, "从ws_send_queue接收数据失败");
        }

        // 短延迟：降低任务调度频率，避免占用过多CPU
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 循环不会执行到此处，仅满足函数返回值要求
    ESP_LOGW(TAG, "OPUS发送任务异常退出");
    return ESP_FAIL;
}

/**
 * @brief 简化版启动WebSocket连接（对外核心接口）
 */
esp_err_t ws_start(const char *uri)
{
    if (uri == NULL || strlen(uri) == 0)
    {
        ESP_LOGE(TAG, "无效的WS地址");
        return ESP_ERR_INVALID_ARG;
    }

    // 构建默认配置（所有参数内置，仅URI由用户传入）
    ws_config_t default_config = {
        .uri = uri,
        .cert_pem = server_cert_pem,
        .ping_interval_sec = WS_DEFAULT_PING_INTERVAL,
        .buffer_size = WS_DEFAULT_BUFFER_SIZE,
        .network_timeout_ms = WS_DEFAULT_NETWORK_TIMEOUT,
        .max_reconnect_attempts = WS_DEFAULT_MAX_RECONNECT,
        .initial_reconnect_delay_ms = WS_DEFAULT_INIT_RECONNECT_DELAY,
        .max_reconnect_delay_ms = WS_DEFAULT_MAX_RECONNECT_DELAY,
        .skip_cert_check = WS_DEFAULT_SKIP_CERT_CHECK};

    // 初始化（未初始化时执行）
    esp_err_t ret = ws_init_internal(&default_config);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // 标记非手动断开
    if (xSemaphoreTake(g_ws_ctx.mutex, portMAX_DELAY) == pdTRUE)
    {
        g_ws_ctx.manual_disconnect = false;
        xSemaphoreGive(g_ws_ctx.mutex);
    }

    // 创建客户端并启动连接
    ret = ws_create_client();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = esp_websocket_client_start(g_ws_ctx.client);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "启动WebSocket客户端失败: %s", esp_err_to_name(ret));
        ws_destroy_client();
        return ret;
    }
    ws_register_data_handler(ws_recv_data_handler);

    ESP_LOGI(TAG, "WebSocket客户端启动成功，连接地址: %s", uri);

    xTaskCreatePinnedToCore(ws_send_opus_task, "ws_send_opus_task", 1024 * 4, NULL, 5, &ws_send_opus_task_handle, 1);
    return ESP_OK;
}

/**
 * @brief 停止WebSocket连接
 */
void ws_stop(void)
{
    if (!g_ws_ctx.initialized)
    {
        return;
    }

    // 标记手动断开
    if (xSemaphoreTake(g_ws_ctx.mutex, portMAX_DELAY) == pdTRUE)
    {
        g_ws_ctx.manual_disconnect = true;
        xSemaphoreGive(g_ws_ctx.mutex);
    }

    // 停止重连定时器
    ws_stop_reconnect_timer();

    // 删除重连任务
    if (g_ws_ctx.reconnect_task)
    {
        vTaskDelete(g_ws_ctx.reconnect_task);
        g_ws_ctx.reconnect_task = NULL;
    }

    // 销毁客户端
    ws_destroy_client();

    ws_set_state(WS_STATE_DISCONNECTED);
    ESP_LOGI(TAG, "WebSocket客户端已停止");
}

/**
 * @brief 发送文本数据
 */
esp_err_t ws_send_text(const char *data, size_t len)
{
    if (!g_ws_ctx.initialized || !g_ws_ctx.client)
    {
        ESP_LOGE(TAG, "WebSocket未初始化或客户端不存在");
        return ESP_ERR_INVALID_STATE;
    }

    if (!data || len == 0 || len > INT_MAX)
    {
        ESP_LOGE(TAG, "无效的数据参数");
        return ESP_ERR_INVALID_ARG;
    }

    if (!esp_websocket_client_is_connected(g_ws_ctx.client))
    {
        ESP_LOGE(TAG, "WebSocket未连接");
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_text(g_ws_ctx.client, data, (int)len, portMAX_DELAY);
    if (sent <= 0)
    {
        ESP_LOGE(TAG, "发送文本数据失败: %d", sent);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 发送二进制数据
 */
esp_err_t ws_send_binary(const void *data, size_t len)
{
    if (!g_ws_ctx.initialized || !g_ws_ctx.client)
    {
        ESP_LOGE(TAG, "WebSocket未初始化或客户端不存在");
        return ESP_ERR_INVALID_STATE;
    }

    if (!data || len == 0 || len > INT_MAX)
    {
        ESP_LOGE(TAG, "无效的数据参数");
        return ESP_ERR_INVALID_ARG;
    }

    if (!esp_websocket_client_is_connected(g_ws_ctx.client))
    {
        ESP_LOGE(TAG, "WebSocket未连接");
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_bin(g_ws_ctx.client, (const char *)data, (int)len, portMAX_DELAY);
    if (sent <= 0)
    {
        ESP_LOGE(TAG, "发送二进制数据失败: %d", sent);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 检查连接状态
 */
bool ws_is_connected(void)
{
    if (!g_ws_ctx.initialized || !g_ws_ctx.client)
    {
        return false;
    }

    return esp_websocket_client_is_connected(g_ws_ctx.client);
}

/**
 * @brief 注册数据接收回调
 */
void ws_register_data_handler(ws_data_handler_t handler)
{
    g_ws_ctx.data_handler = handler;
    if(handler) {
        ESP_LOGI(TAG, "数据接收回调已注册");
    } else {
        ESP_LOGW(TAG, "未注册数据处理回调");
    }
}

/**
 * @brief 注册状态变化回调
 */
void ws_register_state_handler(ws_state_handler_t handler)
{
    g_ws_ctx.state_handler = handler;
    ESP_LOGI(TAG, "状态变化回调已注册");
}

/**
 * @brief 反初始化WebSocket客户端
 */
void ws_deinit(void)
{
    if (!g_ws_ctx.initialized)
    {
        return;
    }

    // 停止WebSocket
    ws_stop();

    // 删除定时器
    if (g_ws_ctx.reconnect_timer)
    {
        xTimerDelete(g_ws_ctx.reconnect_timer, portMAX_DELAY);
        g_ws_ctx.reconnect_timer = NULL;
    }

    // 删除互斥锁
    if (g_ws_ctx.mutex)
    {
        vSemaphoreDelete(g_ws_ctx.mutex);
        g_ws_ctx.mutex = NULL;
    }

    g_ws_ctx.initialized = false;
    ESP_LOGI(TAG, "WebSocket客户端已反初始化");
}