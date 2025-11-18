#include <string.h>
// 在文件顶部添加重连相关的全局变量
#include <stdlib.h>
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_tls.h"
#include "websocket.h"
#include "freertos/task.h"
#include <esp_wifi.h>
#include <time.h>
#include <sys/time.h>

// 日志标签
static const char *TAG = "WS_CLIENT";

// WebSocket客户端句柄
static esp_websocket_client_handle_t client = NULL;

// 接收数据处理函数指针（留给用户实现具体逻辑）
static void (*ws_recv_handler)(const char *data, size_t len) = NULL;

// 重连相关全局变量
static const char *current_ws_uri = NULL;                  // 保存当前连接的URI
static TaskHandle_t reconnect_task_handle = NULL;          // 重连任务句柄
static int reconnect_count = 0;                            // 当前重连次数
static const int MAX_RECONNECT_ATTEMPTS = 15;              // 最大重连次数
static const int INITIAL_RECONNECT_INTERVAL_MS = 3000;     // 初始重连间隔(毫秒)
static const int MAX_RECONNECT_INTERVAL_MS = 30000;        // 最大重连间隔(毫秒)
static bool is_manually_disconnected = false;              // 标记是否手动断开连接
static portMUX_TYPE ws_mux = portMUX_INITIALIZER_UNLOCKED; // 互斥锁，保护共享资源

// 在文件顶部的函数声明区域添加 forward declaration
static void reconnect_task(void *pvParameters);

/**
 * @brief WebSocket事件回调函数（增强版，添加所有事件处理和重连优化）
 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "与服务器建立连接成功");
        // 使用互斥锁保护共享资源
        portENTER_CRITICAL(&ws_mux);
        reconnect_count = 0; // 重置重连计数
        portEXIT_CRITICAL(&ws_mux);

        // 获取MAC地址和时间戳并发送给服务器
        uint8_t mac_addr[6];
        char mac_str[18]; // 格式: XX:XX:XX:XX:XX:XX
        if (esp_wifi_get_mac(WIFI_IF_STA, mac_addr) == ESP_OK)
        {
            sprintf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X",
                    mac_addr[0], mac_addr[1], mac_addr[2],
                    mac_addr[3], mac_addr[4], mac_addr[5]);
        }
        else
        {
            strcpy(mac_str, "unknown");
        }

        // 获取当前时间戳
        time_t now = time(NULL);
        char timestamp_str[32];
        sprintf(timestamp_str, "%lld", (long long)now);

        // 构建JSON数据
        char device_info_json[256]; // 确保有足够空间
        sprintf(device_info_json, "{\"type\":\"device_info\",\"mac\":\"%s\",\"timestamp\":%s}",
                mac_str, timestamp_str);

        // 发送设备信息到服务器
        ws_send_json(device_info_json, strlen(device_info_json));

        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "与服务器断开连接，准备重连");
        // 检查是否是手动断开的连接
        portENTER_CRITICAL(&ws_mux);
        bool manual_disconnect = is_manually_disconnected;
        portEXIT_CRITICAL(&ws_mux);

        if (!manual_disconnect && reconnect_task_handle == NULL && current_ws_uri != NULL)
        {
            ESP_LOGI(TAG, "启动重连任务");
            xTaskCreate(reconnect_task, "websocket_reconnect", 4096,
                        (void *)current_ws_uri, 4, &reconnect_task_handle);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        // 接收到服务器数据，调用用户注册的处理函数
        if (ws_recv_handler && data->data_len > 0)
        {
            char *recv_data = malloc(data->data_len + 1);
            if (recv_data != NULL)
            {
                memcpy(recv_data, data->data_ptr, data->data_len);
                recv_data[data->data_len] = '\0'; // 字符串结束符
                ws_recv_handler(recv_data, data->data_len);
                free(recv_data); // 释放内存
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket错误事件，错误代码: %d", (int)event_id);
        // 打印错误详情
        if (data && data->error_handle.error_type != WEBSOCKET_ERROR_TYPE_NONE)
        {
            ESP_LOGE(TAG, "错误类型: %d", data->error_handle.error_type);
            ESP_LOGE(TAG, "ESP错误码: %s", esp_err_to_name(data->error_handle.esp_tls_last_esp_err));
            if (data->error_handle.esp_transport_sock_errno != 0)
            {
                ESP_LOGE(TAG, "套接字错误码: %d (%s)",
                         data->error_handle.esp_transport_sock_errno,
                         strerror(data->error_handle.esp_transport_sock_errno));
            }

            // 特定错误处理
            if (data->error_handle.esp_tls_last_esp_err == ESP_ERR_MBEDTLS_SSL_WRITE_FAILED ||
                data->error_handle.esp_tls_last_esp_err == ESP_ERR_ESP_TLS_TCP_CLOSED_FIN)
            {
                ESP_LOGE(TAG, "检测到SSL连接错误，需要重新建立连接");

                // 停止现有客户端并尝试重连
                if (client != NULL)
                {
                    portENTER_CRITICAL(&ws_mux);
                    esp_websocket_client_stop(client);
                    esp_websocket_client_destroy(client);
                    client = NULL;
                    portEXIT_CRITICAL(&ws_mux);
                }

                if (reconnect_task_handle == NULL && current_ws_uri != NULL)
                {
                    ESP_LOGI(TAG, "启动重连任务以恢复SSL连接");
                    xTaskCreate(reconnect_task, "websocket_reconnect", 4096,
                                (void *)current_ws_uri, 4, &reconnect_task_handle);
                }
            }
        }
        break;

    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "WebSocket连接已干净关闭");
        // 连接关闭也触发重连
        portENTER_CRITICAL(&ws_mux);
        bool manual_close = is_manually_disconnected;
        portEXIT_CRITICAL(&ws_mux);

        if (!manual_close && reconnect_task_handle == NULL && current_ws_uri != NULL)
        {
            ESP_LOGI(TAG, "连接关闭后启动重连任务");
            xTaskCreate(reconnect_task, "websocket_reconnect", 4096,
                        (void *)current_ws_uri, 4, &reconnect_task_handle);
        }
        break;

    case WEBSOCKET_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "准备建立WebSocket连接...");
        break;

    case WEBSOCKET_EVENT_BEGIN:
        ESP_LOGI(TAG, "WebSocket客户端线程已创建，准备进入事件循环");
        break;

    case WEBSOCKET_EVENT_FINISH:
        ESP_LOGI(TAG, "WebSocket客户端事件循环已结束，准备销毁线程");
        break;

    default:
        ESP_LOGW(TAG, "未知事件ID: %d", (int)event_id);
        break;
    }
}

/**
 * @brief 重连任务函数（实现指数退避重连策略，修复版）
 */
static void reconnect_task(void *pvParameters)
{
    char *ws_uri = (char *)pvParameters;
    int current_interval = INITIAL_RECONNECT_INTERVAL_MS;
    bool reconnected = false;
    int local_reconnect_count = reconnect_count; // 使用本地变量跟踪重连次数
    bool client_needs_destroy = false;

    while (local_reconnect_count < MAX_RECONNECT_ATTEMPTS && !reconnected)
    {
        ESP_LOGI(TAG, "尝试重连服务器 (第 %d/%d 次)，间隔: %d ms",
                 local_reconnect_count + 1, MAX_RECONNECT_ATTEMPTS, current_interval);

        // 1. 先在临界区检查客户端状态并标记
        portENTER_CRITICAL(&ws_mux);
        client_needs_destroy = (client != NULL);
        portEXIT_CRITICAL(&ws_mux);

        // 2. 在临界区外停止和销毁客户端（避免死锁）
        if (client_needs_destroy)
        {
            // 再次检查，因为可能已经被其他任务修改
            portENTER_CRITICAL(&ws_mux);
            if (client != NULL)
            {
                // 先设置为NULL，防止其他任务访问
                esp_websocket_client_handle_t temp_client = client;
                client = NULL;
                portEXIT_CRITICAL(&ws_mux);

                // 在临界区外执行阻塞操作
                esp_websocket_client_stop(temp_client);
                esp_websocket_client_destroy(temp_client);
            }
            else
            {
                portEXIT_CRITICAL(&ws_mux);
            }
        }

        // 重新初始化并启动客户端
        esp_err_t ret = ws_start(ws_uri);

        if (ret == ESP_OK)
        {
            // 等待短暂时间验证连接是否稳定
            vTaskDelay(pdMS_TO_TICKS(1000));

            portENTER_CRITICAL(&ws_mux);
            bool is_stable = (client != NULL && esp_websocket_client_is_connected(client));
            portEXIT_CRITICAL(&ws_mux);

            if (is_stable)
            {
                ESP_LOGI(TAG, "重连成功且连接稳定");
                portENTER_CRITICAL(&ws_mux);
                reconnect_count = 0; // 立即重置全局重连计数
                portEXIT_CRITICAL(&ws_mux);
                reconnected = true;
            }
            else
            {
                ESP_LOGW(TAG, "重连成功但连接不稳定，准备重试");
                local_reconnect_count++;
                portENTER_CRITICAL(&ws_mux);
                reconnect_count = local_reconnect_count;
                portEXIT_CRITICAL(&ws_mux);

                // 添加随机抖动，避免多个设备同时重连导致服务器压力
                int jitter = rand() % 1000 - 500; // -500ms到+500ms的随机值
                vTaskDelay(pdMS_TO_TICKS(current_interval + jitter));

                // 指数退避：下一次重连间隔增加50%，但不超过最大值
                current_interval = current_interval * 3 / 2;
                if (current_interval > MAX_RECONNECT_INTERVAL_MS)
                {
                    current_interval = MAX_RECONNECT_INTERVAL_MS;
                }
            }
        }
        else
        {
            ESP_LOGE(TAG, "重连失败: %s", esp_err_to_name(ret));

            // 重连失败，增加计数并等待，实现指数退避
            local_reconnect_count++;
            portENTER_CRITICAL(&ws_mux);
            reconnect_count = local_reconnect_count;
            portEXIT_CRITICAL(&ws_mux);

            // 添加随机抖动
            int jitter = rand() % 1000 - 500;
            vTaskDelay(pdMS_TO_TICKS(current_interval + jitter));

            // 指数退避：下一次重连间隔增加50%，但不超过最大值
            current_interval = current_interval * 3 / 2;
            if (current_interval > MAX_RECONNECT_INTERVAL_MS)
            {
                current_interval = MAX_RECONNECT_INTERVAL_MS;
            }
        }
    }

    if (!reconnected)
    {
        ESP_LOGE(TAG, "达到最大重连次数 (%d次)，停止重连尝试", MAX_RECONNECT_ATTEMPTS);
    }

    // 清理任务句柄
    portENTER_CRITICAL(&ws_mux);
    reconnect_task_handle = NULL;
    portEXIT_CRITICAL(&ws_mux);
    vTaskDelete(NULL);
}

/**
 * @brief 启动WebSocket客户端并连接服务器（增强版）
 */
esp_err_t ws_start(const char *ws_uri)
{
    // 新增：参数合法性校验
    if (ws_uri == NULL || strlen(ws_uri) == 0)
    {
        ESP_LOGE(TAG, "传入的WSS地址为空或无效");
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&ws_mux);
    if (client != NULL)
    {
        portEXIT_CRITICAL(&ws_mux);
        ESP_LOGW(TAG, "WebSocket客户端已处于运行状态");
        return ESP_OK;
    }
    portEXIT_CRITICAL(&ws_mux);

    // 保存当前URI用于重连
    current_ws_uri = ws_uri;

    // 基于结构体定义的正确wss配置（优化配置）
    esp_websocket_client_config_t ws_cfg = {
        .uri = ws_uri,                             // 使用传入的参数作为WSS地址
        .transport = WEBSOCKET_TRANSPORT_OVER_SSL, // wss必须用SSL传输类型
        .cert_pem = server_cert_pem,               // 自签名证书
        .skip_cert_common_name_check = true,       // 跳过域名检查
        .disable_auto_reconnect = true,            // 禁用内置重连，使用我们自定义的重连逻辑
        .task_prio = 3,                            // 进一步降低任务优先级，减少CPU占用
        .buffer_size = 1024 * 16,                  // 进一步增大缓冲区到16KB
        .ping_interval_sec = 5,                    // 进一步缩短PING间隔到5秒
        .reconnect_timeout_ms = 5000,              // 增加重连间隔到5秒
        .network_timeout_ms = 20000,               // 增加网络超时到20秒
    };

    // 创建客户端实例
    client = esp_websocket_client_init(&ws_cfg);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "客户端初始化失败");
        return ESP_FAIL;
    }

    // 注册事件回调
    esp_err_t ret = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                                  websocket_event_handler, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "注册事件回调失败: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(client);
        client = NULL;
        return ret;
    }

    // 启动客户端
    ret = esp_websocket_client_start(client);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "启动客户端失败: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(client);
        client = NULL;
        return ret;
    }

    // 初始化WebSocket之后注册接收处理函数
    // 移除对未定义函数的调用 - 这个处理函数应该由用户代码在外部注册
    // ws_register_recv_handler(ws_message_handler);  // 这一行必须被正确注释或删除

    ESP_LOGI(TAG, "WebSocket客户端启动成功，正在连接服务器: %s", ws_uri);
    return ESP_OK;
}

/**
 * @brief 向服务器发送二进制数据（增强版，添加重连支持）
 */
esp_err_t ws_send_binary(const void *binary_data, size_t len)
{
    // 1. 基础校验：客户端状态和数据合法性
    portENTER_CRITICAL(&ws_mux);
    bool is_client_null = (client == NULL);
    portEXIT_CRITICAL(&ws_mux);

    if (is_client_null)
    {
        ESP_LOGE(TAG, "二进制发送失败：客户端未初始化");
        return ESP_FAIL;
    }

    // 2. 检查连接状态，如果未连接则尝试重新连接
    portENTER_CRITICAL(&ws_mux);
    bool is_connected = esp_websocket_client_is_connected(client);
    portEXIT_CRITICAL(&ws_mux);

    if (!is_connected)
    {
        ESP_LOGE(TAG, "二进制发送失败：客户端未连接服务器，尝试重连...");

        // 如果重连任务未运行，则启动它
        portENTER_CRITICAL(&ws_mux);
        bool need_reconnect = (reconnect_task_handle == NULL && current_ws_uri != NULL);
        portEXIT_CRITICAL(&ws_mux);

        if (need_reconnect)
        {
            ESP_LOGI(TAG, "启动重连任务");
            xTaskCreate(reconnect_task, "websocket_reconnect", 4096,
                        (void *)current_ws_uri, 4, &reconnect_task_handle);
        }

        return ESP_FAIL;
    }

    // 3. 数据校验
    if (binary_data == NULL || len == 0 || len > INT_MAX)
    {
        ESP_LOGE(TAG, "二进制发送失败：数据为空/长度为0/长度超出int范围");
        return ESP_ERR_INVALID_ARG;
    }

    // 4. 调用底层二进制发送函数，添加重试机制
    int send_len = -1;
    int retry_count = 0;
    const int MAX_RETRY = 2;

    while (retry_count < MAX_RETRY && send_len <= 0)
    {
        send_len = esp_websocket_client_send_bin(
            client,
            (const char *)binary_data,
            (int)len,
            portMAX_DELAY);

        if (send_len <= 0)
        {
            ESP_LOGE(TAG, "二进制发送失败：错误码=%d，原因：%s，正在重试(%d/%d)",
                     send_len, esp_err_to_name(-send_len), retry_count + 1, MAX_RETRY);
            retry_count++;

            // 短暂延迟后重试
            vTaskDelay(pdMS_TO_TICKS(10));

            // 检查连接状态，如果已断开则不再重试
            portENTER_CRITICAL(&ws_mux);
            bool still_connected = esp_websocket_client_is_connected(client);
            portEXIT_CRITICAL(&ws_mux);

            if (!still_connected)
            {
                ESP_LOGE(TAG, "连接已断开，停止重试");
                break;
            }
        }
    }

    // 5. 解析返回值
    if (send_len > 0)
    {
        return ESP_OK;
    }
    else
    {
        // 失败：send_len为负数，尝试重连
        ESP_LOGE(TAG, "二进制发送失败：达到最大重试次数，错误码=%d，原因：%s",
                 send_len, esp_err_to_name(-send_len));

        // 如果是连接问题，尝试重连
        portENTER_CRITICAL(&ws_mux);
        bool need_reconnect = (reconnect_task_handle == NULL && current_ws_uri != NULL);
        portEXIT_CRITICAL(&ws_mux);

        if (need_reconnect)
        {
            ESP_LOGI(TAG, "发送失败可能是连接问题，启动重连任务");
            xTaskCreate(reconnect_task, "websocket_reconnect", 4096,
                        (void *)current_ws_uri, 4, &reconnect_task_handle);
        }

        return ESP_FAIL;
    }
}

/**
 * @brief 向服务器发送JSON文本数据（增强版，添加重连支持）
 */
esp_err_t ws_send_json(const char *json_data, size_t len)
{
    // 1. 基础校验：客户端状态和数据合法性
    portENTER_CRITICAL(&ws_mux);
    bool is_client_null = (client == NULL);
    portEXIT_CRITICAL(&ws_mux);

    if (is_client_null)
    {
        ESP_LOGE(TAG, "JSON发送失败：客户端未初始化");
        return ESP_FAIL;
    }

    // 2. 检查连接状态，如果未连接则尝试重新连接
    portENTER_CRITICAL(&ws_mux);
    bool is_connected = esp_websocket_client_is_connected(client);
    portEXIT_CRITICAL(&ws_mux);

    if (!is_connected)
    {
        ESP_LOGE(TAG, "JSON发送失败：客户端未连接服务器，尝试重连...");

        // 如果重连任务未运行，则启动它
        portENTER_CRITICAL(&ws_mux);
        bool need_reconnect = (reconnect_task_handle == NULL && current_ws_uri != NULL);
        portEXIT_CRITICAL(&ws_mux);

        if (need_reconnect)
        {
            ESP_LOGI(TAG, "启动重连任务");
            xTaskCreate(reconnect_task, "websocket_reconnect", 4096,
                        (void *)current_ws_uri, 4, &reconnect_task_handle);
        }

        return ESP_FAIL;
    }

    // 3. 数据校验
    if (json_data == NULL || len == 0 || len > INT_MAX)
    {
        ESP_LOGE(TAG, "JSON发送失败：数据为空/长度为0/长度超出int范围");
        return ESP_ERR_INVALID_ARG;
    }

    // 4. 调用底层文本发送函数，添加重试机制
    int send_len = -1;
    int retry_count = 0;
    const int MAX_RETRY = 2;

    while (retry_count < MAX_RETRY && send_len <= 0)
    {
        send_len = esp_websocket_client_send_text(
            client,
            json_data,
            (int)len,
            portMAX_DELAY);

        if (send_len <= 0)
        {
            ESP_LOGE(TAG, "JSON发送失败：错误码=%d，原因：%s，正在重试(%d/%d)",
                     send_len, esp_err_to_name(-send_len), retry_count + 1, MAX_RETRY);
            retry_count++;

            // 短暂延迟后重试
            vTaskDelay(pdMS_TO_TICKS(10));

            // 检查连接状态，如果已断开则不再重试
            portENTER_CRITICAL(&ws_mux);
            bool still_connected = esp_websocket_client_is_connected(client);
            portEXIT_CRITICAL(&ws_mux);

            if (!still_connected)
            {
                ESP_LOGE(TAG, "连接已断开，停止重试");
                break;
            }
        }
    }

    // 5. 解析返回值
    if (send_len > 0)
    {
        return ESP_OK;
    }
    else
    {
        // 失败：send_len为负数，尝试重连
        ESP_LOGE(TAG, "JSON发送失败：达到最大重试次数，错误码=%d，原因：%s",
                 send_len, esp_err_to_name(-send_len));

        // 如果是连接问题，尝试重连
        portENTER_CRITICAL(&ws_mux);
        bool need_reconnect = (reconnect_task_handle == NULL && current_ws_uri != NULL);
        portEXIT_CRITICAL(&ws_mux);

        if (need_reconnect)
        {
            ESP_LOGI(TAG, "发送失败可能是连接问题，启动重连任务");
            xTaskCreate(reconnect_task, "websocket_reconnect", 4096,
                        (void *)current_ws_uri, 4, &reconnect_task_handle);
        }

        return ESP_FAIL;
    }
}

/**
 * @brief 停止WebSocket客户端并释放资源（增强版）
 */
void ws_stop(void)
{
    // 标记为手动断开连接
    portENTER_CRITICAL(&ws_mux);
    is_manually_disconnected = true;
    portEXIT_CRITICAL(&ws_mux);

    // 停止重连任务
    if (reconnect_task_handle != NULL)
    {
        vTaskDelete(reconnect_task_handle);
        reconnect_task_handle = NULL;
    }

    // 停止客户端
    portENTER_CRITICAL(&ws_mux);
    if (client != NULL)
    {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        client = NULL;
        ESP_LOGI(TAG, "WebSocket客户端已停止");
    }
    portEXIT_CRITICAL(&ws_mux);

    // 重置重连计数
    portENTER_CRITICAL(&ws_mux);
    reconnect_count = 0;
    // 重置手动断开标记
    is_manually_disconnected = false;
    portEXIT_CRITICAL(&ws_mux);
}

esp_err_t is_ws_connected(void)
{
    if (client == NULL)
    {
        return false;
    }
    return esp_websocket_client_is_connected(client);
}

/**
 * @brief 注册接收数据处理函数
 * @param handler: 自定义处理函数（收到数据时回调）
 */
void ws_register_recv_handler(void (*handler)(const char *data, size_t len))
{
    ws_recv_handler = handler;
    ESP_LOGI(TAG, "接收数据处理函数注册成功");
}

/**
 * @brief 立即尝试重连服务器
 * @return ESP_OK: 成功; 其他: 失败
 */
esp_err_t ws_reconnect_now(void)
{
    if (current_ws_uri == NULL)
    {
        ESP_LOGE(TAG, "无法重连：未设置WebSocket服务器地址");
        return ESP_ERR_INVALID_STATE;
    }

    // 停止现有重连任务
    if (reconnect_task_handle != NULL)
    {
        vTaskDelete(reconnect_task_handle);
        reconnect_task_handle = NULL;
    }

    // 重置重连计数并立即重连
    reconnect_count = 0;
    return ws_start(current_ws_uri);
}

/**
 * @brief 获取当前重连计数
 */
int ws_get_reconnect_count(void)
{
    return reconnect_count;
}

/**
 * @brief 重置重连计数
 */
void ws_reset_reconnect_count(void)
{
    reconnect_count = 0;
}